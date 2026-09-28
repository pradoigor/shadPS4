"""Inventory upstream HLE registrations before duplicating an Xbox handler.

Registration means a callable is wired into the core linker; it does not prove
that its semantics are complete or that the source compiles for UWP.
"""

import json
import re
from collections import defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
XBOX = ROOT / "xbox"
CATALOG = ROOT / "src/core/aerolib/aerolib.inl"
SOURCE = ROOT / "src/core/libraries"
OUTPUT = XBOX / "artifacts/core-hle-inventory.json"


def main() -> None:
    names = dict(re.findall(r'STUB\("([^"]+)",\s*([^\s)]+)\)',
                            CATALOG.read_text(encoding="utf-8")))
    registrations: dict[str, list[str]] = defaultdict(list)
    for path in sorted(SOURCE.rglob("*.cpp")):
        source = path.read_text(encoding="utf-8")
        for nid in set(re.findall(r'LIB_FUNCTION\s*\(\s*"([^"]+)"', source)):
            registrations[nid].append(path.relative_to(ROOT).as_posix())

    xbox_source = (XBOX / "HleDispatcher.cpp").read_text(encoding="utf-8")
    resolve = xbox_source.split("HleResolution HleDispatcher::Resolve(", 1)[1]
    resolve = resolve.split("const auto slot =", 1)[0]
    xbox_names = set(re.findall(r'name\s*==\s*"([^"]+)"', resolve))

    rows = [
        {
            "nid": nid,
            "name": names.get(nid, ""),
            "registered_in": paths,
            "xbox_handler_name_match": names.get(nid, "") in xbox_names,
        }
        for nid, paths in sorted(registrations.items())
    ]
    output = {
        "description": "Core HLE registrations are reuse candidates, not UWP compatibility proof.",
        "summary": {
            "core_registered_nids": len(rows),
            "xbox_name_matches": sum(row["xbox_handler_name_match"] for row in rows),
            "xbox_name_scan_limit": "Direct name == comparisons in HleDispatcher::Resolve only",
        },
        "registrations": rows,
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(output, ensure_ascii=False, indent=2) + "\n",
                      encoding="utf-8")
    print(f"Core HLE inventory: {len(rows)} registrations; "
          f"{output['summary']['xbox_name_matches']} Xbox name matches")


if __name__ == "__main__":
    main()
