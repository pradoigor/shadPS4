// SPDX-License-Identifier: GPL-2.0-or-later
#include "Report.h"
#include "BuildInfo.h"
#include <windows.h>
#include <fileapifromapp.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>

using namespace winrt;
using namespace Windows::Data::Json;
namespace Lab {
double Now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::wstring StatusLabel(std::wstring const& s) {
    if (s == L"passed") return L"APROVADO";
    if (s == L"failed") return L"FALHOU";
    if (s == L"unavailable") return L"INDISPONÍVEL";
    if (s == L"running") return L"EM EXECUÇÃO";
    if (s == L"inconclusive") return L"INCONCLUSIVO";
    if (s == L"awaiting_confirmation") return L"AGUARDA CONFIRMAÇÃO";
    return L"NÃO EXECUTADO";
}
void WriteDurable(std::wstring const& path, std::string const& text) {
    auto temp = path + L".tmp";
    CREATEFILE2_EXTENDED_PARAMETERS params{sizeof(params)};
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    winrt::handle file{CreateFile2(temp.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, &params)};
    if (!file) throw_last_error();
    DWORD written{};
    check_bool(WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr));
    if (written != text.size()) throw hresult_error(E_FAIL, L"Gravação incompleta do relatório.");
    check_bool(FlushFileBuffers(file.get()));
    file.close();
    if (std::filesystem::exists(std::filesystem::path(path)))
        check_bool(ReplaceFileFromAppW(path.c_str(), temp.c_str(), nullptr, 0, nullptr, nullptr));
    else
        check_bool(MoveFileFromAppW(temp.c_str(), path.c_str()));
}
Report::Report() {
    directory = Windows::Storage::ApplicationData::Current().LocalFolder().Path();
    Load();
}
JsonObject Report::Json() const {
    JsonObject result;
    result.Insert(L"schema_version", JsonValue::CreateNumberValue(1));
    result.Insert(L"commit", JsonValue::CreateStringValue(XBOX_BUILD_COMMIT));
    result.Insert(L"upstream_commit", JsonValue::CreateStringValue(XBOX_UPSTREAM_COMMIT));
    result.Insert(L"emulator_ported", JsonValue::CreateBooleanValue(false));
    auto version = Windows::ApplicationModel::Package::Current().Id().Version();
    result.Insert(L"app_version", JsonValue::CreateStringValue(std::to_wstring(version.Major) + L"." + std::to_wstring(version.Minor) + L"." + std::to_wstring(version.Build) + L"." + std::to_wstring(version.Revision)));
    auto info = Windows::System::Profile::AnalyticsInfo::VersionInfo();
    result.Insert(L"device_family", JsonValue::CreateStringValue(info.DeviceFamily()));
    result.Insert(L"os_version_raw", JsonValue::CreateStringValue(info.DeviceFamilyVersion()));
    result.Insert(L"updated_at_unix", JsonValue::CreateNumberValue(Now()));
    JsonArray values;
    for (auto const& t : tests) {
        JsonObject item;
        item.Insert(L"id", JsonValue::CreateStringValue(t.id));
        item.Insert(L"title", JsonValue::CreateStringValue(t.title));
        item.Insert(L"status", JsonValue::CreateStringValue(t.status));
        item.Insert(L"detail", JsonValue::CreateStringValue(t.detail));
        item.Insert(L"error", JsonValue::CreateStringValue(t.error));
        item.Insert(L"isolated", JsonValue::CreateBooleanValue(t.isolated));
        item.Insert(L"duration_ms", JsonValue::CreateNumberValue(t.durationMs));
        item.Insert(L"started_at_unix", JsonValue::CreateNumberValue(t.startedAt));
        item.Insert(L"measurements", t.measurements);
        values.Append(item);
    }
    result.Insert(L"tests", values);
    return result;
}
void Report::Save() { WriteDurable(directory + L"\\report.json", to_string(Json().Stringify())); }
void Report::Load() {
    auto path = std::filesystem::path(directory) / L"report.json";
    if (!std::filesystem::exists(path)) return;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw hresult_error(E_FAIL, L"Não foi possível ler o relatório anterior.");
        std::string data{std::istreambuf_iterator<char>(input), {}};
        auto old = JsonObject::Parse(to_hstring(data));
        // Results from another executable are kept on disk as evidence, never claimed for this build.
        if (old.GetNamedString(L"commit", L"") != XBOX_BUILD_COMMIT) {
            WriteDurable(directory + L"\\previous-build-report.json", data);
            recoveryNotice = L"Build alterada. Relatório anterior preservado; testes reiniciados.";
            return;
        }
        for (auto const& value : old.GetNamedArray(L"tests")) {
            auto item = value.GetObject();
            for (auto& t : tests) if (t.id == item.GetNamedString(L"id")) {
                t.status = item.GetNamedString(L"status"); t.detail = item.GetNamedString(L"detail");
                t.error = item.GetNamedString(L"error", L"");
                t.durationMs = item.GetNamedNumber(L"duration_ms", 0);
                t.startedAt = item.GetNamedNumber(L"started_at_unix", 0);
                t.measurements = item.GetNamedObject(L"measurements", JsonObject{});
                if (t.status == L"running" || t.status == L"awaiting_confirmation") {
                    t.status = L"inconclusive";
                    t.detail += L"\nExecução anterior interrompida ou sem confirmação. Execute novamente.";
                    recoveryNotice = L"Teste interrompido recuperado como inconclusivo.";
                }
            }
        }
    } catch (hresult_error const& e) {
        recoveryNotice = L"Relatório anterior inválido: " + std::wstring(e.message());
        std::filesystem::copy_file(path, std::filesystem::path(directory) / L"invalid-report.json", std::filesystem::copy_options::overwrite_existing);
    }
}
std::wstring Report::Summary() const {
    auto info = Windows::System::Profile::AnalyticsInfo::VersionInfo();
    return std::wstring(info.DeviceFamily()) + L" · sistema " + std::wstring(info.DeviceFamilyVersion()) +
        L" · build " + std::wstring(XBOX_BUILD_COMMIT).substr(0, 12);
}
}
