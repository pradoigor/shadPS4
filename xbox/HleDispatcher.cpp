// SPDX-License-Identifier: GPL-2.0-or-later
#include "HleDispatcher.h"

#include "core/aerolib/aerolib.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <cstring>

namespace Lab {
namespace {

constexpr std::uint64_t OrbisEnosys = 0x80020016ull;

std::string BaseNid(std::string_view encoded) {
    const auto separator = encoded.find('#');
    return std::string(encoded.substr(0, separator));
}

} // namespace

HleResolution HleDispatcher::Resolve(std::string_view encodedSymbol) {
    const auto encoded = std::string(encodedSymbol);
    const auto nid = BaseNid(encoded);
    const auto* known = Core::AeroLib::FindByNid(nid.c_str());
    const auto name = known ? std::string(known->name) : std::string{};

    auto handler = &Unimplemented;
    if (name == "sceKernelUsleep") handler = &KernelUsleep;
    if (name == "sysKernelGetUpdVersion") handler = &KernelGetUpdVersion;
    if (name == "sysKernelGetLowerLimitUpdVersion") handler = &KernelGetLowerLimitUpdVersion;
    if (name == "getpid") handler = &KernelGetPid;
    if (name == "geteuid") handler = &KernelGetEuid;
    if (name == "sched_yield") handler = &KernelSchedYield;
    if (name == "pthread_self") handler = &KernelThreadSelf;
    if (name == "eglGetError") handler = &EglGetError;
    if (name == "eglQueryAPI") handler = &EglQueryApi;
    if (name == "glGetError") handler = &GlGetError;
    if (name == "sceNetCtlInit") handler = &NetCtlInit;
    if (name == "sceNetCtlTerm") handler = &NetCtlTerm;
    if (name == "sceSystemServiceHideSplashScreen") handler = &HideSplashScreen;
    if (name == "sceKernelDebugOutText") handler = &KernelDebugOutText;
    if (name == "sceKernelMprotect") handler = &KernelMprotect;
    if (name == "clock_gettime") handler = &ClockGetTime;

    const auto slot = static_cast<std::uint64_t>(entries_.size());
    entries_.push_back(Entry{encoded, nid, handler != &Unimplemented, handler, nullptr});
    entries_.back().address = thunks_.Create(this, slot, &Dispatch);
    return HleResolution{encoded, nid, entries_.back().implemented, entries_.back().address};
}

HleBindingSummary HleDispatcher::Bind(std::vector<std::string> const& encodedSymbols) {
    HleBindingSummary summary;
    summary.requested = encodedSymbols.size();
    for (auto const& encoded : encodedSymbols) {
        const auto resolution = Resolve(encoded);
        if (resolution.address) ++summary.executable_addresses;
        if (resolution.implemented) ++summary.implemented_handlers;
        else ++summary.unimplemented_handlers;
    }
    return summary;
}

void* HleDispatcher::AddressFor(std::string_view encodedSymbol) const noexcept {
    for (auto const& entry : entries_)
        if (entry.encoded == encodedSymbol) return entry.address;
    return nullptr;
}

std::size_t HleDispatcher::implementedCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(),
        [](auto const& entry) { return entry.implemented; }));
}

std::size_t HleDispatcher::unresolvedCount() const noexcept {
    return entries_.size() - implementedCount();
}

std::uint64_t HleDispatcher::Dispatch(void* context, std::uint64_t slot,
                                       GuestCallFrame const* frame, void*) noexcept {
    auto* self = static_cast<HleDispatcher*>(context);
    if (!self || !frame || slot >= self->entries_.size()) return OrbisEnosys;
    const auto& entry = self->entries_[static_cast<std::size_t>(slot)];
    return entry.handler ? entry.handler(*self, *frame) : OrbisEnosys;
}

std::uint64_t HleDispatcher::Unimplemented(HleDispatcher&, GuestCallFrame const&) noexcept {
    return OrbisEnosys;
}

