// SPDX-License-Identifier: GPL-2.0-or-later
#include "ControlledLoader.h"

#include "core/loader/elf.h"

#include <windows.h>
#include <memoryapi.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Lab {
namespace {

constexpr std::uint64_t MaxProgramHeaders = 256;
constexpr std::uint64_t MaxSelfSegments = 64;
constexpr std::uint64_t MaxMappedBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t MaxSelfMemory = 1ull * 1024ull * 1024ull * 1024ull;

void Require(bool condition, char const* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Reader {
    std::ifstream input;
    std::uint64_t size{};

    explicit Reader(std::filesystem::path const& path) : input(path, std::ios::binary) {
        std::error_code error;
        size = std::filesystem::file_size(path, error);
        Require(!error && input.is_open(), "Arquivo ELF/SELF não pôde ser aberto.");
    }

    void Read(std::uint64_t offset, void* destination, std::size_t count) {
        Require(offset <= size && count <= size - offset, "Região do ELF/SELF fora do arquivo.");
        Require(offset <= static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),
                "Offset do ELF/SELF excede o limite do sistema.");
        input.clear();
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        Require(input.good(), "Falha ao posicionar no ELF/SELF.");
        input.read(static_cast<char*>(destination), static_cast<std::streamsize>(count));
        Require(input.gcount() == static_cast<std::streamsize>(count), "Leitura incompleta do ELF/SELF.");
    }

    template <typename T>
    T Object(std::uint64_t offset) {
        T value{};
        Read(offset, &value, sizeof(value));
        return value;
    }
};

std::uint64_t AddChecked(std::uint64_t left, std::uint64_t right, char const* message) {
    Require(right <= std::numeric_limits<std::uint64_t>::max() - left, message);
    return left + right;
}

std::uint64_t Fnv1a(std::vector<std::uint8_t> const& bytes) {
    std::uint64_t hash = 1469598103934665603ull;
    for (auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

struct Allocation {
    void* value{};
    ~Allocation() { if (value) VirtualFree(value, 0, MEM_RELEASE); }
};

bool IsElf(elf_header const& header) {
    return header.e_ident.magic[EI_MAG0] == ELFMAG0 &&
           header.e_ident.magic[EI_MAG1] == ELFMAG1 &&
           header.e_ident.magic[EI_MAG2] == ELFMAG2 &&
           header.e_ident.magic[EI_MAG3] == ELFMAG3;
}

bool IsSelf(self_header const& header) {
    return header.magic == self_header::signature && header.version == 0 &&
           header.mode == 1 && header.endian == 1 && header.attributes == 0x12 &&
           header.category == 1 && header.program_type == 1;
}

void ValidateElfIdentity(elf_header const& header) {
    Require(IsElf(header), "Assinatura ELF inválida.");
    Require(header.e_ident.ei_class == ELF_CLASS_64 && header.e_ident.ei_data == ELF_DATA_2LSB &&
                header.e_ident.ei_version == ELF_VERSION_CURRENT &&
                header.e_ident.ei_osabi == ELF_OSABI_FREEBSD &&
                header.e_ident.ei_abiversion == ELF_ABI_VERSION_AMDGPU_HSA_V2,
            "Identidade ELF incompatível com o formato PS4.");
    Require(header.e_type == ET_SCE_EXEC || header.e_type == ET_SCE_DYNEXEC ||
                header.e_type == ET_SCE_DYNAMIC,
            "Tipo ELF PS4 não suportado.");
    Require(header.e_machine == EM_X86_64 && header.e_version == EV_CURRENT,
            "Arquitetura ELF incompatível com o formato PS4.");
    Require(header.e_ehsize == sizeof(elf_header) &&
                header.e_phentsize == sizeof(elf_program_header),
            "Tamanho de cabeçalho ELF incompatível.");
    Require(header.e_phnum > 0 && header.e_phnum <= MaxProgramHeaders,
            "Quantidade de program headers ELF inválida.");
}

using LogicalRead = std::function<void(std::uint64_t, void*, std::size_t)>;

ControlledLoadResult LoadElf(Reader& reader, elf_header const& header, std::uint64_t headerOffset,
                             LogicalRead logicalRead) {
    ValidateElfIdentity(header);
    auto tableBytes = AddChecked(0, static_cast<std::uint64_t>(header.e_phnum) * sizeof(elf_program_header),
                                 "Tabela de program headers ELF excede o limite.");
    auto tableOffset = AddChecked(headerOffset, header.e_phoff, "Tabela de program headers ELF excede o limite.");
    Require(tableBytes <= reader.size && tableOffset <= reader.size - tableBytes,
            "Tabela de program headers ELF fora do arquivo.");

    std::vector<elf_program_header> programs(header.e_phnum);
    reader.Read(tableOffset, programs.data(), static_cast<std::size_t>(tableBytes));

    ControlledLoadResult result;
    result.recognized = true;
    result.validated = true;
    result.file_size = reader.size;
    result.entry = header.e_entry;
    result.segment_count = programs.size();
    result.min_virtual_address = std::numeric_limits<std::uint64_t>::max();
    result.max_virtual_address = 0;

    // Read only the runtime metadata tables. This audit never resolves an
    // import and never follows an initializer; it records what a future
    // UWP loader would still need before guest control flow is possible.
    for (auto const& program : programs) {
        if (program.p_type == PT_TLS) {
            result.has_tls = true;
            ++result.tls_segments;
        }
        if (program.p_type != PT_DYNAMIC) continue;
        result.has_dynamic = true;
        ++result.dynamic_segments;
        constexpr std::uint64_t maxDynamicBytes = 4ull * 1024ull * 1024ull;
        if (program.p_filesz == 0 || program.p_filesz > maxDynamicBytes ||
            program.p_filesz % sizeof(elf_dynamic) != 0)
            continue;
        const auto count = program.p_filesz / sizeof(elf_dynamic);
        std::vector<elf_dynamic> dynamic(count);
        logicalRead(program.p_offset, dynamic.data(), static_cast<std::size_t>(program.p_filesz));
        std::uint64_t relaSize = 0;
        std::uint64_t relaEntrySize = sizeof(elf_relocation);
        std::uint64_t jmpRelaSize = 0;
        for (auto const& entry : dynamic) {
            if (entry.d_tag == DT_NULL) break;
            ++result.dynamic_entries;
            switch (entry.d_tag) {
            case DT_SCE_RELASZ:
                relaSize = entry.d_un.d_val;
                break;
            case DT_SCE_RELAENT:
                relaEntrySize = entry.d_un.d_val;
                break;
            case DT_SCE_PLTRELSZ:
                jmpRelaSize = entry.d_un.d_val;
                break;
            case DT_SCE_IMPORT_LIB:
                ++result.import_libraries;
                break;
            case DT_SCE_NEEDED_MODULE:
            case DT_NEEDED:
                ++result.needed_modules;
                break;
            default:
                break;
            }
        }
        if (relaEntrySize == sizeof(elf_relocation) && relaSize % relaEntrySize == 0)
            result.rela_entries = relaSize / relaEntrySize;
        if (jmpRelaSize % sizeof(elf_relocation) == 0)
            result.jmp_rela_entries = jmpRelaSize / sizeof(elf_relocation);
        result.has_relocations = result.rela_entries != 0 || result.jmp_rela_entries != 0;
        result.has_imports = result.import_libraries != 0 || result.needed_modules != 0;
    }

    struct LoadRange { elf_program_header header; };
    std::vector<LoadRange> loads;
    for (auto const& program : programs) {
        if (program.p_type != PT_LOAD) continue;
        ++result.load_segments;
        Require(program.p_filesz <= program.p_memsz, "Segmento ELF tem filesz maior que memsz.");
        Require(program.p_memsz > 0, "Segmento ELF vazio.");
        Require(program.p_filesz <= MaxMappedBytes, "Segmento ELF excede o limite seguro.");
        auto end = AddChecked(program.p_vaddr, program.p_memsz, "Endereço virtual ELF excede o limite.");
        if (program.p_align > 1)
            Require((program.p_align & (program.p_align - 1)) == 0 && program.p_align <= 64ull * 1024ull * 1024ull,
                    "Alinhamento de segmento ELF inválido.");
        result.min_virtual_address = (std::min)(result.min_virtual_address, program.p_vaddr);
        result.max_virtual_address = (std::max)(result.max_virtual_address, end);
        loads.push_back({program});
    }
    Require(!loads.empty(), "ELF não possui segmento PT_LOAD.");
    Require(result.max_virtual_address >= result.min_virtual_address &&
                result.max_virtual_address - result.min_virtual_address <= MaxMappedBytes,
            "Mapa de segmentos ELF excede o limite seguro.");
    bool entryInExecutable = false;
    for (auto const& load : loads) {
        auto const& program = load.header;
        if ((program.p_flags & PF_EXEC) != 0 && result.entry >= program.p_vaddr &&
            result.entry < program.p_vaddr + program.p_memsz)
            entryInExecutable = true;
    }
    Require(entryInExecutable, "Ponto de entrada ELF não está em segmento executável.");

    std::vector<std::uint8_t> mapped(static_cast<std::size_t>(result.max_virtual_address - result.min_virtual_address));
    for (auto const& load : loads) {
        auto const& program = load.header;
        if (program.p_filesz == 0) continue;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(program.p_filesz));
        logicalRead(program.p_offset, bytes.data(), bytes.size());
        auto target = static_cast<std::size_t>(program.p_vaddr - result.min_virtual_address);
        std::copy(bytes.begin(), bytes.end(), mapped.begin() + target);
    }
    result.mapped = true;
    result.mapped_bytes = mapped.size();
    result.checksum = Fnv1a(mapped);
    result.detail = L"ELF validado e mapeado em buffer privado não executável. "
                    L"Nenhum byte do arquivo recebeu controle de fluxo.";
    return result;
}

ControlledLoadResult LoadSelf(Reader& reader, self_header const& header) {
    Require(IsSelf(header), "Cabeçalho SELF incompatível com o formato PS4.");
    Require(reader.size >= sizeof(self_header), "Cabeçalho SELF truncado.");
    if (header.file_size != 0)
        Require(header.file_size <= reader.size, "Tamanho declarado pelo SELF excede o arquivo.");
    Require(header.segment_count > 0 && header.segment_count <= MaxSelfSegments,
            "Quantidade de segmentos SELF inválida.");
    auto tableBytes = static_cast<std::uint64_t>(header.segment_count) * sizeof(self_segment_header);
    Require(tableBytes <= reader.size - sizeof(self_header), "Tabela SELF fora do arquivo.");
    std::vector<self_segment_header> segments(header.segment_count);
    reader.Read(sizeof(self_header), segments.data(), static_cast<std::size_t>(tableBytes));

    ControlledLoadResult result;
    result.recognized = true;
    result.self = true;
    result.validated = true;
    result.file_size = reader.size;
    result.segment_count = segments.size();
    for (auto const& segment : segments) {
        Require(segment.file_offset <= reader.size && segment.file_size <= reader.size - segment.file_offset,
                "Dados do segmento SELF fora do arquivo.");
        Require(segment.file_size <= segment.memory_size && segment.memory_size <= MaxSelfMemory,
                "Tamanho de memória SELF inválido.");
        result.mapped_bytes = AddChecked(result.mapped_bytes, segment.memory_size,
                                         "Memória total SELF excede o limite.");
        if (segment.IsEncrypted() || segment.IsCompressed()) result.protected_segments = true;
    }
    Require(result.mapped_bytes <= MaxMappedBytes, "Mapa SELF excede o limite seguro.");
    if (result.protected_segments) {
        result.detail = L"SELF reconhecido e limites dos segmentos validados; conteúdo protegido ou comprimido. "
                        L"O mapa executável fica bloqueado até existir descriptografia compatível.";
        return result;
    }

    const auto elfOffset = sizeof(self_header) + tableBytes;
    auto innerHeader = reader.Object<elf_header>(elfOffset);
    Require(IsElf(innerHeader), "SELF não contém um ELF interno no offset esperado.");

    std::vector<elf_program_header> programs;
    if (innerHeader.e_phnum > 0 && innerHeader.e_phnum <= MaxProgramHeaders &&
        innerHeader.e_phentsize == sizeof(elf_program_header)) {
        const auto programBytes = static_cast<std::uint64_t>(innerHeader.e_phnum) * sizeof(elf_program_header);
        const auto programOffset = AddChecked(elfOffset, innerHeader.e_phoff,
                                              "Tabela ELF interna excede o limite.");
        Require(programBytes <= reader.size && programOffset <= reader.size - programBytes,
                "Tabela ELF interna fora do arquivo.");
        programs.resize(innerHeader.e_phnum);
        reader.Read(programOffset, programs.data(), static_cast<std::size_t>(programBytes));
    }
    Require(!programs.empty(), "Tabela ELF interna inválida.");

    LogicalRead selfRead = [&](std::uint64_t offset, void* destination, std::size_t count) {
        auto* output = static_cast<std::uint8_t*>(destination);
        while (count != 0) {
            const self_segment_header* selected = nullptr;
            const elf_program_header* selectedProgram = nullptr;
            for (std::size_t index = 0; index < segments.size(); ++index) {
                const auto& segment = segments[index];
                if (!segment.IsBlocked() || segment.GetId() >= programs.size()) continue;
                const auto& program = programs[segment.GetId()];
                if (offset >= program.p_offset && offset < program.p_offset + program.p_filesz) {
                    selected = &segment;
                    selectedProgram = &program;
                    break;
                }
            }
            Require(selected && selectedProgram, "Segmento ELF interno não possui correspondência SELF.");
            const auto delta = offset - selectedProgram->p_offset;
            const auto available = selectedProgram->p_filesz - delta;
            const auto part = (std::min<std::uint64_t>)(available, count);
            Require(delta <= selected->file_size && part <= selected->file_size - delta,
                    "Dados do segmento SELF interno fora dos limites.");
            reader.Read(selected->file_offset + delta, output, static_cast<std::size_t>(part));
            offset += part;
            output += part;
            count -= static_cast<std::size_t>(part);
        }
    };
    auto inner = LoadElf(reader, innerHeader, elfOffset, std::move(selfRead));
    result.inner_elf = inner.recognized;
    result.inner_mapped = inner.mapped;
    result.inner_segment_count = inner.segment_count;
    result.inner_load_segments = inner.load_segments;
    result.inner_entry = inner.entry;
    result.inner_mapped_bytes = inner.mapped_bytes;
    result.inner_checksum = inner.checksum;
    result.dynamic_segments = inner.dynamic_segments;
    result.tls_segments = inner.tls_segments;
    result.dynamic_entries = inner.dynamic_entries;
    result.rela_entries = inner.rela_entries;
    result.jmp_rela_entries = inner.jmp_rela_entries;
    result.import_libraries = inner.import_libraries;
    result.needed_modules = inner.needed_modules;
    result.has_dynamic = inner.has_dynamic;
    result.has_tls = inner.has_tls;
    result.has_relocations = inner.has_relocations;
    result.has_imports = inner.has_imports;
    result.entry = inner.entry;
    result.load_segments = inner.load_segments;
    result.mapped = inner.mapped;
    result.mapped_bytes = inner.mapped_bytes;
    result.checksum = inner.checksum;
    result.detail = L"SELF e ELF interno validados; segmentos PT_LOAD mapeados em buffer privado não executável. "
                    L"Nenhum byte do arquivo recebeu controle de fluxo.";
    return result;
}

} // namespace

ControlledLoadResult LoadControlled(std::filesystem::path const& path) {
    Reader reader(path);
    Require(reader.size >= 4, "Arquivo ELF/SELF vazio ou truncado.");
    std::array<std::uint8_t, 4> magic{};
    reader.Read(0, magic.data(), magic.size());

    if (magic == std::array<std::uint8_t, 4>{ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3}) {
        auto header = reader.Object<elf_header>(0);
        return LoadElf(reader, header, 0, [&](std::uint64_t offset, void* destination, std::size_t count) {
            reader.Read(offset, destination, count);
        });
    }
    auto selfMagic = self_header::signature;
    std::array<std::uint8_t, 4> selfBytes{
        static_cast<std::uint8_t>(selfMagic & 0xFF),
        static_cast<std::uint8_t>((selfMagic >> 8) & 0xFF),
        static_cast<std::uint8_t>((selfMagic >> 16) & 0xFF),
        static_cast<std::uint8_t>((selfMagic >> 24) & 0xFF)};
    if (magic == selfBytes) {
        auto header = reader.Object<self_header>(0);
        return LoadSelf(reader, header);
    }
    throw std::runtime_error("Arquivo não é ELF ou SELF PS4.");
}

GeneratedExecutionResult ExecuteGeneratedProbe(std::filesystem::path const& directory) {
    constexpr std::array<std::uint8_t, 6> code{0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    constexpr std::uint64_t payloadOffset = 0x1000;
    const auto path = directory / L"controlled-execution-probe.elf";

    elf_header header{};
    header.e_ident.magic[EI_MAG0] = ELFMAG0;
    header.e_ident.magic[EI_MAG1] = ELFMAG1;
    header.e_ident.magic[EI_MAG2] = ELFMAG2;
    header.e_ident.magic[EI_MAG3] = ELFMAG3;
    header.e_ident.ei_class = ELF_CLASS_64;
    header.e_ident.ei_data = ELF_DATA_2LSB;
    header.e_ident.ei_version = ELF_VERSION_CURRENT;
    header.e_ident.ei_osabi = ELF_OSABI_FREEBSD;
    header.e_ident.ei_abiversion = ELF_ABI_VERSION_AMDGPU_HSA_V2;
    header.e_type = ET_SCE_EXEC;
    header.e_machine = EM_X86_64;
    header.e_version = EV_CURRENT;
    header.e_entry = 0x400000;
    header.e_phoff = sizeof(elf_header);
    header.e_ehsize = sizeof(elf_header);
    header.e_phentsize = sizeof(elf_program_header);
    header.e_phnum = 1;

    elf_program_header program{};
    program.p_type = PT_LOAD;
    program.p_flags = PF_READ_EXEC;
    program.p_offset = payloadOffset;
    program.p_vaddr = header.e_entry;
    program.p_filesz = code.size();
    program.p_memsz = 0x1000;
    program.p_align = 0x1000;

    std::vector<std::uint8_t> image(static_cast<std::size_t>(payloadOffset + code.size()));
    std::memcpy(image.data(), &header, sizeof(header));
    std::memcpy(image.data() + sizeof(header), &program, sizeof(program));
    std::memcpy(image.data() + payloadOffset, code.data(), code.size());
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Não foi possível criar o ELF de execução controlada.");
        output.write(reinterpret_cast<char const*>(image.data()), static_cast<std::streamsize>(image.size()));
        if (!output) throw std::runtime_error("Falha ao gravar o ELF de execução controlada.");
    }

    auto loaded = LoadControlled(path);
    Require(loaded.mapped && loaded.load_segments == 1, "ELF de execução controlada não foi mapeado.");

    Allocation allocation;
    allocation.value = VirtualAllocFromApp(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!allocation.value) throw std::runtime_error("VirtualAllocFromApp falhou no probe controlado.");
    std::memcpy(allocation.value, code.data(), code.size());
    DWORD previous{};
    if (!VirtualProtectFromApp(allocation.value, 4096, PAGE_EXECUTE_READ, &previous))
        throw std::runtime_error("VirtualProtectFromApp falhou no probe controlado.");
    if (!FlushInstructionCache(GetCurrentProcess(), allocation.value, code.size()))
        throw std::runtime_error("FlushInstructionCache falhou no probe controlado.");
    auto function = reinterpret_cast<int (*)()>(allocation.value);
    const auto value = function();

    GeneratedExecutionResult result;
    result.returned_value = value;
    result.executable_address = reinterpret_cast<std::uint64_t>(allocation.value);
    result.elf_file_size = image.size();
    result.passed = value == 42;
    result.detail = result.passed
        ? L"ELF gerado pelo projeto foi validado, protegido como RX e retornou 42. "
          L"O eboot.bin selecionado não foi executado."
        : L"O ELF gerado pelo projeto retornou um valor inesperado. O eboot.bin selecionado não foi executado.";
    if (!result.passed) throw std::runtime_error("Probe de execução controlada retornou valor incorreto.");
    return result;
}

} // namespace Lab
