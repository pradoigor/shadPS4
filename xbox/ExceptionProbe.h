// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace Lab {

struct ExceptionProbeResult {
    bool passed{};
    std::uint32_t code{};
    std::uint32_t handler_registered{};
    std::uint32_t handler_invocations{};
    std::uint32_t handler_completed{};
};

ExceptionProbeResult ProbeOriginalExceptionDelivery(const std::wstring& directory);

} // namespace Lab
