// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Lab {

// Owns a private UWP allocation for a validated image. The allocation is
// copied while writable and then changed to read-only; it is never executable.
class GuestMemory {
public:
    GuestMemory() = default;
    ~GuestMemory();

    GuestMemory(GuestMemory const&) = delete;
    GuestMemory& operator=(GuestMemory const&) = delete;

    bool MapReadOnly(std::vector<std::uint8_t> const& image, std::uint64_t guestBase);
    void* Translate(std::uint64_t guestAddress, std::size_t bytes) const noexcept;
    bool mapped() const noexcept { return base_ != nullptr; }
    std::size_t size() const noexcept { return size_; }
    std::uint64_t guestBase() const noexcept { return guestBase_; }
    std::uint64_t hostAddress() const noexcept {
        return reinterpret_cast<std::uint64_t>(base_);
    }

private:
    void* base_{};
    std::size_t size_{};
    std::uint64_t guestBase_{};
};

} // namespace Lab
