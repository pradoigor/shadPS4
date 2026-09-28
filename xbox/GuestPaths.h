// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
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

// Resolve paths against the guest's app0 working directory, regardless of
// which HLE file or module API received them. Keep every path inside its mount.
inline std::optional<std::string> NormalizeGuestPath(std::string_view rawPath) {
    if (rawPath.empty() || rawPath.find('\\') != std::string_view::npos ||
        rawPath.find(':') != std::string_view::npos ||
        rawPath.find('\0') != std::string_view::npos)
        return std::nullopt;

    std::string path;
    path.reserve(rawPath.size() + 6);
    for (char character : rawPath) {
        if (character != '/' || path.empty() || path.back() != '/')
            path.push_back(character);
    }
    if (auto alias = SandboxAppPath(path)) path = *alias;
    else if (path.front() != '/') path.insert(0, "/app0/");

    std::vector<std::string_view> components;
    for (std::size_t start = 1; start < path.size();) {
        const auto end = path.find('/', start);
        const auto component = std::string_view(path).substr(
            start, end == std::string::npos ? end : end - start);
        if (component == "..") {
            if (components.size() <= 1) return std::nullopt;
            components.pop_back();
        } else if (!component.empty() && component != ".") {
            components.push_back(component);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (components.empty()) return std::nullopt;

    std::string normalized;
    normalized.reserve(path.size());
    for (auto component : components) {
        normalized.push_back('/');
        normalized.append(component);
    }
    return normalized;
}
}
