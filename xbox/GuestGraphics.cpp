// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestGraphics.h"
#include "AngleVideo.h"
#include "PigletShaderBinary.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
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
std::uint64_t Call_glViewport(HleDispatcher&, GuestCallFrame const&) noexcept;
std::uint64_t Call_glDrawElements(HleDispatcher&, GuestCallFrame const&) noexcept;
std::uint64_t Call_glDrawArrays(HleDispatcher&, GuestCallFrame const&) noexcept;
std::uint64_t Call_glTexImage2D(HleDispatcher&, GuestCallFrame const&) noexcept;
std::uint64_t Call_glTexSubImage2D(HleDispatcher&, GuestCallFrame const&) noexcept;
struct NamedHandler { std::string_view name; HleHandler handler; };
constexpr NamedHandler GlHandlers[] = {
#define GL_FUNCTION(name) {#name, &Call_##name},
#include "GuestGlFunctions.inc"
#undef GL_FUNCTION
    {"glShaderBinary", &Call_glShaderBinary},
    {"glViewport", &Call_glViewport},
    {"glDrawElements", &Call_glDrawElements},
    {"glDrawArrays", &Call_glDrawArrays},
    {"glTexImage2D", &Call_glTexImage2D},
    {"glTexSubImage2D", &Call_glTexSubImage2D},
};

std::uint64_t GuestStackWord(HleDispatcher& d, GuestCallFrame const& f,
                             std::size_t index) noexcept {
    auto* word = static_cast<std::uint64_t const*>(
        d.GuestReadable(f, f.guest_stack + 8 + index * 8, 8));
    return word ? *word : 0;
}

std::uint64_t Call_glTexImage2D(HleDispatcher& d,
                                 GuestCallFrame const& f) noexcept {
    static std::atomic_uint32_t logged{};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 12) {
        char line[256]{};
        std::snprintf(line, sizeof(line),
                      "GL: texImage2D internal=0x%x size=%dx%d format=0x%x type=0x%x pixels=%s",
                      static_cast<unsigned>(f.gpr[2]),
                      static_cast<int>(f.gpr[3]), static_cast<int>(f.gpr[4]),
                      static_cast<unsigned>(GuestStackWord(d, f, 0)),
                      static_cast<unsigned>(GuestStackWord(d, f, 1)),
                      GuestStackWord(d, f, 2) ? "present" : "null");
        d.GraphicsLog(line);
    }
    return ForwardGl<&::glTexImage2D>(d, f, "glTexImage2D");
}

std::uint64_t Call_glTexSubImage2D(HleDispatcher& d,
                                    GuestCallFrame const& f) noexcept {
    static std::atomic_uint32_t smallLogged{}, largeLogged{};
    const auto width = static_cast<std::int32_t>(f.gpr[4]);
    const auto height = static_cast<std::int32_t>(f.gpr[5]);
    const auto large = width >= 64 && height >= 64;
    const auto ordinal = (large ? largeLogged : smallLogged).fetch_add(1, std::memory_order_relaxed);
    if (ordinal < 8) {
        char line[512]{};
        std::snprintf(line, sizeof(line),
                      "GL: texSubImage2D size=%dx%d format=0x%x type=0x%x pixels=%s",
                      width, height,
                      static_cast<unsigned>(GuestStackWord(d, f, 0)),
                      static_cast<unsigned>(GuestStackWord(d, f, 1)),
                      GuestStackWord(d, f, 2) ? "present" : "null");
        d.GraphicsLog(line);
        if (width > 0 && height > 0 &&
            GuestStackWord(d, f, 0) == GL_RGBA &&
            GuestStackWord(d, f, 1) == GL_UNSIGNED_BYTE &&
            GuestStackWord(d, f, 2) != 0) {
            const auto pixels = (std::min)(std::uint64_t(width) * height, std::uint64_t{256});
            auto* bytes = static_cast<std::uint8_t const*>(d.GuestReadable(
                f, GuestStackWord(d, f, 2), static_cast<std::size_t>(pixels * 4)));
            if (bytes) {
                unsigned zero[4]{}, full[4]{}, middle[4]{};
                for (std::uint64_t pixel = 0; pixel < pixels; ++pixel) {
                    for (unsigned channel = 0; channel < 4; ++channel) {
                        const auto value = bytes[pixel * 4 + channel];
                        if (value == 0) ++zero[channel];
                        else if (value == 255) ++full[channel];
                        else ++middle[channel];
                    }
                }
                std::snprintf(line, sizeof(line),
                              "GL: amostra RGBA %llu pixels; zero=%u,%u,%u,%u full=%u,%u,%u,%u mid=%u,%u,%u,%u",
                              static_cast<unsigned long long>(pixels),
                              zero[0], zero[1], zero[2], zero[3],
                              full[0], full[1], full[2], full[3],
                              middle[0], middle[1], middle[2], middle[3]);
                d.GraphicsLog(line);
            }
        }
    }
    return ForwardGl<&::glTexSubImage2D>(d, f, "glTexSubImage2D");
}

