// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <vector>
#include <winrt/Windows.Data.Json.h>

namespace Lab {
struct Test {
    std::wstring id, title, status{L"not_run"}, detail, error;
    bool isolated{};
    double durationMs{}, startedAt{};
    winrt::Windows::Data::Json::JsonObject measurements;
};
struct Report {
    std::vector<Test> tests;
    std::wstring directory;
    std::wstring recoveryNotice;
    Report();
    void Save();
    void Load();
    winrt::Windows::Data::Json::JsonObject Json() const;
    std::wstring Summary() const;
};
double Now();
std::wstring StatusLabel(std::wstring const& status);
void WriteDurable(std::wstring const& path, std::string const& text);
}
