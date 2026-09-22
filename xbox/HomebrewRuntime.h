// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "ControlledLoader.h"
#include "GuestMemory.h"
#include "HleDispatcher.h"

#include <atomic>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace Lab {

class HomebrewRuntime {
public:
  HomebrewRuntime() = default;
  ~HomebrewRuntime();
  HomebrewRuntime(HomebrewRuntime const &) = delete;
  HomebrewRuntime &operator=(HomebrewRuntime const &) = delete;

  // Builds a persistent guest image and starts its real ELF entry point.
  // The runtime owns memory, HLE thunks and the worker for the whole session.
  void Start(std::filesystem::path executable, std::filesystem::path stateRoot);
  bool running() const noexcept { return running_.load(); }

private:
  void Record(std::string const &stage, std::string const &detail) noexcept;
  void RunEntry() noexcept;

  struct EntryParams {
    std::int32_t argc{};
    std::uint32_t padding{};
    const char *argv[33]{};
    std::uint64_t entry_addr{};
  };

  std::filesystem::path executable_;
  std::filesystem::path stateFile_;
  std::filesystem::path sessionFile_;
  std::string sessionId_;
  std::string guestPath_;
  ControlledLoadResult load_;
  std::unique_ptr<GuestMemory> memory_;
  std::unique_ptr<HleDispatcher> dispatcher_;
  EntryParams params_{};
  void* mainTlsPage_{};
  std::array<std::uint64_t, 4> mainDtv_{};
  std::uint32_t tlsSlot_{UINT32_MAX};
  std::uint32_t patchedFsReads_{};
  std::thread worker_;
  std::atomic_bool running_{};
};

} // namespace Lab
