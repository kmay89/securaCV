#!/usr/bin/env python3
"""Bake tv/index.html into the PROGMEM header the firmware serves at /tv.

The Canary already serves its phone mirror from a hand-maintained PROGMEM
string (mirror_html.h). The TV page is larger and edited as a real .html
file (so it lints, screenshots, and opens straight in a browser), so we
generate its header instead of hand-copying — no drift between the file you
edit and the bytes the device ships.

    python3 gen_tv_html.py         # regenerate ../arduino/canary_display/tv_html.h

The chosen raw-string delimiter must never appear in the page; we assert it.
"""
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
SRC = HERE / "index.html"
OUT = HERE.parent / "arduino" / "canary_display" / "tv_html.h"
DELIM = "TVGLASS"  # yields the sentinel )TVGLASS"


def main() -> int:
    html = SRC.read_text(encoding="utf-8")
    sentinel = f"){DELIM}\""
    if sentinel in html:
        print(f"error: page contains the raw-string sentinel {sentinel!r}", file=sys.stderr)
        return 1
    header = (
        "// tv_html.h — GENERATED from tv/index.html by tv/gen_tv_html.py.\n"
        "// Do not edit by hand: edit tv/index.html and re-run the generator.\n"
        "// The Canary serves this at GET /tv — a 10-foot ambient security\n"
        "// surface for any television on the home WiFi. Self-contained (no\n"
        "// CDN, no internet): the LAN promise holds, same as mirror_html.h.\n"
        "#pragma once\n"
        "#include <pgmspace.h>\n\n"
        f'static const char TV_HTML[] PROGMEM = R"{DELIM}(' + html + f'){DELIM}";\n'
    )
    OUT.write_text(header, encoding="utf-8")
    print(f"wrote {OUT} ({len(header)} bytes from {len(html)} bytes of HTML)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