void LogDrawState(HleDispatcher& d, GuestCallFrame const& f,
                  char const* operation, std::int32_t count) noexcept {
    static std::atomic_uint32_t logged{};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        auto* graphics = d.Graphics();
        if (graphics && graphics->Ready()) {
            auto isEnabled = reinterpret_cast<decltype(&::glIsEnabled)>(
                GetProcAddress(graphics->GlesModule(), "glIsEnabled"));
            auto getInteger = reinterpret_cast<decltype(&::glGetIntegerv)>(
                GetProcAddress(graphics->GlesModule(), "glGetIntegerv"));
            if (isEnabled && getInteger) {
                GLint src{}, dst{}, program{}, texture{};
                getInteger(GL_BLEND_SRC_RGB, &src);
                getInteger(GL_BLEND_DST_RGB, &dst);
                getInteger(GL_CURRENT_PROGRAM, &program);
                getInteger(GL_TEXTURE_BINDING_2D, &texture);
                char line[256]{};
                std::snprintf(line, sizeof(line),
                              "GL: %s blend=%u src=0x%x dst=0x%x program=%d texture=%d count=%d",
                              operation,
                              static_cast<unsigned>(isEnabled(GL_BLEND)), src, dst,
                              program, texture, count);
                d.GraphicsLog(line);
            }
        }
    }
}

std::uint64_t Call_glDrawElements(HleDispatcher& d,
                                  GuestCallFrame const& f) noexcept {
    LogDrawState(d, f, "drawElements", static_cast<std::int32_t>(f.gpr[1]));
    return ForwardGl<&::glDrawElements>(d, f, "glDrawElements");
}

std::uint64_t Call_glDrawArrays(HleDispatcher& d,
                                GuestCallFrame const& f) noexcept {
    LogDrawState(d, f, "drawArrays", static_cast<std::int32_t>(f.gpr[2]));
    return ForwardGl<&::glDrawArrays>(d, f, "glDrawArrays");
}

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
    if (result) {
        if (auto* graphics = d.Graphics()) graphics->NoteGuestFrame();
    }
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
    std::string adaptedSource;
    const bool adaptedAlpha = AdaptApolloTextureShader(source, adaptedSource);
    const auto shaderSource = adaptedAlpha ? std::string_view(adaptedSource) : source;
    const GLchar* sourceText = shaderSource.data();
    const GLint sourceLength = static_cast<GLint>(shaderSource.size());
    static std::atomic_uint32_t translated{};
    const auto ordinal = translated.fetch_add(1, std::memory_order_relaxed) + 1;
    if (adaptedAlpha)
        d.GraphicsLog("Piglet: shader de textura ABGR adaptado para RGBA com alfa correto.");
    if (ordinal <= 3) {
        std::string printable(shaderSource);
        std::replace(printable.begin(), printable.end(), '\n', ' ');
        std::replace(printable.begin(), printable.end(), '\r', ' ');
        d.GraphicsLog("Piglet: GLSL " + std::to_string(ordinal) + ": " + printable);
    }
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

