// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestMemory.h"

#include <windows.h>
#include <memoryapi.h>

#include <cstring>

namespace Lab {

GuestMemory::~GuestMemory() {
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
        constexpr std::uint64_t PageSize = 0x1000;
        if (offset > image.size() || segment.size > image.size() - offset)
            continue;
        const auto end = offset + segment.size;
        const auto pageStart = offset & ~(PageSize - 1);
        const auto pageEnd = (end + PageSize - 1) & ~(PageSize - 1);
        bool overlapsReadOnly = false;
        for (auto const& other : segments) {
            if ((other.flags & 0x2u) != 0 || other.size == 0 ||
                other.address < guestBase)
                continue;
            const auto otherStart = other.address - guestBase;
            if (otherStart > image.size() || other.size > image.size() - otherStart)
                continue;
            const auto otherEnd = otherStart + other.size;
            const auto otherPageStart = otherStart & ~(PageSize - 1);
            const auto otherPageEnd = (otherEnd + PageSize - 1) & ~(PageSize - 1);
            if (pageStart < otherPageEnd && otherPageStart < pageEnd) {
                overlapsReadOnly = true;
                break;
            }
        }
        if (overlapsReadOnly || pageEnd > image.size()) continue;
        auto* writable = static_cast<std::uint8_t*>(allocation) + pageStart;
        if (!VirtualProtectFromApp(writable, pageEnd - pageStart, PAGE_READWRITE, &previous)) {
            VirtualFree(allocation, 0, MEM_RELEASE);
            return false;
        }
        writable_.push_back(WritableRange{segment.address, segment.size});
    }
    base_ = allocation;
    size_ = image.size();
    guestBase_ = guestBase;
    return true;
}

void* GuestMemory::Translate(std::uint64_t guestAddress, std::size_t bytes) const noexcept {
    if (!base_ || guestAddress < guestBase_) return nullptr;
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
            return Translate(guestAddress, bytes);
    }
    return nullptr;
}

} // namespace Lab
