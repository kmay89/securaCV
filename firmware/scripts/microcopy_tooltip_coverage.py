#!/usr/bin/env python3
"""
Tooltip-coverage lint for the headline Sensing dashboard.

Every interactive element in csi_dashboard_html.h carries a `data-tip="key"`
attribute that the dashboard's tooltip module looks up in the COPY.tooltips
object. If a key is referenced but not defined, the user gets a silent empty
tooltip — usability bug, not a crash, so easy to ship.

This script:
  1. Greps every `data-tip="<key>"` value out of the HTML body of
     csi_dashboard_html.h.
  2. Greps every key inside the `tooltips: { ... }` block of the COPY
     object.
  3. Reports any used-but-not-defined keys (and any defined-but-unused
     keys, as a warning) and exits non-zero on the former.

Usage:
    python3 firmware/scripts/microcopy_tooltip_coverage.py [path/to/dashboard.h]
"""

from __future__ import annotations
import re
import sys
from pathlib import Path

DEFAULT_PATH = Path(
    "firmware/projects/canary-wap/arduino/canary_wap/csi_dashboard_html.h"
)


def extract_used_tip_keys(text: str) -> set[str]:
    """Every `data-tip="key"` in the HTML body. Tolerant of single quotes."""
    return set(re.findall(r"""data-tip=["']([^"']+)["']""", text))


def extract_defined_tip_keys(text: str) -> set[str]:
    """Keys inside the COPY.tooltips object literal.

    The COPY object is a single JS literal in the inline <script>. We look
    for the `tooltips: {` opener and walk lines until the matching `}` (the
    object closer). Inside, every line that starts with whitespace + an
    identifier + ':' is a key.
    """
    # Find the start of the tooltips block.
    m = re.search(r"\btooltips:\s*\{", text)
    if not m:
        return set()
    # Walk forward, brace-counting, to find the closing }.
    i = m.end()
    depth = 1
    end = i
    while i < len(text) and depth > 0:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
        i += 1
    block = text[m.end():end]

    # Match `<whitespace><identifier>:` at line starts (the keys).
    keys: set[str] = set()
    for line in block.splitlines():
        km = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*:", line)
        if km:
            keys.add(km.group(1))
    return keys


def main() -> int:
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PATH
    if not path.exists():
        print(f"[tooltip-coverage] file not found: {path}", file=sys.stderr)
        return 2

    text = path.read_text(encoding="utf-8")

    used = extract_used_tip_keys(text)
    defined = extract_defined_tip_keys(text)

    missing = sorted(used - defined)
    unused  = sorted(defined - used)

    if missing:
        print(
            "[tooltip-coverage] FAIL — these data-tip keys are used in the "
            "HTML but missing from COPY.tooltips:",
            file=sys.stderr,
        )
        for k in missing:
            print(f"  - {k}", file=sys.stderr)
        print(
            "\nAdd them to the tooltips: {} block in csi_dashboard_html.h's "
            "COPY object, or remove the data-tip attribute.",
            file=sys.stderr,
        )
        return 1

    if unused:
        # Warning, not failure — defining a tooltip for a future control is
        # fine, but flag them so dead copy doesn't accumulate.
        print(
            "[tooltip-coverage] WARN — these COPY.tooltips keys are defined "
            "but unused in the HTML:"
        )
        for k in unused:
            print(f"  - {k}")

    print(f"[tooltip-coverage] OK — {len(used)} data-tip keys, all defined.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
