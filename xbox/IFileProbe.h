// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace Lab {

struct IFileProbeResult {
    bool passed{};
    std::uint32_t file_size{};
    std::uint32_t bytes_read{};
    std::uint32_t segment_id{};
    std::uint64_t elf_entry{};
};

IFileProbeResult ProbeUwpIFileAdapter(const std::wstring& directory);

} // namespace Lab
