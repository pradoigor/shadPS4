// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestSymbolIdentity.h"
#include "Generated Files/CoreHleInventory.h"
#include <iostream>
#include <stdexcept>

static void Check(bool result, char const* message) {
    if (!result) throw std::runtime_error(message);
}

int main() {
    using namespace Lab;
    const auto imported = CanonicalGuestSymbol("NID#A#B", {"A=libc@1"}, {"B=libc@1.0"});
    const auto exported = CanonicalGuestSymbol("NID#C#D", {"C=libc@1"}, {"D=libc@1.0"});
    const auto unrelated = CanonicalGuestSymbol("NID#A#B", {"A=other@1"}, {"B=other@1.0"});
    GuestSymbolMap exports;
    AddGuestSymbol(exports, exported, 0x1000);
    Check(FindGuestSymbol(imported, exports) == 0x1000, "different local IDs must resolve by identity");
    Check(FindGuestSymbol(unrelated, exports) == 0, "same local IDs must not cross libraries");
    const auto wrongVersion = CanonicalGuestSymbol("NID#A#B", {"A=libc@2"}, {"B=libc@1.0"});
    Check(!FindGuestSymbol(wrongVersion, exports), "library version mismatch");
    const auto moduleVersion = CanonicalGuestSymbol("NID#A#B", {"A=libc@1"}, {"B=libc@9.7"});
    Check(FindGuestSymbol(moduleVersion, exports) == 0x1000, "core ignores module version");
    const auto missing = CanonicalGuestSymbol("NID#A#B", {}, {});
    AddGuestSymbol(exports, missing, 0x2000);
    Check(!FindGuestSymbol(missing, exports), "missing metadata must not bind by local IDs");
    Check(!FindGuestSymbol("NID#A#B", exports), "raw IDs are not global keys");
    GuestSymbolMap collisions;
    AddGuestSymbol(collisions, exported, 0x1000);
    AddGuestSymbol(collisions, exported, 0x1000);
    Check(FindGuestSymbol(imported, collisions) == 0x1000, "same-address alias");
    AddGuestSymbol(collisions, exported, 0x3000);
    Check(!FindGuestSymbol(imported, collisions), "duplicate definitions must not pick first");
    GuestSymbolMap lookup{{"NID#@libc@1#@libc", 0x1000}};
    Check(FindUniqueGuestNid("NID", lookup) == 0x1000, "unique dlsym NID");
    Check(!FindUniqueGuestNid("NI", lookup), "NID prefix is not a match");
    lookup.emplace("NID#@other@1#@other", 0x2000);
    Check(!FindUniqueGuestNid("NID", lookup), "ambiguous dlsym NID");
    Check(CoreHleInventory::Find("2Q0z6rnBrTE") != nullptr, "core pthread registration absent");
    Check(CoreHleInventory::Find("not-a-real-nid") == nullptr, "unknown core registration");
    std::cout << "Guest symbol identities and embedded core lookup passed\n";
}
