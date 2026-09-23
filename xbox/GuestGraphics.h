// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HleDispatcher.h"

namespace Lab {
// Returns a SysV-to-Windows HLE handler for an EGL/GLES2 import. Every
// forwarded call remains on the guest worker, which owns the EGL context.
HleHandler LookupGraphicsHandler(std::string_view name) noexcept;
}
