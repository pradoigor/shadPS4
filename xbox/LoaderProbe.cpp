// SPDX-License-Identifier: GPL-2.0-or-later
#include "LoaderProbe.h"

// Original shadPS4 loader declarations. No loader implementation or host
// filesystem backend is linked by this diagnostic target.
#include "core/loader/elf.h"

#include <type_traits>

namespace Lab {

LoaderProbeResult ProbeUpstreamLoaderStructures() {
    static_assert(sizeof(self_header) == 0x20);
    static_assert(sizeof(self_segment_header) == 0x20);
    static_assert(sizeof(elf_ident) == 0x10);
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

} // namespace Lab
