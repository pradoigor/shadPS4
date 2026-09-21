// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _WIN32
#include <windows.h>
#include <memoryapi.h>

namespace Core::PlatformMemory {

inline void* Allocate(HANDLE process, void* base, SIZE_T size, ULONG allocation_type,
                      ULONG protection, MEM_EXTENDED_PARAMETER* parameters = nullptr,
                      ULONG parameter_count = 0) {
#ifdef SHADPS4_XBOX_UWP
    const bool executable = protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
                            protection == PAGE_EXECUTE_READWRITE;
    const auto allocation_protection = executable ? PAGE_READWRITE : protection;
    auto* result = VirtualAlloc2FromApp(process, base, size, allocation_type,
                                        allocation_protection, parameters, parameter_count);
    if (!result || !executable || protection == PAGE_EXECUTE_READWRITE) return result;
    ULONG previous{};
    if (!VirtualProtectFromApp(result, size, protection, &previous)) {
        VirtualFree(result, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(process, result, size);
    return result;
#else
    return VirtualAlloc2(process, base, size, allocation_type, protection, parameters,
                         parameter_count);
#endif
}

inline HANDLE CreateBacking(HANDLE file, PSECURITY_ATTRIBUTES attributes, ULONG desired_access,
                            ULONG protection, ULONG allocation_attributes, ULONG64 size,
                            PCWSTR name = nullptr, MEM_EXTENDED_PARAMETER* parameters = nullptr,
                            ULONG parameter_count = 0) {
#ifdef SHADPS4_XBOX_UWP
    (void)desired_access;
    (void)parameters;
    (void)parameter_count;
    // UWP does not permit executable file mappings. Guest code is written
    // through RW views and promoted to RX with Protect() under codeGeneration.
    const auto writable_protection = (protection & 0xFFu) == PAGE_READONLY
                                         ? PAGE_READONLY
                                         : PAGE_READWRITE;
    return CreateFileMappingFromApp(file, attributes,
                                    writable_protection | allocation_attributes, size, name);
#else
    return CreateFileMapping2(file, attributes, desired_access, protection, allocation_attributes,
                              size, name, parameters, parameter_count);
#endif
}

inline void* MapView(HANDLE mapping, HANDLE process, void* base, ULONG64 offset, SIZE_T size,
                     ULONG allocation_type, ULONG protection,
                     MEM_EXTENDED_PARAMETER* parameters = nullptr, ULONG parameter_count = 0) {
#ifdef SHADPS4_XBOX_UWP
    auto* result = MapViewOfFile3FromApp(mapping, process, base, offset, size, allocation_type,
                                         PAGE_READWRITE, parameters, parameter_count);
    if (!result || protection == PAGE_READWRITE) return result;
    ULONG previous{};
    if (!VirtualProtectFromApp(result, size, protection, &previous)) {
        UnmapViewOfFileEx(result, (allocation_type & MEM_REPLACE_PLACEHOLDER)
                                     ? MEM_PRESERVE_PLACEHOLDER
                                     : 0);
        return nullptr;
    }
    if (protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ)
        FlushInstructionCache(process, result, size);
    return result;
#else
    return MapViewOfFile3(mapping, process, base, offset, size, allocation_type, protection,
                          parameters, parameter_count);
#endif
}

inline BOOL Protect(HANDLE process, void* address, SIZE_T size, ULONG protection,
                    PULONG old_protection) {
#ifdef SHADPS4_XBOX_UWP
    (void)process;
    return VirtualProtectFromApp(address, size, protection, old_protection);
#else
    return VirtualProtectEx(process, address, size, protection, old_protection);
#endif
}

inline BOOL Free(HANDLE process, void* address, SIZE_T size, ULONG free_type) {
#ifdef SHADPS4_XBOX_UWP
    (void)process;
    return VirtualFree(address, size, free_type);
#else
    return VirtualFreeEx(process, address, size, free_type);
#endif
}

inline BOOL UnmapView(HANDLE process, void* address, ULONG flags) {
#ifdef SHADPS4_XBOX_UWP
    (void)process;
    return UnmapViewOfFileEx(address, flags);
#else
    return UnmapViewOfFile2(process, address, flags);
#endif
}

} // namespace Core::PlatformMemory
#endif
