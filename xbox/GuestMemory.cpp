// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestMemory.h"
#include "core/platform_memory.h"

#include <windows.h>

#include <cstring>

namespace Lab {
namespace {

constexpr std::uint64_t GuestPage = 0x4000;

std::uint64_t AlignGuest(std::uint64_t value) noexcept {
  const auto remainder = value % GuestPage;
  if (remainder == 0)
    return value;
  const auto padding = GuestPage - remainder;
  return value > UINT64_MAX - padding ? 0 : value + padding;
}

bool Overlaps(std::uint64_t address, std::uint64_t size,
              std::uint64_t otherAddress, std::uint64_t otherSize) noexcept {
  if (size == 0 || otherSize == 0 || address > UINT64_MAX - size ||
      otherAddress > UINT64_MAX - otherSize)
    return true;
  return address < otherAddress + otherSize && otherAddress < address + size;
}

DWORD ProtectionFor(std::uint64_t prot) noexcept {
  if ((prot & 0x2ull) != 0)
    return PAGE_READWRITE;
  if ((prot & 0x1ull) != 0)
    return PAGE_READONLY;
  return PAGE_NOACCESS;
}

void *CurrentStackPointer(std::uint64_t address, std::size_t bytes,
                          bool writable) noexcept {
  if (!address || !bytes || address > UINT64_MAX - bytes)
    return nullptr;
  std::uint8_t stackMarker{};
  MEMORY_BASIC_INFORMATION current{}, candidate{};
  if (!VirtualQueryFromApp(&stackMarker, &current, sizeof(current)) ||
      !VirtualQueryFromApp(reinterpret_cast<void *>(address), &candidate,
                           sizeof(candidate)) ||
      current.AllocationBase != candidate.AllocationBase ||
      candidate.State != MEM_COMMIT || (candidate.Protect & PAGE_GUARD) != 0 ||
      candidate.Protect == PAGE_NOACCESS)
    return nullptr;
  const auto region = reinterpret_cast<std::uint64_t>(candidate.BaseAddress);
  if (address < region || address - region > candidate.RegionSize ||
      bytes > candidate.RegionSize - (address - region))
    return nullptr;
  if (writable && candidate.Protect != PAGE_READWRITE &&
      candidate.Protect != PAGE_WRITECOPY &&
      candidate.Protect != PAGE_EXECUTE_READWRITE &&
      candidate.Protect != PAGE_EXECUTE_WRITECOPY)
    return nullptr;
  return reinterpret_cast<void *>(address);
}

} // namespace

GuestMemory::~GuestMemory() {
  for (auto const &range : anonymous_)
    if (range.host)
      VirtualFree(range.host, 0, MEM_RELEASE);
  if (base_)
    Core::PlatformMemory::Free(GetCurrentProcess(), base_, 0, MEM_RELEASE);
}

bool GuestMemory::MapValidated(
    std::vector<std::uint8_t> const &image, std::uint64_t guestBase,
    std::vector<GuestSegmentInfo> const &segments,
    std::vector<PendingRelativeRelocation> const &relocations) {
  if (base_ || image.empty() || guestBase > UINT64_MAX - image.size())
    return false;
  auto *allocation =
      Core::PlatformMemory::Allocate(GetCurrentProcess(), nullptr, image.size(),
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!allocation)
    return false;
  const auto hostBase = reinterpret_cast<std::uint64_t>(allocation);
  if (hostBase < guestBase) {
    Core::PlatformMemory::Free(GetCurrentProcess(), allocation, 0, MEM_RELEASE);
    return false;
  }
  const auto loadBias = hostBase - guestBase;
  std::memcpy(allocation, image.data(), image.size());
  for (auto const &relocation : relocations) {
    if (relocation.target < guestBase || image.size() < sizeof(std::uint64_t) ||
        relocation.target - guestBase > image.size() - sizeof(std::uint64_t)) {
      Core::PlatformMemory::Free(GetCurrentProcess(), allocation, 0,
                                 MEM_RELEASE);
      return false;
    }
    const auto offset = static_cast<std::size_t>(relocation.target - guestBase);
    const auto value = loadBias + static_cast<std::uint64_t>(relocation.addend);
    std::memcpy(static_cast<std::uint8_t *>(allocation) + offset, &value,
                sizeof(value));
  }
  DWORD previous{};
  if (!Core::PlatformMemory::Protect(GetCurrentProcess(), allocation,
                                     image.size(), PAGE_READONLY, &previous)) {
    Core::PlatformMemory::Free(GetCurrentProcess(), allocation, 0, MEM_RELEASE);
    return false;
  }
  for (auto const &segment : segments) {
    if (segment.size == 0 || segment.address < guestBase)
      continue;
    const auto offset = segment.address - guestBase;
    if (offset > image.size() || segment.size > image.size() - offset)
      continue;
    const bool writable = (segment.flags & 0x2u) != 0;
    const bool executable = (segment.flags & 0x1u) != 0;
    if (writable && executable) {
      writable_.clear();
      executable_.clear();
      Core::PlatformMemory::Free(GetCurrentProcess(), allocation, 0,
                                 MEM_RELEASE);
      return false;
    }
    const DWORD protection = writable     ? PAGE_READWRITE
                             : executable ? PAGE_EXECUTE_READ
                                          : PAGE_READONLY;
    auto *segmentHost = static_cast<std::uint8_t *>(allocation) + offset;
    if (!Core::PlatformMemory::Protect(GetCurrentProcess(), segmentHost,
                                       segment.size, protection, &previous)) {
      writable_.clear();
      executable_.clear();
      Core::PlatformMemory::Free(GetCurrentProcess(), allocation, 0,
                                 MEM_RELEASE);
      return false;
    }
    if (writable)
      writable_.push_back(WritableRange{loadBias + segment.address,
                                        segment.size, segmentHost, protection});
    if (executable)
      executable_.push_back(
          ExecutableRange{loadBias + segment.address, segment.size});
    if (executable)
      executableBytes_ += static_cast<std::size_t>(segment.size);
  }
  if (!FlushInstructionCache(GetCurrentProcess(), allocation, image.size())) {
    writable_.clear();
    executable_.clear();
    executableBytes_ = 0;
    Core::PlatformMemory::Free(GetCurrentProcess(), allocation, 0, MEM_RELEASE);
    return false;
  }
  base_ = allocation;
  size_ = image.size();
  guestBase_ = hostBase;
  loadBias_ = loadBias;
  return true;
}

void *GuestMemory::Translate(std::uint64_t guestAddress,
                             std::size_t bytes) const noexcept {
  if (!base_ || guestAddress < guestBase_)
    return nullptr;
  for (auto const &range : writable_) {
    if (guestAddress >= range.address &&
        guestAddress - range.address <= range.size &&
        bytes <= range.size - (guestAddress - range.address) &&
        range.protection != PAGE_NOACCESS)
      return static_cast<std::uint8_t *>(range.host) +
             (guestAddress - range.address);
  }
  for (auto const &range : anonymous_) {
    if (guestAddress >= range.address &&
        guestAddress - range.address <= range.size &&
        bytes <= range.size - (guestAddress - range.address) &&
        range.protection != PAGE_NOACCESS)
      return static_cast<std::uint8_t *>(range.host) +
             (guestAddress - range.address);
  }
  const auto offset = guestAddress - guestBase_;
  if (offset > static_cast<std::uint64_t>(size_) ||
      bytes > size_ - static_cast<std::size_t>(offset))
    return CurrentStackPointer(guestAddress, bytes, false);
  return static_cast<std::uint8_t *>(base_) + offset;
}

std::size_t GuestMemory::writableBytes() const noexcept {
  std::size_t total = 0;
  for (auto const &range : writable_)
    total += static_cast<std::size_t>(range.size);
  return total;
}

void *GuestMemory::TranslateWritable(std::uint64_t guestAddress,
                                     std::size_t bytes) const noexcept {
  for (auto const &range : writable_) {
    if (guestAddress >= range.address &&
        guestAddress - range.address <= range.size &&
        bytes <= range.size - (guestAddress - range.address) &&
        range.protection == PAGE_READWRITE)
      return static_cast<std::uint8_t *>(range.host) +
             (guestAddress - range.address);
  }
  for (auto const &range : anonymous_) {
    if (guestAddress >= range.address &&
        guestAddress - range.address <= range.size &&
        bytes <= range.size - (guestAddress - range.address) &&
        range.protection == PAGE_READWRITE)
      return static_cast<std::uint8_t *>(range.host) +
             (guestAddress - range.address);
  }
  return CurrentStackPointer(guestAddress, bytes, true);
}

bool GuestMemory::IsExecutable(std::uint64_t guestAddress,
                               std::size_t bytes) const noexcept {
  for (auto const &range : executable_) {
    if (guestAddress >= range.address &&
        guestAddress - range.address <= range.size &&
        bytes <= range.size - (guestAddress - range.address))
      return true;
  }
  return false;
}

bool GuestMemory::MapAnonymous(std::size_t bytes, std::uint64_t prot,
                               std::uint64_t requestedAddress, bool fixed,
                               std::uint64_t &guestAddress) noexcept {
  if (!base_ || bytes == 0 || (prot & ~0x7ull) != 0 || (prot & 0x4ull) != 0)
    return false;
  if (static_cast<std::uint64_t>(bytes) > UINT64_MAX - (GuestPage - 1))
    return false;
  const auto size = AlignGuest(static_cast<std::uint64_t>(bytes));
  if (size == 0)
    return false;

  auto address = requestedAddress;
  if (fixed) {
    if (address == 0 || address % GuestPage != 0)
      return false;
  }
  if (fixed && address > UINT64_MAX - size)
    return false;

  auto occupied = [&](std::uint64_t candidate) {
    if (Overlaps(candidate, size, guestBase_, size_))
      return true;
    for (auto const &range : anonymous_)
      if (Overlaps(candidate, size, range.address, range.size))
        return true;
    return false;
  };
  if (fixed && occupied(address)) {
    return false;
  }

  const auto protection = ProtectionFor(prot);
  auto *host = VirtualAllocFromApp(
      fixed ? reinterpret_cast<void *>(address) : nullptr,
      static_cast<SIZE_T>(size), MEM_RESERVE | MEM_COMMIT, protection);
  if (!host)
    return false;
  address = reinterpret_cast<std::uint64_t>(host);
  if (fixed && address != requestedAddress) {
    VirtualFree(host, 0, MEM_RELEASE);
    return false;
  }
  anonymous_.push_back(AnonymousRange{address, size, host, protection});
  guestAddress = address;
  return true;
}

bool GuestMemory::Unmap(std::uint64_t guestAddress,
                        std::size_t bytes) noexcept {
  if (!base_ || guestAddress == 0 || bytes == 0)
    return false;
  if (static_cast<std::uint64_t>(bytes) > UINT64_MAX - (GuestPage - 1))
    return false;
  const auto size = AlignGuest(static_cast<std::uint64_t>(bytes));
  if (size == 0)
    return false;
  for (auto iterator = anonymous_.begin(); iterator != anonymous_.end();
       ++iterator) {
    if (iterator->address == guestAddress && iterator->size == size) {
      if (iterator->host)
        VirtualFree(iterator->host, 0, MEM_RELEASE);
      anonymous_.erase(iterator);
      return true;
    }
  }
  return false;
}

bool GuestMemory::ProtectNoExecute(std::uint64_t guestAddress,
                                   std::size_t bytes,
                                   std::uint64_t prot) noexcept {
  if (!base_ || guestAddress == 0 || bytes == 0 || (prot & ~0x7ull) != 0 ||
      (prot & 0x4ull) != 0)
    return false;
  for (auto &range : writable_) {
    if (guestAddress < range.address ||
        guestAddress - range.address > range.size)
      continue;
    const auto offset = guestAddress - range.address;
    if (bytes > range.size - offset)
      return false;
    auto *host = static_cast<std::uint8_t *>(range.host) + offset;
    const DWORD protection =
        (prot & 0x2ull) != 0
            ? PAGE_READWRITE
            : ((prot & 0x1ull) != 0 ? PAGE_READONLY : PAGE_NOACCESS);
    DWORD previous{};
    if (VirtualProtectFromApp(host, bytes, protection, &previous) == FALSE)
      return false;
    range.protection = protection;
    return true;
  }
  for (auto &range : anonymous_) {
    if (guestAddress < range.address ||
        guestAddress - range.address > range.size)
      continue;
    const auto offset = guestAddress - range.address;
    if (bytes > range.size - offset)
      return false;
    auto *host = static_cast<std::uint8_t *>(range.host) + offset;
    const auto protection = ProtectionFor(prot);
    DWORD previous{};
    if (VirtualProtectFromApp(host, bytes, protection, &previous) == FALSE)
      return false;
    range.protection = protection;
    return true;
  }
  return false;
}

} // namespace Lab
