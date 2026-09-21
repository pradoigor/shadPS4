// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace Lab {

struct ControlledLoadResult {
    bool recognized{};
    bool self{};
    bool validated{};
    bool mapped{};
    bool protected_segments{};
    std::uint64_t file_size{};
    std::uint64_t segment_count{};
    std::uint64_t load_segments{};
    std::uint64_t mapped_bytes{};
    std::uint64_t entry{};
    std::uint64_t min_virtual_address{};
    std::uint64_t max_virtual_address{};
    std::uint64_t checksum{};
    std::wstring detail;
};

// Validates an ELF/SELF and maps only validated PT_LOAD bytes into a private,
// non-executable buffer. It never transfers control to the input file.
ControlledLoadResult LoadControlled(std::filesystem::path const& path);

} // namespace Lab
