// SPDX-License-Identifier: GPL-2.0-or-later
#include "ExecProbe.h"

#include "IFileProbe.h"
#include "core/loader/elf.h"

#include <array>
#include <cstring>
#include <windows.h>
#include <memoryapi.h>
#include <fileapifromapp.h>
#include <vector>
#include <winrt/base.h>

namespace Lab {
namespace {

constexpr std::array<std::uint8_t, 6> Code = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};

struct Allocation {
    void* address{};
    ~Allocation() {
        if (address) VirtualFree(address, 0, MEM_RELEASE);
    }
};

std::vector<std::uint8_t> MakeExecutableInput() {
    constexpr std::size_t header_size = sizeof(self_header) + sizeof(self_segment_header) +
                                         sizeof(elf_header) + sizeof(elf_program_header);
    self_header self{};
    self.magic = self_header::signature;
    self.version = 0;
    self.mode = 1;
    self.endian = 1;
    self.attributes = 0x12;
    self.category = 1;
    self.program_type = 1;
    self.segment_count = 1;
    self.unknown1A = 0x22;
    self.file_size = static_cast<u32>(header_size + Code.size());

    self_segment_header segment{};
    segment.flags = 0x800u | 0xFu;
    segment.file_offset = header_size;
    segment.file_size = Code.size();
    segment.memory_size = Code.size();

    elf_header elf{};
    elf.e_ident.magic[EI_MAG0] = ELFMAG0;
    elf.e_ident.magic[EI_MAG1] = ELFMAG1;
    elf.e_ident.magic[EI_MAG2] = ELFMAG2;
    elf.e_ident.magic[EI_MAG3] = ELFMAG3;
    elf.e_ident.ei_class = ELF_CLASS_64;
    elf.e_ident.ei_data = ELF_DATA_2LSB;
    elf.e_ident.ei_version = ELF_VERSION_CURRENT;
    elf.e_ident.ei_osabi = ELF_OSABI_FREEBSD;
    elf.e_ident.ei_abiversion = ELF_ABI_VERSION_AMDGPU_HSA_V2;
    elf.e_type = ET_SCE_EXEC;
    elf.e_machine = EM_X86_64;
    elf.e_version = EV_CURRENT;
    elf.e_entry = 0x400000;
    elf.e_phoff = sizeof(elf_header);
    elf.e_ehsize = 256;
    elf.e_phentsize = sizeof(elf_program_header);
    elf.e_phnum = 1;

    elf_program_header program{};
    program.p_type = PT_LOAD;
    program.p_flags = PF_READ_EXEC;
    program.p_offset = 0x1000;
    program.p_vaddr = 0x400000;
    program.p_filesz = Code.size();
    program.p_memsz = Code.size();
    program.p_align = 0x1000;

    std::vector<std::uint8_t> bytes(header_size + Code.size());
    std::memcpy(bytes.data(), &self, sizeof(self));
    std::memcpy(bytes.data() + sizeof(self), &segment, sizeof(segment));
    std::memcpy(bytes.data() + sizeof(self) + sizeof(segment), &elf, sizeof(elf));
    std::memcpy(bytes.data() + sizeof(self) + sizeof(segment) + sizeof(elf), &program,
                sizeof(program));
    std::memcpy(bytes.data() + header_size, Code.data(), Code.size());
    return bytes;
}

} // namespace

ExecProbeResult ProbeOriginalElfExecuteSegment(const std::wstring& directory) {
    const auto bytes = MakeExecutableInput();
    const auto path = directory + L"\\elf-execute-segment-probe.bin";
    CREATEFILE2_EXTENDED_PARAMETERS params{sizeof(params)};
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    winrt::handle writer{CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                     CREATE_ALWAYS, &params)};
    if (!writer) winrt::throw_last_error();
    DWORD written{};
    winrt::check_bool(WriteFile(writer.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                                &written, nullptr));
    winrt::check_bool(FlushFileBuffers(writer.get()));
    writer.close();

    Core::Loader::Elf elf;
    auto backend = MakeUwpFile(path);
    if (!backend || !backend->IsOpen())
        throw winrt::hresult_error(E_FAIL, L"Backend IFile UWP não abriu o código ELF.");
    elf.Open(std::move(backend));

    Allocation allocation{VirtualAllocFromApp(nullptr, 4096, MEM_RESERVE | MEM_COMMIT,
                                               PAGE_READWRITE)};
    if (!allocation.address) winrt::throw_last_error();
    elf.LoadSegment(reinterpret_cast<u64>(allocation.address), 0x1000, Code.size());
    DWORD old_protection{};
    winrt::check_bool(VirtualProtectFromApp(allocation.address, 4096, PAGE_EXECUTE_READ,
                                            &old_protection));
    winrt::check_bool(FlushInstructionCache(GetCurrentProcess(), allocation.address, Code.size()));
    const auto function = reinterpret_cast<int (*)()>(allocation.address);
    const auto returned = function();

    ExecProbeResult result;
    result.file_size = static_cast<std::uint32_t>(bytes.size());
    result.payload_size = Code.size();
    result.returned_value = returned;
    result.executable_address = reinterpret_cast<std::uint64_t>(allocation.address);
    result.passed = written == bytes.size() && returned == 42;
    return result;
}

} // namespace Lab
