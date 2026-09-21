// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "Report.h"
namespace Lab {
// Runs the single user-facing loader validation. The path must point to a
// selected ELF/SELF or to an extracted eboot.bin. No input code is executed.
void RunProbe(Test& test, std::wstring const& executablePath);
}
