// SPDX-License-Identifier: GPL-2.0-or-later
#include "Probes.h"

#include "ControlledLoader.h"
#include "GuestMemory.h"
#include "HleDispatcher.h"
#include "RuntimeGate.h"

#include <cstring>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <windows.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>

namespace Lab {

void RunProbe(Test &test, std::wstring const &executablePath,
              std::wstring const &directory) {
  if (test.id != L"controlled_execution")
    throw winrt::hresult_error(E_INVALIDARG, L"Teste automático desconhecido.");
  if (executablePath.empty())
    throw winrt::hresult_error(
        E_INVALIDARG,
        L"Selecione um ELF/SELF ou extraia um PKG antes de validar.");

  auto result = LoadControlled(executablePath);
  HleDispatcher hleDispatcher;
  const auto executable = std::filesystem::path(executablePath);
  hleDispatcher.ConfigureFileSystem(executable.parent_path(),
                                    executable.parent_path() / L"RuntimeData");
  const auto hleBindings = hleDispatcher.Bind(result.pending_symbol_names);
  result.hle_addresses_created = hleBindings.executable_addresses;
  result.hle_handlers_implemented = hleBindings.implemented_handlers;
  result.hle_handlers_unimplemented = hleBindings.unimplemented_handlers;
  for (auto const &relocation : result.pending_symbol_relocations) {
    auto *address = hleDispatcher.AddressFor(relocation.symbol);
    if (!address || relocation.target < result.min_virtual_address) {
      ++result.hle_relocations_unresolved;
      continue;
    }
    const auto target = relocation.target - result.min_virtual_address;
    if (target > result.private_image.size() ||
        sizeof(std::uint64_t) > result.private_image.size() - target) {
      ++result.hle_relocations_unresolved;
      continue;
    }
    const auto value = reinterpret_cast<std::uint64_t>(address) +
                       static_cast<std::uint64_t>(relocation.addend);
    std::memcpy(result.private_image.data() + target, &value, sizeof(value));
    ++result.hle_relocations_applied;
  }
  GuestMemory guestMemory;
  result.guest_memory_mapped = guestMemory.MapValidated(
      result.private_image, result.min_virtual_address, result.guest_segments,
      result.pending_relative_relocations);
  if (result.guest_memory_mapped) {
    result.guest_memory_bytes = guestMemory.size();
    result.guest_memory_host_address = guestMemory.hostAddress();
    result.guest_memory_identity_mapped =
        result.guest_memory_host_address ==
        guestMemory.RuntimeAddress(result.min_virtual_address);
    result.guest_memory_writable_bytes = guestMemory.writableBytes();
    result.guest_memory_executable_bytes = guestMemory.executableBytes();
    std::uint64_t anonymousAddress = 0;
    if (guestMemory.MapAnonymous(0x4000, 0x3, 0, false, anonymousAddress)) {
      result.guest_memory_anonymous_probe_address = anonymousAddress;
      auto *writable = static_cast<std::uint8_t *>(
          guestMemory.TranslateWritable(anonymousAddress, 16));
      if (writable)
        std::memset(writable, 0xA5, 16);
      const auto protectedReadOnly =
          guestMemory.ProtectNoExecute(anonymousAddress, 0x4000, 0x1);
      auto *readable = guestMemory.Translate(anonymousAddress, 16);
      const auto writeDenied =
          guestMemory.TranslateWritable(anonymousAddress, 16) == nullptr;
      const auto unmapped = guestMemory.Unmap(anonymousAddress, 0x4000);
      const auto noLongerMapped =
          guestMemory.Translate(anonymousAddress, 1) == nullptr;
      result.guest_memory_anonymous_probe_passed =
          writable && protectedReadOnly && readable && writeDenied &&
          unmapped && noLongerMapped;
    }
  }
  hleDispatcher.AttachGuestMemory(&guestMemory);
  std::string clockSymbol;
  for (auto const &mapping : result.hle_symbol_mappings) {
    constexpr std::string_view suffix = "=clock_gettime";
    if (mapping.size() > suffix.size() &&
        mapping.compare(mapping.size() - suffix.size(), suffix.size(),
                        suffix) == 0) {
      clockSymbol = mapping.substr(0, mapping.size() - suffix.size());
      break;
    }
  }
  if (!clockSymbol.empty() && result.guest_memory_writable_bytes >= 16) {
    std::uint64_t guestAddress = 0;
    for (auto const &segment : result.guest_segments) {
      if ((segment.flags & 0x2u) != 0 && segment.size >= 16) {
        guestAddress = guestMemory.RuntimeAddress(segment.address);
        break;
      }
    }
    auto *thunk = hleDispatcher.AddressFor(clockSymbol);
    if (guestAddress != 0 && thunk) {
      result.hle_pointer_probe_guest_address = guestAddress;
      result.hle_pointer_probe_return = InvokeSysv2(thunk, 0, guestAddress);
      struct Timespec {
        std::int64_t seconds;
        std::int64_t nanoseconds;
      } value{};
      auto *output = static_cast<Timespec *>(
          guestMemory.TranslateWritable(guestAddress, sizeof(value)));
      result.hle_pointer_probe_passed = result.hle_pointer_probe_return == 0 &&
                                        output && output->seconds > 0 &&
                                        output->nanoseconds >= 0 &&
                                        output->nanoseconds < 1'000'000'000;
    }
  }
  std::string initialUserSymbol;
  std::string systemParamSymbol;
  for (auto const &mapping : result.hle_symbol_mappings) {
    constexpr std::string_view initialSuffix = "=sceUserServiceGetInitialUser";
    constexpr std::string_view paramSuffix = "=sceSystemServiceParamGetInt";
    if (mapping.size() > initialSuffix.size() &&
        mapping.compare(mapping.size() - initialSuffix.size(),
                        initialSuffix.size(), initialSuffix) == 0)
      initialUserSymbol =
          mapping.substr(0, mapping.size() - initialSuffix.size());
    if (mapping.size() > paramSuffix.size() &&
        mapping.compare(mapping.size() - paramSuffix.size(), paramSuffix.size(),
                        paramSuffix) == 0)
      systemParamSymbol =
          mapping.substr(0, mapping.size() - paramSuffix.size());
  }
  if (!initialUserSymbol.empty() && !systemParamSymbol.empty() &&
      result.guest_memory_writable_bytes >= 32) {
    std::uint64_t guestAddress = 0;
    for (auto const &segment : result.guest_segments) {
      if ((segment.flags & 0x2u) != 0 && segment.size >= 32) {
        guestAddress = guestMemory.RuntimeAddress(segment.address);
        break;
      }
    }
    auto *initialThunk = hleDispatcher.AddressFor(initialUserSymbol);
    auto *paramThunk = hleDispatcher.AddressFor(systemParamSymbol);
    auto *values = static_cast<std::int32_t *>(
        guestMemory.TranslateWritable(guestAddress, 2 * sizeof(std::int32_t)));
    if (guestAddress && initialThunk && paramThunk && values) {
      values[0] = -1;
      values[1] = -1;
      const auto initialResult = InvokeSysv2(initialThunk, guestAddress, 0);
      const auto paramResult =
          InvokeSysv2(paramThunk, 1, guestAddress + sizeof(std::int32_t));
      result.hle_service_probe_passed = initialResult == 0 &&
                                        paramResult == 0 && values[0] == 1 &&
                                        values[1] == 1;
    }
  }
  std::string regGetSymbol;
  std::string regSetSymbol;
  for (auto const &mapping : result.hle_symbol_mappings) {
    constexpr std::string_view getSuffix = "=sceRegMgrGetBin";
    constexpr std::string_view setSuffix = "=sceRegMgrSetBin";
    if (mapping.size() > getSuffix.size() &&
        mapping.compare(mapping.size() - getSuffix.size(), getSuffix.size(),
                        getSuffix) == 0)
      regGetSymbol = mapping.substr(0, mapping.size() - getSuffix.size());
    if (mapping.size() > setSuffix.size() &&
        mapping.compare(mapping.size() - setSuffix.size(), setSuffix.size(),
                        setSuffix) == 0)
      regSetSymbol = mapping.substr(0, mapping.size() - setSuffix.size());
  }
  if (!regGetSymbol.empty() && !regSetSymbol.empty() &&
      result.guest_memory_writable_bytes >= 32) {
    std::uint64_t guestAddress = 0;
    for (auto const &segment : result.guest_segments) {
      if ((segment.flags & 0x2u) != 0 && segment.size >= 32) {
        guestAddress = guestMemory.RuntimeAddress(segment.address);
        break;
      }
    }
    auto *getThunk = hleDispatcher.AddressFor(regGetSymbol);
    auto *setThunk = hleDispatcher.AddressFor(regSetSymbol);
    auto *bytes = static_cast<std::uint8_t *>(
        guestMemory.TranslateWritable(guestAddress, 16));
    if (guestAddress && getThunk && setThunk && bytes) {
      constexpr std::uint64_t pattern = 0xA5B6C7D8E9FA1021ull;
      std::memcpy(bytes, &pattern, sizeof(pattern));
      std::memset(bytes + 8, 0, 8);
      const auto setResult = InvokeSysv3(setThunk, 0x1234, guestAddress, 8);
      const auto getResult = InvokeSysv3(getThunk, 0x1234, guestAddress + 8, 8);
      std::uint64_t restored{};
      std::memcpy(&restored, bytes + 8, sizeof(restored));
      result.hle_regmgr_probe_passed =
          setResult == 0 && getResult == 0 && restored == pattern;
    }
  }
  std::unordered_map<std::string, std::string> fileSymbols;
  for (auto const &mapping : result.hle_symbol_mappings) {
    for (auto const &name : {"sceKernelOpen", "sceKernelClose", "sceKernelRead",
                             "sceKernelWrite", "sceKernelLseek", "sceKernelFsync"}) {
      const auto suffix = std::string("=") + name;
      if (mapping.size() > suffix.size() &&
          mapping.compare(mapping.size() - suffix.size(), suffix.size(), suffix) == 0)
        fileSymbols[name] = mapping.substr(0, mapping.size() - suffix.size());
    }
  }
  if (fileSymbols.size() == 6 && result.guest_memory_writable_bytes >= 1024) {
    std::uint64_t guestAddress = 0;
    for (auto const &segment : result.guest_segments) {
      if ((segment.flags & 0x2u) != 0 && segment.size >= 1024) {
        guestAddress = guestMemory.RuntimeAddress(segment.address);
        break;
      }
    }
    auto *memory = static_cast<std::uint8_t *>(
        guestMemory.TranslateWritable(guestAddress, 1024));
    if (memory) {
      constexpr char path[] = "/data/shadps4-fs-probe.bin";
      constexpr std::uint64_t pattern = 0x1029384756AABBCCull;
      std::memcpy(memory, path, sizeof(path));
      std::memcpy(memory + 64, &pattern, sizeof(pattern));
      std::memset(memory + 80, 0, sizeof(pattern));
      auto thunk = [&](char const *name) {
        return hleDispatcher.AddressFor(fileSymbols.at(name));
      };
      std::unordered_map<std::string, void *> metadataOperations;
      for (auto const &name : {"stat", "_fstat", "ftruncate"}) {
        for (auto const &mapping : result.hle_symbol_mappings) {
          const auto suffix = std::string("=") + name;
          if (mapping.size() > suffix.size() &&
              mapping.compare(mapping.size() - suffix.size(), suffix.size(),
                              suffix) == 0)
            metadataOperations[name] = hleDispatcher.AddressFor(
                mapping.substr(0, mapping.size() - suffix.size()));
        }
      }
      const auto descriptor = InvokeSysv3(
          thunk("sceKernelOpen"), guestAddress, 0x2u | 0x200u | 0x400u, 0600);
      const auto wrote = InvokeSysv3(thunk("sceKernelWrite"), descriptor,
                                     guestAddress + 64, sizeof(pattern));
      const auto synced = InvokeSysv2(thunk("sceKernelFsync"), descriptor, 0);
      const auto sought = InvokeSysv3(thunk("sceKernelLseek"), descriptor, 0, 0);
      const auto read = InvokeSysv3(thunk("sceKernelRead"), descriptor,
                                    guestAddress + 80, sizeof(pattern));
      auto truncated = UINT64_MAX;
      auto descriptorStat = UINT64_MAX;
      if (metadataOperations.size() == 3) {
        truncated = InvokeSysv2(metadataOperations["ftruncate"], descriptor, 4);
        descriptorStat = InvokeSysv2(metadataOperations["_fstat"], descriptor,
                                      guestAddress + 256);
      }
      const auto closed = InvokeSysv2(thunk("sceKernelClose"), descriptor, 0);
      std::uint64_t restored{};
      std::memcpy(&restored, memory + 80, sizeof(restored));
      result.hle_filesystem_probe_passed =
          descriptor < 0x80000000ull && wrote == sizeof(pattern) && synced == 0 &&
          sought == 0 && read == sizeof(pattern) && closed == 0 && restored == pattern;
      std::unordered_map<std::string, void *> operations;
      for (auto const &name : {"access", "mkdir", "rmdir", "rename", "unlink",
                               "chmod", "getdents"}) {
        for (auto const &mapping : result.hle_symbol_mappings) {
          const auto suffix = std::string("=") + name;
          if (mapping.size() > suffix.size() &&
              mapping.compare(mapping.size() - suffix.size(), suffix.size(),
                              suffix) == 0)
            operations[name] = hleDispatcher.AddressFor(
                mapping.substr(0, mapping.size() - suffix.size()));
        }
      }
      if (operations.size() == 7) {
        constexpr char target[] = "/data/shadps4-fs-renamed.bin";
        constexpr char directory[] = "/data/shadps4-fs-directory";
        std::memcpy(memory + 128, target, sizeof(target));
        std::memcpy(memory + 192, directory, sizeof(directory));
        const auto renamed = InvokeSysv2(operations["rename"], guestAddress,
                                         guestAddress + 128);
        auto pathStat = UINT64_MAX;
        if (metadataOperations.size() == 3)
          pathStat = InvokeSysv2(metadataOperations["stat"], guestAddress + 128,
                                 guestAddress + 384);
        std::int64_t descriptorSize{};
        std::int64_t pathSize{};
        std::memcpy(&descriptorSize, memory + 256 + 72, sizeof(descriptorSize));
        std::memcpy(&pathSize, memory + 384 + 72, sizeof(pathSize));
        result.hle_metadata_probe_passed =
            truncated == 0 && descriptorStat == 0 && pathStat == 0 &&
            descriptorSize == 4 && pathSize == 4;
        const auto accessed = InvokeSysv2(operations["access"],
                                          guestAddress + 128, 0);
        const auto chmodded = InvokeSysv2(operations["chmod"],
                                          guestAddress + 128, 0600);
        const auto unlinked =
            InvokeSysv2(operations["unlink"], guestAddress + 128, 0);
        const auto made =
            InvokeSysv2(operations["mkdir"], guestAddress + 192, 0700);
        const auto directoryAccessed =
            InvokeSysv2(operations["access"], guestAddress + 192, 0);
        constexpr char nestedFile[] =
            "/data/shadps4-fs-directory/item.bin";
        std::memcpy(memory, nestedFile, sizeof(nestedFile));
        const auto nestedDescriptor = InvokeSysv3(
            thunk("sceKernelOpen"), guestAddress, 0x2u | 0x200u | 0x400u, 0600);
        const auto nestedClosed =
            InvokeSysv2(thunk("sceKernelClose"), nestedDescriptor, 0);
        const auto directoryDescriptor = InvokeSysv3(
            thunk("sceKernelOpen"), guestAddress + 192, 0x20000u, 0);
        const auto directoryBytes = InvokeSysv3(
            operations["getdents"], directoryDescriptor, guestAddress + 512, 512);
        const bool directoryEntryValid =
            directoryBytes >= 20 && memory[512 + 6] == 8 &&
            memory[512 + 7] == 8 &&
            std::memcmp(memory + 512 + 8, "item.bin", 8) == 0;
        const auto directoryClosed =
            InvokeSysv2(thunk("sceKernelClose"), directoryDescriptor, 0);
        const auto nestedUnlinked =
            InvokeSysv2(operations["unlink"], guestAddress, 0);
        const auto removed =
            InvokeSysv2(operations["rmdir"], guestAddress + 192, 0);
        result.hle_directory_probe_passed =
            renamed == 0 && accessed == 0 && chmodded == 0 && unlinked == 0 &&
            made == 0 && directoryAccessed == 0 && removed == 0;
        result.hle_directory_enumeration_probe_passed =
            nestedDescriptor < 0x80000000ull && nestedClosed == 0 &&
            directoryDescriptor < 0x80000000ull && directoryEntryValid &&
            directoryClosed == 0 && nestedUnlinked == 0;
      }
    }
  }
  std::unordered_map<std::string, void *> synchronizationOperations;
  for (auto const &name : {
           "pthread_mutexattr_init", "pthread_mutexattr_settype",
           "pthread_mutex_init", "pthread_mutex_destroy", "pthread_mutex_lock",
           "pthread_mutex_trylock", "pthread_mutex_unlock", "pthread_cond_init",
           "pthread_cond_destroy", "pthread_cond_signal", "pthread_cond_broadcast",
           "sem_init", "sem_destroy", "sem_trywait", "sem_wait", "sem_getvalue",
           "sem_post"}) {
    for (auto const &mapping : result.hle_symbol_mappings) {
      const auto suffix = std::string("=") + name;
      if (mapping.size() > suffix.size() &&
          mapping.compare(mapping.size() - suffix.size(), suffix.size(), suffix) == 0)
        synchronizationOperations[name] = hleDispatcher.AddressFor(
            mapping.substr(0, mapping.size() - suffix.size()));
    }
  }
  if (synchronizationOperations.size() == 17 &&
      result.guest_memory_writable_bytes >= 64) {
    std::uint64_t guestAddress = 0;
    for (auto const &segment : result.guest_segments) {
      if ((segment.flags & 0x2u) != 0 && segment.size >= 64) {
        guestAddress = guestMemory.RuntimeAddress(segment.address);
        break;
      }
    }
    auto *memory = static_cast<std::uint8_t *>(
        guestMemory.TranslateWritable(guestAddress, 64));
    if (memory) {
      std::memset(memory, 0, 64);
      auto operation = [&](char const *name) {
        return synchronizationOperations.at(name);
      };
      const auto attributeInitialized = InvokeSysv2(
          operation("pthread_mutexattr_init"), guestAddress + 8, 0);
      const auto attributeTyped = InvokeSysv2(
          operation("pthread_mutexattr_settype"), guestAddress + 8, 2);
      const auto mutexInitialized = InvokeSysv2(
          operation("pthread_mutex_init"), guestAddress, guestAddress + 8);
      const auto firstLock =
          InvokeSysv2(operation("pthread_mutex_lock"), guestAddress, 0);
      const auto recursiveTryLock =
          InvokeSysv2(operation("pthread_mutex_trylock"), guestAddress, 0);
      const auto firstUnlock =
          InvokeSysv2(operation("pthread_mutex_unlock"), guestAddress, 0);
      const auto secondUnlock =
          InvokeSysv2(operation("pthread_mutex_unlock"), guestAddress, 0);
      const auto mutexDestroyed =
          InvokeSysv2(operation("pthread_mutex_destroy"), guestAddress, 0);
      const auto conditionInitialized = InvokeSysv2(
          operation("pthread_cond_init"), guestAddress + 32, 0);
      const auto conditionSignaled = InvokeSysv2(
          operation("pthread_cond_signal"), guestAddress + 32, 0);
      const auto conditionBroadcast = InvokeSysv2(
          operation("pthread_cond_broadcast"), guestAddress + 32, 0);
      const auto conditionDestroyed = InvokeSysv2(
          operation("pthread_cond_destroy"), guestAddress + 32, 0);
      const auto semaphoreInitialized = InvokeSysv3(
          operation("sem_init"), guestAddress + 16, 0, 1);
      const auto initialValue = InvokeSysv2(
          operation("sem_getvalue"), guestAddress + 16, guestAddress + 24);
      std::int32_t valueBefore{};
      std::memcpy(&valueBefore, memory + 24, sizeof(valueBefore));
      const auto semaphoreTryWait = InvokeSysv2(
          operation("sem_trywait"), guestAddress + 16, 0);
      const auto emptyValue = InvokeSysv2(
          operation("sem_getvalue"), guestAddress + 16, guestAddress + 24);
      std::int32_t valueAfter{};
      std::memcpy(&valueAfter, memory + 24, sizeof(valueAfter));
      const auto semaphorePosted =
          InvokeSysv2(operation("sem_post"), guestAddress + 16, 0);
      const auto semaphoreWaited =
          InvokeSysv2(operation("sem_wait"), guestAddress + 16, 0);
      const auto semaphoreDestroyed =
          InvokeSysv2(operation("sem_destroy"), guestAddress + 16, 0);
      result.hle_synchronization_probe_passed =
          attributeInitialized == 0 && attributeTyped == 0 &&
          mutexInitialized == 0 && firstLock == 0 && recursiveTryLock == 0 &&
          firstUnlock == 0 && secondUnlock == 0 && mutexDestroyed == 0 &&
          conditionInitialized == 0 && conditionSignaled == 0 &&
          conditionBroadcast == 0 && conditionDestroyed == 0 &&
          semaphoreInitialized == 0 && initialValue == 0 && valueBefore == 1 &&
          semaphoreTryWait == 0 && emptyValue == 0 && valueAfter == 0 &&
          semaphorePosted == 0 && semaphoreWaited == 0 &&
          semaphoreDestroyed == 0;
    }
  }
  auto gate = EvaluateRuntimeGate(result);
  result.runtime_preflight_ready = gate.ready;
  result.runtime_blockers = gate.blockers;
  using winrt::Windows::Data::Json::JsonValue;
  test.measurements.Insert(
      L"file_size",
      JsonValue::CreateNumberValue(static_cast<double>(result.file_size)));
  test.measurements.Insert(L"self", JsonValue::CreateBooleanValue(result.self));
  test.measurements.Insert(L"validated",
                           JsonValue::CreateBooleanValue(result.validated));
  test.measurements.Insert(L"mapped",
                           JsonValue::CreateBooleanValue(result.mapped));
  test.measurements.Insert(L"inner_elf",
                           JsonValue::CreateBooleanValue(result.inner_elf));
  test.measurements.Insert(L"inner_mapped",
                           JsonValue::CreateBooleanValue(result.inner_mapped));
  test.measurements.Insert(
      L"protected_segments",
      JsonValue::CreateBooleanValue(result.protected_segments));
  test.measurements.Insert(
      L"segment_count",
      JsonValue::CreateNumberValue(static_cast<double>(result.segment_count)));
  test.measurements.Insert(
      L"load_segments",
      JsonValue::CreateNumberValue(static_cast<double>(result.load_segments)));
  test.measurements.Insert(
      L"mapped_bytes",
      JsonValue::CreateNumberValue(static_cast<double>(result.mapped_bytes)));
  test.measurements.Insert(L"entry", JsonValue::CreateNumberValue(
                                         static_cast<double>(result.entry)));
  test.measurements.Insert(L"min_virtual_address",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.min_virtual_address)));
  test.measurements.Insert(L"max_virtual_address",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.max_virtual_address)));
  test.measurements.Insert(
      L"checksum_fnv1a",
      JsonValue::CreateNumberValue(static_cast<double>(result.checksum)));
  test.measurements.Insert(L"inner_segment_count",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.inner_segment_count)));
  test.measurements.Insert(L"inner_load_segments",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.inner_load_segments)));
  test.measurements.Insert(
      L"inner_entry",
      JsonValue::CreateNumberValue(static_cast<double>(result.inner_entry)));
  test.measurements.Insert(L"inner_mapped_bytes",
                           JsonValue::CreateNumberValue(
                               static_cast<double>(result.inner_mapped_bytes)));
  test.measurements.Insert(
      L"inner_checksum_fnv1a",
      JsonValue::CreateNumberValue(static_cast<double>(result.inner_checksum)));
  test.measurements.Insert(L"dynamic_segments",
                           JsonValue::CreateNumberValue(
                               static_cast<double>(result.dynamic_segments)));
  test.measurements.Insert(
      L"tls_segments",
      JsonValue::CreateNumberValue(static_cast<double>(result.tls_segments)));
  test.measurements.Insert(L"dynamic_entries",
                           JsonValue::CreateNumberValue(
                               static_cast<double>(result.dynamic_entries)));
  test.measurements.Insert(
      L"rela_entries",
      JsonValue::CreateNumberValue(static_cast<double>(result.rela_entries)));
  test.measurements.Insert(L"jmp_rela_entries",
                           JsonValue::CreateNumberValue(
                               static_cast<double>(result.jmp_rela_entries)));
  test.measurements.Insert(L"import_libraries",
                           JsonValue::CreateNumberValue(
                               static_cast<double>(result.import_libraries)));
  test.measurements.Insert(
      L"needed_modules",
      JsonValue::CreateNumberValue(static_cast<double>(result.needed_modules)));
  test.measurements.Insert(L"supported_relocations",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.supported_relocations)));
  test.measurements.Insert(L"unsupported_relocations",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.unsupported_relocations)));
  test.measurements.Insert(L"relocation_targets_outside_segments",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.relocation_targets_outside_segments)));
  test.measurements.Insert(L"relative_relocations_applied",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.relative_relocations_applied)));
  test.measurements.Insert(L"symbol_relocations_pending",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.symbol_relocations_pending)));
  test.measurements.Insert(L"tls_relocations_pending",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.tls_relocations_pending)));
  test.measurements.Insert(L"symbol_relocations_valid",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.symbol_relocations_valid)));
  test.measurements.Insert(L"symbol_relocations_invalid",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.symbol_relocations_invalid)));
  test.measurements.Insert(L"hle_symbols_known",
                           JsonValue::CreateNumberValue(
                               static_cast<double>(result.hle_symbols_known)));
  test.measurements.Insert(L"hle_symbols_unknown",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.hle_symbols_unknown)));
  test.measurements.Insert(L"hle_addresses_created",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.hle_addresses_created)));
  test.measurements.Insert(L"hle_handlers_implemented",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.hle_handlers_implemented)));
  test.measurements.Insert(L"hle_handlers_unimplemented",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.hle_handlers_unimplemented)));
  test.measurements.Insert(L"hle_relocations_applied",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.hle_relocations_applied)));
  test.measurements.Insert(L"hle_relocations_unresolved",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.hle_relocations_unresolved)));
  test.measurements.Insert(
      L"guest_memory_mapped",
      JsonValue::CreateBooleanValue(result.guest_memory_mapped));
  test.measurements.Insert(
      L"guest_memory_identity_mapped",
      JsonValue::CreateBooleanValue(result.guest_memory_identity_mapped));
  test.measurements.Insert(L"guest_memory_bytes",
                           JsonValue::CreateNumberValue(
                               static_cast<double>(result.guest_memory_bytes)));
  test.measurements.Insert(L"guest_memory_writable_bytes",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.guest_memory_writable_bytes)));
  test.measurements.Insert(L"guest_memory_executable_bytes",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.guest_memory_executable_bytes)));
  test.measurements.Insert(L"guest_memory_host_address",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.guest_memory_host_address)));
  test.measurements.Insert(L"guest_memory_anonymous_probe_passed",
                           JsonValue::CreateBooleanValue(
                               result.guest_memory_anonymous_probe_passed));
  test.measurements.Insert(L"guest_memory_anonymous_probe_address",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.guest_memory_anonymous_probe_address)));
  test.measurements.Insert(
      L"hle_pointer_probe_passed",
      JsonValue::CreateBooleanValue(result.hle_pointer_probe_passed));
  test.measurements.Insert(
      L"hle_service_probe_passed",
      JsonValue::CreateBooleanValue(result.hle_service_probe_passed));
  test.measurements.Insert(
      L"hle_regmgr_probe_passed",
      JsonValue::CreateBooleanValue(result.hle_regmgr_probe_passed));
  test.measurements.Insert(
      L"hle_filesystem_probe_passed",
      JsonValue::CreateBooleanValue(result.hle_filesystem_probe_passed));
  test.measurements.Insert(
      L"hle_directory_probe_passed",
      JsonValue::CreateBooleanValue(result.hle_directory_probe_passed));
  test.measurements.Insert(
      L"hle_metadata_probe_passed",
      JsonValue::CreateBooleanValue(result.hle_metadata_probe_passed));
  test.measurements.Insert(
      L"hle_directory_enumeration_probe_passed",
      JsonValue::CreateBooleanValue(
          result.hle_directory_enumeration_probe_passed));
  test.measurements.Insert(
      L"hle_synchronization_probe_passed",
      JsonValue::CreateBooleanValue(result.hle_synchronization_probe_passed));
  test.measurements.Insert(L"hle_pointer_probe_return",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.hle_pointer_probe_return)));
  test.measurements.Insert(L"hle_pointer_probe_guest_address",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.hle_pointer_probe_guest_address)));
  test.measurements.Insert(
      L"runtime_preflight_ready",
      JsonValue::CreateBooleanValue(result.runtime_preflight_ready));
  winrt::Windows::Data::Json::JsonArray symbolNames;
  for (auto const &name : result.pending_symbol_names)
    symbolNames.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
  test.measurements.Insert(L"pending_symbol_names", symbolNames);
  winrt::Windows::Data::Json::JsonArray hleMappings;
  for (auto const &mapping : result.hle_symbol_mappings)
    hleMappings.Append(
        JsonValue::CreateStringValue(winrt::to_hstring(mapping)));
  test.measurements.Insert(L"hle_symbol_mappings", hleMappings);
  winrt::Windows::Data::Json::JsonArray hleUnmapped;
  for (auto const &name : result.hle_unmapped_symbols)
    hleUnmapped.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
  test.measurements.Insert(L"hle_unmapped_symbols", hleUnmapped);
  winrt::Windows::Data::Json::JsonArray runtimeBlockers;
  for (auto const &blocker : result.runtime_blockers)
    runtimeBlockers.Append(JsonValue::CreateStringValue(blocker));
  test.measurements.Insert(L"runtime_blockers", runtimeBlockers);
  winrt::Windows::Data::Json::JsonArray libraryIds;
  for (auto const &id : result.import_library_ids)
    libraryIds.Append(JsonValue::CreateStringValue(winrt::to_hstring(id)));
  test.measurements.Insert(L"import_library_ids", libraryIds);
  winrt::Windows::Data::Json::JsonArray libraryNames;
  for (auto const &name : result.import_library_names)
    libraryNames.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
  test.measurements.Insert(L"import_library_names", libraryNames);
  winrt::Windows::Data::Json::JsonArray moduleIds;
  for (auto const &id : result.needed_module_ids)
    moduleIds.Append(JsonValue::CreateStringValue(winrt::to_hstring(id)));
  test.measurements.Insert(L"needed_module_ids", moduleIds);
  winrt::Windows::Data::Json::JsonArray moduleNames;
  for (auto const &name : result.needed_module_names)
    moduleNames.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
  test.measurements.Insert(L"needed_module_names", moduleNames);
  test.measurements.Insert(L"relocation_dry_run_checksum",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               result.relocation_dry_run_checksum)));
  test.measurements.Insert(L"has_dynamic",
                           JsonValue::CreateBooleanValue(result.has_dynamic));
  test.measurements.Insert(L"has_tls",
                           JsonValue::CreateBooleanValue(result.has_tls));
  test.measurements.Insert(L"has_relocations", JsonValue::CreateBooleanValue(
                                                   result.has_relocations));
  test.measurements.Insert(L"has_imports",
                           JsonValue::CreateBooleanValue(result.has_imports));
  test.measurements.Insert(
      L"relocation_data_valid",
      JsonValue::CreateBooleanValue(result.relocation_data_valid));
  auto execution = ExecuteGeneratedProbe(std::filesystem::path(directory));
  test.measurements.Insert(
      L"execution_returned_value",
      JsonValue::CreateNumberValue(execution.returned_value));
  test.measurements.Insert(L"sysv_abi_passed", JsonValue::CreateBooleanValue(
                                                   execution.sysv_abi_passed));
  test.measurements.Insert(L"sysv_abi_returned_value",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               execution.sysv_abi_returned_value)));
  test.measurements.Insert(L"hle_thunk_returned_value",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               execution.hle_thunk_returned_value)));
  test.measurements.Insert(L"execution_address",
                           JsonValue::CreateNumberValue(static_cast<double>(
                               execution.executable_address)));
  test.measurements.Insert(L"execution_elf_file_size",
                           JsonValue::CreateNumberValue(
                               static_cast<double>(execution.elf_file_size)));
  test.status = L"passed";
  std::wstring gateDetail = L"\nGate de runtime: ";
  gateDetail += result.runtime_preflight_ready ? L"pronto." : L"bloqueado.";
  for (auto const &blocker : result.runtime_blockers)
    gateDetail += L"\n- " + blocker;
  test.detail =
      result.detail + L"\nMetadados runtime: dynamic=" +
      std::to_wstring(result.dynamic_entries) + L", relocations=" +
      std::to_wstring(result.rela_entries + result.jmp_rela_entries) +
      L", imports=" +
      std::to_wstring(result.import_libraries + result.needed_modules) +
      L", TLS=" + std::to_wstring(result.tls_segments) +
      L", relocation types supported=" +
      std::to_wstring(result.supported_relocations) + L", unsupported=" +
      std::to_wstring(result.unsupported_relocations) +
      L", targets outside mapped segments=" +
      std::to_wstring(result.relocation_targets_outside_segments) +
      L", relative applied in private dry-run=" +
      std::to_wstring(result.relative_relocations_applied) +
      L", symbol pending=" +
      std::to_wstring(result.symbol_relocations_pending) +
      L", symbol names valid=" +
      std::to_wstring(result.symbol_relocations_valid) + L", invalid=" +
      std::to_wstring(result.symbol_relocations_invalid) +
      L", NIDs conhecidos no registro AeroLib=" +
      std::to_wstring(result.hle_symbols_known) +
      L", NIDs sem correspondência=" +
      std::to_wstring(result.hle_symbols_unknown) +
      L", endereços HLE criados=" +
      std::to_wstring(result.hle_addresses_created) +
      L", handlers implementados=" +
      std::to_wstring(result.hle_handlers_implemented) +
      L", handlers pendentes=" +
      std::to_wstring(result.hle_handlers_unimplemented) +
      L", relocations HLE aplicadas antes do mapa coerente=" +
      std::to_wstring(result.hle_relocations_applied) +
      L", relocations HLE sem resolução=" +
      std::to_wstring(result.hle_relocations_unresolved) +
      L", memória convidada mapeada com proteção=" +
      (result.guest_memory_mapped ? L"sim" : L"não") +
      L", mapeamento guest/host idêntico=" +
      (result.guest_memory_identity_mapped ? L"sim" : L"não") +
      L", bytes PF_W graváveis=" +
      std::to_wstring(result.guest_memory_writable_bytes) +
      L", bytes PF_X executáveis=" +
      std::to_wstring(result.guest_memory_executable_bytes) +
      L", probe mmap/mprotect/munmap=" +
      (result.guest_memory_anonymous_probe_passed ? L"aprovado" : L"pendente") +
      L", smoke test de ponteiro clock_gettime=" +
      (result.hle_pointer_probe_passed ? L"aprovado" : L"pendente") +
      L", serviços de usuário/sistema=" +
      (result.hle_service_probe_passed ? L"aprovado" : L"pendente") +
      L", serviço RegMgr=" +
      (result.hle_regmgr_probe_passed ? L"aprovado" : L"pendente") +
      L", sistema de arquivos HLE=" +
      (result.hle_filesystem_probe_passed ? L"aprovado" : L"pendente") +
      L", diretórios e manutenção HLE=" +
      (result.hle_directory_probe_passed ? L"aprovado" : L"pendente") +
      L", metadados e truncamento HLE=" +
      (result.hle_metadata_probe_passed ? L"aprovado" : L"pendente") +
      L", enumeração de diretórios HLE=" +
      (result.hle_directory_enumeration_probe_passed ? L"aprovado"
                                                      : L"pendente") +
      L", sincronização POSIX HLE=" +
      (result.hle_synchronization_probe_passed ? L"aprovado" : L"pendente") +
      L", TLS pending=" + std::to_wstring(result.tls_relocations_pending) +
      std::wstring(L". Gate de runtime=") +
      (result.runtime_preflight_ready ? L"pronto" : L"bloqueado") +
      L". As relocations foram reaplicadas com a base escolhida pelo UWP e "
      L"copiadas para o mapa coerente; o homebrew não foi executado.\n" +
      gateDetail + L"\n" + execution.detail + L"\nArquivo: " + executablePath;
}

} // namespace Lab
