// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestFontLibrary.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <limits>
#include <unordered_map>
#include <vector>

namespace Lab {
namespace {
template<class T> std::uint64_t Address(T* value) { return reinterpret_cast<std::uint64_t>(value); }
struct FaceOwner {
    FT_Face native{};
    std::vector<std::uint8_t> data;
    FontAbi::Face face;
    FontAbi::Slot slot;
    FontAbi::Size size;
    std::vector<FontAbi::Vector> points;
    std::vector<FontAbi::BitmapSize> strikes;
    std::vector<FontAbi::CharMap> maps;
    std::vector<std::uint64_t> mapPointers;
    ~FaceOwner() { if (native) FT_Done_Face(native); }
    void Sync() {
        auto n = native;
        face.num_faces=n->num_faces; face.face_index=n->face_index;
        face.face_flags=n->face_flags; face.style_flags=n->style_flags; face.num_glyphs=n->num_glyphs;
        face.family_name=Address(n->family_name); face.style_name=Address(n->style_name);
        face.bbox={n->bbox.xMin,n->bbox.yMin,n->bbox.xMax,n->bbox.yMax};
        face.units_per_EM=n->units_per_EM; face.ascender=n->ascender; face.descender=n->descender;
        face.height=n->height; face.max_advance_width=n->max_advance_width;
        face.max_advance_height=n->max_advance_height; face.underline_position=n->underline_position;
        face.underline_thickness=n->underline_thickness;
        strikes.resize(n->num_fixed_sizes);
        for (int i=0; i<n->num_fixed_sizes; ++i) {
            auto const& s=n->available_sizes[i]; strikes[i]={s.height,s.width,s.size,s.x_ppem,s.y_ppem};
        }
        face.num_fixed_sizes=n->num_fixed_sizes; face.available_sizes=strikes.empty()?0:Address(strikes.data());
        maps.resize(n->num_charmaps); mapPointers.resize(n->num_charmaps); face.charmap=0;
        for (int i=0; i<n->num_charmaps; ++i) {
            auto m=n->charmaps[i]; maps[i]={Address(&face),static_cast<std::uint32_t>(m->encoding),m->platform_id,m->encoding_id};
            mapPointers[i]=Address(&maps[i]); if(m==n->charmap) face.charmap=mapPointers[i];
        }
        face.num_charmaps=n->num_charmaps; face.charmaps=mapPointers.empty()?0:Address(mapPointers.data());
        face.size=n->size?Address(&size):0;
        if(n->size) {
            auto const& m=n->size->metrics; size.face=Address(&face);
            size.metrics={m.x_ppem,m.y_ppem,m.x_scale,m.y_scale,m.ascender,m.descender,m.height,m.max_advance};
        }
        face.glyph=n->glyph?Address(&slot):0;
        if(!n->glyph) return;
        auto g=n->glyph;
        slot.face=Address(&face); slot.glyph_index=g->glyph_index;
        auto const& m=g->metrics;
        slot.metrics={m.width,m.height,m.horiBearingX,m.horiBearingY,m.horiAdvance,m.vertBearingX,m.vertBearingY,m.vertAdvance};
        slot.linearHoriAdvance=g->linearHoriAdvance; slot.linearVertAdvance=g->linearVertAdvance;
        slot.advance={g->advance.x,g->advance.y}; slot.format=g->format;
        auto const& b=g->bitmap;
        slot.bitmap={b.rows,b.width,b.pitch,Address(b.buffer),b.num_grays,b.pixel_mode,b.palette_mode,Address(b.palette)};
        slot.bitmap_left=g->bitmap_left; slot.bitmap_top=g->bitmap_top;
        points.resize(g->outline.n_points > 0 ? g->outline.n_points : 0);
        for(std::size_t i=0;i<points.size();++i) points[i]={g->outline.points[i].x,g->outline.points[i].y};
        slot.outline={static_cast<std::int16_t>(g->outline.n_contours),static_cast<std::int16_t>(g->outline.n_points),points.empty()?0:Address(points.data()),
                      Address(g->outline.tags),Address(g->outline.contours),g->outline.flags};
        slot.lsb_delta=g->lsb_delta; slot.rsb_delta=g->rsb_delta;
    }
};
}
struct GuestFontLibrary::Impl {
    FT_Library library{};
    int error{};
    std::unordered_map<std::uint64_t,std::unique_ptr<FaceOwner>> faces;
    Impl() : error(FT_Init_FreeType(&library)) {}
    ~Impl() { faces.clear(); if(library) FT_Done_FreeType(library); }
};
GuestFontLibrary::GuestFontLibrary() : impl_(std::make_unique<Impl>()) {}
GuestFontLibrary::~GuestFontLibrary() = default;
int GuestFontLibrary::Error() const noexcept { return impl_->error; }
int GuestFontLibrary::NewMemoryFace(std::span<std::uint8_t const> bytes, std::int64_t index, std::uint64_t& output) {
    output=0;
    if(impl_->error) return impl_->error;
    if(bytes.empty() || bytes.size()>64*1024*1024 || index<0 || index>std::numeric_limits<FT_Long>::max())
        return FT_Err_Invalid_Argument;
    auto owner=std::make_unique<FaceOwner>(); owner->data.assign(bytes.begin(),bytes.end());
    int error=FT_New_Memory_Face(impl_->library,owner->data.data(),static_cast<FT_Long>(owner->data.size()),
                               static_cast<FT_Long>(index),&owner->native);
    if(error) return error;
    owner->slot.library=Address(this); owner->Sync();
    const auto handle=Address(&owner->face);
    impl_->faces.emplace(handle,std::move(owner)); output=handle;
    return 0;
}
int GuestFontLibrary::DoneFace(std::uint64_t face) {
    return impl_->faces.erase(face)?0:FT_Err_Invalid_Face_Handle;
}
int GuestFontLibrary::SetPixelSizes(std::uint64_t face,std::uint32_t width,std::uint32_t height) {
    auto it=impl_->faces.find(face); if(it==impl_->faces.end()) return FT_Err_Invalid_Face_Handle;
    // Bound glyph memory allocation for untrusted guest requests.
    if(width>4096 || height>4096) return FT_Err_Invalid_Pixel_Size;
    auto error=FT_Set_Pixel_Sizes(it->second->native,width,height); it->second->Sync(); return error;
}
std::uint32_t GuestFontLibrary::CharIndex(std::uint64_t face,std::uint64_t codepoint) {
    auto it=impl_->faces.find(face); if(it==impl_->faces.end() || codepoint>0x10ffff) return 0;
    return FT_Get_Char_Index(it->second->native,static_cast<FT_ULong>(codepoint));
}
int GuestFontLibrary::LoadGlyph(std::uint64_t face,std::uint32_t index,std::int32_t flags) {
    auto it=impl_->faces.find(face); if(it==impl_->faces.end()) return FT_Err_Invalid_Face_Handle;
    auto error=FT_Load_Glyph(it->second->native,index,flags); it->second->Sync(); return error;
}
}
