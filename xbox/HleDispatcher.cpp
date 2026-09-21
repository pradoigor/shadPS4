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

constexpr std::uint64_t OrbisEnosys = 0x8002004Eull;

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
    bool implemented = false;
    auto use = [&](HleHandler value) { handler = value; implemented = true; };
    if (name == "sceKernelUsleep") use(&KernelUsleep);
    if (name == "sysKernelGetUpdVersion") handler = &KernelGetUpdVersion;
    if (name == "sysKernelGetLowerLimitUpdVersion") handler = &KernelGetLowerLimitUpdVersion;
    if (name == "getpid") handler = &KernelGetPid;
    if (name == "geteuid") handler = &KernelGetEuid;
    if (name == "sched_yield") use(&KernelSchedYield);
    if (name == "pthread_self") handler = &KernelThreadSelf;
    if (name == "eglGetError") handler = &EglGetError;
    if (name == "eglQueryAPI") handler = &EglQueryApi;
    if (name == "glGetError") handler = &GlGetError;
    if (name == "sceNetCtlInit") handler = &NetCtlInit;
    if (name == "sceNetCtlTerm") handler = &NetCtlTerm;
    if (name == "sceSystemServiceHideSplashScreen") handler = &HideSplashScreen;
    if (name == "sceKernelDebugOutText") use(&KernelDebugOutText);
    if (name == "sceKernelMprotect") use(&KernelMprotect);
    if (name == "memcpy") use(&MemoryMemcpy);
    if (name == "memmove") use(&MemoryMemmove);
    if (name == "memset") use(&MemoryMemset);
    if (name == "memcmp") use(&MemoryMemcmp);
    if (name == "strlen") use(&MemoryStrlen);
    if (name == "mmap" || name == "mmap_np" || name == "__wrap_mmap") use(&MemoryMmap);
    if (name == "sceKernelMmap") use(&KernelMmap);
    if (name == "munmap") use(&MemoryMunmap);
    if (name == "sceKernelMunmap") use(&KernelMunmap);
    if (name == "clock_gettime") use(&ClockGetTime);
    if (name == "sceUserServiceInitialize") use(&UserServiceInitialize);
    if (name == "sceUserServiceGetInitialUser") use(&UserServiceGetInitialUser);
    if (name == "sceUserServiceGetLoginUserIdList") use(&UserServiceGetLoginUsers);
    if (name == "sceUserServiceGetUserName") use(&UserServiceGetUserName);
    if (name == "sceSystemServiceParamGetInt") use(&SystemServiceParamGetInt);
    if (name == "sceRegMgrGetBin") use(&RegMgrGetBin);
    if (name == "sceRegMgrSetBin") use(&RegMgrSetBin);
    if (name == "sceRegMgrGetStr") use(&RegMgrGetStr);
    if (name == "sceRegMgrSetStr") use(&RegMgrSetStr);
    if (name == "sceRegMgrSetInt") use(&RegMgrSetInt);
    if (name == "sceKernelOpen") use(&KernelOpen);
    if (name == "sceKernelClose") use(&KernelClose);
    if (name == "sceKernelRead") use(&KernelRead);
    if (name == "sceKernelWrite") use(&KernelWrite);
    if (name == "sceKernelLseek") use(&KernelLseek);
    if (name == "sceKernelFsync") use(&KernelFsync);

    const auto slot = static_cast<std::uint64_t>(entries_.size());
    entries_.push_back(Entry{encoded, nid, implemented, handler, nullptr});
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

void HleDispatcher::ConfigureFileSystem(std::filesystem::path appRoot,
                                        std::filesystem::path dataRoot) {
    appRoot_ = std::filesystem::weakly_canonical(std::move(appRoot));
    dataRoot_ = std::filesystem::weakly_canonical(std::move(dataRoot));
    std::filesystem::create_directories(dataRoot_);
}

std::size_t HleDispatcher::implementedCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(),
        [](auto const& entry) { return entry.implemented; }));
}

std::size_t HleDispatcher::unresolvedCount() const noexcept {
    return entries_.size() - implementedCount();
}

