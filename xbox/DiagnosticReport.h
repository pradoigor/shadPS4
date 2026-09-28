// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string_view>
#include <windows.h>

namespace Lab {

inline bool AppendDiagnosticLine(std::filesystem::path const& path,
                                 std::string_view line) noexcept {
    if (path.empty() || line.size() > MAXDWORD - 1) return false;
    CREATEFILE2_EXTENDED_PARAMETERS parameters{sizeof(parameters)};
    parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    HANDLE file = CreateFile2(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              OPEN_ALWAYS, &parameters);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::string record;
    try {
        record.assign(line);
        record.push_back('\n');
    } catch (...) {
        CloseHandle(file);
        return false;
    }
    DWORD written{};
    const bool success = WriteFile(file, record.data(),
                                   static_cast<DWORD>(record.size()),
                                   &written, nullptr) && written == record.size();
    if (success) FlushFileBuffers(file);
    CloseHandle(file);
    return success;
}

} // namespace Lab
