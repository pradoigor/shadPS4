// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace Lab {

struct LoaderProbeResult {
    bool passed{};
    std::uint32_t self_header_size{};
    std::uint32_t self_segment_size{};
    std::uint32_t elf_header_size{};
    std::uint32_t elf_program_header_size{};
    std::uint32_t self_signature{};
    std::uint32_t elf_signature{};
    std::uint32_t segment_id{};
};

LoaderProbeResult ProbeUpstreamLoaderStructures();

} // namespace Lab
