// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ControlledLoader.h"

namespace Lab {

// Owns a coherent UWP allocation for a validated position-independent image.
// Runtime guest addresses include the chosen load bias and are identical to
// host pointers, so native x86-64 references and HLE pointer translation agree.
// Code remains non-executable until the runtime gate authorizes an RW-to-RX
// change.
class GuestMemory {
public:
  GuestMemory() = default;
  ~GuestMemory();

  GuestMemory(GuestMemory const &) = delete;
  GuestMemory &operator=(GuestMemory const &) = delete;

  bool MapValidated(std::vector<std::uint8_t> const &image,
                    std::uint64_t guestBase,
                    std::vector<GuestSegmentInfo> const &segments,
                    std::vector<PendingRelativeRelocation> const &relocations);
  void *Translate(std::uint64_t guestAddress, std::size_t bytes) const noexcept;
  void *TranslateWritable(std::uint64_t guestAddress,
                          std::size_t bytes) const noexcept;
  bool IsExecutable(std::uint64_t guestAddress,
                    std::size_t bytes = 1) const noexcept;
  bool MapAnonymous(std::size_t bytes, std::uint64_t prot,
                    std::uint64_t requestedAddress, bool fixed,
                    std::uint64_t &guestAddress) noexcept;
  bool ReserveVirtualRange(std::uint64_t bytes, std::uint64_t requestedAddress,
                           bool fixed, std::uint64_t alignment,
                           std::uint64_t &guestAddress) noexcept;
  bool MapLazySystem(std::uint64_t address, std::uint64_t bytes,
                     std::uint64_t prot) noexcept;
  bool CommitLazyPage(std::uint64_t address) noexcept;
  bool Unmap(std::uint64_t guestAddress, std::size_t bytes) noexcept;
  // Applies only non-executable page protection to an isolated PF_W range.
  // The UWP image mapping and all code/read-only ranges remain unchanged.
  bool ProtectNoExecute(std::uint64_t guestAddress, std::size_t bytes,
                        std::uint64_t prot) noexcept;
  bool mapped() const noexcept { return base_ != nullptr; }
  std::size_t size() const noexcept { return size_; }
  std::size_t writableBytes() const noexcept;
  std::size_t executableBytes() const noexcept { return executableBytes_; }
  std::uint64_t guestBase() const noexcept { return guestBase_; }
  std::uint64_t loadBias() const noexcept { return loadBias_; }
  std::uint64_t RuntimeAddress(std::uint64_t virtualAddress) const noexcept {
    return loadBias_ + virtualAddress;
  }
  std::uint64_t hostAddress() const noexcept {
    return reinterpret_cast<std::uint64_t>(base_);
  }

private:
  void *base_{};
  std::size_t size_{};
  std::uint64_t guestBase_{};
  std::uint64_t loadBias_{};
  std::size_t executableBytes_{};
  struct WritableRange {
    std::uint64_t address{};
    std::uint64_t size{};
    void *host{};
    std::uint32_t protection{};
  };
  std::vector<WritableRange> writable_;
  struct ExecutableRange {
    std::uint64_t address{};
    std::uint64_t size{};
  };
  std::vector<ExecutableRange> executable_;
  struct AnonymousRange {
    std::uint64_t address{};
    std::uint64_t size{};
    void *host{};
    std::uint32_t protection{};
    bool insideReservation{};
  };
  std::vector<AnonymousRange> anonymous_;
  struct ReservedRange { std::uint64_t address{}, size{}; void* host{}; };
  std::vector<ReservedRange> reserved_;
  struct LazyRange { std::uint64_t address{}, size{}; std::uint32_t protection{}; };
  std::vector<LazyRange> lazy_;
  std::size_t lazyCommittedBytes_{};
};

} // namespace Lab
