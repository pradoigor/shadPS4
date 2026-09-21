// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestMemory.h"

#include <windows.h>
#include <memoryapi.h>

#include <cstring>

namespace Lab {

GuestMemory::~GuestMemory() {
    if (base_) VirtualFree(base_, 0, MEM_RELEASE);
}

bool GuestMemory::MapReadOnly(std::vector<std::uint8_t> const& image,
                              std::uint64_t guestBase) {
    if (base_ || image.empty()) return false;
    auto* allocation = VirtualAllocFromApp(nullptr, image.size(),
                                            MEM_RESERVE | MEM_COMMIT,
                                            PAGE_READWRITE);
    if (!allocation) return false;
    std::memcpy(allocation, image.data(), image.size());
    DWORD previous{};
    if (!VirtualProtectFromApp(allocation, image.size(), PAGE_READONLY, &previous)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return false;
    }
    base_ = allocation;
    size_ = image.size();
    guestBase_ = guestBase;
    return true;
}

void* GuestMemory::Translate(std::uint64_t guestAddress, std::size_t bytes) const noexcept {
    if (!base_ || guestAddress < guestBase_) return nullptr;
    const auto offset = guestAddress - guestBase_;
    if (offset > size_ || bytes > size_ - static_cast<std::size_t>(offset)) return nullptr;
    return static_cast<std::uint8_t*>(base_) + offset;
}

} // namespace Lab
