// SPDX-License-Identifier: GPL-2.0-or-later
#include "AngleVideo.h"
#include <sstream>

namespace Lab {
namespace {
using GetDisplay = void* (WINAPI*)(void*);
using Initialize = unsigned (WINAPI*)(void*, int*, int*);
using BindApi = unsigned (WINAPI*)(unsigned);
using ChooseConfig = unsigned (WINAPI*)(void*, const int*, void**, int, int*);
using CreateWindowSurface = void* (WINAPI*)(void*, void*, void*, const int*);
using CreateContext = void* (WINAPI*)(void*, void*, void*, const int*);
using MakeCurrent = unsigned (WINAPI*)(void*, void*, void*, void*);
using SwapBuffers = unsigned (WINAPI*)(void*, void*);
using GetError = unsigned (WINAPI*)();
using DestroySurface = unsigned (WINAPI*)(void*, void*);
using DestroyContext = unsigned (WINAPI*)(void*, void*);
using Terminate = unsigned (WINAPI*)(void*);
using Viewport = void (WINAPI*)(int, int, int, int);
using ClearColor = void (WINAPI*)(float, float, float, float);
using Clear = void (WINAPI*)(unsigned);
template<class T> T Proc(HMODULE module, char const* name) { return reinterpret_cast<T>(GetProcAddress(module, name)); }
}

bool AngleVideo::Start(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel, std::wstring& detail) {
    Stop();
    auto fail = [&](wchar_t const* stage) {
        auto error = egl_ ? Proc<GetError>(egl_, "eglGetError") : nullptr;
        std::wostringstream out;
        out << stage << L"; EGL=0x" << std::hex << (error ? error() : 0u)
            << L", Win32=" << GetLastError();
        detail = out.str(); Stop(); return false;
    };
    egl_ = LoadPackagedLibrary(L"libEGL.dll", 0);
    if (!egl_) return fail(L"LoadPackagedLibrary(libEGL.dll)");
    gles_ = LoadPackagedLibrary(L"libGLESv2.dll", 0);
    if (!gles_) return fail(L"LoadPackagedLibrary(libGLESv2.dll)");
    auto getDisplay = Proc<GetDisplay>(egl_, "eglGetDisplay");
    auto initialize = Proc<Initialize>(egl_, "eglInitialize");
    auto bindApi = Proc<BindApi>(egl_, "eglBindAPI");
    auto chooseConfig = Proc<ChooseConfig>(egl_, "eglChooseConfig");
    auto createSurface = Proc<CreateWindowSurface>(egl_, "eglCreateWindowSurface");
    auto createContext = Proc<CreateContext>(egl_, "eglCreateContext");
    auto makeCurrent = Proc<MakeCurrent>(egl_, "eglMakeCurrent");
    auto swapBuffers = Proc<SwapBuffers>(egl_, "eglSwapBuffers");
    auto viewport = Proc<Viewport>(gles_, "glViewport");
    auto clearColor = Proc<ClearColor>(gles_, "glClearColor");
    auto clear = Proc<Clear>(gles_, "glClear");
    if (!getDisplay || !initialize || !bindApi || !chooseConfig || !createSurface ||
        !createContext || !makeCurrent || !swapBuffers || !viewport || !clearColor || !clear)
        return fail(L"Símbolo EGL/GLES ausente");
    display_ = getDisplay(nullptr);
    if (!display_) return fail(L"eglGetDisplay");
    int major{}, minor{};
    if (!initialize(display_, &major, &minor)) return fail(L"eglInitialize");
    if (!bindApi(0x30A0)) return fail(L"eglBindAPI ES");
    const int configAttrs[] = {0x3033, 0x0004, 0x3040, 0x0004, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3038};
    void* config{}; int count{};
    if (!chooseConfig(display_, configAttrs, &config, 1, &count) || count != 1)
        return fail(L"eglChooseConfig");
    config_ = config;
    properties_ = winrt::Windows::Foundation::Collections::PropertySet();
    properties_.Insert(L"EGLNativeWindowTypeProperty", panel);
    surface_ = createSurface(display_, config, winrt::get_abi(properties_), nullptr);
    if (!surface_) return fail(L"eglCreateWindowSurface(SwapChainPanel)");
    const int contextAttrs[] = {0x3098, 2, 0x3038};
    context_ = createContext(display_, config, nullptr, contextAttrs);
    if (!context_) return fail(L"eglCreateContext ES2");
    if (!makeCurrent(display_, surface_, surface_, context_)) return fail(L"eglMakeCurrent");
    auto width = static_cast<int>(panel.ActualWidth());
    auto height = static_cast<int>(panel.ActualHeight());
    if (width <= 0 || height <= 0) return fail(L"SwapChainPanel sem dimensões");
    viewport(0, 0, width, height);
    clearColor(0.05f, 0.55f, 0.85f, 1.f);
    clear(0x4000);
    if (!swapBuffers(display_, surface_)) return fail(L"eglSwapBuffers");
    detail = L"ANGLE EGL " + std::to_wstring(major) + L"." + std::to_wstring(minor) +
             L"; GLES2; quadro azul apresentado em " + std::to_wstring(width) + L"x" + std::to_wstring(height);
    return true;
}

bool AngleVideo::ReleaseForGuest() noexcept {
    if (!Ready()) return false;
    auto makeCurrent = Proc<MakeCurrent>(egl_, "eglMakeCurrent");
    return makeCurrent && makeCurrent(display_, nullptr, nullptr, nullptr) != 0;
}

void AngleVideo::Stop() noexcept {
    if (egl_ && display_) {
        if (auto makeCurrent = Proc<MakeCurrent>(egl_, "eglMakeCurrent")) makeCurrent(display_, nullptr, nullptr, nullptr);
        if (context_) if (auto destroy = Proc<DestroyContext>(egl_, "eglDestroyContext")) destroy(display_, context_);
        if (surface_) if (auto destroy = Proc<DestroySurface>(egl_, "eglDestroySurface")) destroy(display_, surface_);
        if (auto terminate = Proc<Terminate>(egl_, "eglTerminate")) terminate(display_);
    }
    context_ = surface_ = display_ = config_ = nullptr;
    properties_ = nullptr;
    if (gles_) FreeLibrary(gles_);
    if (egl_) FreeLibrary(egl_);
    gles_ = egl_ = nullptr;
}
}