std::uint64_t HleDispatcher::Dispatch(void* context, std::uint64_t slot,
                                       GuestCallFrame const* frame, void* guestStack) noexcept {
    auto* self = static_cast<HleDispatcher*>(context);
    if (!self || !frame || slot >= self->entries_.size() ||
        frame->guest_stack != reinterpret_cast<std::uint64_t>(guestStack))
        return OrbisEnosys;
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

std::uint64_t HleDispatcher::MemoryMemcpy(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    if (!dispatcher.memory_ || frame.gpr[2] > static_cast<std::uint64_t>(SIZE_MAX))
        return OrbisEfault;
    const auto bytes = static_cast<std::size_t>(frame.gpr[2]);
    auto* destination = dispatcher.memory_->TranslateWritable(frame.gpr[0], bytes);
    auto* source = dispatcher.memory_->Translate(frame.gpr[1], bytes);
    if (bytes != 0 && (!destination || !source)) return OrbisEfault;
    if (bytes != 0) std::memcpy(destination, source, bytes);
    return frame.gpr[0];
}

std::uint64_t HleDispatcher::MemoryMemmove(HleDispatcher& dispatcher,
                                           GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    if (!dispatcher.memory_ || frame.gpr[2] > static_cast<std::uint64_t>(SIZE_MAX))
        return OrbisEfault;
    const auto bytes = static_cast<std::size_t>(frame.gpr[2]);
    auto* destination = dispatcher.memory_->TranslateWritable(frame.gpr[0], bytes);
    auto* source = dispatcher.memory_->Translate(frame.gpr[1], bytes);
    if (bytes != 0 && (!destination || !source)) return OrbisEfault;
    if (bytes != 0) std::memmove(destination, source, bytes);
    return frame.gpr[0];
}

std::uint64_t HleDispatcher::MemoryMemset(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    if (!dispatcher.memory_ || frame.gpr[2] > static_cast<std::uint64_t>(SIZE_MAX))
        return OrbisEfault;
    const auto bytes = static_cast<std::size_t>(frame.gpr[2]);
    auto* destination = dispatcher.memory_->TranslateWritable(frame.gpr[0], bytes);
    if (bytes != 0 && !destination) return OrbisEfault;
    if (bytes != 0) std::memset(destination, static_cast<int>(frame.gpr[1] & 0xFFu), bytes);
    return frame.gpr[0];
}

std::uint64_t HleDispatcher::MemoryMemcmp(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    if (!dispatcher.memory_ || frame.gpr[2] > static_cast<std::uint64_t>(SIZE_MAX))
        return OrbisEfault;
    const auto bytes = static_cast<std::size_t>(frame.gpr[2]);
    auto* left = dispatcher.memory_->Translate(frame.gpr[0], bytes);
    auto* right = dispatcher.memory_->Translate(frame.gpr[1], bytes);
    if (bytes != 0 && (!left || !right)) return OrbisEfault;
    if (bytes == 0) return 0;
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::memcmp(left, right, bytes)));
}

std::uint64_t HleDispatcher::MemoryStrlen(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    constexpr std::size_t MaxString = 1u << 20;
    if (!dispatcher.memory_ || frame.gpr[0] == 0) return OrbisEfault;
    for (std::size_t length = 0; length < MaxString; ++length) {
        if (frame.gpr[0] > UINT64_MAX - length) return OrbisEfault;
        auto* byte = static_cast<char*>(dispatcher.memory_->Translate(frame.gpr[0] + length, 1));
        if (!byte) return OrbisEfault;
        if (*byte == '\0') return length;
    }
    return OrbisEfault;
}

std::uint64_t HleDispatcher::MemoryMmap(HleDispatcher& dispatcher,
                                        GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t Failure = UINT64_MAX;
    if (!dispatcher.memory_ || frame.gpr[1] == 0 || (frame.gpr[2] & 0x4ull) != 0 ||
        (frame.gpr[2] & ~0x7ull) != 0)
        return Failure;
    const auto flags = frame.gpr[3];
    const auto fd = static_cast<std::int64_t>(frame.gpr[4]);
    if ((flags & 0x1000ull) == 0 && fd != -1) return Failure;
    std::uint64_t address = 0;
    if (!dispatcher.memory_->MapAnonymous(frame.gpr[1], frame.gpr[2], frame.gpr[0],
                                          (flags & 0x10ull) != 0, address))
        return Failure;
    return address;
}

