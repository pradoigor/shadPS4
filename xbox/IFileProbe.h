// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace Core::FileSys {
class IFile;
}

namespace Lab {

struct IFileProbeResult {
    bool passed{};
    std::uint32_t file_size{};
    std::uint32_t bytes_read{};
    std::uint32_t segment_id{};
    std::uint64_t elf_entry{};
};

IFileProbeResult ProbeUwpIFileAdapter(const std::wstring& directory);
std::unique_ptr<Core::FileSys::IFile> MakeUwpFile(const std::wstring& path);

} // namespace Lab
