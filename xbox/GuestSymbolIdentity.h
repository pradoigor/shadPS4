// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Lab {
using GuestSymbolMap = std::unordered_map<std::string, std::uint64_t>;

// The # IDs in an ELF symbol refer to that ELF's own dynamic descriptors.
// Match core/linker.cpp: library name/version and module name (not module version).
inline std::string CanonicalGuestSymbol(std::string_view encoded,
    std::vector<std::string> const& libraries,
    std::vector<std::string> const& modules) {
    const auto first = encoded.find('#');
    if (first == std::string_view::npos) return std::string(encoded);
    const auto second = encoded.find('#', first + 1);
    auto descriptor = [](std::string_view id, auto const& rows) -> std::string_view {
        for (auto const& row : rows) {
            const auto equal = row.find('=');
            if (equal != std::string::npos && std::string_view(row).substr(0, equal) == id)
                return std::string_view(row).substr(equal + 1);
        }
        return {};
    };
    if (second != std::string_view::npos) {
        const auto library = descriptor(encoded.substr(first + 1, second - first - 1), libraries);
        const auto module = descriptor(encoded.substr(second + 1), modules);
        if (!library.empty() && !module.empty())
            return std::string(encoded.substr(0, first)) + "#@" + std::string(library) +
                   "#@" + std::string(module.substr(0, module.rfind('@')));
    }
    // Preserve the unresolved IDs for diagnostics, but never bind them globally.
    return std::string(encoded.substr(0, first)) + "#?" + std::string(encoded.substr(first + 1));
}

inline std::uint64_t FindGuestSymbol(std::string const& symbol,
                                    GuestSymbolMap const& exports) noexcept {
    if (symbol.find('#') != std::string::npos && symbol.find("#@") == std::string::npos)
        return 0;
    const auto found = exports.find(symbol);
    return found == exports.end() ? 0 : found->second;
}

inline void AddGuestSymbol(GuestSymbolMap& exports, std::string const& symbol,
                           std::uint64_t address) {
    const auto [it, inserted] = exports.emplace(symbol, address);
    // Duplicate definitions at different addresses are ambiguous, not first-wins.
    if (!inserted && it->second != address) it->second = 0;
}

inline std::uint64_t FindUniqueGuestNid(std::string_view nid,
                                      GuestSymbolMap const& exports) noexcept {
    std::uint64_t result{};
    for (auto const& [symbol, address] : exports) {
        if (std::string_view(symbol).substr(0, symbol.find('#')) != nid) continue;
        if (!address || (result && result != address)) return 0;
        result = address;
    }
    return result;
}
} // namespace Lab