std::uint64_t HleDispatcher::KernelMmap(HleDispatcher& dispatcher,
                                        GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEnomem = 0x8002000Cull;
    constexpr std::uint64_t OrbisEacces = 0x8002000Dull;
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    if (!dispatcher.memory_ || frame.gpr[1] == 0) return OrbisEinval;
    if ((frame.gpr[2] & 0x4ull) != 0) return OrbisEacces;
    if ((frame.gpr[2] & ~0x7ull) != 0 ||
        (frame.gpr[3] & 0x1000ull) == 0 || frame.guest_stack > UINT64_MAX - 8)
        return OrbisEinval;
    auto* resultSlot = dispatcher.memory_->Translate(frame.guest_stack + 8, sizeof(std::uint64_t));
    if (!resultSlot) return OrbisEfault;
    std::uint64_t resultAddress = 0;
    std::memcpy(&resultAddress, resultSlot, sizeof(resultAddress));
    auto* output = dispatcher.memory_->TranslateWritable(resultAddress, sizeof(std::uint64_t));
    if (!output) return OrbisEfault;
    std::uint64_t address = 0;
    if (!dispatcher.memory_->MapAnonymous(frame.gpr[1], frame.gpr[2], frame.gpr[0],
                                          (frame.gpr[3] & 0x10ull) != 0, address))
        return OrbisEnomem;
    std::memcpy(output, &address, sizeof(address));
    return 0;
}

std::uint64_t HleDispatcher::MemoryMunmap(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t Failure = UINT64_MAX;
    if (!dispatcher.memory_ || !dispatcher.memory_->Unmap(frame.gpr[0],
                                                           static_cast<std::size_t>(frame.gpr[1])))
        return Failure;
    return 0;
}

