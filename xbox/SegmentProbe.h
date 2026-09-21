// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace Lab {

struct SegmentProbeResult {
    bool passed{};
    std::uint32_t file_size{};
    std::uint32_t payload_size{};
    std::uint32_t checksum{};
    std::uint64_t virtual_address{};
};

SegmentProbeResult ProbeOriginalElfLoadSegment(const std::wstring& directory);

} // namespace Lab
