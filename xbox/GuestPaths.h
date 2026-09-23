// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace Lab {
// OpenOrbis binaries may embed the process sandbox spelling of /app0.
// Resolve only that mount; never use the sandbox token as a host directory.
inline std::optional<std::string> SandboxAppPath(std::string_view path) {
    constexpr std::string_view prefix="/mnt/sandbox/";
    if(!path.starts_with(prefix)) return std::nullopt;
    path.remove_prefix(prefix.size());
    const auto slash=path.find('/');
    if(slash==std::string_view::npos || slash==0) return std::nullopt;
    for(auto c:path.substr(0,slash))
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'))
            return std::nullopt;
    const auto mount=path.substr(slash);
    if(mount!="/app0" && !mount.starts_with("/app0/")) return std::nullopt;
    return std::string(mount);
}
}