std::uint64_t HleDispatcher::KernelMunmap(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    if (!dispatcher.memory_ || frame.gpr[1] == 0) return OrbisEinval;
    return dispatcher.memory_->Unmap(frame.gpr[0], static_cast<std::size_t>(frame.gpr[1]))
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

std::uint64_t HleDispatcher::UserServiceInitialize(HleDispatcher&,
                                                    GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::UserServiceGetInitialUser(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t InvalidArgument = 0x80960005ull;
    if (!dispatcher.memory_) return InvalidArgument;
    auto* output = static_cast<std::int32_t*>(
        dispatcher.memory_->TranslateWritable(frame.gpr[0], sizeof(std::int32_t)));
    if (!output) return InvalidArgument;
    *output = 1;
    return 0;
}

std::uint64_t HleDispatcher::UserServiceGetLoginUsers(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t InvalidArgument = 0x80960005ull;
    constexpr std::int32_t users[4] = {1, -1, -1, -1};
    if (!dispatcher.memory_) return InvalidArgument;
    auto* output = dispatcher.memory_->TranslateWritable(frame.gpr[0], sizeof(users));
    if (!output) return InvalidArgument;
    std::memcpy(output, users, sizeof(users));
    return 0;
}

std::uint64_t HleDispatcher::UserServiceGetUserName(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t InvalidArgument = 0x80960005ull;
    constexpr std::uint64_t BufferTooShort = 0x8096000Aull;
    constexpr char name[] = "Xbox";
    if (frame.gpr[0] == UINT64_MAX || frame.gpr[2] < sizeof(name))
        return frame.gpr[2] < sizeof(name) ? BufferTooShort : InvalidArgument;
    if (!dispatcher.memory_) return InvalidArgument;
    auto* output = dispatcher.memory_->TranslateWritable(
        frame.gpr[1], static_cast<std::size_t>(frame.gpr[2]));
    if (!output) return InvalidArgument;
    std::memcpy(output, name, sizeof(name));
    return 0;
}

std::uint64_t HleDispatcher::SystemServiceParamGetInt(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t ParameterError = 0x80A10003ull;
    if (!dispatcher.memory_) return ParameterError;
    auto* output = static_cast<std::int32_t*>(
        dispatcher.memory_->TranslateWritable(frame.gpr[1], sizeof(std::int32_t)));
    if (!output) return ParameterError;
    switch (frame.gpr[0]) {
    case 1: *output = 1; break;
    case 2: *output = 1; break;
    case 3: *output = 1; break;
    case 4: *output = -180; break;
    case 5: *output = 0; break;
    case 7: *output = 0; break;
    case 1000: *output = 1; break;
    default: *output = 0; break;
    }
    return 0;
}

std::uint64_t HleDispatcher::RegMgrGetBin(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    const auto size = static_cast<std::size_t>(frame.gpr[2]);
    if (!dispatcher.memory_ || size == 0 || size > 4096) return OrbisEinval;
    auto* output = dispatcher.memory_->TranslateWritable(frame.gpr[1], size);
    if (!output) return OrbisEfault;
    std::memset(output, 0, size);
    const auto found = dispatcher.registry_.find(static_cast<std::uint32_t>(frame.gpr[0]));
    if (found != dispatcher.registry_.end())
        std::memcpy(output, found->second.data(),
                    (std::min)(size, found->second.size()));
    return 0;
}

std::uint64_t HleDispatcher::RegMgrSetBin(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    const auto size = static_cast<std::size_t>(frame.gpr[2]);
    if (!dispatcher.memory_ || size == 0 || size > 4096) return OrbisEinval;
    auto* input = static_cast<std::uint8_t*>(dispatcher.memory_->Translate(frame.gpr[1], size));
    if (!input) return OrbisEfault;
    dispatcher.registry_[static_cast<std::uint32_t>(frame.gpr[0])] =
        std::vector<std::uint8_t>(input, input + size);
    return 0;
}

std::uint64_t HleDispatcher::RegMgrGetStr(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    return RegMgrGetBin(dispatcher, frame);
}

std::uint64_t HleDispatcher::RegMgrSetStr(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    return RegMgrSetBin(dispatcher, frame);
}

std::uint64_t HleDispatcher::RegMgrSetInt(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    const auto value = static_cast<std::int32_t>(frame.gpr[1]);
    auto* bytes = reinterpret_cast<std::uint8_t const*>(&value);
    dispatcher.registry_[static_cast<std::uint32_t>(frame.gpr[0])] =
        std::vector<std::uint8_t>(bytes, bytes + sizeof(value));
    return 0;
}

bool HleDispatcher::ReadGuestString(std::uint64_t address, std::string& value,
                                    std::size_t limit) const noexcept {
    value.clear();
    if (!memory_ || address == 0) return false;
    for (std::size_t index = 0; index < limit; ++index) {
        if (address > UINT64_MAX - index) return false;
        auto* byte = static_cast<char*>(memory_->Translate(address + index, 1));
        if (!byte) return false;
        if (*byte == '\0') return true;
        value.push_back(*byte);
    }
    return false;
}

bool HleDispatcher::ResolveGuestPath(std::string const& guestPath, bool write,
                                     std::filesystem::path& hostPath) const noexcept {
    try {
        if (guestPath.empty() || guestPath.find('\\') != std::string::npos)
            return false;
        std::filesystem::path root;
        std::string relative;
        if (guestPath == "/app0" || guestPath.starts_with("/app0/")) {
            if (write) return false;
            root = appRoot_;
            relative = guestPath.size() > 6 ? guestPath.substr(6) : "";
        } else if (guestPath == "/data" || guestPath.starts_with("/data/")) {
            root = dataRoot_ / L"data";
            relative = guestPath.size() > 6 ? guestPath.substr(6) : "";
        } else if (guestPath == "/savedata0" || guestPath.starts_with("/savedata0/")) {
            root = dataRoot_ / L"savedata0";
            relative = guestPath.size() > 11 ? guestPath.substr(11) : "";
        } else if (guestPath.starts_with("/user/")) {
            root = dataRoot_ / L"user";
            relative = guestPath.substr(6);
        } else {
            return false;
        }
        auto relativePath = std::filesystem::path(relative).lexically_normal();
        if (relativePath.is_absolute()) return false;
        for (auto const& component : relativePath)
            if (component == L"..") return false;
        hostPath = (root / relativePath).lexically_normal();
        return !root.empty();
    } catch (...) {
        return false;
    }
}

std::uint64_t HleDispatcher::KernelOpen(HleDispatcher& dispatcher,
                                        GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEacces = 0x8002000Dull;
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    try {
        std::string guestPath;
        if (!dispatcher.ReadGuestString(frame.gpr[0], guestPath)) return OrbisEfault;
        const auto flags = static_cast<std::uint32_t>(frame.gpr[1]);
        const bool writable = (flags & 0x3u) != 0;
        std::filesystem::path hostPath;
        if (!dispatcher.ResolveGuestPath(guestPath, writable, hostPath)) return OrbisEacces;
        if (writable) std::filesystem::create_directories(hostPath.parent_path());
        std::ios::openmode mode = std::ios::binary;
        if ((flags & 0x3u) == 0) mode |= std::ios::in;
        else if ((flags & 0x3u) == 1) mode |= std::ios::out;
        else mode |= std::ios::in | std::ios::out;
        if ((flags & 0x400u) != 0) mode |= std::ios::trunc;
        if ((flags & 0x8u) != 0) mode |= std::ios::app;
        if ((flags & 0x200u) != 0 && !std::filesystem::exists(hostPath)) {
            std::ofstream create(hostPath, std::ios::binary);
            if (!create) return OrbisEacces;
        }
        GuestFile file;
        file.writable = writable;
        file.stream.open(hostPath, mode);
        if (!file.stream) return OrbisEinval;
        const auto descriptor = dispatcher.nextFileDescriptor_++;
        dispatcher.files_.emplace(descriptor, std::move(file));
        return static_cast<std::uint64_t>(descriptor);
    } catch (...) {
        return OrbisEinval;
    }
}

std::uint64_t HleDispatcher::KernelClose(HleDispatcher& dispatcher,
                                         GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEbadf = 0x80020009ull;
    const auto descriptor = static_cast<std::int32_t>(frame.gpr[0]);
    auto found = dispatcher.files_.find(descriptor);
    if (found == dispatcher.files_.end()) return OrbisEbadf;
    found->second.stream.close();
    dispatcher.files_.erase(found);
    return 0;
}

std::uint64_t HleDispatcher::KernelRead(HleDispatcher& dispatcher,
                                        GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEbadf = 0x80020009ull;
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    const auto size = static_cast<std::size_t>(frame.gpr[2]);
    auto found = dispatcher.files_.find(static_cast<std::int32_t>(frame.gpr[0]));
    if (found == dispatcher.files_.end()) return OrbisEbadf;
    auto* output = dispatcher.memory_ ? dispatcher.memory_->TranslateWritable(frame.gpr[1], size)
                                      : nullptr;
    if (size != 0 && !output) return OrbisEfault;
    found->second.stream.read(static_cast<char*>(output), static_cast<std::streamsize>(size));
    return static_cast<std::uint64_t>(found->second.stream.gcount());
}

std::uint64_t HleDispatcher::KernelWrite(HleDispatcher& dispatcher,
                                         GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEbadf = 0x80020009ull;
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    const auto size = static_cast<std::size_t>(frame.gpr[2]);
    auto found = dispatcher.files_.find(static_cast<std::int32_t>(frame.gpr[0]));
    if (found == dispatcher.files_.end() || !found->second.writable) return OrbisEbadf;
    auto* input = dispatcher.memory_ ? dispatcher.memory_->Translate(frame.gpr[1], size) : nullptr;
    if (size != 0 && !input) return OrbisEfault;
    found->second.stream.write(static_cast<char const*>(input),
                               static_cast<std::streamsize>(size));
    return found->second.stream ? frame.gpr[2] : OrbisEbadf;
}

std::uint64_t HleDispatcher::KernelLseek(HleDispatcher& dispatcher,
                                         GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEbadf = 0x80020009ull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    auto found = dispatcher.files_.find(static_cast<std::int32_t>(frame.gpr[0]));
    if (found == dispatcher.files_.end()) return OrbisEbadf;
    std::ios::seekdir direction;
    if (frame.gpr[2] == 0) direction = std::ios::beg;
    else if (frame.gpr[2] == 1) direction = std::ios::cur;
    else if (frame.gpr[2] == 2) direction = std::ios::end;
    else return OrbisEinval;
    found->second.stream.clear();
    found->second.stream.seekg(static_cast<std::int64_t>(frame.gpr[1]), direction);
    const auto position = found->second.stream.tellg();
    if (found->second.writable) {
        found->second.stream.clear();
        found->second.stream.seekp(static_cast<std::int64_t>(frame.gpr[1]), direction);
    }
    return position < 0 ? OrbisEinval : static_cast<std::uint64_t>(position);
}

std::uint64_t HleDispatcher::KernelFsync(HleDispatcher& dispatcher,
                                         GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEbadf = 0x80020009ull;
    auto found = dispatcher.files_.find(static_cast<std::int32_t>(frame.gpr[0]));
    if (found == dispatcher.files_.end()) return OrbisEbadf;
    found->second.stream.flush();
    return found->second.stream ? 0 : OrbisEbadf;
}

} // namespace Lab
