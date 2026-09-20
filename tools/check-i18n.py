#!/usr/bin/env python3
"""Check that the i18n table and the StringId enum still line up.

`Text()` maps an id to a row by index, so the two lists must be in the same order.  Nothing in
C++ enforces that: the host tests only see the resulting table, and a swap like "LabelExit and
AppletModeLibrary traded places" compiles happily and shows the wrong string at runtime (the
footer once read "Album (applet) mode" where it should have said "Exit").  Every row therefore
carries its id as a trailing comment, and this tool compares the two orders.

Usage:
    python3 tools/check-i18n.py            # exit 1 with a diff when they disagree
"""

from __future__ import annotations

import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
HPP = REPO_ROOT / "source" / "i18n" / "strings.hpp"
CPP = REPO_ROOT / "source" / "i18n" / "strings.cpp"


def main() -> int:
    enum_block = HPP.read_text().split("enum class StringId {", 1)[1].split("};", 1)[0]
    ids = [line.strip().rstrip(",") for line in enum_block.splitlines() if line.strip()]
    table = CPP.read_text().split("constexpr Entry kStrings[] = {", 1)[1].split("\n};", 1)[0]
    rows = []
    for line in table.splitlines():
        match = re.match(r'\s*\{.*\},\s*/\* (\w+) \*/\s*$', line)
        if match:
            rows.append(match.group(1))
        elif line.strip().startswith('{"'):
            rows.append("<row without an id comment>")

    if len(ids) != len(rows):
        print(f"error: enum has {len(ids)} ids, table has {len(rows)} rows", file=sys.stderr)
        return 1
    for index, (name, row) in enumerate(zip(ids, rows)):
        if name != row:
            print(f"error: entry {index}: enum says {name}, table says {row}", file=sys.stderr)
            return 1
    print(f"i18n: {len(ids)} entries, enum and table in the same order")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
