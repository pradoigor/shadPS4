// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace Lab {

struct ElfOpenProbeResult {
    bool passed{};
    std::uint32_t file_size{};
    std::uint32_t program_headers{};
    std::uint32_t segments{};
    std::uint64_t elf_entry{};
};

ElfOpenProbeResult ProbeOriginalElfOpen(const std::wstring& directory);

} // namespace Lab