std::uint64_t Call_glViewport(HleDispatcher& d,
                              GuestCallFrame const& f) noexcept {
    auto* graphics = d.Graphics();
    const auto surfaceWidth = graphics ? graphics->SurfaceWidth() : 0;
    const auto surfaceHeight = graphics ? graphics->SurfaceHeight() : 0;
    const auto x = static_cast<std::int32_t>(f.gpr[0]);
    const auto y = static_cast<std::int32_t>(f.gpr[1]);
    const auto width = static_cast<std::int32_t>(f.gpr[2]);
    const auto height = static_cast<std::int32_t>(f.gpr[3]);

    // Apollo creates its display at 1920x1080 while Xbox UWP can expose the
    // SwapChainPanel to EGL at 960x540. A full-screen viewport with the same
    // aspect ratio can safely map to the actual default-framebuffer size;
    // leave sub-viewports and aspect-ratio conversions untouched.
    auto adjusted = f;
    const bool fullScreenViewport = x == 0 && y == 0 && width > surfaceWidth &&
                                    height > surfaceHeight && surfaceWidth > 0 &&
                                    surfaceHeight > 0 && width > 0 && height > 0;
    if (fullScreenViewport) {
        const auto guestAspect = static_cast<double>(width) / height;
        const auto surfaceAspect = static_cast<double>(surfaceWidth) / surfaceHeight;
        if (std::abs(guestAspect - surfaceAspect) <= surfaceAspect * 0.01) {
            adjusted.gpr[2] = static_cast<std::uint32_t>(surfaceWidth);
            adjusted.gpr[3] = static_cast<std::uint32_t>(surfaceHeight);
        }
    }

    const auto appliedX = static_cast<std::int32_t>(adjusted.gpr[0]);
    const auto appliedY = static_cast<std::int32_t>(adjusted.gpr[1]);
    const auto appliedWidth = static_cast<std::int32_t>(adjusted.gpr[2]);
    const auto appliedHeight = static_cast<std::int32_t>(adjusted.gpr[3]);
    const auto result = ForwardGl<&::glViewport>(d, adjusted, "glViewport");
    static std::atomic_uint32_t logged{};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 4) {
        char message[256]{};
        std::snprintf(message, sizeof(message),
                      "GL: viewport guest=(%d,%d,%d,%d); EGL=%dx%d px; "
                      "aplicado=(%d,%d,%d,%d)%s",
                      x, y, width, height, surfaceWidth, surfaceHeight,
                      appliedX, appliedY, appliedWidth, appliedHeight,
                      width != appliedWidth || height != appliedHeight
                          ? " [ajustado para tela inteira]"
                          : "");
        d.GraphicsLog(message);
    }
    return result;
}

std::uint64_t Call_sceKernelLoadStartModule(HleDispatcher& d, GuestCallFrame const& f) noexcept {
    auto* graphics = d.Graphics();
    if (!graphics || !graphics->Ready()) return OrbisEnosys;
    std::string path;
    if (!d.GuestString(f.gpr[0], path, 512)) return OrbisEnoent;
    auto const base = std::filesystem::path(path).filename().string();
    if (base == "libScePigletv2VSH.sprx" || base == "libScePigletv2VSH.prx") return 63;
    if (base == "libSceShaccVSH.sprx" || base == "libSceShaccVSH.prx") return 64;
    if (base == "rsa.prx" || base == "rsa.sprx") {
        std::filesystem::path host;
        std::error_code error;
        if (d.GuestFilePath(path, false, host) &&
            std::filesystem::is_regular_file(host, error) && !error) {
            d.GraphicsLog("HLE module: RSA da Store disponível em " + path);
            return 65;
        }
        d.GraphicsLog("HLE module: RSA da Store ausente em " + path);
    }
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
