// SPDX-License-Identifier: GPL-2.0-or-later
#include "HleDispatcher.h"
#include "GuestGraphics.h"
#include "GuestFreeType.h"
#include "GuestDevices.h"
#include "GuestPaths.h"

#include "core/aerolib/aerolib.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

namespace Lab {
namespace {

constexpr std::uint64_t OrbisEnosys = 0x8002004Eull;
constexpr std::size_t MaxGuestHeapAllocation = 256ull * 1024 * 1024;
constexpr std::size_t MaxGuestHeapBytes = 512ull * 1024 * 1024;
thread_local std::uint64_t CurrentGuestThreadId = 1;
thread_local std::int32_t GuestPosixErrno = 0;
thread_local std::unordered_map<std::uint32_t, std::uint64_t> GuestSpecificValues;
struct GuestRwlockOwnership {
    bool write{};
    std::uint32_t count{};
};
thread_local std::unordered_map<std::uint64_t, GuestRwlockOwnership>
    GuestRwlockOwnerships;

struct OrbisTimespec {
    std::int64_t seconds{};
    std::int64_t nanoseconds{};
};

struct OrbisStat {
    std::uint32_t device{};
    std::uint32_t inode{};
    std::uint16_t mode{};
    std::uint16_t links{};
    std::uint32_t uid{};
    std::uint32_t gid{};
    std::uint32_t specialDevice{};
    OrbisTimespec accessed{};
    OrbisTimespec modified{};
    OrbisTimespec changed{};
    std::int64_t size{};
    std::int64_t blocks{};
    std::uint32_t blockSize{};
    std::uint32_t flags{};
    std::uint32_t generation{};
    std::int32_t spare{};
    OrbisTimespec created{};
};

struct OrbisDirentHeader {
    std::uint32_t fileNumber{};
    std::uint16_t recordLength{};
    std::uint8_t type{};
    std::uint8_t nameLength{};
};

static_assert(sizeof(OrbisTimespec) == 16);
static_assert(sizeof(OrbisStat) == 120);
static_assert(offsetof(OrbisStat, size) == 72);
static_assert(sizeof(OrbisDirentHeader) == 8);

bool PopulateStat(std::filesystem::path const& path, OrbisStat& output) noexcept {
    try {
        std::error_code error;
        const auto status = std::filesystem::status(path, error);
        if (error || !std::filesystem::exists(status)) return false;
        const bool directory = std::filesystem::is_directory(status);
        std::error_code timeError;
        const auto fileTime = std::filesystem::last_write_time(path, timeError);
        if (!timeError) {
            const auto systemTime = std::chrono::system_clock::now() +
                                    (fileTime - std::filesystem::file_time_type::clock::now());
            const auto sinceEpoch = systemTime.time_since_epoch();
            const auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
            output.modified.seconds = seconds.count();
            output.modified.nanoseconds =
                std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch - seconds)
                    .count();
        }
        output.mode = static_cast<std::uint16_t>((directory ? 0040000 : 0100000) | 0777);
        output.links = 1;
        output.blockSize = directory ? 65536u : 512u;
        if (!directory) {
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > static_cast<std::uintmax_t>(INT64_MAX)) return false;
            output.size = static_cast<std::int64_t>(size);
            output.blocks = static_cast<std::int64_t>((size + 511u) / 512u);
        } else {
            output.size = 65536;
            output.blocks = 128;
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string BaseNid(std::string_view encoded) {
    const auto separator = encoded.find('#');
    return std::string(encoded.substr(0, separator));
}

bool IsCommittedGuestProcessRange(std::uint64_t address, std::size_t bytes,
                                  bool writable) noexcept {
    if (address == 0 || bytes == 0 || address > UINT64_MAX - bytes) return false;
    using Query = SIZE_T(WINAPI*)(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
    static const auto query = []() noexcept -> Query {
        auto module = GetModuleHandleW(L"kernelbase.dll");
        return module ? reinterpret_cast<Query>(GetProcAddress(module, "VirtualQuery"))
                      : nullptr;
    }();
    if (!query) return false;

    const auto end = address + bytes;
    auto cursor = address;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION information{};
        if (!query(reinterpret_cast<void const*>(cursor), &information,
                   sizeof(information)))
            return false;
        const auto regionBegin = reinterpret_cast<std::uint64_t>(information.BaseAddress);
        if (information.RegionSize > UINT64_MAX - regionBegin) return false;
        const auto regionEnd = regionBegin + information.RegionSize;
        const auto protection = information.Protect & 0xFFu;
        const bool readable = information.State == MEM_COMMIT &&
                              information.Type == MEM_PRIVATE &&
                              (information.Protect & PAGE_GUARD) == 0 &&
                              protection != 0 && protection != PAGE_NOACCESS &&
                              protection != PAGE_EXECUTE;
        const bool canWrite = protection == PAGE_READWRITE ||
                              protection == PAGE_WRITECOPY;
        if (cursor < regionBegin || regionEnd <= cursor || !readable ||
            (writable && !canWrite))
            return false;
        cursor = (std::min)(end, regionEnd);
    }
    return true;
}

} // namespace

HleDispatcher::~HleDispatcher() {
    SetPaused(false);
    threads_.clear();
    for (auto const& allocation : guestAllocations_) std::free(allocation.first);
}

void HleDispatcher::SetPaused(bool paused) noexcept {
    paused_.store(paused, std::memory_order_release);
    if (!paused) pauseChanged_.notify_all();
}

HleDispatcher::MessageDialogSnapshot HleDispatcher::GetMessageDialog() const {
    std::scoped_lock lock(dialogMutex_);
    return dialog_;
}

void HleDispatcher::CompleteMessageDialog(bool canceled) noexcept {
    std::scoped_lock lock(dialogMutex_);
    if (dialog_.status != 2) return;
    dialogCanceled_ = canceled;
    dialog_.status = 3;
}

HleResolution HleDispatcher::Resolve(std::string_view encodedSymbol) {
    const auto encoded = std::string(encodedSymbol);
    const auto nid = BaseNid(encoded);
    const auto* known = Core::AeroLib::FindByNid(nid.c_str());
    const auto name = known ? std::string(known->name) : std::string{};

    auto handler = &Unimplemented;
    bool implemented = false;
    auto use = [&](HleHandler value) { handler = value; implemented = true; };
    if (name == "sceKernelUsleep") use(&KernelUsleep);
    if (auto graphics = LookupGraphicsHandler(name)) use(graphics);
    if (auto font = LookupFreeTypeHandler(name)) use(font);
    if (name == "sysKernelGetUpdVersion") use(&KernelGetUpdVersion);
    if (name == "sysKernelGetLowerLimitUpdVersion") use(&KernelGetLowerLimitUpdVersion);
    if (name == "getpid") use(&KernelGetPid);
    if (name == "geteuid") use(&KernelGetEuid);
    if (name == "sched_yield") use(&KernelSchedYield);
    if (name == "_exit" || name == "exit") use(&GenericSuccess);
    if (name == "pthread_self") use(&KernelThreadSelf);
    if (name == "sceNetCtlInit") use(&NetCtlInit);
    if (name == "sceNetCtlTerm") use(&NetCtlTerm);
    if (name == "sceNetCtlGetInfo") use(&NetCtlGetInfo);
    if (name == "_ioctl" || name == "ioctl") use(&KernelIoctl);
    if (name == "sceSystemServiceHideSplashScreen") use(&HideSplashScreen);
    if (name == "sceMsgDialogInitialize") use(&MsgDialogInitialize);
    if (name == "sceMsgDialogTerminate") use(&MsgDialogTerminate);
    if (name == "sceMsgDialogOpen") use(&MsgDialogOpen);
    if (name == "sceMsgDialogUpdateStatus" || name == "sceMsgDialogGetStatus")
        use(&MsgDialogStatus);
    if (name == "sceMsgDialogClose") use(&MsgDialogClose);
    if (name == "sceMsgDialogGetResult") use(&MsgDialogGetResult);
    if (name == "sceKernelDebugOutText") use(&KernelDebugOutText);
    if (name == "sceKernelMprotect") use(&KernelMprotect);
    if (name == "memcpy") use(&MemoryMemcpy);
    if (name == "memmove") use(&MemoryMemmove);
    if (name == "memset") use(&MemoryMemset);
    if (name == "memcmp") use(&MemoryMemcmp);
    if (name == "strlen") use(&MemoryStrlen);
    if (name == "mmap" || name == "mmap_np" || name == "__wrap_mmap") use(&MemoryMmap);
    if (name == "sceLibcMspaceMalloc") use(&MspaceMalloc);
    if (name == "sceLibcMspaceCreate") use(&MspaceCreate);
    if (name == "sceLibcMspaceDestroy") use(&MspaceDestroy);
    if (name == "sceLibcMspaceMallocStatsFast") use(&MspaceMallocStatsFast);
    if (name == "sceLibcMspaceCalloc") use(&MspaceCalloc);
    if (name == "sceLibcMspaceRealloc") use(&MspaceRealloc);
    if (name == "sceLibcMspaceFree") use(&MspaceFree);
    if (name == "sceLibcMspaceMallocUsableSize") use(&MspaceUsableSize);
    if (name == "sceKernelMmap") use(&KernelMmap);
    if (name == "sceKernelReserveVirtualRange") use(&KernelReserveVirtualRange);
    if (name == "sceKernelMapNamedSystemFlexibleMemory") use(&KernelMapNamedSystemFlexibleMemory);
    if (name == "munmap") use(&MemoryMunmap);
    if (name == "sceKernelMunmap") use(&KernelMunmap);
    if (name == "clock_gettime") use(&ClockGetTime);
    if (name == "sigaction") use(&SignalAction);
    if (name == "sceUserServiceInitialize") use(&UserServiceInitialize);
    if (name == "sceUserServiceGetInitialUser") use(&UserServiceGetInitialUser);
    if (name == "sceUserServiceGetLoginUserIdList") use(&UserServiceGetLoginUsers);
    if (name == "sceUserServiceGetRegisteredUserIdList") use(&UserServiceGetRegisteredUsers);
    if (name == "sceUserServiceGetNpAccountId") use(&UserServiceGetNpAccountId);
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
    if (name == "_open") use(&PosixOpen);
    if (name == "close") use(&KernelClose);
    if (name == "read" || name == "_read") use(&KernelRead);
    if (name == "_readv" || name == "readv" || name == "sceKernelReadv")
        use(&KernelReadv);
    if (name == "write" || name == "_write") use(&KernelWrite);
    if (name == "_writev" || name == "writev" || name == "sceKernelWritev")
        use(&KernelWritev);
    if (name == "lseek") use(&KernelLseek);
    if (name == "fsync") use(&KernelFsync);
    if (name == "access") use(&FileAccess);
    if (name == "mkdir") use(&FileMkdir);
    if (name == "rmdir") use(&FileRmdir);
    if (name == "rename") use(&FileRename);
    if (name == "unlink") use(&FileUnlink);
    if (name == "chmod") use(&FileChmod);
    if (name == "flock") use(&FileFlock);
    if (name == "stat") use(&FileStat);
    if (name == "_fstat") use(&FileFstat);
    if (name == "ftruncate") use(&FileFtruncate);
    if (name == "getdents") use(&FileGetdents);
    if (name == "pthread_mutexattr_init") use(&PthreadMutexAttrInit);
    if (name == "pthread_mutexattr_settype") use(&PthreadMutexAttrSetType);
    if (name == "pthread_mutex_init") use(&PthreadMutexInit);
    if (name == "pthread_mutex_destroy") use(&PthreadMutexDestroy);
    if (name == "pthread_mutex_lock") use(&PthreadMutexLock);
    if (name == "pthread_mutex_trylock") use(&PthreadMutexTryLock);
    if (name == "pthread_mutex_unlock") use(&PthreadMutexUnlock);
    if (name == "pthread_cond_init") use(&PthreadCondInit);
    if (name == "pthread_cond_destroy") use(&PthreadCondDestroy);
    if (name == "pthread_cond_wait") use(&PthreadCondWait);
    if (name == "pthread_cond_signal") use(&PthreadCondSignal);
    if (name == "pthread_cond_broadcast") use(&PthreadCondBroadcast);
    if (name == "sem_init") use(&SemaphoreInit);
    if (name == "sem_destroy") use(&SemaphoreDestroy);
    if (name == "sem_trywait") use(&SemaphoreTryWait);
    if (name == "sem_wait") use(&SemaphoreWait);
    if (name == "sem_timedwait") use(&SemaphoreTimedWait);
    if (name == "sem_getvalue") use(&SemaphoreGetValue);
    if (name == "sem_post") use(&SemaphorePost);
    if (name == "pthread_attr_init") use(&PthreadAttrInit);
    if (name == "pthread_attr_setdetachstate") use(&PthreadAttrSetDetachState);
    if (name == "pthread_attr_setstacksize") use(&PthreadAttrSetStackSize);
    if (name == "pthread_create") use(&PthreadCreate);
    if (name == "pthread_join") use(&PthreadJoin);
    if (name == "pthread_detach") use(&PthreadDetach);
    if (name == "pthread_key_create") use(&PthreadKeyCreate);
    if (name == "pthread_getspecific") use(&PthreadGetSpecific);
    if (name == "pthread_setspecific") use(&PthreadSetSpecific);
    if (name == "pthread_once") use(&PthreadOnce);
    if (name == "pthread_rwlock_rdlock") use(&PthreadRwlockReadLock);
    if (name == "pthread_rwlock_wrlock") use(&PthreadRwlockWriteLock);
    if (name == "pthread_rwlock_unlock") use(&PthreadRwlockUnlock);
    if (name == "__error") use(&KernelErrorPointer);
    if (name == "sysconf") use(&KernelSysconf);
    if (name == "sceKernelGetFsSandboxRandomWord") use(&KernelSandboxWord);
    if (name == "_nanosleep") use(&KernelNanosleep);
    if (name == "setuid" || name == "madvise" || name == "fchmod" ||
        name == "sceSysmoduleLoadModule" ||
        name == "sceSysmoduleLoadModuleInternal" ||
        name == "sceSysmoduleUnloadModuleInternal" ||
        name == "sceCommonDialogInitialize" ||
        name == "sceSystemServiceParamGetString" ||
        name == "sceKernelSync" || name == "pthread_setcancelstate" ||
        name == "pthread_setcanceltype" ||
        name == "__pthread_cleanup_push_imp" ||
        name == "__pthread_cleanup_pop_imp" ||
        name == "pthread_set_name_np") use(&GenericSuccess);
    if (auto device = LookupDeviceHandler(name)) use(device);

    const auto slot = static_cast<std::uint64_t>(entries_.size());
    entries_.push_back(Entry{encoded, nid, name, implemented, handler, nullptr});
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

void* HleDispatcher::GraphicsAddress(std::string_view name) {
    if (name.empty() || name.size() > 128) return nullptr;
    for (auto const& entry : entries_)
        if (entry.name == name && entry.handler == LookupGraphicsHandler(name))
            return entry.address;
    auto handler = LookupGraphicsHandler(name);
    if (!handler) return nullptr;
    const auto slot = static_cast<std::uint64_t>(entries_.size());
    entries_.push_back(Entry{std::string(name), std::string{}, std::string(name), true, handler, nullptr});
    entries_.back().address = thunks_.Create(this, slot, &Dispatch);
    return entries_.back().address;
}

void* HleDispatcher::GuestWritable(GuestCallFrame const& frame, std::uint64_t address, std::size_t bytes) noexcept {
    return WritablePointer(*this, frame, address, bytes);
}
void const* HleDispatcher::GuestReadable(GuestCallFrame const& frame, std::uint64_t address, std::size_t bytes) noexcept {
    return ReadablePointer(*this, frame, address, bytes);
}
bool HleDispatcher::GuestString(std::uint64_t address, std::string& value, std::size_t limit) const noexcept {
    return ReadGuestString(address, value, limit);
}
void HleDispatcher::GraphicsLog(std::string_view line) noexcept {
    AppendConsole(line.data(), line.size());
    static constexpr char newline = '\n';
    AppendConsole(&newline, 1);
}

void HleDispatcher::ConfigureFileSystem(std::filesystem::path appRoot,
                                        std::filesystem::path dataRoot) {
    // Both roots originate from the already opened eboot.bin under LocalState.
    // AppContainer denies the directory enumeration performed internally by
    // weakly_canonical on Xbox, so normalize textually and validate every guest
    // component in ResolveGuestPath instead.
    appRoot_ = std::move(appRoot).lexically_normal();
    dataRoot_ = std::move(dataRoot).lexically_normal();
    if (appRoot_.empty() || dataRoot_.empty() || !appRoot_.is_absolute() ||
        !dataRoot_.is_absolute())
        throw std::invalid_argument("Raízes do VFS UWP inválidas.");
    std::filesystem::create_directories(dataRoot_);
}

void HleDispatcher::ConfigureTrace(std::filesystem::path path,
                                   std::string const& sessionId) {
    tracePath_ = std::move(path);
    traceHistoryPath_ = tracePath_.parent_path() / L"homebrew-hle-trace.jsonl";
    traceArchivePath_ = sessionId.empty()
        ? std::filesystem::path{}
        : tracePath_.parent_path() / ("homebrew-hle-" + sessionId + ".jsonl");
    consolePath_ = sessionId.empty()
        ? std::filesystem::path{}
        : tracePath_.parent_path() / ("homebrew-console-" + sessionId + ".log");
    consoleBytes_ = 0;
    std::error_code ignored;
    std::filesystem::remove(tracePath_, ignored);
    std::filesystem::remove(traceHistoryPath_, ignored);
}

void HleDispatcher::AppendConsole(void const* bytes, std::size_t length) noexcept {
    constexpr std::size_t ConsoleLimit = 1024 * 1024;
    if (!bytes || length == 0 || consolePath_.empty()) return;
    try {
        std::scoped_lock lock(consoleMutex_);
        if (consoleBytes_ >= ConsoleLimit) return;
        const auto accepted = (std::min)(length, ConsoleLimit - consoleBytes_);
        std::ofstream output(consolePath_, std::ios::binary | std::ios::app);
        output.write(static_cast<char const*>(bytes),
                     static_cast<std::streamsize>(accepted));
        output.flush();
        if (output) consoleBytes_ += accepted;
    } catch (...) {}
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
    if (self->paused_.load(std::memory_order_acquire)) {
        std::unique_lock pauseLock(self->pauseMutex_);
        self->pauseChanged_.wait(pauseLock, [self] {
            return !self->paused_.load(std::memory_order_acquire);
        });
    }
    const auto entry = self->entries_[static_cast<std::size_t>(slot)];
    std::uint64_t sequence{};
    const bool dialogPoll = entry.name == "sceMsgDialogUpdateStatus" ||
                            entry.name == "sceMsgDialogGetStatus";
    const auto pollCount = dialogPoll
        ? self->dialogPollCount_.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    const auto thread = static_cast<std::uint64_t>(GetCurrentThreadId());
    auto writeTrace = [&](char const* phase, std::uint64_t result,
                          bool includeResult) noexcept {
        if (self->tracePath_.empty()) return;
        if (dialogPoll && pollCount > 8 && pollCount % 1024 != 0) return;
        // Preserve startup in full; avoid synchronous disk writes for every
        // glyph/GL call once the render loop starts. Always retain guest errors,
        // missing services, presentation and exit, even after the startup cap.
        const bool failure = includeResult &&
            ((result <= UINT32_MAX && (result & 0x80000000u)) || result == UINT64_MAX ||
             (entry.name.starts_with("FT_") && entry.name != "FT_Get_Char_Index" && result != 0));
        if (sequence > 32768 && entry.implemented && !failure &&
            entry.name != "eglSwapBuffers" && entry.name != "_exit" && sequence % 512 != 0)
            return;
        try {
            std::scoped_lock traceLock(self->traceMutex_);
            std::ofstream latest(self->tracePath_, std::ios::binary | std::ios::trunc);
            auto write = [&](std::ostream& output) {
                output << "{\"sequence\":" << sequence
                       << ",\"thread\":" << thread
                       << ",\"phase\":\"" << phase
                       << "\",\"symbol\":\"" << entry.encoded
                       << "\",\"nid\":\"" << entry.nid
                       << "\",\"name\":\"" << entry.name
                       << "\",\"implemented\":"
                       << (entry.implemented ? "true" : "false")
                       << ",\"arguments\":[" << frame->gpr[0] << ','
                       << frame->gpr[1] << ',' << frame->gpr[2] << ','
                       << frame->gpr[3] << ',' << frame->gpr[4] << ','
                       << frame->gpr[5] << ']'
                       << ",\"guest_stack\":" << frame->guest_stack;
                if (includeResult) output << ",\"result\":" << result;
                output << '}';
            };
            write(latest);
            latest.flush();
            {
                if (sequence % 4096 == 0 && !includeResult) {
                    std::error_code ignored;
                    std::filesystem::copy_file(self->traceHistoryPath_, self->traceHistoryPath_.wstring() + L".previous",
                        std::filesystem::copy_options::overwrite_existing, ignored);
                    std::ofstream reset(self->traceHistoryPath_, std::ios::binary | std::ios::trunc);
                }
                std::ofstream history(self->traceHistoryPath_,
                                      std::ios::binary | std::ios::app);
                write(history);
                history << '\n';
                history.flush();
                if (!self->traceArchivePath_.empty()) {
                    std::ofstream archive(self->traceArchivePath_,
                                         std::ios::binary | std::ios::app);
                    write(archive);
                    archive << '\n';
                    archive.flush();
                }
            }
        } catch (...) {
        }
    };
    if (!self->tracePath_.empty()) {
        std::scoped_lock traceLock(self->traceMutex_);
        sequence = ++self->callSequence_;
    }
    writeTrace("enter", 0, false);
    const auto result = entry.handler ? entry.handler(*self, *frame) : OrbisEnosys;
    writeTrace("return", result, true);
    if (entry.nid == "6Z83sYWFlA8" || entry.name == "exit") {
        if (auto* slot = WritablePointer(*self, *frame, frame->guest_stack,
                                         sizeof(std::uint64_t))) {
            const auto target = reinterpret_cast<std::uint64_t>(
                PrepareGuestExitFromHle(static_cast<std::int32_t>(frame->gpr[0])));
            if (target) std::memcpy(slot, &target, sizeof(target));
        }
    }
    return result;
}

void* HleDispatcher::WritablePointer(HleDispatcher& dispatcher,
                                     GuestCallFrame const& frame,
                                     std::uint64_t address,
                                     std::size_t bytes) noexcept {
    if (dispatcher.memory_) {
        if (auto* translated = dispatcher.memory_->TranslateWritable(address, bytes))
            return translated;
        if (bytes && bytes <= 65536 && address <= UINT64_MAX - bytes) {
            auto page = address & ~0x3fffull;
            const auto end = (address + bytes - 1) & ~0x3fffull;
            while (page <= end) {
                if (!dispatcher.memory_->CommitLazyPage(page)) break;
                if (page > UINT64_MAX - 0x4000ull) break;
                page += 0x4000ull;
            }
        }
    }
    // Guest code runs directly on a native worker stack. OpenOrbis places
    // short-lived pthread attributes, TLS keys and other ABI structures there,
    // outside the mapped ELF image. Accept only the current thread's narrow
    // stack neighbourhood; unrelated host pointers remain rejected.
    constexpr std::uint64_t StackWindow = 2ull * 1024ull * 1024ull;
    if (address == 0 || bytes > StackWindow || address > UINT64_MAX - bytes)
        return nullptr;
    const auto lower = frame.guest_stack > StackWindow
                           ? frame.guest_stack - StackWindow
                           : 0;
    const auto upper = frame.guest_stack <= UINT64_MAX - StackWindow
                           ? frame.guest_stack + StackWindow
                           : UINT64_MAX;
    if (address >= lower && address <= upper && bytes <= upper - address &&
        IsCommittedGuestProcessRange(address, bytes, true))
        return reinterpret_cast<void*>(address);
    if (IsCommittedGuestProcessRange(address, bytes, true))
        return reinterpret_cast<void*>(address);
    return nullptr;
}

void const* HleDispatcher::ReadablePointer(HleDispatcher& dispatcher,
                                           GuestCallFrame const& frame,
                                           std::uint64_t address,
                                           std::size_t bytes) noexcept {
    if (dispatcher.memory_) {
        if (auto* translated = dispatcher.memory_->Translate(address, bytes))
            return translated;
    }
    if (auto* translated = WritablePointer(dispatcher, frame, address, bytes))
        return translated;
    return IsCommittedGuestProcessRange(address, bytes, false)
               ? reinterpret_cast<void const*>(address)
               : nullptr;
}

std::uint64_t HleDispatcher::Unimplemented(HleDispatcher&, GuestCallFrame const&) noexcept {
    return OrbisEnosys;
}

std::uint64_t HleDispatcher::KernelErrorPointer(HleDispatcher&,
                                                 GuestCallFrame const&) noexcept {
    return reinterpret_cast<std::uint64_t>(&GuestPosixErrno);
}

std::uint64_t HleDispatcher::GenericSuccess(HleDispatcher&,
                                             GuestCallFrame const&) noexcept {
    return 0;
}

std::uint64_t HleDispatcher::GenericHandle(HleDispatcher&,
                                            GuestCallFrame const&) noexcept {
    return 1;
}

std::uint64_t HleDispatcher::KernelSysconf(HleDispatcher&,
                                            GuestCallFrame const& frame) noexcept {
    switch (static_cast<std::uint32_t>(frame.gpr[0])) {
    case 0: return 0x20000;
    case 1: return 0x588bc000;
    case 2: return 0x64;
    case 3: return 0x20;
    case 4: return 0x644;
    case 5: return static_cast<std::uint64_t>(-1ll);
    case 26: return 0x7fffffff;
    case 47: return 0x4000;
    case 85: return 4;
    case 86: return 128;
    case 93: return 0x4000;
    case 94: return 2048;
    default: return 0;
    }
}

std::uint64_t HleDispatcher::KernelSandboxWord(HleDispatcher&,
                                                GuestCallFrame const&) noexcept {
    static constexpr char Sandbox[] = "sys";
    return reinterpret_cast<std::uint64_t>(Sandbox);
}

std::uint64_t HleDispatcher::KernelNanosleep(HleDispatcher& self,
                                              GuestCallFrame const& frame) noexcept {
    struct Timespec { std::int64_t seconds; std::int64_t nanoseconds; };
    auto const* request = static_cast<Timespec const*>(self.memory_
        ? self.memory_->Translate(frame.gpr[0], sizeof(Timespec)) : nullptr);
    // libc commonly creates timespec on the native guest thread stack. The
    // thunk captured that RSP, so accept only a narrow range around it.
    constexpr std::uint64_t StackWindow = 2ull * 1024ull * 1024ull;
    const auto distance = frame.gpr[0] > frame.guest_stack
        ? frame.gpr[0] - frame.guest_stack : frame.guest_stack - frame.gpr[0];
    if (!request && frame.gpr[0] && distance <= StackWindow)
        request = reinterpret_cast<Timespec const*>(frame.gpr[0]);
    if (!request || request->seconds < 0 || request->nanoseconds < 0 ||
        request->nanoseconds >= 1'000'000'000) {
        GuestPosixErrno = 22;
        return static_cast<std::uint64_t>(-1ll);
    }
    auto duration = std::chrono::seconds((std::min<std::int64_t>)(request->seconds, 2)) +
                    std::chrono::nanoseconds(request->nanoseconds);
    std::this_thread::sleep_for(duration);
    if (frame.gpr[1] && self.memory_) {
        auto* remaining = static_cast<Timespec*>(
            self.memory_->TranslateWritable(frame.gpr[1], sizeof(Timespec)));
        const auto remainingDistance = frame.gpr[1] > frame.guest_stack
            ? frame.gpr[1] - frame.guest_stack : frame.guest_stack - frame.gpr[1];
        if (!remaining && remainingDistance <= StackWindow)
            remaining = reinterpret_cast<Timespec*>(frame.gpr[1]);
        if (remaining)
            *remaining = {};
    }
    return 0;
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
    return CurrentGuestThreadId;
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

std::uint64_t HleDispatcher::NetCtlGetInfo(HleDispatcher&,
                                           GuestCallFrame const&) noexcept {
    // The current UWP HLE has no PS4 network-control implementation. Match the
    // official shadPS4 offline result so applications can take their offline
    // path instead of seeing a generic ENOSYS error.
    constexpr std::uint64_t OrbisNetCtlNotConnected = 0x80412108ull;
    return OrbisNetCtlNotConnected;
}

std::uint64_t HleDispatcher::KernelIoctl(HleDispatcher& dispatcher,
                                         GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t Tiocgwinsz = 0x5413;
    constexpr std::int32_t PosixEbadf = 9;
    constexpr std::int32_t PosixEfault = 14;
    constexpr std::int32_t PosixEnotty = 25;
    struct GuestWindowSize {
        std::uint16_t rows{};
        std::uint16_t columns{};
        std::uint16_t pixelWidth{};
        std::uint16_t pixelHeight{};
    };
    static_assert(sizeof(GuestWindowSize) == 8);

    const auto descriptor = static_cast<std::int32_t>(frame.gpr[0]);
    const bool standardDescriptor = descriptor >= 0 && descriptor <= 2;
    if (!standardDescriptor && !dispatcher.files_.contains(descriptor)) {
        GuestPosixErrno = PosixEbadf;
        return UINT64_MAX;
    }
    if (frame.gpr[1] != Tiocgwinsz) {
        GuestPosixErrno = PosixEnotty;
        return UINT64_MAX;
    }
    if (dispatcher.files_.contains(descriptor)) {
        // TIOCGWINSZ on a regular file (the observed Apollo call) is POSIX
        // ENOTTY, not an unimplemented Orbis import.
        GuestPosixErrno = PosixEnotty;
        return UINT64_MAX;
    }
    auto* output = static_cast<GuestWindowSize*>(dispatcher.GuestWritable(
        frame, frame.gpr[2], sizeof(GuestWindowSize)));
    if (!output) {
        GuestPosixErrno = PosixEfault;
        return UINT64_MAX;
    }
    *output = GuestWindowSize{45, 80, 1920, 1080};
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

std::uint64_t HleDispatcher::AllocateFromMspace(GuestMspace& space,
                                                 std::uint64_t requested) noexcept {
    if (requested > MaxGuestHeapAllocation || requested > UINT64_MAX - 15) return 0;
    const auto bytes = ((std::max<std::uint64_t>)(requested, 1) + 15) & ~15ull;
    if (bytes > MaxGuestHeapBytes - space.inUse) return 0;
    for (auto it = space.freeBlocks.begin(); it != space.freeBlocks.end(); ++it) {
        if (it->size < bytes) continue;
        const auto address = it->address;
        try { space.allocations.emplace(address, bytes); } catch (...) { return 0; }
        it->address += bytes;
        it->size -= bytes;
        if (!it->size) space.freeBlocks.erase(it);
        space.inUse += bytes;
        space.peakInUse = (std::max)(space.peakInUse, space.inUse);
        return address;
    }
    if (space.next > space.capacity || bytes > space.capacity - space.next) return 0;
    const auto address = space.base + space.next;
    try { space.allocations.emplace(address, bytes); } catch (...) { return 0; }
    space.next += bytes;
    space.inUse += bytes;
    space.peakInUse = (std::max)(space.peakInUse, space.inUse);
    return address;
}

std::uint64_t HleDispatcher::MspaceCreate(HleDispatcher& dispatcher,
                                           GuestCallFrame const& frame) noexcept {
    if (!dispatcher.memory_ || !ReadablePointer(dispatcher, frame, frame.gpr[0], 1) ||
        frame.gpr[2] <= 0x10000 || frame.gpr[2] > 4ull * 1024 * 1024 * 1024 ||
        !dispatcher.memory_->IsLazySystemRange(frame.gpr[1], frame.gpr[2])) return 0;
    std::scoped_lock lock(dispatcher.allocationsMutex_);
    try {
        auto [it, inserted] = dispatcher.guestMspaces_.try_emplace(frame.gpr[1]);
        if (!inserted) return 0;
        it->second.base = frame.gpr[1];
        it->second.capacity = frame.gpr[2];
        return frame.gpr[1];
    } catch (...) { return 0; }
}

std::uint64_t HleDispatcher::MspaceDestroy(HleDispatcher& dispatcher,
                                            GuestCallFrame const& frame) noexcept {
    std::scoped_lock lock(dispatcher.allocationsMutex_);
    return dispatcher.guestMspaces_.erase(frame.gpr[0]) ? 0 : 0x80020016ull;
}

std::uint64_t HleDispatcher::MspaceMallocStatsFast(HleDispatcher& dispatcher,
                                                    GuestCallFrame const& frame) noexcept {
    struct ManagedSize {
        std::uint16_t size{40}, version{1};
        std::uint32_t reserved{};
        std::uint64_t maxSystem{}, currentSystem{}, maxInUse{}, currentInUse{};
    };
    static_assert(sizeof(ManagedSize) == 40);
    std::scoped_lock lock(dispatcher.allocationsMutex_);
    auto found = dispatcher.guestMspaces_.find(frame.gpr[0]);
    if (found == dispatcher.guestMspaces_.end()) return 0x80020016ull;
    auto* output = WritablePointer(dispatcher, frame, frame.gpr[1], sizeof(ManagedSize));
    if (!output) return 0x8002000Eull;
    ManagedSize result;
    result.maxSystem = found->second.capacity;
    result.currentSystem = found->second.capacity;
    result.maxInUse = found->second.peakInUse;
    result.currentInUse = found->second.inUse;
    std::memcpy(output, &result, sizeof(result));
    return 0;
}

std::uint64_t HleDispatcher::MspaceMalloc(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    // The zero handle is the default libc mspace used by the observed store.
    // Do not pretend that a nonzero, guest-owned mspace is a host heap.
    if (frame.gpr[0] != 0) {
        std::scoped_lock lock(dispatcher.allocationsMutex_);
        auto found = dispatcher.guestMspaces_.find(frame.gpr[0]);
        return found == dispatcher.guestMspaces_.end() ? 0 :
            AllocateFromMspace(found->second, frame.gpr[1]);
    }
    if (frame.gpr[1] > MaxGuestHeapAllocation) return 0;
    const auto bytes = static_cast<std::size_t>(frame.gpr[1] ? frame.gpr[1] : std::uint64_t{1});
    std::scoped_lock lock(dispatcher.allocationsMutex_);
    if (bytes > MaxGuestHeapBytes - dispatcher.guestAllocationBytes_) return 0;
    auto* allocation = std::malloc(bytes);
    if (!allocation) return 0;
    try { dispatcher.guestAllocations_.emplace(allocation, bytes); }
    catch (...) { std::free(allocation); return 0; }
    dispatcher.guestAllocationBytes_ += bytes;
    return reinterpret_cast<std::uint64_t>(allocation);
}

std::uint64_t HleDispatcher::MspaceCalloc(HleDispatcher& dispatcher,
                                        GuestCallFrame const& frame) noexcept {
    if (frame.gpr[0] != 0) {
        if (frame.gpr[2] != 0 && frame.gpr[1] > MaxGuestHeapAllocation / frame.gpr[2]) return 0;
        const auto product = frame.gpr[1] * frame.gpr[2];
        std::scoped_lock lock(dispatcher.allocationsMutex_);
        auto found = dispatcher.guestMspaces_.find(frame.gpr[0]);
        if (found == dispatcher.guestMspaces_.end()) return 0;
        const auto address = AllocateFromMspace(found->second, product);
        if (address) std::memset(reinterpret_cast<void*>(address), 0,
                                 static_cast<std::size_t>((std::max<std::uint64_t>)(product, 1)));
        return address;
    }
    if (frame.gpr[0] != 0 ||
        (frame.gpr[2] != 0 && frame.gpr[1] > MaxGuestHeapAllocation / frame.gpr[2]))
        return 0;
    const auto product = frame.gpr[1] * frame.gpr[2];
    if (product > MaxGuestHeapAllocation) return 0;
    const auto bytes = static_cast<std::size_t>(product ? product : std::uint64_t{1});
    std::scoped_lock lock(dispatcher.allocationsMutex_);
    if (bytes > MaxGuestHeapBytes - dispatcher.guestAllocationBytes_) return 0;
    auto* allocation = std::calloc(1, bytes);
    if (!allocation) return 0;
    try { dispatcher.guestAllocations_.emplace(allocation, bytes); }
    catch (...) { std::free(allocation); return 0; }
    dispatcher.guestAllocationBytes_ += bytes;
    return reinterpret_cast<std::uint64_t>(allocation);
}

std::uint64_t HleDispatcher::MspaceRealloc(HleDispatcher& dispatcher,
                                           GuestCallFrame const& frame) noexcept {
    if (frame.gpr[0] != 0) {
        if (frame.gpr[2] > MaxGuestHeapAllocation) return 0;
        std::scoped_lock lock(dispatcher.allocationsMutex_);
        auto found = dispatcher.guestMspaces_.find(frame.gpr[0]);
        if (found == dispatcher.guestMspaces_.end()) return 0;
        auto& space = found->second;
        if (!frame.gpr[1]) return AllocateFromMspace(space, frame.gpr[2]);
        auto old = space.allocations.find(frame.gpr[1]);
        if (old == space.allocations.end()) return 0;
        if (!frame.gpr[2]) {
            try { space.freeBlocks.push_back({old->first, old->second}); }
            catch (...) { return 0; }
            space.inUse -= old->second;
            space.allocations.erase(old);
            return 0;
        }
        if (frame.gpr[2] <= old->second) return old->first;
        const auto oldAddress = old->first, oldSize = old->second;
        const auto address = AllocateFromMspace(space, frame.gpr[2]);
        if (!address) return 0;
        std::memcpy(reinterpret_cast<void*>(address), reinterpret_cast<void const*>(oldAddress),
                    static_cast<std::size_t>(oldSize));
        try { space.freeBlocks.push_back({oldAddress, oldSize}); }
        catch (...) { return address; }
        space.inUse -= oldSize;
        space.allocations.erase(oldAddress);
        return address;
    }
    if (frame.gpr[0] != 0 || frame.gpr[2] > MaxGuestHeapAllocation) return 0;
    if (!frame.gpr[1]) {
        GuestCallFrame mallocFrame = frame;
        mallocFrame.gpr[1] = frame.gpr[2];
        return MspaceMalloc(dispatcher, mallocFrame);
    }
    auto* old = reinterpret_cast<void*>(frame.gpr[1]);
    std::scoped_lock lock(dispatcher.allocationsMutex_);
    auto found = dispatcher.guestAllocations_.find(old);
    if (found == dispatcher.guestAllocations_.end()) return 0;
    if (!frame.gpr[2]) {
        dispatcher.guestAllocationBytes_ -= found->second;
        dispatcher.guestAllocations_.erase(found);
        std::free(old);
        return 0;
    }
    const auto bytes = static_cast<std::size_t>(frame.gpr[2]);
    if (bytes > MaxGuestHeapBytes - (dispatcher.guestAllocationBytes_ - found->second)) return 0;
    const auto oldBytes = found->second;
    auto* resized = std::realloc(old, bytes);
    if (!resized) return 0;
    auto node = dispatcher.guestAllocations_.extract(found);
    node.key() = resized;
    node.mapped() = bytes;
    dispatcher.guestAllocations_.insert(std::move(node));
    dispatcher.guestAllocationBytes_ = dispatcher.guestAllocationBytes_ - oldBytes + bytes;
    return reinterpret_cast<std::uint64_t>(resized);
}

std::uint64_t HleDispatcher::MspaceFree(HleDispatcher& dispatcher,
                                        GuestCallFrame const& frame) noexcept {
    if (frame.gpr[0] != 0) {
        std::scoped_lock lock(dispatcher.allocationsMutex_);
        auto found = dispatcher.guestMspaces_.find(frame.gpr[0]);
        if (found == dispatcher.guestMspaces_.end()) return 0;
        auto old = found->second.allocations.find(frame.gpr[1]);
        if (old == found->second.allocations.end()) return 0;
        try { found->second.freeBlocks.push_back({old->first, old->second}); }
        catch (...) { return 0; }
        found->second.inUse -= old->second;
        found->second.allocations.erase(old);
        return 0;
    }
    if (frame.gpr[0] != 0 || frame.gpr[1] == 0) return 0;
    auto* allocation = reinterpret_cast<void*>(frame.gpr[1]);
    std::scoped_lock lock(dispatcher.allocationsMutex_);
    auto found = dispatcher.guestAllocations_.find(allocation);
    if (found == dispatcher.guestAllocations_.end()) return 0;
    dispatcher.guestAllocationBytes_ -= found->second;
    dispatcher.guestAllocations_.erase(found);
    std::free(allocation);
    return 0;
}

std::uint64_t HleDispatcher::MspaceUsableSize(HleDispatcher& dispatcher,
                                              GuestCallFrame const& frame) noexcept {
    if (frame.gpr[0] != 0) {
        std::scoped_lock lock(dispatcher.allocationsMutex_);
        auto found = dispatcher.guestMspaces_.find(frame.gpr[0]);
        if (found == dispatcher.guestMspaces_.end()) return 0;
        auto allocation = found->second.allocations.find(frame.gpr[1]);
        return allocation == found->second.allocations.end() ? 0 : allocation->second;
    }
    if (frame.gpr[0] != 0 || frame.gpr[1] == 0) return 0;
    std::scoped_lock lock(dispatcher.allocationsMutex_);
    auto found = dispatcher.guestAllocations_.find(reinterpret_cast<void*>(frame.gpr[1]));
    return found == dispatcher.guestAllocations_.end() ? 0 : found->second;
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

std::uint64_t HleDispatcher::KernelReserveVirtualRange(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEnomem = 0x8002000Cull;
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    if (!dispatcher.memory_ || frame.gpr[0] == 0 || frame.gpr[1] == 0 ||
        (frame.gpr[1] & 0x3fffull) != 0 || (frame.gpr[2] & ~0x10ull) != 0)
        return OrbisEinval;
    auto* output = WritablePointer(dispatcher, frame, frame.gpr[0], sizeof(std::uint64_t));
    if (!output) return OrbisEfault;
    std::uint64_t requested{};
    std::memcpy(&requested, output, sizeof(requested));
    std::uint64_t address{};
    if (!dispatcher.memory_->ReserveVirtualRange(frame.gpr[1], requested,
        (frame.gpr[2] & 0x10ull) != 0, frame.gpr[3], address))
        return OrbisEnomem;
    std::memcpy(output, &address, sizeof(address));
    return 0;
}

std::uint64_t HleDispatcher::KernelMapNamedSystemFlexibleMemory(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEnomem = 0x8002000Cull;
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    if (!dispatcher.memory_ || frame.gpr[1] == 0 || (frame.gpr[1] & 0x3fffull) ||
        (frame.gpr[2] & ~0x3ull) || frame.gpr[2] == 0 ||
        (frame.gpr[3] & ~0x10ull)) return OrbisEinval;
    auto* output = WritablePointer(dispatcher, frame, frame.gpr[0], sizeof(std::uint64_t));
    if (!output || !ReadablePointer(dispatcher, frame, frame.gpr[4], 1)) return OrbisEfault;
    std::uint64_t address{};
    std::memcpy(&address, output, sizeof(address));
    if (!(frame.gpr[3] & 0x10ull) || !address ||
        !dispatcher.memory_->MapLazySystem(address, frame.gpr[1], frame.gpr[2]))
        return OrbisEnomem;
    return 0;
}

std::uint64_t HleDispatcher::MsgDialogInitialize(
    HleDispatcher& dispatcher, GuestCallFrame const&) noexcept {
    std::scoped_lock lock(dispatcher.dialogMutex_);
    if (dispatcher.dialog_.status != 0) return 0x80B80004ull;
    dispatcher.dialog_.status = 1;
    dispatcher.dialog_.message.clear();
    return 0;
}

std::uint64_t HleDispatcher::MsgDialogTerminate(
    HleDispatcher& dispatcher, GuestCallFrame const&) noexcept {
    std::scoped_lock lock(dispatcher.dialogMutex_);
    if (dispatcher.dialog_.status == 0) return 0x80B80003ull;
    dispatcher.dialog_.status = 0;
    dispatcher.dialog_.message.clear();
    return 0;
}

std::uint64_t HleDispatcher::MsgDialogOpen(HleDispatcher& dispatcher,
                                           GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t InvalidState = 0x80B80006ull;
    constexpr std::uint64_t NullArgument = 0x80B8000Dull;
    {
        std::scoped_lock lock(dispatcher.dialogMutex_);
        if (dispatcher.dialog_.status != 1 && dispatcher.dialog_.status != 3)
            return InvalidState;
    }
    // OrbisParam: BaseParam[48], size[8], mode[4], padding[4], then
    // pointers to user/progress/system parameter blocks at offsets 64/72/80.
    auto* param = static_cast<std::uint8_t const*>(
        ReadablePointer(dispatcher, frame, frame.gpr[0], 88));
    if (!param) return NullArgument;
    std::uint32_t mode{};
    std::memcpy(&mode, param + 56, sizeof(mode));
    if (mode < 1 || mode > 3) return 0x80B8000Aull;
    std::uint64_t block{};
    std::memcpy(&block, param + (mode == 1 ? 64 : mode == 2 ? 72 : 80), sizeof(block));
    std::string message;
    if ((mode == 1 || mode == 2) && block <= UINT64_MAX - 16) {
        auto* textPointer = static_cast<std::uint8_t const*>(
            ReadablePointer(dispatcher, frame, block, 16));
        std::uint64_t textAddress{};
        if (textPointer) std::memcpy(&textAddress, textPointer + 8, sizeof(textAddress));
        if (textAddress) {
            try {
                for (std::size_t i = 0; i < 512 && textAddress <= UINT64_MAX - i; ++i) {
                    auto* character = static_cast<char const*>(
                        ReadablePointer(dispatcher, frame, textAddress + i, 1));
                    if (!character || !*character) break;
                    if (static_cast<unsigned char>(*character) >= 0x20 ||
                        *character == '\n') message.push_back(*character);
                }
            } catch (...) { message.clear(); }
        }
    }
    try {
        if (message.empty()) message = "O conteúdo abriu uma caixa de diálogo.";
        std::scoped_lock lock(dispatcher.dialogMutex_);
        dispatcher.dialog_.status = 2;
        dispatcher.dialog_.mode = mode;
        ++dispatcher.dialog_.generation;
        dispatcher.dialog_.message = std::move(message);
        dispatcher.dialogCanceled_ = false;
    } catch (...) { return 0x80B80009ull; }
    return 0;
}

std::uint64_t HleDispatcher::MsgDialogStatus(
    HleDispatcher& dispatcher, GuestCallFrame const&) noexcept {
    std::uint32_t status{};
    {
        std::scoped_lock lock(dispatcher.dialogMutex_);
        status = dispatcher.dialog_.status;
    }
    if (status == 2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return status;
}

std::uint64_t HleDispatcher::MsgDialogClose(
    HleDispatcher& dispatcher, GuestCallFrame const&) noexcept {
    std::scoped_lock lock(dispatcher.dialogMutex_);
    if (dispatcher.dialog_.status != 2) return 0x80B8000Bull;
    dispatcher.dialog_.status = 3;
    dispatcher.dialogCanceled_ = true;
    return 0;
}

std::uint64_t HleDispatcher::MsgDialogGetResult(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    std::uint32_t result{};
    std::uint32_t button{1};
    {
        std::scoped_lock lock(dispatcher.dialogMutex_);
        if (dispatcher.dialog_.status != 3) return 0x80B80005ull;
        result = dispatcher.dialogCanceled_ ? 1u : 0u;
        button = dispatcher.dialogCanceled_ ? 0u : 1u;
    }
    auto* output = WritablePointer(dispatcher, frame, frame.gpr[0], 44);
    if (!output) return 0x80B8000Dull;
    std::array<std::uint8_t, 44> data{};
    std::memcpy(data.data() + 4, &result, sizeof(result));
    std::memcpy(data.data() + 8, &button, sizeof(button));
    std::memcpy(output, data.data(), data.size());
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
    auto* output = static_cast<Timespec*>(WritablePointer(
        dispatcher, frame, frame.gpr[1], sizeof(Timespec)));
    if (!output) return OrbisEfault;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);
    value.seconds = seconds.count();
    value.nanoseconds = nanos.count();
    std::memcpy(output, &value, sizeof(value));
    return 0;
}

std::uint64_t HleDispatcher::SignalAction(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t Failure = UINT64_MAX;
    const auto signal = static_cast<std::int32_t>(frame.gpr[0]);
    if (signal < 1 || signal > 128 || signal == 9 || signal == 17 || signal == 32) {
        GuestPosixErrno = 22;
        return Failure;
    }
    auto const* action = frame.gpr[1] == 0
        ? nullptr
        : static_cast<GuestSignalAction const*>(ReadablePointer(
              dispatcher, frame, frame.gpr[1], sizeof(GuestSignalAction)));
    auto* previous = frame.gpr[2] == 0
        ? nullptr
        : static_cast<GuestSignalAction*>(WritablePointer(
              dispatcher, frame, frame.gpr[2], sizeof(GuestSignalAction)));
    if ((frame.gpr[1] != 0 && !action) || (frame.gpr[2] != 0 && !previous)) {
        GuestPosixErrno = 14;
        return Failure;
    }
    const auto index = static_cast<std::size_t>(signal - 1);
    std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
    const auto old = dispatcher.signalActions_[index];
    if (action) dispatcher.signalActions_[index] = *action;
    if (previous) *previous = old;
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
        WritablePointer(dispatcher, frame, frame.gpr[0], sizeof(std::int32_t)));
    if (!output) return InvalidArgument;
    *output = 1;
    return 0;
}

std::uint64_t HleDispatcher::UserServiceGetLoginUsers(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t InvalidArgument = 0x80960005ull;
    constexpr std::int32_t users[4] = {1, -1, -1, -1};
    if (!dispatcher.memory_) return InvalidArgument;
    auto* output = WritablePointer(dispatcher, frame, frame.gpr[0], sizeof(users));
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
    auto* output = WritablePointer(dispatcher, frame,
        frame.gpr[1], static_cast<std::size_t>(frame.gpr[2]));
    if (!output) return InvalidArgument;
    std::memcpy(output, name, sizeof(name));
    return 0;
}

std::uint64_t HleDispatcher::UserServiceGetRegisteredUsers(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    // Registered-user ABI has 16 slots, unlike the 4-slot login-user list.
    std::array<std::int32_t, 16> users;
    users.fill(-1); users[0] = 1;
    auto* output = WritablePointer(dispatcher, frame, frame.gpr[0], sizeof(users));
    if (!output) return 0x80960005ull;
    std::memcpy(output, users.data(), sizeof(users));
    return 0;
}

std::uint64_t HleDispatcher::UserServiceGetNpAccountId(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* output = static_cast<std::uint64_t*>(WritablePointer(dispatcher, frame, frame.gpr[1], 8));
    if (!output) return 0x80960005ull;
    *output = 0;
    // The local virtual user has no PSN identity. Do not report a successful
    // lookup while leaving guest output memory uninitialized.
    return 0x80960006ull;
}

std::uint64_t HleDispatcher::SystemServiceParamGetInt(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t ParameterError = 0x80A10003ull;
    if (!dispatcher.memory_) return ParameterError;
    auto* output = static_cast<std::int32_t*>(
        WritablePointer(dispatcher, frame, frame.gpr[1], sizeof(std::int32_t)));
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
        if (auto alias = SandboxAppPath(guestPath)) return ResolveGuestPath(*alias, write, hostPath);
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
        if (relativePath.is_absolute() || relativePath.has_root_name() || relative.find(':') != std::string::npos) return false;
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
        const auto access = flags & 0x3u;
        if (access == 0x3u) return OrbisEinval;
        const bool readable = access != 0x1u;
        const bool writable = access != 0x0u;
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
        file.path = hostPath;
        file.readable = readable;
        file.writable = writable;
        file.directory = std::filesystem::is_directory(hostPath);
        if ((flags & 0x20000u) != 0 && !file.directory) return OrbisEinval;
        if (file.directory) {
            if (writable) return OrbisEacces;
            for (auto const& entry : std::filesystem::directory_iterator(hostPath)) {
                const auto name = entry.path().filename().u8string();
                if (name.size() <= 255)
                    file.directoryEntries.push_back(
                        {std::string(name.begin(), name.end()), entry.is_directory()});
            }
            const auto descriptor = dispatcher.nextFileDescriptor_++;
            dispatcher.files_.emplace(descriptor, std::move(file));
            return static_cast<std::uint64_t>(descriptor);
        }
        file.stream.open(hostPath, mode);
        if (!file.stream) return OrbisEinval;
        const auto descriptor = dispatcher.nextFileDescriptor_++;
        dispatcher.files_.emplace(descriptor, std::move(file));
        return static_cast<std::uint64_t>(descriptor);
    } catch (...) {
        return OrbisEinval;
    }
}

std::uint64_t HleDispatcher::PosixOpen(HleDispatcher& dispatcher,
                                       GuestCallFrame const& frame) noexcept {
    constexpr std::int32_t PosixEnetunreach = 51;
    try {
        std::string guestPath;
        if (dispatcher.ReadGuestString(frame.gpr[0], guestPath) &&
            guestPath == "/data/apollo/cache/ver.check" &&
            (static_cast<std::uint32_t>(frame.gpr[1]) & 0x3u) != 0) {
            // Apollo's default startup path checks GitHub for updates before
            // entering the main menu. Network calls are not implemented in
            // UWP yet, so use an ordinary offline POSIX error and let Apollo
            // take its own "update check failed" return path.
            GuestPosixErrno = PosixEnetunreach;
            dispatcher.GraphicsLog(
                "Apollo: verificação automática de atualização ignorada (sem rede HLE)");
            return UINT64_MAX;
        }
    } catch (...) {
        GuestPosixErrno = 12; // POSIX ENOMEM
        return UINT64_MAX;
    }
    return KernelOpen(dispatcher, frame);
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
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    constexpr std::size_t MaxTransfer = 16u * 1024u * 1024u;
    if (frame.gpr[2] > MaxTransfer) return OrbisEinval;
    const auto size = static_cast<std::size_t>(frame.gpr[2]);
    auto found = dispatcher.files_.find(static_cast<std::int32_t>(frame.gpr[0]));
    if (found == dispatcher.files_.end() || found->second.directory ||
        !found->second.readable)
        return OrbisEbadf;
    auto* output = WritablePointer(dispatcher, frame, frame.gpr[1], size);
    if (size != 0 && !output) return OrbisEfault;
    found->second.stream.read(static_cast<char*>(output), static_cast<std::streamsize>(size));
    return static_cast<std::uint64_t>(found->second.stream.gcount());
}

std::uint64_t HleDispatcher::KernelReadv(HleDispatcher& dispatcher,
                                         GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEbadf = 0x80020009ull;
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    constexpr std::uint64_t OrbisEnomem = 0x8002000Cull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    constexpr std::size_t MaxVectors = 1024;
    constexpr std::size_t MaxTransfer = 16u * 1024u * 1024u;
    struct GuestIovec {
        std::uint64_t base;
        std::uint64_t length;
    };
    static_assert(sizeof(GuestIovec) == 16);

    const auto count = frame.gpr[2];
    if (count > MaxVectors) return OrbisEinval;
    if (count == 0) return 0;
    const auto descriptor = static_cast<std::int32_t>(frame.gpr[0]);
    auto file = dispatcher.files_.find(descriptor);
    if (file == dispatcher.files_.end() || file->second.directory ||
        !file->second.readable)
        return OrbisEbadf;

    auto const* vectors = static_cast<GuestIovec const*>(ReadablePointer(
        dispatcher, frame, frame.gpr[1],
        static_cast<std::size_t>(count) * sizeof(GuestIovec)));
    if (!vectors) {
        dispatcher.GraphicsLog("HLE _readv EFAULT: vetor iovec inválido; fd=" +
                               std::to_string(descriptor) + " address=" +
                               std::to_string(frame.gpr[1]) + " count=" +
                               std::to_string(count));
        return OrbisEfault;
    }

    try {
        std::vector<std::pair<void*, std::size_t>> parts;
        parts.reserve(static_cast<std::size_t>(count));
        std::size_t total{};
        for (std::size_t index = 0; index < count; ++index) {
            GuestIovec vector{};
            std::memcpy(&vector, vectors + index, sizeof(vector));
            if (vector.length > MaxTransfer - total) return OrbisEinval;
            const auto length = static_cast<std::size_t>(vector.length);
            auto* output = WritablePointer(dispatcher, frame, vector.base, length);
            if (length != 0 && !output) {
                dispatcher.GraphicsLog("HLE _readv EFAULT: buffer de destino inválido; fd=" +
                                       std::to_string(descriptor) + " iov=" +
                                       std::to_string(index) + " address=" +
                                       std::to_string(vector.base) + " bytes=" +
                                       std::to_string(length));
                return OrbisEfault;
            }
            parts.emplace_back(output, length);
            total += length;
        }

        std::size_t transferred{};
        for (auto const& [output, length] : parts) {
            if (length == 0) continue;
            file->second.stream.read(static_cast<char*>(output),
                                     static_cast<std::streamsize>(length));
            const auto read = static_cast<std::size_t>(file->second.stream.gcount());
            transferred += read;
            if (read != length) break;
        }
        return static_cast<std::uint64_t>(transferred);
    } catch (...) {
        return OrbisEnomem;
    }
}

std::uint64_t HleDispatcher::KernelWrite(HleDispatcher& dispatcher,
                                         GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEbadf = 0x80020009ull;
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    const auto size = static_cast<std::size_t>(frame.gpr[2]);
    if (size > 16u * 1024u * 1024u) return OrbisEinval;
    auto* input = ReadablePointer(dispatcher, frame, frame.gpr[1], size);
    if (size != 0 && !input) return OrbisEfault;
    const auto descriptor = static_cast<std::int32_t>(frame.gpr[0]);
    if (descriptor >= 0 && descriptor <= 2) {
        dispatcher.AppendConsole(input, size);
        return frame.gpr[2];
    }
    auto found = dispatcher.files_.find(descriptor);
    if (found == dispatcher.files_.end() || found->second.directory ||
        !found->second.writable)
        return OrbisEbadf;
    found->second.stream.write(static_cast<char const*>(input),
                               static_cast<std::streamsize>(size));
    return found->second.stream ? frame.gpr[2] : OrbisEbadf;
}

std::uint64_t HleDispatcher::KernelWritev(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEbadf = 0x80020009ull;
    constexpr std::uint64_t OrbisEfault = 0x8002000Eull;
    constexpr std::uint64_t OrbisEnomem = 0x8002000Cull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    constexpr std::size_t MaxVectors = 1024;
    constexpr std::size_t MaxTransfer = 16u * 1024u * 1024u;
    struct GuestIovec {
        std::uint64_t base;
        std::uint64_t length;
    };
    static_assert(sizeof(GuestIovec) == 16);
    const auto count = frame.gpr[2];
    if (count > MaxVectors) return OrbisEinval;
    if (count == 0) return 0;
    auto const* vectors = static_cast<GuestIovec const*>(ReadablePointer(
        dispatcher, frame, frame.gpr[1],
        static_cast<std::size_t>(count) * sizeof(GuestIovec)));
    if (!vectors) return OrbisEfault;
    const auto descriptor = static_cast<std::int32_t>(frame.gpr[0]);
    const bool console = descriptor >= 0 && descriptor <= 2;
    auto file = dispatcher.files_.find(descriptor);
    if (!console && (file == dispatcher.files_.end() || file->second.directory ||
                     !file->second.writable))
        return OrbisEbadf;
    try {
        std::vector<std::pair<void const*, std::size_t>> parts;
        parts.reserve(static_cast<std::size_t>(count));
        std::size_t total{};
        for (std::size_t index = 0; index < count; ++index) {
            GuestIovec vector{};
            std::memcpy(&vector, vectors + index, sizeof(vector));
            if (vector.length > MaxTransfer - total) return OrbisEinval;
            const auto length = static_cast<std::size_t>(vector.length);
            auto const* input = ReadablePointer(dispatcher, frame, vector.base, length);
            if (length != 0 && !input) return OrbisEfault;
            parts.emplace_back(input, length);
            total += length;
        }
        for (auto const& [input, length] : parts) {
            if (length == 0) continue;
            if (console) dispatcher.AppendConsole(input, length);
            else {
                file->second.stream.write(static_cast<char const*>(input),
                                          static_cast<std::streamsize>(length));
                if (!file->second.stream) return OrbisEbadf;
            }
        }
        return static_cast<std::uint64_t>(total);
    } catch (...) {
        return OrbisEnomem;
    }
}

std::uint64_t HleDispatcher::KernelLseek(HleDispatcher& dispatcher,
                                         GuestCallFrame const& frame) noexcept {
    constexpr std::uint64_t OrbisEbadf = 0x80020009ull;
    constexpr std::uint64_t OrbisEinval = 0x80020016ull;
    auto found = dispatcher.files_.find(static_cast<std::int32_t>(frame.gpr[0]));
    if (found == dispatcher.files_.end()) return OrbisEbadf;
    if (found->second.directory) {
        if (frame.gpr[2] != 0 || frame.gpr[1] != 0) return OrbisEinval;
        found->second.directoryIndex = 0;
        return 0;
    }
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
    if (found->second.directory) return 0;
    found->second.stream.flush();
    return found->second.stream ? 0 : OrbisEbadf;
}

std::uint64_t HleDispatcher::FileAccess(HleDispatcher& dispatcher,
                                        GuestCallFrame const& frame) noexcept {
    try {
        std::string guestPath;
        if (!dispatcher.ReadGuestString(frame.gpr[0], guestPath)) return UINT64_MAX;
        std::filesystem::path hostPath;
        const bool write = (frame.gpr[1] & 0x2u) != 0;
        if (!dispatcher.ResolveGuestPath(guestPath, write, hostPath)) return UINT64_MAX;
        return std::filesystem::exists(hostPath) ? 0 : UINT64_MAX;
    } catch (...) {
        return UINT64_MAX;
    }
}

std::uint64_t HleDispatcher::FileMkdir(HleDispatcher& dispatcher,
                                       GuestCallFrame const& frame) noexcept {
    try {
        std::string guestPath;
        if (!dispatcher.ReadGuestString(frame.gpr[0], guestPath)) return UINT64_MAX;
        std::filesystem::path hostPath;
        if (!dispatcher.ResolveGuestPath(guestPath, true, hostPath)) return UINT64_MAX;
        std::error_code error;
        if (std::filesystem::exists(hostPath, error))
            return std::filesystem::is_directory(hostPath, error) ? 0 : UINT64_MAX;
        return std::filesystem::create_directories(hostPath, error) && !error ? 0
                                                                             : UINT64_MAX;
    } catch (...) {
        return UINT64_MAX;
    }
}

std::uint64_t HleDispatcher::FileRmdir(HleDispatcher& dispatcher,
                                       GuestCallFrame const& frame) noexcept {
    try {
        std::string guestPath;
        if (!dispatcher.ReadGuestString(frame.gpr[0], guestPath)) return UINT64_MAX;
        std::filesystem::path hostPath;
        if (!dispatcher.ResolveGuestPath(guestPath, true, hostPath)) return UINT64_MAX;
        std::error_code error;
        return std::filesystem::remove(hostPath, error) && !error ? 0 : UINT64_MAX;
    } catch (...) {
        return UINT64_MAX;
    }
}

std::uint64_t HleDispatcher::FileRename(HleDispatcher& dispatcher,
                                        GuestCallFrame const& frame) noexcept {
    try {
        std::string sourceGuest;
        std::string targetGuest;
        if (!dispatcher.ReadGuestString(frame.gpr[0], sourceGuest) ||
            !dispatcher.ReadGuestString(frame.gpr[1], targetGuest))
            return UINT64_MAX;
        std::filesystem::path source;
        std::filesystem::path target;
        if (!dispatcher.ResolveGuestPath(sourceGuest, true, source) ||
            !dispatcher.ResolveGuestPath(targetGuest, true, target))
            return UINT64_MAX;
        std::filesystem::create_directories(target.parent_path());
        std::error_code error;
        std::filesystem::rename(source, target, error);
        return error ? UINT64_MAX : 0;
    } catch (...) {
        return UINT64_MAX;
    }
}

std::uint64_t HleDispatcher::FileUnlink(HleDispatcher& dispatcher,
                                        GuestCallFrame const& frame) noexcept {
    try {
        std::string guestPath;
        if (!dispatcher.ReadGuestString(frame.gpr[0], guestPath)) return UINT64_MAX;
        std::filesystem::path hostPath;
        if (!dispatcher.ResolveGuestPath(guestPath, true, hostPath)) return UINT64_MAX;
        std::error_code error;
        if (std::filesystem::is_directory(hostPath, error)) return UINT64_MAX;
        return std::filesystem::remove(hostPath, error) && !error ? 0 : UINT64_MAX;
    } catch (...) {
        return UINT64_MAX;
    }
}

std::uint64_t HleDispatcher::FileChmod(HleDispatcher& dispatcher,
                                       GuestCallFrame const& frame) noexcept {
    std::string guestPath;
    if (!dispatcher.ReadGuestString(frame.gpr[0], guestPath)) return UINT64_MAX;
    std::filesystem::path hostPath;
    if (!dispatcher.ResolveGuestPath(guestPath, true, hostPath)) return UINT64_MAX;
    return std::filesystem::exists(hostPath) ? 0 : UINT64_MAX;
}

std::uint64_t HleDispatcher::FileFlock(HleDispatcher& dispatcher,
                                       GuestCallFrame const& frame) noexcept {
    return dispatcher.files_.contains(static_cast<std::int32_t>(frame.gpr[0])) ? 0
                                                                               : UINT64_MAX;
}

std::uint64_t HleDispatcher::FileStat(HleDispatcher& dispatcher,
                                      GuestCallFrame const& frame) noexcept {
    try {
        std::string guestPath;
        if (!dispatcher.ReadGuestString(frame.gpr[0], guestPath)) return UINT64_MAX;
        std::filesystem::path hostPath;
        if (!dispatcher.ResolveGuestPath(guestPath, false, hostPath)) return UINT64_MAX;
        auto* output = dispatcher.memory_
                           ? static_cast<OrbisStat*>(dispatcher.memory_->TranslateWritable(
                                 frame.gpr[1], sizeof(OrbisStat)))
                           : nullptr;
        if (!output) return UINT64_MAX;
        OrbisStat value{};
        if (!PopulateStat(hostPath, value)) return UINT64_MAX;
        std::memcpy(output, &value, sizeof(value));
        return 0;
    } catch (...) {
        return UINT64_MAX;
    }
}

std::uint64_t HleDispatcher::FileFstat(HleDispatcher& dispatcher,
                                       GuestCallFrame const& frame) noexcept {
    try {
        const auto found = dispatcher.files_.find(static_cast<std::int32_t>(frame.gpr[0]));
        if (found == dispatcher.files_.end()) return UINT64_MAX;
        auto* output = dispatcher.memory_
                           ? static_cast<OrbisStat*>(dispatcher.memory_->TranslateWritable(
                                 frame.gpr[1], sizeof(OrbisStat)))
                           : nullptr;
        if (!output) return UINT64_MAX;
        found->second.stream.flush();
        OrbisStat value{};
        if (!PopulateStat(found->second.path, value)) return UINT64_MAX;
        std::memcpy(output, &value, sizeof(value));
        return 0;
    } catch (...) {
        return UINT64_MAX;
    }
}

std::uint64_t HleDispatcher::FileFtruncate(HleDispatcher& dispatcher,
                                           GuestCallFrame const& frame) noexcept {
    try {
        auto found = dispatcher.files_.find(static_cast<std::int32_t>(frame.gpr[0]));
        const auto length = static_cast<std::int64_t>(frame.gpr[1]);
        if (found == dispatcher.files_.end() || !found->second.writable || length < 0)
            return UINT64_MAX;
        auto& file = found->second;
        file.stream.flush();
        file.stream.close();
        std::error_code error;
        std::filesystem::resize_file(file.path, static_cast<std::uintmax_t>(length), error);
        file.stream.open(file.path, std::ios::binary | std::ios::in | std::ios::out);
        return !error && file.stream ? 0 : UINT64_MAX;
    } catch (...) {
        return UINT64_MAX;
    }
}

std::uint64_t HleDispatcher::FileGetdents(HleDispatcher& dispatcher,
                                          GuestCallFrame const& frame) noexcept {
    try {
        auto found = dispatcher.files_.find(static_cast<std::int32_t>(frame.gpr[0]));
        if (found == dispatcher.files_.end() || !found->second.directory)
            return UINT64_MAX;
        const auto capacity = static_cast<std::size_t>(frame.gpr[2]);
        if (capacity < 512) return UINT64_MAX;
        auto* output = dispatcher.memory_
                           ? static_cast<std::uint8_t*>(
                                 dispatcher.memory_->TranslateWritable(frame.gpr[1], capacity))
                           : nullptr;
        if (capacity != 0 && !output) return UINT64_MAX;
        std::size_t written = 0;
        auto& directory = found->second;
        while (directory.directoryIndex < directory.directoryEntries.size()) {
            auto const& entry = directory.directoryEntries[directory.directoryIndex];
            const auto rawLength = sizeof(OrbisDirentHeader) + entry.name.size() + 1;
            const auto recordLength = (rawLength + 3u) & ~std::size_t{3u};
            if (recordLength > capacity - written) break;
            OrbisDirentHeader header{};
            header.fileNumber = static_cast<std::uint32_t>(directory.directoryIndex + 1);
            header.recordLength = static_cast<std::uint16_t>(recordLength);
            header.type = entry.directory ? 4u : 8u;
            header.nameLength = static_cast<std::uint8_t>(entry.name.size());
            std::memcpy(output + written, &header, sizeof(header));
            std::memcpy(output + written + sizeof(header), entry.name.c_str(),
                        entry.name.size() + 1);
            std::memset(output + written + rawLength, 0, recordLength - rawLength);
            written += recordLength;
            ++directory.directoryIndex;
        }
        return static_cast<std::uint64_t>(written);
    } catch (...) {
        return UINT64_MAX;
    }
}

std::uint64_t HleDispatcher::PthreadMutexAttrInit(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* slot = static_cast<std::uint64_t*>(WritablePointer(
        dispatcher, frame, frame.gpr[0], sizeof(std::uint64_t)));
    if (!slot) return 22;
    try {
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        dispatcher.mutexAttributes_[frame.gpr[0]] = 1;
        *slot = frame.gpr[0];
        return 0;
    } catch (...) {
        return 12;
    }
}

std::uint64_t HleDispatcher::PthreadMutexAttrSetType(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    if (frame.gpr[1] < 1 || frame.gpr[1] > 4) return 22;
    std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
    auto found = dispatcher.mutexAttributes_.find(frame.gpr[0]);
    if (found == dispatcher.mutexAttributes_.end()) return 22;
    found->second = static_cast<std::uint32_t>(frame.gpr[1]);
    return 0;
}

std::uint64_t HleDispatcher::PthreadMutexInit(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* slot = static_cast<std::uint64_t*>(WritablePointer(
        dispatcher, frame, frame.gpr[0], sizeof(std::uint64_t)));
    if (!slot) return 22;
    try {
        auto mutex = std::make_shared<GuestMutex>();
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        if (frame.gpr[1] != 0) {
            const auto attribute = dispatcher.mutexAttributes_.find(frame.gpr[1]);
            if (attribute == dispatcher.mutexAttributes_.end()) return 22;
            mutex->recursive = attribute->second == 2;
        }
        dispatcher.mutexes_[frame.gpr[0]] = std::move(mutex);
        *slot = frame.gpr[0];
        return 0;
    } catch (...) {
        return 12;
    }
}

std::uint64_t HleDispatcher::PthreadMutexDestroy(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* slot = dispatcher.memory_
                     ? static_cast<std::uint64_t*>(dispatcher.memory_->TranslateWritable(
                           frame.gpr[0], sizeof(std::uint64_t)))
                     : nullptr;
    if (!slot) return 22;
    std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
    const auto found = dispatcher.mutexes_.find(frame.gpr[0]);
    if (found != dispatcher.mutexes_.end() && found->second->isLocked()) return 16;
    dispatcher.mutexes_.erase(frame.gpr[0]);
    *slot = 2;
    return 0;
}

std::uint64_t HleDispatcher::PthreadMutexLock(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    try {
        std::shared_ptr<GuestMutex> mutex;
        {
            std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
            auto found = dispatcher.mutexes_.find(frame.gpr[0]);
            if (found == dispatcher.mutexes_.end()) {
                auto* slot = dispatcher.memory_
                                 ? static_cast<std::uint64_t*>(
                                       dispatcher.memory_->TranslateWritable(
                                           frame.gpr[0], sizeof(std::uint64_t)))
                                 : nullptr;
                if (!slot || *slot == 2) return 22;
                mutex = std::make_shared<GuestMutex>();
                dispatcher.mutexes_[frame.gpr[0]] = mutex;
                *slot = frame.gpr[0];
            } else {
                mutex = found->second;
            }
        }
        if (mutex->ownedByCurrentThread() && !mutex->recursive) return 35;
        mutex->lock();
        return 0;
    } catch (...) {
        return 22;
    }
}

std::uint64_t HleDispatcher::PthreadMutexTryLock(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    try {
        std::shared_ptr<GuestMutex> mutex;
        {
            std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
            auto found = dispatcher.mutexes_.find(frame.gpr[0]);
            if (found == dispatcher.mutexes_.end()) return 22;
            mutex = found->second;
        }
        if (mutex->ownedByCurrentThread() && !mutex->recursive) return 16;
        return mutex->try_lock() ? 0 : 16;
    } catch (...) {
        return 22;
    }
}

std::uint64_t HleDispatcher::PthreadMutexUnlock(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    try {
        std::shared_ptr<GuestMutex> mutex;
        {
            std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
            auto found = dispatcher.mutexes_.find(frame.gpr[0]);
            if (found == dispatcher.mutexes_.end()) return 22;
            mutex = found->second;
        }
        mutex->unlock();
        return 0;
    } catch (...) {
        return 1;
    }
}

std::uint64_t HleDispatcher::PthreadCondInit(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* slot = dispatcher.memory_
                     ? static_cast<std::uint64_t*>(dispatcher.memory_->TranslateWritable(
                           frame.gpr[0], sizeof(std::uint64_t)))
                     : nullptr;
    if (!slot) return 22;
    try {
        auto condition = std::make_shared<GuestCondition>();
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        dispatcher.conditions_[frame.gpr[0]] = std::move(condition);
        *slot = frame.gpr[0];
        return 0;
    } catch (...) {
        return 12;
    }
}

std::uint64_t HleDispatcher::PthreadCondDestroy(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* slot = dispatcher.memory_
                     ? static_cast<std::uint64_t*>(dispatcher.memory_->TranslateWritable(
                           frame.gpr[0], sizeof(std::uint64_t)))
                     : nullptr;
    if (!slot) return 22;
    std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
    dispatcher.conditions_.erase(frame.gpr[0]);
    *slot = 2;
    return 0;
}

std::uint64_t HleDispatcher::PthreadCondWait(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    try {
        std::shared_ptr<GuestCondition> condition;
        std::shared_ptr<GuestMutex> mutex;
        {
            std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
            auto foundCondition = dispatcher.conditions_.find(frame.gpr[0]);
            auto foundMutex = dispatcher.mutexes_.find(frame.gpr[1]);
            if (foundCondition == dispatcher.conditions_.end() ||
                foundMutex == dispatcher.mutexes_.end())
                return 22;
            condition = foundCondition->second;
            mutex = foundMutex->second;
        }
        if (!mutex->ownedByCurrentThread()) return 1;
        std::unique_lock<GuestMutex> lock(*mutex, std::adopt_lock);
        condition->primitive.wait(lock);
        lock.release();
        return 0;
    } catch (...) {
        return 22;
    }
}

std::uint64_t HleDispatcher::PthreadCondSignal(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    std::shared_ptr<GuestCondition> condition;
    {
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        auto found = dispatcher.conditions_.find(frame.gpr[0]);
        if (found == dispatcher.conditions_.end()) return 22;
        condition = found->second;
    }
    condition->primitive.notify_one();
    return 0;
}

std::uint64_t HleDispatcher::PthreadCondBroadcast(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    std::shared_ptr<GuestCondition> condition;
    {
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        auto found = dispatcher.conditions_.find(frame.gpr[0]);
        if (found == dispatcher.conditions_.end()) return 22;
        condition = found->second;
    }
    condition->primitive.notify_all();
    return 0;
}

std::uint64_t HleDispatcher::SemaphoreInit(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* slot = dispatcher.memory_
                     ? static_cast<std::uint64_t*>(dispatcher.memory_->TranslateWritable(
                           frame.gpr[0], sizeof(std::uint64_t)))
                     : nullptr;
    if (!slot || frame.gpr[2] > 32767) return UINT64_MAX;
    try {
        auto semaphore = std::make_shared<GuestSemaphore>();
        semaphore->value = static_cast<std::uint32_t>(frame.gpr[2]);
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        dispatcher.semaphores_[frame.gpr[0]] = std::move(semaphore);
        *slot = frame.gpr[0];
        return 0;
    } catch (...) {
        return UINT64_MAX;
    }
}

std::uint64_t HleDispatcher::SemaphoreDestroy(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* slot = dispatcher.memory_
                     ? static_cast<std::uint64_t*>(dispatcher.memory_->TranslateWritable(
                           frame.gpr[0], sizeof(std::uint64_t)))
                     : nullptr;
    if (!slot) return UINT64_MAX;
    std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
    if (dispatcher.semaphores_.erase(frame.gpr[0]) == 0) return UINT64_MAX;
    *slot = 0;
    return 0;
}

std::uint64_t HleDispatcher::SemaphoreTryWait(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    std::shared_ptr<GuestSemaphore> semaphore;
    {
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        auto found = dispatcher.semaphores_.find(frame.gpr[0]);
        if (found == dispatcher.semaphores_.end()) return UINT64_MAX;
        semaphore = found->second;
    }
    std::scoped_lock lock(semaphore->mutex);
    if (semaphore->value == 0) return UINT64_MAX;
    --semaphore->value;
    return 0;
}

std::uint64_t HleDispatcher::SemaphoreWait(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    std::shared_ptr<GuestSemaphore> semaphore;
    {
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        auto found = dispatcher.semaphores_.find(frame.gpr[0]);
        if (found == dispatcher.semaphores_.end()) return UINT64_MAX;
        semaphore = found->second;
    }
    std::unique_lock lock(semaphore->mutex);
    semaphore->condition.wait(lock, [&] { return semaphore->value != 0; });
    --semaphore->value;
    return 0;
}

std::uint64_t HleDispatcher::SemaphoreTimedWait(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto const* timeout = dispatcher.memory_
                              ? static_cast<OrbisTimespec const*>(
                                    dispatcher.memory_->Translate(frame.gpr[1],
                                                                  sizeof(OrbisTimespec)))
                              : nullptr;
    if (!timeout || timeout->seconds < 0 || timeout->nanoseconds < 0 ||
        timeout->nanoseconds >= 1'000'000'000)
        return UINT64_MAX;
    std::shared_ptr<GuestSemaphore> semaphore;
    {
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        auto found = dispatcher.semaphores_.find(frame.gpr[0]);
        if (found == dispatcher.semaphores_.end()) return UINT64_MAX;
        semaphore = found->second;
    }
    const auto deadline = std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds(timeout->seconds) +
            std::chrono::nanoseconds(timeout->nanoseconds))};
    std::unique_lock lock(semaphore->mutex);
    if (!semaphore->condition.wait_until(lock, deadline,
                                         [&] { return semaphore->value != 0; }))
        return UINT64_MAX;
    --semaphore->value;
    return 0;
}

std::uint64_t HleDispatcher::SemaphoreGetValue(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* output = dispatcher.memory_
                       ? static_cast<std::int32_t*>(dispatcher.memory_->TranslateWritable(
                             frame.gpr[1], sizeof(std::int32_t)))
                       : nullptr;
    if (!output) return UINT64_MAX;
    std::shared_ptr<GuestSemaphore> semaphore;
    {
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        auto found = dispatcher.semaphores_.find(frame.gpr[0]);
        if (found == dispatcher.semaphores_.end()) return UINT64_MAX;
        semaphore = found->second;
    }
    std::scoped_lock lock(semaphore->mutex);
    *output = static_cast<std::int32_t>(semaphore->value);
    return 0;
}

std::uint64_t HleDispatcher::SemaphorePost(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    std::shared_ptr<GuestSemaphore> semaphore;
    {
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        auto found = dispatcher.semaphores_.find(frame.gpr[0]);
        if (found == dispatcher.semaphores_.end()) return UINT64_MAX;
        semaphore = found->second;
    }
    {
        std::scoped_lock lock(semaphore->mutex);
        if (semaphore->value >= 32767) return UINT64_MAX;
        ++semaphore->value;
    }
    semaphore->condition.notify_one();
    return 0;
}

std::uint64_t HleDispatcher::PthreadAttrInit(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* slot = static_cast<std::uint64_t*>(WritablePointer(
        dispatcher, frame, frame.gpr[0], sizeof(std::uint64_t)));
    if (!slot) return 22;
    try {
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        dispatcher.threadAttributes_[frame.gpr[0]] = GuestThreadAttribute{};
        *slot = frame.gpr[0];
        return 0;
    } catch (...) {
        return 12;
    }
}

std::uint64_t HleDispatcher::PthreadAttrSetDetachState(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    if (frame.gpr[1] > 1) return 22;
    std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
    auto found = dispatcher.threadAttributes_.find(frame.gpr[0]);
    if (found == dispatcher.threadAttributes_.end()) return 22;
    found->second.detached = frame.gpr[1] != 0;
    return 0;
}

std::uint64_t HleDispatcher::PthreadAttrSetStackSize(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    if (frame.gpr[1] < 16 * 1024 || frame.gpr[1] > 64 * 1024 * 1024) return 22;
    std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
    auto found = dispatcher.threadAttributes_.find(frame.gpr[0]);
    if (found == dispatcher.threadAttributes_.end()) return 22;
    found->second.stackSize = static_cast<std::size_t>(frame.gpr[1]);
    return 0;
}

std::uint64_t HleDispatcher::PthreadCreate(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* output = static_cast<std::uint64_t*>(WritablePointer(
        dispatcher, frame, frame.gpr[0], sizeof(std::uint64_t)));
    if (!output || !dispatcher.memory_ ||
        !dispatcher.memory_->IsExecutable(frame.gpr[2]))
        return 22;
    try {
        GuestThreadAttribute attribute{};
        std::uint64_t identifier{};
        auto thread = std::make_shared<GuestThread>();
        {
            std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
            if (frame.gpr[1] != 0) {
                const auto found = dispatcher.threadAttributes_.find(frame.gpr[1]);
                if (found == dispatcher.threadAttributes_.end()) return 22;
                attribute = found->second;
            }
            identifier = dispatcher.nextThreadId_++;
            thread->detached = attribute.detached;
            dispatcher.threads_[identifier] = thread;
        }
        auto* entry = reinterpret_cast<void*>(frame.gpr[2]);
        const auto argument = frame.gpr[3];
        thread->native = std::thread([thread, entry, argument, identifier] {
            CurrentGuestThreadId = identifier;
            std::uint64_t result = UINT64_MAX;
            try {
                result = InvokeGuestSysv1(entry, argument);
            } catch (...) {
            }
            {
                std::scoped_lock lock(thread->state);
                thread->result = result;
                thread->finished = true;
            }
            thread->completed.notify_all();
        });
        *output = identifier;
        return 0;
    } catch (...) {
        return 11;
    }
}

std::uint64_t HleDispatcher::PthreadJoin(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    if (frame.gpr[0] == CurrentGuestThreadId) return 35;
    try {
        std::shared_ptr<GuestThread> thread;
        {
            std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
            auto found = dispatcher.threads_.find(frame.gpr[0]);
            if (found == dispatcher.threads_.end() || found->second->detached) return 22;
            thread = found->second;
        }
        std::uint64_t result{};
        {
            std::unique_lock lock(thread->state);
            thread->completed.wait(lock, [&] { return thread->finished; });
            result = thread->result;
        }
        if (thread->native.joinable()) thread->native.join();
        if (frame.gpr[1] != 0) {
            auto* output = dispatcher.memory_
                               ? static_cast<std::uint64_t*>(
                                     dispatcher.memory_->TranslateWritable(
                                         frame.gpr[1], sizeof(std::uint64_t)))
                               : nullptr;
            if (!output) return 14;
            *output = result;
        }
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        dispatcher.threads_.erase(frame.gpr[0]);
        return 0;
    } catch (...) {
        return 22;
    }
}

std::uint64_t HleDispatcher::PthreadDetach(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
    auto found = dispatcher.threads_.find(frame.gpr[0]);
    if (found == dispatcher.threads_.end() || found->second->detached) return 22;
    found->second->detached = true;
    return 0;
}

std::uint64_t HleDispatcher::PthreadKeyCreate(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* output = static_cast<std::uint32_t*>(WritablePointer(
        dispatcher, frame, frame.gpr[0], sizeof(std::uint32_t)));
    if (!output) return 22;
    if (frame.gpr[1] != 0 &&
        (!dispatcher.memory_ || !dispatcher.memory_->IsExecutable(frame.gpr[1])))
        return 22;
    std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
    if (dispatcher.nextKey_ == UINT32_MAX) return 11;
    const auto key = dispatcher.nextKey_++;
    dispatcher.keys_[key] = GuestKey{frame.gpr[1]};
    *output = key;
    return 0;
}

std::uint64_t HleDispatcher::PthreadGetSpecific(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    {
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        if (!dispatcher.keys_.contains(static_cast<std::uint32_t>(frame.gpr[0])))
            return 0;
    }
    const auto found = GuestSpecificValues.find(static_cast<std::uint32_t>(frame.gpr[0]));
    return found == GuestSpecificValues.end() ? 0 : found->second;
}

std::uint64_t HleDispatcher::PthreadSetSpecific(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    {
        std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
        if (!dispatcher.keys_.contains(static_cast<std::uint32_t>(frame.gpr[0])))
            return 22;
    }
    try {
        GuestSpecificValues[static_cast<std::uint32_t>(frame.gpr[0])] = frame.gpr[1];
        return 0;
    } catch (...) {
        return 12;
    }
}

std::uint64_t HleDispatcher::PthreadOnce(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    auto* control = dispatcher.memory_
                        ? static_cast<std::uint32_t*>(dispatcher.memory_->TranslateWritable(
                              frame.gpr[0], sizeof(std::uint32_t)))
                        : nullptr;
    if (!control || !dispatcher.memory_ ||
        !dispatcher.memory_->IsExecutable(frame.gpr[1]))
        return 22;
    try {
        std::shared_ptr<GuestOnce> once;
        {
            std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
            auto& slot = dispatcher.onceControls_[frame.gpr[0]];
            if (!slot) slot = std::make_shared<GuestOnce>();
            once = slot;
        }
        {
            std::unique_lock lock(once->mutex);
            if (once->done) return 0;
            if (once->running) {
                once->completed.wait(lock, [&] { return once->done || !once->running; });
                return once->done ? 0 : 22;
            }
            once->running = true;
        }
        bool completed = false;
        try {
            InvokeGuestSysv1(reinterpret_cast<void*>(frame.gpr[1]), 0);
            completed = true;
        } catch (...) {
        }
        {
            std::scoped_lock lock(once->mutex);
            once->running = false;
            once->done = completed;
            *control = completed ? 1u : 0u;
        }
        once->completed.notify_all();
        return completed ? 0 : 22;
    } catch (...) {
        return 12;
    }
}

std::uint64_t HleDispatcher::PthreadRwlockReadLock(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    try {
        auto existing = GuestRwlockOwnerships.find(frame.gpr[0]);
        if (existing != GuestRwlockOwnerships.end()) {
            if (existing->second.write) return 35;
            ++existing->second.count;
            return 0;
        }
        std::shared_ptr<GuestRwlock> rwlock;
        {
            std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
            auto& slot = dispatcher.rwlocks_[frame.gpr[0]];
            if (!slot) {
                auto* guestSlot = dispatcher.memory_
                                      ? dispatcher.memory_->TranslateWritable(
                                            frame.gpr[0], sizeof(std::uint64_t))
                                      : nullptr;
                if (!guestSlot) return 22;
                slot = std::make_shared<GuestRwlock>();
            }
            rwlock = slot;
        }
        rwlock->primitive.lock_shared();
        GuestRwlockOwnerships[frame.gpr[0]] = GuestRwlockOwnership{false, 1};
        return 0;
    } catch (...) {
        return 22;
    }
}

std::uint64_t HleDispatcher::PthreadRwlockWriteLock(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    try {
        if (GuestRwlockOwnerships.contains(frame.gpr[0])) return 35;
        std::shared_ptr<GuestRwlock> rwlock;
        {
            std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
            auto& slot = dispatcher.rwlocks_[frame.gpr[0]];
            if (!slot) {
                auto* guestSlot = dispatcher.memory_
                                      ? dispatcher.memory_->TranslateWritable(
                                            frame.gpr[0], sizeof(std::uint64_t))
                                      : nullptr;
                if (!guestSlot) return 22;
                slot = std::make_shared<GuestRwlock>();
            }
            rwlock = slot;
        }
        rwlock->primitive.lock();
        GuestRwlockOwnerships[frame.gpr[0]] = GuestRwlockOwnership{true, 1};
        return 0;
    } catch (...) {
        return 22;
    }
}

std::uint64_t HleDispatcher::PthreadRwlockUnlock(
    HleDispatcher& dispatcher, GuestCallFrame const& frame) noexcept {
    try {
        std::shared_ptr<GuestRwlock> rwlock;
        {
            std::scoped_lock lock(dispatcher.synchronizationStateMutex_);
            auto found = dispatcher.rwlocks_.find(frame.gpr[0]);
            if (found == dispatcher.rwlocks_.end()) return 22;
            rwlock = found->second;
        }
        auto ownership = GuestRwlockOwnerships.find(frame.gpr[0]);
        if (ownership == GuestRwlockOwnerships.end() || ownership->second.count == 0)
            return 1;
        if (--ownership->second.count == 0) {
            if (ownership->second.write)
                rwlock->primitive.unlock();
            else
                rwlock->primitive.unlock_shared();
            GuestRwlockOwnerships.erase(ownership);
        }
        return 0;
    } catch (...) {
        return 1;
    }
}

} // namespace Lab
