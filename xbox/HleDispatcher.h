// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "GuestMemory.h"
#include "SysvThunk.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Lab {

struct HleResolution {
    std::string symbol;
    std::string nid;
    bool implemented{};
    void* address{};
};

struct HleBindingSummary {
    std::size_t requested{};
    std::size_t executable_addresses{};
    std::size_t implemented_handlers{};
    std::size_t unimplemented_handlers{};
};

class HleDispatcher;
using HleHandler = std::uint64_t (*)(HleDispatcher&, GuestCallFrame const&) noexcept;

class HleDispatcher {
public:
    HleDispatcher() = default;
    ~HleDispatcher() = default;

    HleDispatcher(HleDispatcher const&) = delete;
    HleDispatcher& operator=(HleDispatcher const&) = delete;

    // Returns a thunk for an import. Unimplemented imports receive a safe
    // ENOSYS-style return value and are still reported as unavailable.
    HleResolution Resolve(std::string_view encodedSymbol);
    HleBindingSummary Bind(std::vector<std::string> const& encodedSymbols);
    void* AddressFor(std::string_view encodedSymbol) const noexcept;
    void AttachGuestMemory(GuestMemory* memory) noexcept { memory_ = memory; }

    std::size_t implementedCount() const noexcept;
    std::size_t unresolvedCount() const noexcept;

private:
    struct Entry {
        std::string encoded;
        std::string nid;
        bool implemented{};
        HleHandler handler{};
        void* address{};
    };

    static std::uint64_t Dispatch(void* context, std::uint64_t slot,
                                  GuestCallFrame const* frame, void* guestStack) noexcept;
    static std::uint64_t Unimplemented(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelUsleep(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelGetUpdVersion(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelGetLowerLimitUpdVersion(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelGetPid(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelGetEuid(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelSchedYield(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelThreadSelf(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t EglGetError(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t EglQueryApi(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t GlGetError(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t NetCtlInit(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t NetCtlTerm(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t HideSplashScreen(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelDebugOutText(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelMprotect(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryMemcpy(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryMemmove(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryMemset(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryMemcmp(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryStrlen(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryMmap(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelMmap(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryMunmap(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelMunmap(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t ClockGetTime(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t UserServiceInitialize(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t UserServiceGetInitialUser(HleDispatcher&,
                                                   GuestCallFrame const&) noexcept;
    static std::uint64_t UserServiceGetLoginUsers(HleDispatcher&,
                                                  GuestCallFrame const&) noexcept;
    static std::uint64_t UserServiceGetUserName(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t SystemServiceParamGetInt(HleDispatcher&,
                                                  GuestCallFrame const&) noexcept;

    SysvThunkArena thunks_;
    std::vector<Entry> entries_;
    GuestMemory* memory_{};
};

} // namespace Lab
