// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Lab {
using Bytes = std::vector<unsigned char>;
using RsaFields = std::map<std::string, Bytes>;
struct PackageKeys { RsaFields derived, fake; };
struct InstallProgress {
    std::atomic<bool> cancel{false};
    std::atomic<unsigned> percent{0};
    std::atomic<uint64_t> files{0}, bytes{0};
};
// The caller owns an empty staging directory. Never passes an installed game.
void ExtractPackage(const std::filesystem::path& package,
                    const std::filesystem::path& staging,
                    const PackageKeys& keys, InstallProgress& progress);
void ValidatePackageKeys(const PackageKeys& keys);
}
