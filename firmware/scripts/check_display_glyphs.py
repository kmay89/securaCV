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

# The FontAwesome glyphs LVGL bakes into the same font files — what the
# LV_SYMBOL_* macros expand to. Written out because code may spell a symbol as
# raw UTF-8 bytes rather than through the macro, and those bytes are legal:
# the glyph really is there.
SYMBOLS = {
    61441, 61448, 61451, 61452, 61453, 61457, 61459, 61461, 61465, 61468,
    61473, 61478, 61479, 61480, 61502, 61507, 61512, 61515, 61516, 61517,
    61521, 61522, 61523, 61524, 61543, 61544, 61550, 61552, 61553, 61556,
    61559, 61560, 61561, 61563, 61587, 61589, 61636, 61637, 61639, 61641,
    61664, 61671, 61674, 61683, 61724, 61732, 61787, 61931, 62016, 62017,
    62018, 62019, 62020, 62087, 62099, 62189, 62212, 62810, 63426, 63650,
}

# The built-in Montserrat range, verified against the generator options in the
# header of lvgl/src/font/lv_font_montserrat_*.c:
#   -r 0x20-0x7F,0xB0,0x2022  (plus the FontAwesome list above)
ALLOWED = set(range(0x20, 0x80)) | {0xB0, 0x2022} | SYMBOLS

ROOT = Path(__file__).resolve().parents[2]
PROJ = ROOT / "firmware/projects/canary-display"

# Everything that runs on the device. Naming the few directories that "render
# LVGL text" was the first draft of this guard and it was wrong: src/mode and
# src/playground call lv_label_set_text too, so the check passed while those
# screens still drew boxes — a gate that reports success without enforcing its
# invariant is worse than no gate. Scan it all; exclude by what the text is
# FOR, not where it lives (see EXCLUDE_SUFFIXES and the raw-string skip).
SCAN_DIRS = [PROJ / "src", PROJ / "include/canary"]

# Browser-served payloads: system fonts apply, so typography is free there.
# The captive portal's PORTAL_HTML is caught by the raw-string skip in
# strip_comments rather than by filename.
EXCLUDE_SUFFIXES = ("_html.h",)

# Text that never reaches the glass. The serial/web log is read in a terminal
# or a browser, and #error/#warning strings are consumed by the compiler — all
# three have real fonts, so the range rule is about the LVGL faces only.
NOT_ON_GLASS = (
    "log_line(",
    "log_header(",
    "dbg_serial(",
    "Serial.print",
    "boot_kv(",       # the serial boot banner (firmware/common/boot)
    "boot_kvf(",
    "say_evt(",       # mic_alarm's Serial.printf event trace
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
        # A raw string literal, R"delim(...)delim". In this tree that is only
        # ever an HTML/JS payload for a browser, so drop it whole rather than
        # scan it. The guard on the preceding character keeps an ordinary
        # identifier ending in R from being mistaken for one.
        if (
            c == "R"
            and i + 1 < n
            and text[i + 1] == '"'
            and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_"))
        ):
            j = text.find("(", i + 2)
            if j != -1:
                delim = text[i + 2 : j]
                close = ")" + delim + '"'
                end = text.find(close, j + 1)
                if end != -1:
                    out.append("\n" * text.count("\n", i, end + len(close)))
                    i = end + len(close)
                    continue
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
        # Step over char literals. '"' is a real thing this code writes (the
        # JSON escapers test for it), and treating that quote as the start of
        # a string desynchronizes everything after it — which showed up as
        # impossible "raw newline inside a string literal" findings.
        if text[i] == "'":
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
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
            if path.name.endswith(EXCLUDE_SUFFIXES):
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
                    # A raw control byte cannot legally sit in a narrow string
                    # literal, so seeing one means this parser lost its place,
                    # not that the font is missing a glyph. Never report it as
                    # a finding — a guard that cries wolf gets switched off.
                    if cp < 0x20 or cp == 0x7F:
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
