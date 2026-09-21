// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "SysvThunk.h"

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

class HleDispatcher {
public:
    HleDispatcher() = default;
    ~HleDispatcher() = default;

    HleDispatcher(HleDispatcher const&) = delete;
    HleDispatcher& operator=(HleDispatcher const&) = delete;

    // Returns a thunk for an import. Unimplemented imports receive a safe
    // ENOSYS-style return value and are still reported as unavailable.
    HleResolution Resolve(std::string_view encodedSymbol);

    std::size_t implementedCount() const noexcept;
    std::size_t unresolvedCount() const noexcept;

private:
    struct Entry {
        std::string encoded;
        std::string nid;
        bool implemented{};
        std::uint64_t (*handler)(GuestCallFrame const&){};
    };

    static std::uint64_t Dispatch(void* context, std::uint64_t slot,
                                  GuestCallFrame const* frame, void* guestStack) noexcept;
    static std::uint64_t Unimplemented(GuestCallFrame const&) noexcept;
    static std::uint64_t KernelUsleep(GuestCallFrame const&) noexcept;
    static std::uint64_t KernelGetUpdVersion(GuestCallFrame const&) noexcept;

    SysvThunkArena thunks_;
    std::vector<Entry> entries_;
};

} // namespace Lab
