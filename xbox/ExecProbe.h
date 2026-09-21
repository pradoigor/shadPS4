// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace Lab {

struct ExecProbeResult {
    bool passed{};
    std::uint32_t file_size{};
    std::uint32_t payload_size{};
    std::int32_t returned_value{};
    std::uint64_t executable_address{};
};

ExecProbeResult ProbeOriginalElfExecuteSegment(const std::wstring& directory);

} // namespace Lab
