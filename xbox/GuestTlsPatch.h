// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "ControlledLoader.h"

namespace Lab {
std::uint32_t PatchGuestFsTcbReads(
    std::vector<std::uint8_t>& image, std::uint64_t virtualBase,
    std::vector<GuestSegmentInfo> const& segments, std::uint32_t tlsSlot);
}
