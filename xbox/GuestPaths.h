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
    // Retail/OpenOrbis packages also expose app0 below pfsmnt/<TITLEID>-app0.
    // Only the app0 mount is aliased; the title is never used as a host path.
    if (path.starts_with("pfsmnt/")) {
        path.remove_prefix(sizeof("pfsmnt/") - 1);
        const auto slash = path.find('/');
        const auto mount = path.substr(0, slash);
        constexpr std::string_view suffix = "-app0";
        if (mount.size() != 9 + suffix.size() || !mount.ends_with(suffix))
            return std::nullopt;
        for (char c : mount.substr(0, 9))
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
                return std::nullopt;
        return slash == std::string_view::npos ? std::string("/app0")
            : std::string("/app0") + std::string(path.substr(slash));
    }
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
