// SPDX-License-Identifier: GPL-2.0-or-later
#include "Probes.h"

#include "ControlledLoader.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>

namespace Lab {

void RunProbe(Test& test, std::wstring const& executablePath) {
    if (test.id != L"controlled_loader")
        throw winrt::hresult_error(E_INVALIDARG, L"Teste automático desconhecido.");
    if (executablePath.empty())
        throw winrt::hresult_error(E_INVALIDARG, L"Selecione um ELF/SELF ou extraia um PKG antes de validar.");

    auto result = LoadControlled(executablePath);
    using winrt::Windows::Data::Json::JsonValue;
    test.measurements.Insert(L"file_size", JsonValue::CreateNumberValue(static_cast<double>(result.file_size)));
    test.measurements.Insert(L"self", JsonValue::CreateBooleanValue(result.self));
    test.measurements.Insert(L"validated", JsonValue::CreateBooleanValue(result.validated));
    test.measurements.Insert(L"mapped", JsonValue::CreateBooleanValue(result.mapped));
    test.measurements.Insert(L"protected_segments", JsonValue::CreateBooleanValue(result.protected_segments));
    test.measurements.Insert(L"segment_count", JsonValue::CreateNumberValue(static_cast<double>(result.segment_count)));
    test.measurements.Insert(L"load_segments", JsonValue::CreateNumberValue(static_cast<double>(result.load_segments)));
    test.measurements.Insert(L"mapped_bytes", JsonValue::CreateNumberValue(static_cast<double>(result.mapped_bytes)));
    test.measurements.Insert(L"entry", JsonValue::CreateNumberValue(static_cast<double>(result.entry)));
    test.measurements.Insert(L"min_virtual_address", JsonValue::CreateNumberValue(static_cast<double>(result.min_virtual_address)));
    test.measurements.Insert(L"max_virtual_address", JsonValue::CreateNumberValue(static_cast<double>(result.max_virtual_address)));
    test.measurements.Insert(L"checksum_fnv1a", JsonValue::CreateNumberValue(static_cast<double>(result.checksum)));
    test.status = L"passed";
    test.detail = result.detail + L"\nArquivo: " + executablePath;
}

} // namespace Lab
