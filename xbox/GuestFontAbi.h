// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace Lab::FontAbi {
// PS4 uses LP64; Windows x64 uses LLP64. Never expose native FT_Face or
// FT_GlyphSlot structs to guest code: every FreeType long below is 64 bits.
struct Generic { std::uint64_t data{}, finalizer{}; };
struct Vector { std::int64_t x{}, y{}; };
struct BBox { std::int64_t xMin{}, yMin{}, xMax{}, yMax{}; };
struct Bitmap {
    std::uint32_t rows{}, width{};
    std::int32_t pitch{};
    std::uint64_t buffer{};
    std::uint16_t num_grays{};
    std::uint8_t pixel_mode{}, palette_mode{};
    std::uint64_t palette{};
};
struct Metrics {
    std::int64_t width{}, height{}, horiBearingX{}, horiBearingY{}, horiAdvance{};
    std::int64_t vertBearingX{}, vertBearingY{}, vertAdvance{};
};
struct Outline {
    std::int16_t n_contours{}, n_points{};
    std::uint64_t points{}, tags{}, contours{};
    std::int32_t flags{};
};
struct Slot {
    std::uint64_t library{}, face{}, next{};
    std::uint32_t glyph_index{};
    Generic generic;
    Metrics metrics;
    std::int64_t linearHoriAdvance{}, linearVertAdvance{};
    Vector advance;
    std::uint32_t format{};
    Bitmap bitmap;
    std::int32_t bitmap_left{}, bitmap_top{};
    Outline outline;
    std::uint32_t num_subglyphs{};
    std::uint64_t subglyphs{}, control_data{};
    std::int64_t control_len{}, lsb_delta{}, rsb_delta{};
    std::uint64_t other{}, internal{};
};
struct SizeMetrics {
    std::uint16_t x_ppem{}, y_ppem{};
    std::int64_t x_scale{}, y_scale{}, ascender{}, descender{}, height{}, max_advance{};
};
struct Size { std::uint64_t face{}; Generic generic; SizeMetrics metrics; std::uint64_t internal{}; };
struct BitmapSize { std::int16_t height{}, width{}; std::int64_t size{}, x_ppem{}, y_ppem{}; };
struct CharMap {
    std::uint64_t face{};
    std::uint32_t encoding{};
    std::uint16_t platform_id{}, encoding_id{};
};
struct Face {
    std::int64_t num_faces{}, face_index{}, face_flags{}, style_flags{}, num_glyphs{};
    std::uint64_t family_name{}, style_name{};
    std::int32_t num_fixed_sizes{};
    std::uint64_t available_sizes{};
    std::int32_t num_charmaps{};
    std::uint64_t charmaps{};
    Generic generic;
    BBox bbox;
    std::uint16_t units_per_EM{};
    std::int16_t ascender{}, descender{}, height{}, max_advance_width{}, max_advance_height{};
    std::int16_t underline_position{}, underline_thickness{};
    std::uint64_t glyph{}, size{}, charmap{};
    std::uint64_t private_fields[9]{};
};
static_assert(sizeof(Face) == 248 && offsetof(Face, glyph) == 152);
static_assert(sizeof(Slot) == 304 && offsetof(Slot, bitmap) == 152);
static_assert(offsetof(Slot, advance) == 128 && offsetof(Slot, bitmap_top) == 196);
static_assert(sizeof(Bitmap) == 40 && sizeof(Size) == 88 && sizeof(CharMap) == 16);
}
