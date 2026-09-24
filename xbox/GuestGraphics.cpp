// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestGraphics.h"
#include "AngleVideo.h"
#include "PigletShaderBinary.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <tuple>
#include <type_traits>

namespace Lab {
namespace {
constexpr std::uint64_t OrbisEnosys = 0x8002004Eull;
constexpr std::uint64_t OrbisEnoent = 0x80020002ull;
struct Cursor {
    HleDispatcher& dispatcher;
    GuestCallFrame const& frame;
    std::string_view name;
    unsigned integers{}, floats{}, stack{};
    bool valid{true};
    std::uint64_t Integer() noexcept {
        if (integers < 6) return frame.gpr[integers++];
        auto address = frame.guest_stack + 8 + stack++ * 8;
        auto* value = static_cast<std::uint64_t const*>(dispatcher.GuestReadable(frame, address, 8));
        if (!value) { valid = false; return 0; }
        return *value;
    }
    template<class T> T Decode() noexcept {
        if constexpr (std::is_floating_point_v<T>) {
            T value{};
            if (floats < 8) { std::memcpy(&value, frame.xmm[floats++], sizeof(value)); return value; }
            auto address = frame.guest_stack + 8 + stack++ * 8;
            auto* data = dispatcher.GuestReadable(frame, address, sizeof(value));
            if (!data) { valid = false; return value; }
            std::memcpy(&value, data, sizeof(value)); return value;
        } else {
            const auto raw = Integer();
            if constexpr (std::is_pointer_v<T>) {
                if (raw && !name.starts_with("egl") && name != "glVertexAttribPointer" &&
                    !dispatcher.GuestReadable(frame, raw, 1)) valid = false;
                return reinterpret_cast<T>(raw);
            } else return static_cast<T>(raw);
        }
    }
};
template<class T> struct Forwarder;
template<class R, class... A> struct Forwarder<R (*)(A...)> {
    static std::uint64_t Call(HleDispatcher& dispatcher, GuestCallFrame const& frame,
                              FARPROC address, std::string_view name) noexcept {
        Cursor cursor{dispatcher, frame, name};
        std::tuple<A...> arguments{cursor.Decode<A>()...};
        if (!cursor.valid) return 0;
        auto function = reinterpret_cast<R (*)(A...)>(address);
        if constexpr (std::is_void_v<R>) {
            std::apply(function, arguments); return 0;
        } else {
            auto result = std::apply(function, arguments);
            if constexpr (std::is_pointer_v<R>) return reinterpret_cast<std::uint64_t>(result);
            else return static_cast<std::uint64_t>(result);
        }
    }
};

template<auto Function> std::uint64_t ForwardGl(HleDispatcher& dispatcher,
                                                 GuestCallFrame const& frame,
                                                 char const* name) noexcept {
    auto* graphics = dispatcher.Graphics();
    if (!graphics || !graphics->Ready()) return OrbisEnosys;
    auto address = GetProcAddress(graphics->GlesModule(), name);
    if (!address) return OrbisEnosys;
    return Forwarder<decltype(Function)>::Call(dispatcher, frame, address, name);
}

#define GL_FUNCTION(name) \
std::uint64_t Call_##name(HleDispatcher& d, GuestCallFrame const& f) noexcept { \
    return ForwardGl<&::name>(d, f, #name); \
}
#include "GuestGlFunctions.inc"
#undef GL_FUNCTION

std::uint64_t Call_glShaderBinary(HleDispatcher&, GuestCallFrame const&) noexcept;
struct NamedHandler { std::string_view name; HleHandler handler; };
constexpr NamedHandler GlHandlers[] = {
#define GL_FUNCTION(name) {#name, &Call_##name},
#include "GuestGlFunctions.inc"
#undef GL_FUNCTION
    {"glShaderBinary", &Call_glShaderBinary},
};

template<auto Function> std::uint64_t ForwardEgl(HleDispatcher& dispatcher,
                                                  GuestCallFrame const& frame,
                                                  char const* name) noexcept {
    auto* graphics = dispatcher.Graphics();
    if (!graphics || !graphics->Ready()) return 0;
    auto address = GetProcAddress(graphics->EglModule(), name);
    if (!address) return 0;
    return Forwarder<decltype(Function)>::Call(dispatcher, frame, address, name);
}
#define EGL_FORWARD(name) \
std::uint64_t Call_##name(HleDispatcher& d, GuestCallFrame const& f) noexcept { \
    return ForwardEgl<&::name>(d, f, #name); \
}
EGL_FORWARD(eglBindAPI)
EGL_FORWARD(eglGetConfigAttrib)
EGL_FORWARD(eglGetError)
EGL_FORWARD(eglQueryAPI)
EGL_FORWARD(eglQueryString)
EGL_FORWARD(eglSwapInterval)
EGL_FORWARD(eglWaitGL)
EGL_FORWARD(eglWaitNative)
EGL_FORWARD(eglCreatePbufferSurface)
#undef EGL_FORWARD

std::uint64_t Call_eglSwapBuffers(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    auto result = ForwardEgl<&::eglSwapBuffers>(d, f, "eglSwapBuffers");
    static thread_local std::uint64_t frames{};
    if (result && ++frames <= 3) {
        d.GraphicsLog("EGL: quadro apresentado pelo homebrew #" + std::to_string(frames));
    } else if (!result) {
        d.GraphicsLog("EGL: eglSwapBuffers falhou");
    }
    return result;
}

std::uint64_t Call_glShaderBinary(HleDispatcher& d,
                                  GuestCallFrame const& f) noexcept {
    auto* graphics = d.Graphics();
    if (!graphics || !graphics->Ready()) return OrbisEnosys;
    auto binaryFunction = GetProcAddress(graphics->GlesModule(), "glShaderBinary");
    if (!binaryFunction) return OrbisEnosys;

    const auto count = static_cast<std::int32_t>(f.gpr[0]);
    const auto length = static_cast<std::int32_t>(f.gpr[4]);
    if (count <= 0 || count > 256 || length <= 0 ||
        length > 16 * 1024 * 1024)
        return Forwarder<decltype(&::glShaderBinary)>::Call(
            d, f, binaryFunction, "glShaderBinary");

    auto const* shaderIds = static_cast<GLuint const*>(
        d.GuestReadable(f, f.gpr[1],
                        static_cast<std::size_t>(count) * sizeof(GLuint)));
    auto const* binary = static_cast<std::uint8_t const*>(
        d.GuestReadable(f, f.gpr[3], static_cast<std::size_t>(length)));
    if (!shaderIds || !binary)
        return 0;

    std::string_view source;
    if (!DecodePigletShaderSource(binary, static_cast<std::size_t>(length),
                                  source))
        return Forwarder<decltype(&::glShaderBinary)>::Call(
            d, f, binaryFunction, "glShaderBinary");

    auto sourceFunction = GetProcAddress(graphics->GlesModule(), "glShaderSource");
    auto compileFunction = GetProcAddress(graphics->GlesModule(), "glCompileShader");
    auto statusFunction = GetProcAddress(graphics->GlesModule(), "glGetShaderiv");
    if (!sourceFunction || !compileFunction || !statusFunction) {
        d.GraphicsLog("Piglet: ANGLE não expõe as funções para compilar o GLSL extraído.");
        return 0;
    }

    auto setSource = reinterpret_cast<decltype(&::glShaderSource)>(sourceFunction);
    auto compile = reinterpret_cast<decltype(&::glCompileShader)>(compileFunction);
    auto getShader = reinterpret_cast<decltype(&::glGetShaderiv)>(statusFunction);
    const GLchar* sourceText = source.data();
    const GLint sourceLength = static_cast<GLint>(source.size());
    static std::atomic_uint32_t translated{};
    const auto ordinal = translated.fetch_add(1, std::memory_order_relaxed) + 1;
    for (std::int32_t i = 0; i < count; ++i) {
        const auto shader = shaderIds[i];
        if (!shader) continue;
        setSource(shader, 1, &sourceText, &sourceLength);
        compile(shader);
        GLint compiled{};
        getShader(shader, GL_COMPILE_STATUS, &compiled);

        if (ordinal <= 8) {
            char message[192]{};
            std::snprintf(message, sizeof(message),
                          "Piglet: binário convertido para GLSL; format=0x%llx, "
                          "bytes=%d, GLSL=%d, compile=%s",
                          static_cast<unsigned long long>(f.gpr[2]), length,
                          sourceLength, compiled ? "aprovado" : "falhou");
            d.GraphicsLog(message);
        }
        if (!compiled && ordinal <= 3) {
            std::array<GLchar, 512> info{};
            GLsizei written{};
            auto getInfo = reinterpret_cast<decltype(&::glGetShaderInfoLog)>(
                GetProcAddress(graphics->GlesModule(), "glGetShaderInfoLog"));
            if (getInfo) {
                getInfo(shader, static_cast<GLsizei>(info.size()), &written,
                        info.data());
                if (written > 0) {
                    d.GraphicsLog("Piglet: erro do compilador ANGLE:");
                    d.GraphicsLog(std::string_view(
                        info.data(), (std::min)(static_cast<std::size_t>(written),
                                                info.size() - 1)));
                }
            }
        }
    }
    return 0;
}

std::uint64_t Call_sceKernelLoadStartModule(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    auto* graphics = d.Graphics();
    if (!graphics || !graphics->Ready()) return OrbisEnosys;
    std::string path;
    if (!d.GuestString(f.gpr[0], path, 512)) return OrbisEnoent;
    auto const base = std::filesystem::path(path).filename().string();
    if (base == "libScePigletv2VSH.sprx" || base == "libScePigletv2VSH.prx") return 63;
    if (base == "libSceShaccVSH.sprx" || base == "libSceShaccVSH.prx") return 64;
    return OrbisEnoent;
}
std::uint64_t Call_scePigletSetConfigurationVSH(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    return d.Graphics() && d.Graphics()->Ready() &&
           d.GuestReadable(f, f.gpr[0], 1) ? 1 : 0;
}
std::uint64_t Call_eglGetDisplay(HleDispatcher& d, GuestCallFrame const&) noexcept {
    auto* graphics = d.Graphics();
    return graphics && graphics->Ready() ? reinterpret_cast<std::uint64_t>(graphics->Display()) : 0;
}
std::uint64_t Call_eglInitialize(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    auto* graphics = d.Graphics();
    if (!graphics || !graphics->Ready() || f.gpr[0] != reinterpret_cast<std::uint64_t>(graphics->Display())) return 0;
    if (f.gpr[1]) {
        auto* major = static_cast<EGLint*>(d.GuestWritable(f, f.gpr[1], sizeof(EGLint)));
        if (!major) return 0;
        *major = 1;
    }
    if (f.gpr[2]) {
        auto* minor = static_cast<EGLint*>(d.GuestWritable(f, f.gpr[2], sizeof(EGLint)));
        if (!minor) return 0;
        *minor = 5;
    }
    return 1;
}
std::uint64_t Call_eglChooseConfig(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    auto* graphics = d.Graphics();
    if (!graphics || !graphics->Ready() || f.gpr[0] != reinterpret_cast<std::uint64_t>(graphics->Display()) ||
        !f.gpr[4]) return 0;
    EGLint const* attrs = nullptr;
    std::ostringstream request;
    request << "EGL: eglChooseConfig attributes";
    if (f.gpr[1]) {
        attrs = static_cast<EGLint const*>(d.GuestReadable(f, f.gpr[1], sizeof(EGLint)));
        if (!attrs) return 0;
        bool terminated = false;
        for (unsigned i = 0; i < 64; i += 2) {
            auto* key = static_cast<EGLint const*>(d.GuestReadable(f, f.gpr[1] + i * sizeof(EGLint), sizeof(EGLint)));
            if (!key) return 0;
            if (*key == EGL_NONE) { terminated = true; break; }
            auto* value = static_cast<EGLint const*>(d.GuestReadable(f, f.gpr[1] + (i + 1) * sizeof(EGLint), sizeof(EGLint)));
            if (!value) return 0;
            request << " 0x" << std::hex << *key << '=' << std::dec << *value;
        }
        if (!terminated) return 0;
    }
    auto* count = static_cast<EGLint*>(d.GuestWritable(f, f.gpr[4], sizeof(EGLint)));
    if (!count) return 0;
    auto choose = reinterpret_cast<decltype(&::eglChooseConfig)>(GetProcAddress(graphics->EglModule(), "eglChooseConfig"));
    if (!choose) return 0;
    EGLConfig matching[512]{};
    EGLint available{};
    if (!choose(graphics->Display(), attrs, matching, 512, &available)) return 0;
    bool found = std::find(matching, matching + (std::min)(available, 512), graphics->Config()) !=
                 matching + (std::min)(available, 512);
    request << " candidates=" << available << " surface_match=" << (found ? 1 : 0);
    d.GraphicsLog(request.str());
    *count = found ? 1 : 0;
    if (found && f.gpr[2] && static_cast<std::int32_t>(f.gpr[3]) > 0) {
        auto* config = static_cast<EGLConfig*>(d.GuestWritable(f, f.gpr[2], sizeof(EGLConfig)));
        if (!config) return 0;
        *config = graphics->Config();
    }
    return 1;
}
std::uint64_t Call_eglCreateWindowSurface(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    auto* graphics = d.Graphics();
    if (!graphics || !graphics->Ready() || f.gpr[0] != reinterpret_cast<std::uint64_t>(graphics->Display()) ||
        f.gpr[1] != reinterpret_cast<std::uint64_t>(graphics->Config())) return 0;
    return reinterpret_cast<std::uint64_t>(graphics->Surface());
}
std::uint64_t Call_eglCreateContext(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    auto* graphics = d.Graphics();
    if (!graphics || !graphics->Ready() || f.gpr[0] != reinterpret_cast<std::uint64_t>(graphics->Display()) ||
        f.gpr[1] != reinterpret_cast<std::uint64_t>(graphics->Config())) return 0;
    auto create = reinterpret_cast<decltype(&::eglCreateContext)>(GetProcAddress(graphics->EglModule(), "eglCreateContext"));
    auto* attrs = f.gpr[3] ? static_cast<EGLint const*>(d.GuestReadable(f, f.gpr[3], sizeof(EGLint))) : nullptr;
    if (f.gpr[3] && !attrs) return 0;
    auto context = create ? create(graphics->Display(), graphics->Config(),
                                   reinterpret_cast<EGLContext>(f.gpr[2]), attrs) : nullptr;
    return reinterpret_cast<std::uint64_t>(context);
}
std::uint64_t Call_eglMakeCurrent(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    auto* graphics = d.Graphics();
    if (!graphics || !graphics->Ready() || f.gpr[0] != reinterpret_cast<std::uint64_t>(graphics->Display())) return 0;
    auto make = reinterpret_cast<decltype(&::eglMakeCurrent)>(GetProcAddress(graphics->EglModule(), "eglMakeCurrent"));
    if (!make) return 0;
    auto draw = reinterpret_cast<EGLSurface>(f.gpr[1]);
    auto read = reinterpret_cast<EGLSurface>(f.gpr[2]);
    if ((draw && draw != graphics->Surface()) || (read && read != graphics->Surface())) return 0;
    return make(graphics->Display(), draw, read, reinterpret_cast<EGLContext>(f.gpr[3]));
}
std::uint64_t Call_eglDestroySurface(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    auto* graphics = d.Graphics();
    if (!graphics || !graphics->Ready()) return 0;
    if (f.gpr[1] == reinterpret_cast<std::uint64_t>(graphics->Surface())) return 1;
    return ForwardEgl<&::eglDestroySurface>(d, f, "eglDestroySurface");
}
std::uint64_t Call_eglDestroyContext(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    auto* graphics = d.Graphics();
    if (!graphics || !graphics->Ready()) return 0;
    if (f.gpr[1] == reinterpret_cast<std::uint64_t>(graphics->Context())) return 1;
    return ForwardEgl<&::eglDestroyContext>(d, f, "eglDestroyContext");
}
std::uint64_t Call_eglTerminate(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    auto* graphics = d.Graphics();
    return graphics && graphics->Ready() && f.gpr[0] == reinterpret_cast<std::uint64_t>(graphics->Display()) ? 1 : 0;
}
std::uint64_t Call_eglGetProcAddress(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    std::string name;
    if (!d.GuestString(f.gpr[0], name, 128)) return 0;
    return reinterpret_cast<std::uint64_t>(d.GraphicsAddress(name));
}
constexpr NamedHandler EglHandlers[] = {
    {"sceKernelLoadStartModule", &Call_sceKernelLoadStartModule},
    {"scePigletSetConfigurationVSH", &Call_scePigletSetConfigurationVSH},
    {"eglGetDisplay", &Call_eglGetDisplay},
    {"eglInitialize", &Call_eglInitialize},
    {"eglChooseConfig", &Call_eglChooseConfig},
    {"eglCreateWindowSurface", &Call_eglCreateWindowSurface},
    {"eglCreateContext", &Call_eglCreateContext},
    {"eglMakeCurrent", &Call_eglMakeCurrent},
    {"eglDestroySurface", &Call_eglDestroySurface},
    {"eglDestroyContext", &Call_eglDestroyContext},
    {"eglTerminate", &Call_eglTerminate},
    {"eglGetProcAddress", &Call_eglGetProcAddress},
    {"eglBindAPI", &Call_eglBindAPI},
    {"eglGetConfigAttrib", &Call_eglGetConfigAttrib},
    {"eglGetError", &Call_eglGetError},
    {"eglQueryAPI", &Call_eglQueryAPI},
    {"eglQueryString", &Call_eglQueryString},
    {"eglSwapBuffers", &Call_eglSwapBuffers},
    {"eglSwapInterval", &Call_eglSwapInterval},
    {"eglWaitGL", &Call_eglWaitGL},
    {"eglWaitNative", &Call_eglWaitNative},
    {"eglCreatePbufferSurface", &Call_eglCreatePbufferSurface},
};

} // namespace

HleHandler LookupGraphicsHandler(std::string_view name) noexcept {
    auto found = std::find_if(std::begin(GlHandlers), std::end(GlHandlers),
                              [=](auto const& row) { return row.name == name; });
    if (found != std::end(GlHandlers)) return found->handler;
    auto egl = std::find_if(std::begin(EglHandlers), std::end(EglHandlers),
                             [=](auto const& row) { return row.name == name; });
    return egl == std::end(EglHandlers) ? nullptr : egl->handler;
}
} // namespace Lab
