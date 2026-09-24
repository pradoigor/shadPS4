// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace Lab {

// Piglet's PS4 shader container starts with 71 bc 91 e8, followed by three
// little-endian u32 fields. The final field is the byte length of the GLSL
// source immediately following the 16-byte header; the remaining bytes are
// Piglet-specific microcode that ANGLE cannot consume.
inline bool DecodePigletShaderSource(void const* binary, std::size_t length,
                                     std::string_view& source) noexcept {
    constexpr std::uint8_t Magic[]{0x71, 0xbc, 0x91, 0xe8};
    constexpr std::size_t HeaderSize = 16;
    constexpr std::size_t MaxSourceSize = 1024 * 1024;
    source = {};
    if (!binary || length < HeaderSize)
        return false;

    auto const* bytes = static_cast<std::uint8_t const*>(binary);
    if (std::memcmp(bytes, Magic, sizeof(Magic)) != 0)
        return false;

    const auto sourceSize = std::uint32_t(bytes[12]) |
                            (std::uint32_t(bytes[13]) << 8) |
                            (std::uint32_t(bytes[14]) << 16) |
                            (std::uint32_t(bytes[15]) << 24);
    if (sourceSize == 0 || sourceSize > MaxSourceSize ||
        sourceSize > length - HeaderSize)
        return false;

    std::string_view candidate(
        reinterpret_cast<char const*>(bytes + HeaderSize), sourceSize);
    if (candidate.find("void main") == std::string_view::npos ||
        candidate.find('\0') != std::string_view::npos)
        return false;
    for (unsigned char value : candidate) {
        if (value < 0x20 && value != '\n' && value != '\r' && value != '\t')
            return false;
    }
    source = candidate;
    return true;
}

// Apollo's SDL/Piglet fragment shader treats the uploaded byte stream as
// ABGR but only swaps red and blue. Its RGBA upload is actually laid out as
// [alpha, blue, green, red], so ANGLE sees opaque texels where alpha is zero.
// Match this one known shader exactly; other Piglet shaders are untouched.
inline bool AdaptApolloTextureShader(std::string_view source,
                                     std::string& adapted) {
    constexpr std::string_view sample = "vec4 abgr = texture2D(u_texture, v_texCoord);";
    constexpr std::string_view assign = "gl_FragColor = abgr;";
    constexpr std::string_view red = "gl_FragColor.r = abgr.b;";
    constexpr std::string_view blue = "gl_FragColor.b = abgr.r;";
    constexpr std::string_view tint = "gl_FragColor *= v_color;";
    if (source.find(sample) == std::string_view::npos ||
        source.find(assign) == std::string_view::npos ||
        source.find(red) == std::string_view::npos ||
        source.find(blue) == std::string_view::npos ||
        source.find(tint) == std::string_view::npos) return false;
    adapted.assign(source);
    adapted.replace(adapted.find(assign), assign.size(),
                    "gl_FragColor = abgr.abgr;");
    adapted.erase(adapted.find(red), red.size());
    adapted.erase(adapted.find(blue), blue.size());
    return true;
}

} // namespace Lab
