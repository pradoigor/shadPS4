// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "GuestMemory.h"
#include "SysvThunk.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Lab {
class AngleVideo;
struct GuestFontState;
struct GuestDeviceState;

struct HleResolution {
    std::string symbol;
    std::string nid;
    bool implemented{};
    void* address{};
};

struct HleBindingSummary {
    std::size_t requested{};
    std::size_t executable_addresses{};
    std::size_t implemented_handlers{};
    std::size_t unimplemented_handlers{};
};

class HleDispatcher;
using HleHandler = std::uint64_t (*)(HleDispatcher&, GuestCallFrame const&) noexcept;

class HleDispatcher {
public:
    HleDispatcher() = default;
    ~HleDispatcher();

    HleDispatcher(HleDispatcher const&) = delete;
    HleDispatcher& operator=(HleDispatcher const&) = delete;

    // Returns a thunk for an import. Unimplemented imports receive a safe
    // ENOSYS-style return value and are still reported as unavailable.
    HleResolution Resolve(std::string_view encodedSymbol);
    HleBindingSummary Bind(std::vector<std::string> const& encodedSymbols);
    void* AddressFor(std::string_view encodedSymbol) const noexcept;
    void AttachGuestMemory(GuestMemory* memory) noexcept { memory_ = memory; }
    void AttachGraphics(AngleVideo* graphics) noexcept { graphics_ = graphics; }
    AngleVideo* Graphics() const noexcept { return graphics_; }
    std::shared_ptr<GuestFontState>& FontState() noexcept { return fonts_; }
    std::shared_ptr<GuestDeviceState>& DeviceState() noexcept { return devices_; }
    bool GuestFilePath(std::string const& path, bool write, std::filesystem::path& host) const noexcept {
        return ResolveGuestPath(path, write, host);
    }
    void* GuestWritable(GuestCallFrame const& frame, std::uint64_t address, std::size_t bytes) noexcept;
    void const* GuestReadable(GuestCallFrame const& frame, std::uint64_t address, std::size_t bytes) noexcept;
    bool GuestString(std::uint64_t address, std::string& value, std::size_t limit = 1024) const noexcept;
    void GraphicsLog(std::string_view line) noexcept;
    void* GraphicsAddress(std::string_view name);
    void ConfigureFileSystem(std::filesystem::path appRoot,
                             std::filesystem::path dataRoot);
    void ConfigureTrace(std::filesystem::path path, std::string const& sessionId = {});

    std::size_t implementedCount() const noexcept;
    std::size_t unresolvedCount() const noexcept;

private:
    struct Entry {
        std::string encoded;
        std::string nid;
        std::string name;
        bool implemented{};
        HleHandler handler{};
        void* address{};
    };
    AngleVideo* graphics_{};
    std::shared_ptr<GuestFontState> fonts_;
    std::shared_ptr<GuestDeviceState> devices_;

