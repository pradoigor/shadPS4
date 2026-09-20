// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "Report.h"
#include <d3d11.h>
#include <dxgi1_4.h>
#include <winrt/base.h>
namespace Lab {
void RunProbe(Test& test, std::wstring const& directory);
winrt::com_ptr<IDXGISwapChain1> RenderTriangle();
}
