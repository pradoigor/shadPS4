// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace Lab {

struct RelocProbeResult {
    bool passed{};
    std::uint32_t record_count{};
    std::uint32_t relative_value{};
    std::uint32_t local_symbol_value{};
    std::uint32_t tls_module_value{};
    std::uint32_t encoded_size{};
};

RelocProbeResult ProbeOriginalRelocationModel(const std::wstring& directory);

} // namespace Lab
