// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HleDispatcher.h"
namespace Lab {
HleHandler LookupFreeTypeHandler(std::string_view name) noexcept;
}
