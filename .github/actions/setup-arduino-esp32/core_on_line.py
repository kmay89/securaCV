#!/usr/bin/env python3
"""The newest stable esp32:esp32 release on one major line.

    python3 core_on_line.py <package_esp32_index.json> <major>

Prints the version (e.g. "3.3.11") and exits 0, or prints nothing and exits 0
when the index lists no stable release on that line: the caller (action.yml)
turns an empty answer into an error, because the one thing a row floating on
a line must never do is fall back to "latest" and cross the major.

Reads the Boards-Manager index arduino-cli has just downloaded, so there is no
second request and nothing depends on the CLI's own output format. A release
is "stable" when its version is exactly x.y.z: the index Espressif publishes
for releases carries only those, and a pre-release ("3.4.0-rc1") that ever
appears in it is not what a build row should take unasked.
"""
from __future__ import annotations

import json
import re
import sys

STABLE = re.compile(r"\d+\.\d+\.\d+")


def releases(index: dict) -> list[str]:
    """Every esp32:esp32 platform version the index lists."""
    return [str(p.get("version", ""))
            for pkg in index.get("packages", []) if pkg.get("name") == "esp32"
            for p in pkg.get("platforms", []) if p.get("architecture") == "esp32"]


def newest_on_line(index: dict, line: str) -> str | None:
    on_line = [v for v in releases(index)
               if STABLE.fullmatch(v) and v.split(".")[0] == line]
    if not on_line:
        return None
    return max(on_line, key=lambda v: tuple(int(x) for x in v.split(".")))


def main(argv: list[str]) -> int:
    if len(argv) != 3 or not argv[2].isdigit():
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    with open(argv[1], encoding="utf-8") as f:
        index = json.load(f)
    found = newest_on_line(index, argv[2])
    if found:
        print(found)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
