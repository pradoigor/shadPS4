// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace Lab {

struct GuestCallFrame {
    std::uint64_t gpr[6]{};
    std::uint64_t guest_stack{};
    alignas(16) std::uint8_t xmm[8][16]{};
};

using SysvDispatch = std::uint64_t (*)(void* context, std::uint64_t slot,
                                       GuestCallFrame const* frame, void* guestStack);

// Allocates small executable thunks that convert the PS4 SysV register frame
// into the Windows x64 call used by the UWP dispatcher. The thunks are not
// guest entry points until a real HLE dispatcher is supplied.
class SysvThunkArena {
public:
    SysvThunkArena() = default;
    ~SysvThunkArena();

    SysvThunkArena(SysvThunkArena const&) = delete;
    SysvThunkArena& operator=(SysvThunkArena const&) = delete;

    void* Create(void* context, std::uint64_t slot, SysvDispatch dispatch);
    bool empty() const noexcept { return pages_.empty(); }

private:
    std::vector<void*> pages_;
};

} // namespace Lab
