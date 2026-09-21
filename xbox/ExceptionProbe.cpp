// SPDX-License-Identifier: GPL-2.0-or-later
#include "ExceptionProbe.h"

#include <array>
#include <windows.h>
#include <fileapifromapp.h>
#include <winrt/base.h>

namespace Lab {
namespace {

constexpr DWORD ProbeExceptionCode = 0xE0424253u;
volatile LONG handler_invocations{};

int ProbeStructuredExceptionFilter(EXCEPTION_POINTERS* pointers) noexcept {
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr &&
        pointers->ExceptionRecord->ExceptionCode == ProbeExceptionCode) {
        InterlockedIncrement(&handler_invocations);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

std::uint32_t DeliverStructuredException() noexcept {
    InterlockedExchange(&handler_invocations, 0);
    __try {
        RaiseException(ProbeExceptionCode, 0, 0, nullptr);
    } __except (ProbeStructuredExceptionFilter(GetExceptionInformation())) {
    }
    return static_cast<std::uint32_t>(InterlockedCompareExchange(&handler_invocations, 0, 0));
}

} // namespace

ExceptionProbeResult ProbeOriginalExceptionDelivery(const std::wstring& directory) {
    const auto invocations = DeliverStructuredException();
    const auto path = directory + L"\\exception-core-probe.bin";
    CREATEFILE2_EXTENDED_PARAMETERS params{sizeof(params)};
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    winrt::handle writer{CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                     CREATE_ALWAYS, &params)};
    if (!writer) winrt::throw_last_error();
    std::array<std::uint32_t, 4> evidence{ProbeExceptionCode, 1u, invocations,
                                          invocations == 1 ? 1u : 0u};
    DWORD written{};
    winrt::check_bool(WriteFile(writer.get(), evidence.data(), static_cast<DWORD>(sizeof(evidence)),
                                &written, nullptr));
    winrt::check_bool(FlushFileBuffers(writer.get()));

    ExceptionProbeResult result;
    result.code = ProbeExceptionCode;
    result.handler_registered = 1;
    result.handler_invocations = invocations;
    result.handler_completed = invocations == 1 ? 1u : 0u;
    result.passed = invocations == 1 && written == sizeof(evidence);
    return result;
}

} // namespace Lab
