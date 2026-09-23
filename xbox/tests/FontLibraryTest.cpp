// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestFontLibrary.h"
#include "GuestPaths.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace Lab;
static_assert(sizeof(long)!=8 || sizeof(FontAbi::Face)==sizeof(FT_FaceRec));
static_assert(sizeof(long)!=8 || offsetof(FontAbi::Face,glyph)==offsetof(FT_FaceRec,glyph));
static_assert(sizeof(long)!=8 || sizeof(FontAbi::Slot)==sizeof(FT_GlyphSlotRec));
static_assert(sizeof(long)!=8 || offsetof(FontAbi::Slot,bitmap)==offsetof(FT_GlyphSlotRec,bitmap));
static_assert(sizeof(long)!=8 || offsetof(FontAbi::Slot,bitmap_top)==offsetof(FT_GlyphSlotRec,bitmap_top));
static_assert(sizeof(long)!=8 || sizeof(FontAbi::Size)==sizeof(FT_SizeRec));
void Check(bool okay,char const* detail) { if(!okay) throw std::runtime_error(detail); }
void Render(std::filesystem::path const& path,std::uint32_t character) {
    std::ifstream input(path,std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    GuestFontLibrary fonts; Check(fonts.Error()==0,"init");
    std::uint64_t handle{};
    Check(fonts.NewMemoryFace(bytes,0,handle)==0,"load face");
    bytes.clear(); bytes.shrink_to_fit(); // Library must retain its own bytes.
    Check(fonts.SetPixelSizes(handle,32,32)==0,"size");
    auto index=fonts.CharIndex(handle,character); Check(index!=0,"character coverage");
    Check(fonts.LoadGlyph(handle,index,FT_LOAD_RENDER)==0,"render");
    auto* face=reinterpret_cast<FontAbi::Face const*>(handle);
    auto* slot=reinterpret_cast<FontAbi::Slot const*>(face->glyph);
    Check(slot && slot->bitmap.rows>0 && slot->bitmap.width>0,"guest bitmap layout");
    Check(slot->bitmap.pixel_mode==FT_PIXEL_MODE_GRAY && slot->advance.x>0,"guest glyph metrics");
    auto* pixels=reinterpret_cast<std::uint8_t const*>(slot->bitmap.buffer);
    bool ink=false;
    for(std::uint32_t y=0;y<slot->bitmap.rows;++y)
        for(std::uint32_t x=0;x<slot->bitmap.width;++x) ink|=pixels[y*slot->bitmap.pitch+x]!=0;
    Check(ink,"nonempty rendered pixels");
    Check(fonts.SetPixelSizes(handle,100000,1)!=0,"oversized glyph rejected");
    Check(fonts.DoneFace(handle)==0 && fonts.LoadGlyph(handle,index,0)!=0,"released handle rejected");
    std::uint8_t invalid[]{1,2,3,4}; handle=123;
    Check(fonts.NewMemoryFace(invalid,0,handle)!=0 && handle==0,"malformed font rejected");
    Check(fonts.CharIndex(0,character)==0,"invalid face");
}
int main(int argc,char** argv) {
    try {
        Check(argc==2,"font directory required");
        Check(SandboxAppPath("/mnt/sandbox/APOL00004_000/app0/assets/fonts/font.ttf")=="/app0/assets/fonts/font.ttf","sandbox app0 alias");
        Check(SandboxAppPath("/mnt/sandbox/OTHER0001_000/app0")=="/app0","generic process mount");
        Check(!SandboxAppPath("/mnt/sandbox/../app0/asset"),"sandbox traversal rejected");
        Check(!SandboxAppPath("/mnt/sandbox/APP_000/app0-other/asset"),"mount boundary");
        Check(!SandboxAppPath("/mnt/sandbox/APP_000/data/file"),"other mount rejected");
        Render(std::filesystem::path(argv[1])/"NotoSans-Regular.ttf",0xe7);
        Render(std::filesystem::path(argv[1])/"NotoSansCJK-Regular.ttc",0x4e2d);
        std::cout<<"LP64 guest structures, Latin/CJK rasterization and resource lifetime passed\n";
        return 0;
    } catch(std::exception const& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