    static std::uint64_t Dispatch(void* context, std::uint64_t slot,
                                  GuestCallFrame const* frame, void* guestStack) noexcept;
    static void* WritablePointer(HleDispatcher&, GuestCallFrame const&,
                                 std::uint64_t address, std::size_t bytes) noexcept;
    static void const* ReadablePointer(HleDispatcher&, GuestCallFrame const&,
                                       std::uint64_t address,
                                       std::size_t bytes) noexcept;
    static std::uint64_t Unimplemented(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelErrorPointer(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t GenericSuccess(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t GenericHandle(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelSysconf(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelSandboxWord(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelNanosleep(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelUsleep(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelGetUpdVersion(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelGetLowerLimitUpdVersion(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelGetPid(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelGetEuid(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelSchedYield(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelThreadSelf(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t EglGetError(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t EglQueryApi(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t GlGetError(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t NetCtlInit(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t NetCtlTerm(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t HideSplashScreen(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelDebugOutText(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelMprotect(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryMemcpy(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryMemmove(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryMemset(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryMemcmp(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryStrlen(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryMmap(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelMmap(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t MemoryMunmap(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelMunmap(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t ClockGetTime(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t SignalAction(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t UserServiceInitialize(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t UserServiceGetInitialUser(HleDispatcher&,
                                                   GuestCallFrame const&) noexcept;
    static std::uint64_t UserServiceGetLoginUsers(HleDispatcher&,
                                                  GuestCallFrame const&) noexcept;
    static std::uint64_t UserServiceGetUserName(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t SystemServiceParamGetInt(HleDispatcher&,
                                                  GuestCallFrame const&) noexcept;
    static std::uint64_t RegMgrGetBin(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t RegMgrSetBin(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t RegMgrGetStr(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t RegMgrSetStr(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t RegMgrSetInt(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelOpen(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelClose(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelRead(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelWrite(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelWritev(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelLseek(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t KernelFsync(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t FileAccess(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t FileMkdir(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t FileRmdir(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t FileRename(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t FileUnlink(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t FileChmod(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t FileFlock(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t FileStat(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t FileFstat(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t FileFtruncate(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t FileGetdents(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadMutexAttrInit(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadMutexAttrSetType(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadMutexInit(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadMutexDestroy(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadMutexLock(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadMutexTryLock(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadMutexUnlock(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadCondInit(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadCondDestroy(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadCondWait(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadCondSignal(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadCondBroadcast(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t SemaphoreInit(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t SemaphoreDestroy(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t SemaphoreTryWait(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t SemaphoreWait(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t SemaphoreTimedWait(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t SemaphoreGetValue(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t SemaphorePost(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadAttrInit(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadAttrSetDetachState(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadAttrSetStackSize(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadCreate(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadJoin(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadDetach(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadKeyCreate(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadGetSpecific(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadSetSpecific(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadOnce(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadRwlockReadLock(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadRwlockWriteLock(HleDispatcher&, GuestCallFrame const&) noexcept;
    static std::uint64_t PthreadRwlockUnlock(HleDispatcher&, GuestCallFrame const&) noexcept;

    bool ReadGuestString(std::uint64_t address, std::string& value,
                         std::size_t limit = 1024) const noexcept;
    bool ResolveGuestPath(std::string const& guestPath, bool write,
                          std::filesystem::path& hostPath) const noexcept;

    SysvThunkArena thunks_;
    std::vector<Entry> entries_;
    GuestMemory* memory_{};
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> registry_;
    struct GuestFile {
        struct DirectoryEntry {
            std::string name;
            bool directory{};
        };
        std::fstream stream;
        std::filesystem::path path;
        std::vector<DirectoryEntry> directoryEntries;
        std::size_t directoryIndex{};
        bool directory{};
        bool writable{};
    };
    std::filesystem::path appRoot_;
    std::filesystem::path dataRoot_;
    std::unordered_map<std::int32_t, GuestFile> files_;
    std::int32_t nextFileDescriptor_{3};
    std::filesystem::path tracePath_;
    std::filesystem::path traceHistoryPath_;
    std::filesystem::path traceArchivePath_;
    std::filesystem::path consolePath_;
    std::mutex consoleMutex_;
    std::size_t consoleBytes_{};
    void AppendConsole(void const* bytes, std::size_t length) noexcept;
    std::mutex traceMutex_;
    std::uint64_t callSequence_{};
    struct GuestSignalAction {
        std::uint64_t handler{};
        std::int32_t flags{};
        std::uint32_t mask[4]{};
        std::uint32_t padding{};
    };
    static_assert(sizeof(GuestSignalAction) == 32);
    std::array<GuestSignalAction, 128> signalActions_{};
    struct GuestMutex {
        void lock() {
            std::unique_lock lock(state);
            const auto current = std::this_thread::get_id();
            available.wait(lock, [&] { return depth == 0 || owner == current; });
            owner = current;
            ++depth;
        }
        bool try_lock() {
            std::scoped_lock lock(state);
            const auto current = std::this_thread::get_id();
            if (depth != 0 && owner != current) return false;
            owner = current;
            ++depth;
            return true;
        }
        void unlock() {
            std::scoped_lock lock(state);
            if (depth == 0 || owner != std::this_thread::get_id())
                throw std::runtime_error("Mutex convidado não pertence à thread.");
            if (--depth == 0) {
                owner = {};
                available.notify_one();
            }
        }
        bool ownedByCurrentThread() {
            std::scoped_lock lock(state);
            return depth != 0 && owner == std::this_thread::get_id();
        }
        bool isLocked() {
            std::scoped_lock lock(state);
            return depth != 0;
        }
        std::mutex state;
        std::condition_variable available;
        std::thread::id owner;
        std::uint32_t depth{};
        bool recursive{};
    };
    struct GuestCondition {
        std::condition_variable_any primitive;
    };
    struct GuestSemaphore {
        std::mutex mutex;
        std::condition_variable condition;
        std::uint32_t value{};
    };
    std::mutex synchronizationStateMutex_;
    std::unordered_map<std::uint64_t, std::uint32_t> mutexAttributes_;
    std::unordered_map<std::uint64_t, std::shared_ptr<GuestMutex>> mutexes_;
    std::unordered_map<std::uint64_t, std::shared_ptr<GuestCondition>> conditions_;
    std::unordered_map<std::uint64_t, std::shared_ptr<GuestSemaphore>> semaphores_;
    struct GuestThreadAttribute {
        bool detached{};
        std::size_t stackSize{1024 * 1024};
    };
    struct GuestThread {
        ~GuestThread() {
            if (native.joinable()) native.join();
        }
        std::thread native;
        std::mutex state;
        std::condition_variable completed;
        std::uint64_t result{};
        bool finished{};
        bool detached{};
    };
    std::unordered_map<std::uint64_t, GuestThreadAttribute> threadAttributes_;
    std::unordered_map<std::uint64_t, std::shared_ptr<GuestThread>> threads_;
    std::uint64_t nextThreadId_{0x1000};
    struct GuestKey {
        std::uint64_t destructor{};
    };
    struct GuestOnce {
        std::mutex mutex;
        std::condition_variable completed;
        bool running{};
        bool done{};
    };
    struct GuestRwlock {
        std::shared_mutex primitive;
    };
    std::unordered_map<std::uint32_t, GuestKey> keys_;
    std::unordered_map<std::uint64_t, std::shared_ptr<GuestOnce>> onceControls_;
    std::unordered_map<std::uint64_t, std::shared_ptr<GuestRwlock>> rwlocks_;
    std::uint32_t nextKey_{1};
};

} // namespace Lab
