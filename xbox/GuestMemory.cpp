// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestMemory.h"

#include <windows.h>
#include <memoryapi.h>

#include <cstring>

namespace Lab {

GuestMemory::~GuestMemory() {
    for (auto const& range : writable_)
        if (range.host) VirtualFree(range.host, 0, MEM_RELEASE);
    if (base_) VirtualFree(base_, 0, MEM_RELEASE);
}

bool GuestMemory::MapValidated(std::vector<std::uint8_t> const& image,
                               std::uint64_t guestBase,
                               std::vector<GuestSegmentInfo> const& segments) {
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
    for (auto const& segment : segments) {
        if ((segment.flags & 0x2u) == 0 || segment.size == 0 ||
            segment.address < guestBase)
            continue;
        const auto offset = segment.address - guestBase;
        if (offset > image.size() || segment.size > image.size() - offset)
            continue;
        auto* writable = VirtualAllocFromApp(nullptr, segment.size,
                                              MEM_RESERVE | MEM_COMMIT,
                                              PAGE_READWRITE);
        if (!writable) {
            for (auto const& range : writable_)
                if (range.host) VirtualFree(range.host, 0, MEM_RELEASE);
            writable_.clear();
            VirtualFree(allocation, 0, MEM_RELEASE);
            return false;
        }
        std::memcpy(writable, static_cast<std::uint8_t*>(allocation) + offset,
                    static_cast<std::size_t>(segment.size));
        writable_.push_back(WritableRange{segment.address, segment.size, writable});
    }
    base_ = allocation;
    size_ = image.size();
    guestBase_ = guestBase;
    return true;
}

void* GuestMemory::Translate(std::uint64_t guestAddress, std::size_t bytes) const noexcept {
    if (!base_ || guestAddress < guestBase_) return nullptr;
    for (auto const& range : writable_) {
        if (guestAddress >= range.address &&
            guestAddress - range.address <= range.size &&
            bytes <= range.size - (guestAddress - range.address))
            return static_cast<std::uint8_t*>(range.host) +
                   (guestAddress - range.address);
    }
    const auto offset = guestAddress - guestBase_;
    if (offset > static_cast<std::uint64_t>(size_) ||
        bytes > size_ - static_cast<std::size_t>(offset)) return nullptr;
    return static_cast<std::uint8_t*>(base_) + offset;
}

std::size_t GuestMemory::writableBytes() const noexcept {
    std::size_t total = 0;
    for (auto const& range : writable_) total += static_cast<std::size_t>(range.size);
    return total;
}

void* GuestMemory::TranslateWritable(std::uint64_t guestAddress,
                                      std::size_t bytes) const noexcept {
    for (auto const& range : writable_) {
        if (guestAddress >= range.address &&
            guestAddress - range.address <= range.size &&
            bytes <= range.size - (guestAddress - range.address))
            return static_cast<std::uint8_t*>(range.host) +
                   (guestAddress - range.address);
    }
    return nullptr;
}

bool GuestMemory::ProtectNoExecute(std::uint64_t guestAddress, std::size_t bytes,
                                   std::uint64_t prot) noexcept {
    if (!base_ || guestAddress == 0 || bytes == 0 || (prot & ~0x7ull) != 0 ||
        (prot & 0x4ull) != 0)
        return false;
    for (auto const& range : writable_) {
        if (guestAddress < range.address || guestAddress - range.address > range.size)
            continue;
        const auto offset = guestAddress - range.address;
        if (bytes > range.size - offset) return false;
        auto* host = static_cast<std::uint8_t*>(range.host) + offset;
        const DWORD protection = (prot & 0x2ull) != 0
            ? PAGE_READWRITE
            : ((prot & 0x1ull) != 0 ? PAGE_READONLY : PAGE_NOACCESS);
        DWORD previous{};
        return VirtualProtectFromApp(host, bytes, protection, &previous) != FALSE;
    }
    return false;
}

} // namespace Lab
