#!/usr/bin/env python3
"""Guard the display's on-glass text against characters the font cannot draw.

The faces render with LVGL's built-in Montserrat, which is generated over a
fixed range:

    -r 0x20-0x7F,0xB0,0x2022

Anything outside it is not a fallback and not a warning — LVGL draws a hollow
box. That is how "Sunday U+00B7 Aug 9" reached a bedside table reading
"Sunday [] Aug 9": U+00B7 MIDDLE DOT looks like the obvious separator, sits
one codepoint away from the degree sign that *is* in the range, and nothing
failed at build time. The emulator hides it too, because a browser has real
fonts.

So the rule is checked instead of remembered: string literals in the LVGL
faces and the code that feeds them may only use codepoints the font carries.
U+2022 BULLET is in the range and is the separator to reach for.

Not covered, deliberately: mirror_html.h / tv_html.h and anything else served
to a browser, where system fonts apply and typography is free.

Run from the repo root:
    firmware/scripts/check_display_glyphs.py
"""

from __future__ import annotations

import sys
import unicodedata
from pathlib import Path

# The built-in Montserrat range, verified against the generator options in the
# header of lvgl/src/font/lv_font_montserrat_*.c.
ALLOWED = set(range(0x20, 0x80)) | {0xB0, 0x2022}

ROOT = Path(__file__).resolve().parents[2]
PROJ = ROOT / "firmware/projects/canary-display"

# The faces and the modules that build label text for them. Browser-served
# payloads are excluded by construction: they live in *_html.h.
SCAN_DIRS = [PROJ / "src/ui", PROJ / "src/care", PROJ / "src/fleet"]

# Text that never reaches the glass. The serial/web log is read in a terminal
# or a browser, and #error/#warning strings are consumed by the compiler — all
# three have real fonts, so the range rule is about the LVGL faces only.
NOT_ON_GLASS = (
    "log_line(",
    "log_header(",
    "dbg_serial(",
    "Serial.print",
    "#error",
    "#warning",
    "#pragma message",
)

SUGGEST = {
    0x00B7: "U+2022 BULLET (\\xE2\\x80\\xA2) — in range, same job",
    0x2013: "an ASCII hyphen '-'",
    0x2014: "an ASCII hyphen '-'",
    0x2018: "an ASCII apostrophe",
    0x2019: "an ASCII apostrophe",
    0x201C: "an ASCII quote",
    0x201D: "an ASCII quote",
    0x2026: "three ASCII periods '...'",
}


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments, leaving string literals intact.

    Comments are where this codebase keeps its em-dashes and box-drawing, so
    scanning them would be all false positives. Newlines are preserved so the
    line numbers reported below stay true.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':  # a string literal: copy it whole, escapes and all
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "'" and i + 1 < n:  # char literal
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == "'":
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def literals(text: str):
    """Yield (line_number, decoded_text) for every double-quoted literal.

    \\xHH escapes are decoded back to bytes and the bytes read as UTF-8, which
    is how this codebase spells non-ASCII on the glass ("\\xC2\\xB7").
    """
    i, n, line = 0, len(text), 1
    while i < n:
        if text[i] == "\n":
            line += 1
            i += 1
            continue
        if text[i] != '"':
            i += 1
            continue
        start_line = line
        i += 1
        raw = bytearray()
        while i < n and text[i] != '"':
            if text[i] == "\\" and i + 1 < n:
                nxt = text[i + 1]
                if nxt == "x":
                    j = i + 2
                    hexits = ""
                    while j < n and len(hexits) < 2 and text[j] in "0123456789abcdefABCDEF":
                        hexits += text[j]
                        j += 1
                    if hexits:
                        raw.append(int(hexits, 16))
                        i = j
                        continue
                if nxt == "\n":
                    line += 1
                i += 2  # any other escape (\n, \t, \\, \") is plain ASCII
                continue
            if text[i] == "\n":
                line += 1
            raw.extend(text[i].encode("utf-8"))
            i += 1
        i += 1
        yield start_line, raw.decode("utf-8", errors="replace")


def main() -> int:
    if not PROJ.is_dir():
        print(f"::error::{PROJ} not found — run from the repo root")
        return 1

    findings = []
    for directory in SCAN_DIRS:
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*")):
            if path.suffix not in (".cpp", ".h", ".hpp"):
                continue
            if path.name.endswith("_html.h"):
                continue
            source = path.read_text(encoding="utf-8", errors="replace")
            src_lines = source.splitlines()
            body = strip_comments(source)
            for line_no, value in literals(body):
                line = src_lines[line_no - 1] if line_no <= len(src_lines) else ""
                if any(marker in line for marker in NOT_ON_GLASS):
                    continue
                for ch in value:
                    cp = ord(ch)
                    if cp in ALLOWED or cp == 0xFFFD:
                        continue
                    findings.append((path.relative_to(ROOT), line_no, cp, ch))

    if not findings:
        print("✓ Display glass text stays inside the font's glyph range.")
        return 0

    print("::error::On-glass text uses characters the display font cannot draw.")
    print("         LVGL renders these as a hollow box, silently.")
    for rel, line_no, cp, ch in findings:
        try:
            name = unicodedata.name(ch)
        except ValueError:
            name = "unnamed"
        hint = SUGGEST.get(cp, "a character inside 0x20-0x7F, 0xB0, or U+2022")
        print(f"  {rel}:{line_no}: U+{cp:04X} {name} — use {hint}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
