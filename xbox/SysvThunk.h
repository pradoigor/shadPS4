// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace Lab {

struct GuestCallFrame {
  std::uint64_t gpr[6]{};
  std::uint64_t guest_stack{};
  alignas(16) std::uint8_t xmm[8][16]{};
};

using SysvDispatch = std::uint64_t (*)(void *context, std::uint64_t slot,
                                       GuestCallFrame const *frame,
                                       void *guestStack);

struct SysvAbiValidation {
  bool passed{};
  std::uint64_t returned_value{};
};

// Allocates small executable thunks that convert the PS4 SysV register frame
// into the Windows x64 call used by the UWP dispatcher. The thunks are not
// guest entry points until a real HLE dispatcher is supplied.
class SysvThunkArena {
public:
  SysvThunkArena() = default;
  ~SysvThunkArena();

  SysvThunkArena(SysvThunkArena const &) = delete;
  SysvThunkArena &operator=(SysvThunkArena const &) = delete;

  void *Create(void *context, std::uint64_t slot, SysvDispatch dispatch);
  bool empty() const noexcept { return pages_.empty(); }

private:
  std::vector<void *> pages_;
};

// Exercises the generated bridge with known SysV integer, vector and stack
// arguments. This is an internal runtime invariant, not a guest compatibility
// test: callers must refuse guest execution if it fails.
SysvAbiValidation ValidateSysvThunkAbi();

// Calls a generated SysV entry point from Windows x64 code with two integer
// arguments. Used only by internal probes; guest code reaches HLE thunks
// directly with the SysV ABI.
std::uint64_t InvokeSysv2(void *entry, std::uint64_t argument0,
                          std::uint64_t argument1);
std::uint64_t InvokeSysv3(void *entry, std::uint64_t argument0,
                          std::uint64_t argument1, std::uint64_t argument2);
std::uint64_t InvokeSysv4(void *entry, std::uint64_t argument0,
                          std::uint64_t argument1, std::uint64_t argument2,
                          std::uint64_t argument3);

// Calls guest SysV code from a Windows worker thread while preserving every
// nonvolatile register required by the Windows x64 ABI.
std::uint64_t InvokeGuestSysv1(void *entry, std::uint64_t argument0);
std::uint64_t InvokeGuestSysv2(void *entry, std::uint64_t argument0,
                               std::uint64_t argument1);

// Transfers control to a PS4 process entry with the kernel/OpenOrbis stack
// layout. The guest exits through the callback supplied in RSI; the callback
// returns here through a thread-local jump context without terminating UWP.
std::uint64_t InvokeGuestEntry(void *entry, std::uint64_t entryParams,
                               bool *exited);

// Completes a guest _exit call without terminating the UWP host process.
void ExitGuestFromHle(std::int32_t status) noexcept;

} // namespace Lab
