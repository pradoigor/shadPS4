// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "Report.h"
namespace Lab {
// Runs the single user-facing execution gate. The selected ELF/SELF is only
// validated; execution is limited to a project-generated return-42 ELF.
void RunProbe(Test& test, std::wstring const& executablePath, std::wstring const& directory);
}
