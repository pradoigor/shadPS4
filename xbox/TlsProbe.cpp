// SPDX-License-Identifier: GPL-2.0-or-later
#include "TlsProbe.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <windows.h>
#include <fileapifromapp.h>
#include <winrt/base.h>

#include "common/arch.h"
#include "core/tls.h"

namespace Lab {
namespace {

constexpr std::uint64_t Canary = 0x5348414450533454ull;

} // namespace

TlsProbeResult ProbeOriginalTlsModel(const std::wstring& directory) {
    const DWORD slot = TlsAlloc();
    if (slot == TLS_OUT_OF_INDEXES) {
        throw winrt::hresult_error(E_FAIL, L"TlsAlloc não reservou um slot para o probe UWP.");
    }

    std::array<Core::DtvEntry, 2> dtv{};
    Core::Tcb tcb{};
    tcb.tcb_self = &tcb;
    tcb.tcb_dtv = dtv.data();
    tcb.tcb_canary = Canary;
    dtv[0].counter = 1;
    dtv[1].pointer = reinterpret_cast<std::uint8_t*>(&tcb);

    const BOOL set_ok = TlsSetValue(slot, &tcb);
    auto* roundtrip = static_cast<Core::Tcb*>(TlsGetValue(slot));
    const BOOL free_ok = TlsFree(slot);

    const auto path = directory + L"\\tls-core-probe.bin";
    CREATEFILE2_EXTENDED_PARAMETERS params{sizeof(params)};
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    winrt::handle writer{CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                     CREATE_ALWAYS, &params)};
    if (!writer) winrt::throw_last_error();
    std::array<std::uint64_t, 4> evidence{
        sizeof(Core::Tcb), dtv.size(), reinterpret_cast<std::uint64_t>(roundtrip), Canary};
    DWORD written{};
    winrt::check_bool(WriteFile(writer.get(), evidence.data(), static_cast<DWORD>(sizeof(evidence)),
                                &written, nullptr));
    winrt::check_bool(FlushFileBuffers(writer.get()));

    TlsProbeResult result;
    result.slot = slot;
    result.tcb_size = static_cast<std::uint32_t>(sizeof(Core::Tcb));
    result.dtv_entries = static_cast<std::uint32_t>(dtv.size());
    result.roundtrip_address = reinterpret_cast<std::uint64_t>(roundtrip);
    result.canary = roundtrip ? roundtrip->tcb_canary : 0;
    result.passed = set_ok != FALSE && free_ok != FALSE && roundtrip == &tcb &&
                    roundtrip->tcb_self == &tcb && roundtrip->tcb_dtv == dtv.data() &&
                    roundtrip->tcb_canary == Canary && written == sizeof(evidence);
    return result;
}

} // namespace Lab
