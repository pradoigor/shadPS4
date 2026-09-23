// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestFontAbi.h"
#include <memory>
#include <span>

namespace Lab {
// Portable native rasterizer and guest ABI mirrors, also exercised on the host.
class GuestFontLibrary {
public:
    GuestFontLibrary();
    ~GuestFontLibrary();
    GuestFontLibrary(GuestFontLibrary const&) = delete;
    GuestFontLibrary& operator=(GuestFontLibrary const&) = delete;
    int Error() const noexcept;
    int NewMemoryFace(std::span<std::uint8_t const> bytes, std::int64_t index, std::uint64_t& output);
    int DoneFace(std::uint64_t face);
    int SetPixelSizes(std::uint64_t face, std::uint32_t width, std::uint32_t height);
    std::uint32_t CharIndex(std::uint64_t face, std::uint64_t codepoint);
    int LoadGlyph(std::uint64_t face, std::uint32_t index, std::int32_t flags);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
