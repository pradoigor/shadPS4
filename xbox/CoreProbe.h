// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace Lab {

struct CoreProbeResult {
    bool passed{};
    std::uint32_t psf_header_size{};
    std::uint32_t psf_entry_size{};
    std::uint32_t decoded_magic{};
    std::uint32_t stored_magic{};
    std::uint32_t encoded_size{};
    std::int32_t decoded_integer{};
};

CoreProbeResult ProbeUpstreamCoreTypes();

struct CoreFileProbeResult {
    bool passed{};
    std::uint32_t file_size{};
    std::int32_t decoded_integer{};
};

CoreFileProbeResult ProbePsfFileAdapter(const std::wstring& directory);

} // namespace Lab
