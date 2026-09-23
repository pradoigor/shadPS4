// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestFreeType.h"
#include "GuestFontLibrary.h"
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <fstream>
#include <iterator>

namespace Lab {
struct GuestFontState {
    std::mutex mutex;
    std::unordered_map<std::uint64_t,std::unique_ptr<GuestFontLibrary>> libraries;
    std::unordered_map<std::uint64_t,std::uint64_t> faceLibraries;
    unsigned failures{};
};
namespace {
enum class Op { Init, Done, NewFace, NewMemory, DoneFace, PixelSizes, CharIndex, LoadGlyph };
constexpr int InvalidArgument=6, InvalidLibrary=0x21, InvalidFace=0x23, CannotOpen=1;
GuestFontState& State(HleDispatcher& d) {
    static std::mutex creation;
    std::lock_guard lock(creation);
    if(!d.FontState()) d.FontState()=std::make_shared<GuestFontState>();
    return *d.FontState();
}
std::filesystem::path FontPath(HleDispatcher& d,std::string const& guest) {
    std::filesystem::path host;
    if(d.GuestFilePath(guest,false,host)) return host;
    wchar_t const* substitute=nullptr;
    if(guest=="/preinst/common/font/DFHEI5-SONY.ttf") substitute=L"NotoSansCJK-Regular.ttc";
    if(guest=="/system_ex/app/NPXS20113/bdjstack/lib/fonts/SCE-PS3-RD-R-LATIN.TTF") substitute=L"NotoSans-Regular.ttf";
    if(!substitute) return {};
    d.GraphicsLog("FreeType: fonte de sistema substituída por Noto (OFL): "+guest);
    return std::filesystem::path(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()) / L"Fonts" / substitute;
}
template<Op operation> std::uint64_t Call(HleDispatcher& d,GuestCallFrame const& f) noexcept {
    try {
        auto& state=State(d); std::lock_guard lock(state.mutex);
        if constexpr(operation==Op::Init) {
            auto* out=static_cast<std::uint64_t*>(d.GuestWritable(f,f.gpr[0],8));
            if(!out) return InvalidArgument;
            *out=0;
            auto library=std::make_unique<GuestFontLibrary>();
            if(library->Error()) return library->Error();
            auto handle=reinterpret_cast<std::uint64_t>(library.get());
            state.libraries.emplace(handle,std::move(library)); *out=handle;
            d.GraphicsLog("FreeType: biblioteca inicializada; ABI PS4 LP64 adaptada.");
            return 0;
        } else if constexpr(operation==Op::Done) {
            auto it=state.libraries.find(f.gpr[0]); if(it==state.libraries.end()) return InvalidLibrary;
            std::erase_if(state.faceLibraries,[&](auto const& row){return row.second==f.gpr[0];});
            state.libraries.erase(it); return 0;
        } else if constexpr(operation==Op::NewFace || operation==Op::NewMemory) {
            auto it=state.libraries.find(f.gpr[0]); if(it==state.libraries.end()) return InvalidLibrary;
            constexpr auto outputIndex=operation==Op::NewFace?3:4;
            auto* out=static_cast<std::uint64_t*>(d.GuestWritable(f,f.gpr[outputIndex],8));
            if(!out) return InvalidArgument;
            *out=0;
            std::uint64_t face{}; int result{};
            if constexpr(operation==Op::NewFace) {
                std::string guest; if(!d.GuestString(f.gpr[1],guest)) return InvalidArgument;
                auto path=FontPath(d,guest);
                if(path.empty()) { d.GraphicsLog("FreeType: caminho não mapeado: "+guest); return CannotOpen; }
                std::error_code ec; auto size=std::filesystem::file_size(path,ec);
                if(ec || size==0 || size>64*1024*1024) { d.GraphicsLog("FreeType: arquivo ausente ou inválido: "+guest); return CannotOpen; }
                std::ifstream input(path,std::ios::binary);
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
                if(!input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size))) return CannotOpen;
                result=it->second->NewMemoryFace(bytes,static_cast<std::int64_t>(f.gpr[2]),face);
                d.GraphicsLog("FreeType: FT_New_Face "+guest+" result="+std::to_string(result));
            } else {
                auto count=f.gpr[2]; if(count==0 || count>64*1024*1024) return InvalidArgument;
                auto* bytes=static_cast<std::uint8_t const*>(d.GuestReadable(f,f.gpr[1],static_cast<std::size_t>(count)));
                if(!bytes) return InvalidArgument;
                result=it->second->NewMemoryFace({bytes,static_cast<std::size_t>(count)},static_cast<std::int64_t>(f.gpr[3]),face);
            }
            if(!result) {
                try { state.faceLibraries.emplace(face,f.gpr[0]); }
                catch(...) { it->second->DoneFace(face); throw; }
                *out=face;
            }
            return result;
        } else {
            auto owner=state.faceLibraries.find(f.gpr[0]);
            if(owner==state.faceLibraries.end()) return operation==Op::CharIndex?0:InvalidFace;
            auto& library=*state.libraries.at(owner->second);
            if constexpr(operation==Op::CharIndex) return library.CharIndex(f.gpr[0],f.gpr[1]);
            if constexpr(operation==Op::DoneFace) { auto result=library.DoneFace(f.gpr[0]); state.faceLibraries.erase(owner); return result; }
            int result{};
            if constexpr(operation==Op::PixelSizes) result=library.SetPixelSizes(f.gpr[0],static_cast<std::uint32_t>(f.gpr[1]),static_cast<std::uint32_t>(f.gpr[2]));
            if constexpr(operation==Op::LoadGlyph) result=library.LoadGlyph(f.gpr[0],static_cast<std::uint32_t>(f.gpr[1]),static_cast<std::int32_t>(f.gpr[2]));
            if(result && state.failures++<8) d.GraphicsLog("FreeType: operação de glifo falhou, error="+std::to_string(result));
            return result;
        }
    } catch(...) { d.GraphicsLog("FreeType: falha de alocação ou leitura durante chamada HLE."); return 0x40; }
}
}
HleHandler LookupFreeTypeHandler(std::string_view name) noexcept {
    if(name=="FT_Init_FreeType") return &Call<Op::Init>;
    if(name=="FT_Done_FreeType") return &Call<Op::Done>;
    if(name=="FT_New_Face") return &Call<Op::NewFace>;
    if(name=="FT_New_Memory_Face") return &Call<Op::NewMemory>;
    if(name=="FT_Done_Face") return &Call<Op::DoneFace>;
    if(name=="FT_Set_Pixel_Sizes") return &Call<Op::PixelSizes>;
    if(name=="FT_Get_Char_Index") return &Call<Op::CharIndex>;
    if(name=="FT_Load_Glyph") return &Call<Op::LoadGlyph>;
    return nullptr;
}
}
