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

LONG CALLBACK ProbeVectoredHandler(EXCEPTION_POINTERS* pointers) noexcept {
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr &&
        pointers->ExceptionRecord->ExceptionCode == ProbeExceptionCode) {
        InterlockedIncrement(&handler_invocations);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

ExceptionProbeResult ProbeOriginalExceptionDelivery(const std::wstring& directory) {
    InterlockedExchange(&handler_invocations, 0);
    auto handler = AddVectoredExceptionHandler(1, &ProbeVectoredHandler);
    if (handler == nullptr) {
        throw winrt::hresult_error(E_FAIL, L"AddVectoredExceptionHandler não registrou o probe UWP.");
    }

    RaiseException(ProbeExceptionCode, 0, 0, nullptr);
    const auto invocations = static_cast<std::uint32_t>(InterlockedCompareExchange(
        &handler_invocations, 0, 0));
    const auto removed = RemoveVectoredExceptionHandler(handler);

    const auto path = directory + L"\\exception-core-probe.bin";
    CREATEFILE2_EXTENDED_PARAMETERS params{sizeof(params)};
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    winrt::handle writer{CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                     CREATE_ALWAYS, &params)};
    if (!writer) winrt::throw_last_error();
    std::array<std::uint32_t, 4> evidence{
        ProbeExceptionCode, 1u, invocations, removed ? 1u : 0u};
    DWORD written{};
    winrt::check_bool(WriteFile(writer.get(), evidence.data(), static_cast<DWORD>(sizeof(evidence)),
                                &written, nullptr));
    winrt::check_bool(FlushFileBuffers(writer.get()));

    ExceptionProbeResult result;
    result.exception_code = ProbeExceptionCode;
    result.handler_registered = 1;
    result.handler_invocations = invocations;
    result.handler_removed = removed ? 1u : 0u;
    result.passed = invocations == 1 && removed != FALSE && written == sizeof(evidence);
    return result;
}

} // namespace Lab
