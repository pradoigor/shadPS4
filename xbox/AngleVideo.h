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
    void Stop() noexcept;
private:
    HMODULE egl_{};
    HMODULE gles_{};
    void* display_{};
    void* surface_{};
    void* context_{};
    winrt::Windows::Foundation::Collections::PropertySet properties_{nullptr};
};
}
