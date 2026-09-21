// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace Lab {

struct TlsProbeResult {
    bool passed{};
    std::uint32_t slot{};
    std::uint32_t tcb_size{};
    std::uint32_t dtv_entries{};
    std::uint64_t roundtrip_address{};
    std::uint64_t canary{};
};

TlsProbeResult ProbeOriginalTlsModel(const std::wstring& directory);

} // namespace Lab
