// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace Lab {

struct CoreProbeResult {
    bool passed{};
    std::uint32_t psf_header_size{};
    std::uint32_t psf_entry_size{};
    std::uint32_t decoded_magic{};
    std::uint32_t stored_magic{};
};

CoreProbeResult ProbeUpstreamCoreTypes();

} // namespace Lab
