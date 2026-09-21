// SPDX-License-Identifier: GPL-2.0-or-later
#include "HleDispatcher.h"

#include "core/aerolib/aerolib.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <thread>

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
    return entry.handler ? entry.handler(*frame) : OrbisEnosys;
}

std::uint64_t HleDispatcher::Unimplemented(GuestCallFrame const&) noexcept {
    return OrbisEnosys;
}

std::uint64_t HleDispatcher::KernelUsleep(GuestCallFrame const& frame) noexcept {
    // The PS4 argument is microseconds in RDI. Cap the host sleep so a guest
    // cannot make the UWP process unresponsive through one import call.
    const auto microseconds = (std::min<std::uint64_t>)(frame.gpr[0], 2'000'000ull);
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return 0;
}

std::uint64_t HleDispatcher::KernelGetUpdVersion(GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::KernelGetLowerLimitUpdVersion(GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::KernelGetPid(GuestCallFrame const&) noexcept {
    return 1;
}

std::uint64_t HleDispatcher::KernelGetEuid(GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::KernelSchedYield(GuestCallFrame const&) noexcept {
    SwitchToThread();
    return 0;
}

std::uint64_t HleDispatcher::KernelThreadSelf(GuestCallFrame const&) noexcept {
    return static_cast<std::uint64_t>(GetCurrentThreadId());
}

std::uint64_t HleDispatcher::EglGetError(GuestCallFrame const&) noexcept {
    constexpr std::uint64_t EglSuccess = 0x3000;
    return EglSuccess;
}

std::uint64_t HleDispatcher::EglQueryApi(GuestCallFrame const&) noexcept {
    constexpr std::uint64_t EglOpenGlEsApi = 0x30A0;
    return EglOpenGlEsApi;
}

std::uint64_t HleDispatcher::GlGetError(GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::NetCtlInit(GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::NetCtlTerm(GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::HideSplashScreen(GuestCallFrame const&) noexcept {
    return 0;
}

} // namespace Lab
