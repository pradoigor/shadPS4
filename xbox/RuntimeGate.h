// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "ControlledLoader.h"

#include <string>
#include <vector>

namespace Lab {

struct RuntimeGateResult {
    bool ready{};
    std::vector<std::wstring> blockers;
};

// Evaluates whether a validated image has all prerequisites for guest control
// flow. This is a safety gate, not an execution attempt.
RuntimeGateResult EvaluateRuntimeGate(ControlledLoadResult const& load);

} // namespace Lab
