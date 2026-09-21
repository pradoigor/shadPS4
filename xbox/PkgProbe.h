// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>

namespace Lab {

struct PkgProbeResult {
    bool recognized{};
    std::wstring detail;
};

// Reads the public PKG container header and entry table without decrypting or
// extracting content. This is deliberately a metadata-only probe.
PkgProbeResult ProbePkg(std::filesystem::path const& path);

} // namespace Lab
