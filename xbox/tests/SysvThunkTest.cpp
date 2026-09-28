// SPDX-License-Identifier: GPL-2.0-or-later
#include "SysvThunk.h"
#include <iostream>

int main() {
    // Executes the generated x64 code: normal HLE and PRX tail forwarding,
    // six integer arguments, eight full XMM arguments, variadic AL and stack.
    const auto result = Lab::ValidateSysvThunkAbi();
    std::cout << "SysV normal/forwarded ABI: " << result.passed << '\n';
    return result.passed ? 0 : 1;
}
