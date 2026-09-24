// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <string>

namespace Lab {
class AngleVideo {
public:
    ~AngleVideo() { Stop(); }
    bool Start(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel, std::wstring& detail);
    bool ReleaseForGuest() noexcept;
    bool Ready() const noexcept { return egl_ && gles_ && display_ && surface_ && context_; }
    int SurfaceWidth() const noexcept { return surfaceWidth_; }
    int SurfaceHeight() const noexcept { return surfaceHeight_; }
    HMODULE EglModule() const noexcept { return egl_; }
    HMODULE GlesModule() const noexcept { return gles_; }
    void* Display() const noexcept { return display_; }
    void* Surface() const noexcept { return surface_; }
    void* Context() const noexcept { return context_; }
    void* Config() const noexcept { return config_; }
    void Stop() noexcept;
private:
    HMODULE egl_{};
    HMODULE gles_{};
    void* display_{};
    void* surface_{};
    void* context_{};
    void* config_{};
    int surfaceWidth_{};
    int surfaceHeight_{};
    winrt::Windows::Foundation::Collections::PropertySet properties_{nullptr};
};
}
