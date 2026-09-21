// SPDX-License-Identifier: GPL-2.0-or-later
#include "LoaderProbe.h"

// Original shadPS4 loader declarations. No loader implementation or host
// filesystem backend is linked by this diagnostic target.
#include "core/loader/elf.h"

#include <windows.h>
#include <fileapifromapp.h>
#include <cstring>
#include <type_traits>
#include <vector>
#include <winrt/base.h>

namespace Lab {

LoaderProbeResult ProbeUpstreamLoaderStructures() {
    static_assert(sizeof(self_header) == 0x20);
    static_assert(sizeof(self_segment_header) == 0x20);
    // shadPS4's original declaration has six EI_PAD bytes, so this exact
    // source layout is 15 bytes rather than the conventional 16-byte e_ident.
    static_assert(sizeof(elf_ident) == 0x0F);
    static_assert(sizeof(elf_header) == 0x40);
    static_assert(sizeof(elf_program_header) == 0x38);
    static_assert(sizeof(elf_section_header) == 0x40);
    static_assert(std::is_trivially_copyable_v<self_header>);
    static_assert(std::is_trivially_copyable_v<elf_header>);

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

    elf_header elf{};
    elf.e_ident.magic[EI_MAG0] = ELFMAG0;
    elf.e_ident.magic[EI_MAG1] = ELFMAG1;
    elf.e_ident.magic[EI_MAG2] = ELFMAG2;
    elf.e_ident.magic[EI_MAG3] = ELFMAG3;
    elf.e_ident.ei_class = ELF_CLASS_64;
    elf.e_ident.ei_data = ELF_DATA_2LSB;
    elf.e_ident.ei_version = ELF_VERSION_CURRENT;
    elf.e_type = ET_SCE_EXEC;
    elf.e_machine = EM_X86_64;
    elf.e_version = EV_CURRENT;
    elf.e_ehsize = sizeof(elf_header);
    elf.e_phentsize = sizeof(elf_program_header);
    elf.e_phnum = 1;

    self_segment_header segment{};
    segment.flags = (0xABCuLL << 20u) | 0x800u | 0xFu;

    LoaderProbeResult result;
    result.self_header_size = sizeof(self_header);
    result.self_segment_size = sizeof(self_segment_header);
    result.elf_header_size = sizeof(elf_header);
    result.elf_program_header_size = sizeof(elf_program_header);
    result.self_signature = self.magic;
    result.elf_signature = (static_cast<std::uint32_t>(elf.e_ident.magic[EI_MAG0]) << 24u) |
                            (static_cast<std::uint32_t>(elf.e_ident.magic[EI_MAG1]) << 16u) |
                            (static_cast<std::uint32_t>(elf.e_ident.magic[EI_MAG2]) << 8u) |
                            static_cast<std::uint32_t>(elf.e_ident.magic[EI_MAG3]);
    result.segment_id = segment.GetId();
    result.passed = self.magic == self_header::signature &&
                    elf.e_type == ET_SCE_EXEC && elf.e_machine == EM_X86_64 &&
                    elf.e_ident.ei_class == ELF_CLASS_64 &&
                    elf.e_ident.ei_data == ELF_DATA_2LSB && segment.IsBlocked() &&
                    segment.IsOrdered() && segment.IsEncrypted() && segment.IsSigned() &&
                    segment.IsCompressed() && result.segment_id == 0xABC;
    return result;
}

LoaderFileProbeResult ProbeSelfElfFileAdapter(const std::wstring& directory) {
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

    self_segment_header segment{};
    segment.flags = (0xABCuLL << 20u) | 0x800u | 0xFu;
    segment.file_offset = sizeof(self_header) + sizeof(self_segment_header) + sizeof(elf_header);
    segment.file_size = 0x1000;
    segment.memory_size = 0x2000;

    elf_header elf{};
    elf.e_ident.magic[EI_MAG0] = ELFMAG0;
    elf.e_ident.magic[EI_MAG1] = ELFMAG1;
    elf.e_ident.magic[EI_MAG2] = ELFMAG2;
    elf.e_ident.magic[EI_MAG3] = ELFMAG3;
    elf.e_ident.ei_class = ELF_CLASS_64;
    elf.e_ident.ei_data = ELF_DATA_2LSB;
    elf.e_ident.ei_version = ELF_VERSION_CURRENT;
    elf.e_type = ET_SCE_EXEC;
    elf.e_machine = EM_X86_64;
    elf.e_version = EV_CURRENT;
    elf.e_entry = 0x400000;
    elf.e_ehsize = sizeof(elf_header);
    elf.e_phentsize = sizeof(elf_program_header);
    elf.e_phnum = 1;

    const auto total_size = sizeof(self_header) + sizeof(self_segment_header) + sizeof(elf_header);
    std::vector<std::uint8_t> encoded(total_size);
    std::memcpy(encoded.data(), &self, sizeof(self));
    std::memcpy(encoded.data() + sizeof(self), &segment, sizeof(segment));
    std::memcpy(encoded.data() + sizeof(self) + sizeof(segment), &elf, sizeof(elf));
    const auto path = directory + L"\\self-elf-core-probe.bin";

    CREATEFILE2_EXTENDED_PARAMETERS params{sizeof(params)};
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    winrt::handle writer{CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                     CREATE_ALWAYS, &params)};
    if (!writer) winrt::throw_last_error();
    DWORD written{};
    winrt::check_bool(WriteFile(writer.get(), encoded.data(), static_cast<DWORD>(encoded.size()),
                                &written, nullptr));
    winrt::check_bool(FlushFileBuffers(writer.get()));
    writer.close();

    winrt::handle reader{CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     OPEN_EXISTING, &params)};
    if (!reader) winrt::throw_last_error();
    std::vector<std::uint8_t> bytes(encoded.size());
    DWORD read{};
    winrt::check_bool(ReadFile(reader.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                               &read, nullptr));

    self_header read_self{};
    self_segment_header read_segment{};
    elf_header read_elf{};
    if (read == bytes.size()) {
        std::memcpy(&read_self, bytes.data(), sizeof(read_self));
        std::memcpy(&read_segment, bytes.data() + sizeof(read_self), sizeof(read_segment));
        std::memcpy(&read_elf, bytes.data() + sizeof(read_self) + sizeof(read_segment),
                    sizeof(read_elf));
    }

    LoaderFileProbeResult result;
    result.file_size = static_cast<std::uint32_t>(bytes.size());
    result.segment_id = read_segment.GetId();
    result.elf_entry = read_elf.e_entry;
    result.passed = written == encoded.size() && read == encoded.size() &&
                    read_self.magic == self_header::signature &&
                    read_self.segment_count == 1 && read_segment.IsBlocked() &&
                    result.segment_id == 0xABC && read_elf.e_ident.magic[EI_MAG0] == ELFMAG0 &&
                    read_elf.e_ident.magic[EI_MAG3] == ELFMAG3 &&
                    read_elf.e_type == ET_SCE_EXEC && read_elf.e_machine == EM_X86_64 &&
                    read_elf.e_entry == 0x400000;
    return result;
}

} // namespace Lab