std::uint64_t HleDispatcher::KernelUsleep(HleDispatcher&, GuestCallFrame const& frame) noexcept {
    // The PS4 argument is microseconds in RDI. Cap the host sleep so a guest
    // cannot make the UWP process unresponsive through one import call.
    const auto microseconds = (std::min<std::uint64_t>)(frame.gpr[0], 2'000'000ull);
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return 0;
}

std::uint64_t HleDispatcher::KernelGetUpdVersion(HleDispatcher&, GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::KernelGetLowerLimitUpdVersion(HleDispatcher&, GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::KernelGetPid(HleDispatcher&, GuestCallFrame const&) noexcept {
    return 1;
}

std::uint64_t HleDispatcher::KernelGetEuid(HleDispatcher&, GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::KernelSchedYield(HleDispatcher&, GuestCallFrame const&) noexcept {
    SwitchToThread();
    return 0;
}

std::uint64_t HleDispatcher::KernelThreadSelf(HleDispatcher&, GuestCallFrame const&) noexcept {
    return static_cast<std::uint64_t>(GetCurrentThreadId());
}

std::uint64_t HleDispatcher::EglGetError(HleDispatcher&, GuestCallFrame const&) noexcept {
    constexpr std::uint64_t EglSuccess = 0x3000;
    return EglSuccess;
}

std::uint64_t HleDispatcher::EglQueryApi(HleDispatcher&, GuestCallFrame const&) noexcept {
    constexpr std::uint64_t EglOpenGlEsApi = 0x30A0;
    return EglOpenGlEsApi;
}

std::uint64_t HleDispatcher::GlGetError(HleDispatcher&, GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::NetCtlInit(HleDispatcher&, GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::NetCtlTerm(HleDispatcher&, GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::HideSplashScreen(HleDispatcher&, GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::KernelDebugOutText(HleDispatcher& dispatcher,
                                                GuestCallFrame const& frame) noexcept {
    constexpr std::size_t MaxText = 4096;
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    if (!dispatcher.memory_) return OrbisEfault;
    std::string text;
    text.reserve(MaxText);
    for (std::size_t index = 0; index < MaxText; ++index) {
        auto* byte = static_cast<char*>(dispatcher.memory_->Translate(frame.gpr[0] + index, 1));
        if (!byte) return OrbisEfault;
        if (*byte == '\0') break;
        text.push_back(*byte);
    }
    OutputDebugStringA(text.c_str());
    return static_cast<std::uint64_t>(text.size());
}

std::uint64_t HleDispatcher::KernelMprotect(HleDispatcher& dispatcher,
                                            GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEacces = 0x8002000Dull;
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    // PS4 PROT_READ/WRITE/EXEC are represented by bits 0/1/2. EXEC is
    // deliberately rejected: guest code never becomes executable in UWP.
    const auto address = frame.gpr[0];
    const auto bytes = frame.gpr[1];
    const auto prot = frame.gpr[2];
    if (bytes == 0 || (prot & ~0x7ull) != 0) return OrbisEinval;
    if ((prot & 0x4ull) != 0) return OrbisEacces;
    if (!dispatcher.memory_ || address == 0) return OrbisEfault;
    return dispatcher.memory_->ProtectNoExecute(address, bytes, prot)
        ? 0
        : OrbisEfault;
}

std::uint64_t HleDispatcher::ClockGetTime(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    struct Timespec {
        std::int64_t seconds;
        std::int64_t nanoseconds;
    } value{};
    if (!dispatcher.memory_) return OrbisEfault;
    auto* output = static_cast<Timespec*>(
        dispatcher.memory_->TranslateWritable(frame.gpr[1], sizeof(Timespec)));
    if (!output) return OrbisEfault;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);
    value.seconds = seconds.count();
    value.nanoseconds = nanos.count();
    std::memcpy(output, &value, sizeof(value));
    return 0;
}

} // namespace Lab
