// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ControlledLoader.h"

namespace Lab {

// Owns a private UWP allocation for a validated image. Code and read-only
// ranges remain non-executable; PF_W ranges use isolated writable copies so a
// shared ELF page never makes code writable.
class GuestMemory {
public:
    GuestMemory() = default;
    ~GuestMemory();

    GuestMemory(GuestMemory const&) = delete;
    GuestMemory& operator=(GuestMemory const&) = delete;

    bool MapValidated(std::vector<std::uint8_t> const& image, std::uint64_t guestBase,
                      std::vector<GuestSegmentInfo> const& segments);
    void* Translate(std::uint64_t guestAddress, std::size_t bytes) const noexcept;
    void* TranslateWritable(std::uint64_t guestAddress, std::size_t bytes) const noexcept;
    // Applies only non-executable page protection to an isolated PF_W range.
    // The UWP image mapping and all code/read-only ranges remain unchanged.
    bool ProtectNoExecute(std::uint64_t guestAddress, std::size_t bytes,
                          std::uint64_t prot) noexcept;
    bool mapped() const noexcept { return base_ != nullptr; }
    std::size_t size() const noexcept { return size_; }
    std::size_t writableBytes() const noexcept;
    std::uint64_t guestBase() const noexcept { return guestBase_; }
    std::uint64_t hostAddress() const noexcept {
        return reinterpret_cast<std::uint64_t>(base_);
    }

private:
    void* base_{};
    std::size_t size_{};
    std::uint64_t guestBase_{};
    struct WritableRange {
        std::uint64_t address{};
        std::uint64_t size{};
        void* host{};
    };
    std::vector<WritableRange> writable_;
};

} // namespace Lab
