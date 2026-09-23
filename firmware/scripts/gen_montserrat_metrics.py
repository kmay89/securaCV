#!/usr/bin/env python3
"""Carry LVGL's Montserrat metrics into the display host tests.

The first-boot Join scene decides on the glass whether "SecuraCV-XXXX  •
<key>" fits its row, by asking LVGL for each glyph's advance
(lv_font_get_glyph_width, kerning included). The host test that proves no
row is ever cut (tests_host/test_onboard_layout.cpp, F45) has to measure the
same way — with LVGL's own font data, never an estimated character width —
but the host-test job has no LVGL. So this script copies exactly the numbers
LVGL's measure reads out of its built-in Montserrat sources into
tests_host/montserrat_metrics.h:

  * each glyph's advance (adv_w, 1/16 px) for the range the display font is
    built over (U+0020..U+007E, U+00B0, U+2022 — check_display_glyphs.py's
    range, without the FontAwesome symbols no caption uses);
  * the kerning classes of those glyphs and the class-pair table;
  * kern_scale and line_height.

The faces carried are the ones the Join rows can set: every Character type
ladder's label and caption sizes, parsed out of src/ui/character.cpp (so a
ladder change regenerates the table, and CI names the stale file).

The source is the LVGL checkout the emulator build fetches
(canary-local/emulator/third_party/lvgl, pinned by build.sh's LVGL_TAG);
this script refuses any other version. LVGL 9.5.0's tables for these sizes
compared equal to 8.4.0's when this was written, and so does the advance
math (lv_font_fmt_txt.c), so the proof covers both majors the display ships.

Run from the repo root (after canary-local/emulator/build.sh has fetched
third_party):
    python3 firmware/scripts/gen_montserrat_metrics.py           # write
    python3 firmware/scripts/gen_montserrat_metrics.py --check   # CI gate
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROJ = ROOT / "firmware/projects/canary-display"
OUT = PROJ / "tests_host/montserrat_metrics.h"
CHARACTER = PROJ / "src/ui/character.cpp"
BUILD_SH = ROOT / "canary-local/emulator/build.sh"
DEFAULT_LVGL = ROOT / "canary-local/emulator/third_party/lvgl"

# The glyphs carried, in index order: printable ASCII, then the degree sign
# and the bullet (the two non-ASCII code points the display font carries).
CODEPOINTS = list(range(0x20, 0x7F)) + [0xB0, 0x2022]

# The roles whose faces the Join scene's low rows use (onboard_ui.cpp: the
# small glass sets both rows in the caption face and steps down to the
# default Character's caption; the wide glass sets its credentials in the
# label face and the hint in the caption face).
ROLES = ("label", "caption")


def fail(msg: str) -> None:
    print(f"gen_montserrat_metrics: {msg}", file=sys.stderr)
    sys.exit(1)


def c_array(text: str, name: str) -> list[int]:
    m = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        fail(f"no {name}[] in the font source")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    return [int(v, 0) for v in body.replace("\n", " ").split(",") if v.strip()]


def field(text: str, name: str) -> int:
    m = re.search(r"\." + re.escape(name) + r"\s*=\s*(-?\d+)", text)
    if not m:
        fail(f"no .{name} in the font source")
    return int(m.group(1))


def glyph_ids(text: str) -> list[int]:
    """LVGL glyph id of each carried code point, from the font's cmaps."""
    ranges = re.findall(
        r"\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),\s*"
        r"\.glyph_id_start\s*=\s*(\d+),\s*\.unicode_list\s*=\s*(\w+),",
        text,
    )
    if not ranges:
        fail("no cmaps in the font source")
    out = []
    for cp in CODEPOINTS:
        gid = 0
        for start, length, gstart, ulist in ranges:
            start, length, gstart = int(start), int(length), int(gstart)
            if not start <= cp < start + length:
                continue
            if ulist == "NULL":
                gid = gstart + (cp - start)
            else:
                offsets = c_array(text, ulist)
                if cp - start in offsets:
                    gid = gstart + offsets.index(cp - start)
            if gid:
                break
        if not gid:
            fail(f"U+{cp:04X} has no glyph in the font source")
        out.append(gid)
    return out


def ladder_sizes() -> list[int]:
    sizes = set()
    for m in re.finditer(r"&lv_font_montserrat_(\d+),\s*//\s*(\w+)",
                         CHARACTER.read_text()):
        if m.group(2) in ROLES:
            sizes.add(int(m.group(1)))
    if not sizes:
        fail(f"no {'/'.join(ROLES)} faces parsed out of {CHARACTER}")
    return sorted(sizes)


def lvgl_tag() -> str:
    m = re.search(r'^LVGL_TAG="(v[\d.]+)"', BUILD_SH.read_text(), re.M)
    if not m:
        fail(f"no LVGL_TAG in {BUILD_SH}")
    return m.group(1)


def checkout_version(lvgl: Path) -> str:
    # v8 defines the version in lvgl.h, v9 in lv_version.h.
    head = "".join(p.read_text() for p in (lvgl / "lvgl.h", lvgl / "lv_version.h")
                   if p.is_file())
    parts = []
    for p in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"#define LVGL_VERSION_{p} (\d+)", head)
        if not m:
            fail(f"no LVGL_VERSION_{p} in {lvgl}'s lvgl.h / lv_version.h")
        parts.append(m.group(1))
    return "v" + ".".join(parts)


def rows(values: list[int], per: int, width: int) -> str:
    lines = []
    for i in range(0, len(values), per):
        chunk = values[i:i + per]
        lines.append("    " + ", ".join(f"{v:>{width}}" for v in chunk) + ",")
    return "\n".join(lines)


def render(lvgl: Path, tag: str) -> str:
    sizes = ladder_sizes()
    out = [
        "// GENERATED by firmware/scripts/gen_montserrat_metrics.py from LVGL "
        f"{tag}'s",
        "// built-in Montserrat sources — do not edit; re-run the script.",
        "//",
        "// The numbers LVGL's one-line measure reads (lv_font_get_glyph_width:",
        "// adv_w, the kerning classes and their pair table, kern_scale) for the",
        "// glyphs the display font carries, in the faces the first-boot Join",
        "// scene's rows can set. test_onboard_layout.cpp measures with them.",
        "#pragma once",
        "",
        "namespace montserrat_metrics {",
        "",
        "// Glyph index: U+0020..U+007E at 0..94, U+00B0 at 95, U+2022 at 96.",
        f"constexpr int kGlyphs = {len(CODEPOINTS)};",
        "",
        "struct Face {",
        "  int size;                          // px (lv_font_montserrat_<size>)",
        "  int line_height;                   // lv_font_get_line_height()",
        "  int kern_scale;                    // 1/16 units, as the font says",
        "  int right_classes;                 // the pair table's row stride",
        "  const unsigned short* adv_w;       // [kGlyphs], 1/16 px",
        "  const unsigned char* kern_left;    // [kGlyphs], 0 = none",
        "  const unsigned char* kern_right;   // [kGlyphs], 0 = none",
        "  const signed char* kern_values;    // [left classes * right_classes]",
        "};",
        "",
    ]
    faces = []
    for size in sizes:
        src = lvgl / f"src/font/lv_font_montserrat_{size}.c"
        if not src.is_file():
            fail(f"{src} is missing (is montserrat_{size} a built-in face?)")
        text = src.read_text()
        gids = glyph_ids(text)
        gd = text[text.index("glyph_dsc[] = {"):]
        gd = gd[:gd.index("};")]
        adv = [int(v) for v in re.findall(r"\.adv_w\s*=\s*(\d+)", gd)]
        left_map = c_array(text, "kern_left_class_mapping")
        right_map = c_array(text, "kern_right_class_mapping")
        values = c_array(text, "kern_class_values")
        left_cnt = field(text, "left_class_cnt")
        right_cnt = field(text, "right_class_cnt")
        if len(values) != left_cnt * right_cnt:
            fail(f"montserrat_{size}: {len(values)} pair values for "
                 f"{left_cnt} x {right_cnt} classes")
        if field(text, "kern_classes") != 1:
            fail(f"montserrat_{size}: kerning is not class-based")
        # Refuse what the header's C types cannot hold rather than write a
        # table that silently says something else.
        carried = {
            "adv_w": ([adv[g] for g in gids], 0, 0xFFFF),
            "kerning class": ([left_map[g] for g in gids] +
                              [right_map[g] for g in gids], 0, 0xFF),
            "kerning value": (values, -128, 127),
        }
        for what, (vals, lo, hi) in carried.items():
            bad = [v for v in vals if not lo <= v <= hi]
            if bad:
                fail(f"montserrat_{size}: {what} {bad[0]} outside [{lo}, {hi}]")
        if max(left_map[g] for g in gids) > left_cnt or \
                max(right_map[g] for g in gids) > right_cnt:
            fail(f"montserrat_{size}: a kerning class past the pair table")
        n = f"k{size}"
        out += [
            f"// lv_font_montserrat_{size}",
            f"static const unsigned short {n}_adv[kGlyphs] = {{",
            rows([adv[g] for g in gids], 12, 3),
            "};",
            f"static const unsigned char {n}_left[kGlyphs] = {{",
            rows([left_map[g] for g in gids], 16, 2),
            "};",
            f"static const unsigned char {n}_right[kGlyphs] = {{",
            rows([right_map[g] for g in gids], 16, 2),
            "};",
            f"static const signed char {n}_kern[{left_cnt * right_cnt}] = {{",
            rows(values, 24, 1),
            "};",
            "",
        ]
        faces.append(
            f"    {{{size}, {field(text, 'line_height')}, "
            f"{field(text, 'kern_scale')}, {right_cnt}, {n}_adv, {n}_left, "
            f"{n}_right, {n}_kern}},")
    out += [
        "static const Face kFaces[] = {",
        *faces,
        "};",
        f"constexpr int kFaceCount = {len(faces)};",
        "",
        "}  // namespace montserrat_metrics",
        "",
    ]
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="fail if the committed header is stale")
    ap.add_argument("--lvgl", type=Path, default=DEFAULT_LVGL,
                    help="LVGL checkout (default: the emulator's third_party)")
    args = ap.parse_args()
    if not (args.lvgl / "lvgl.h").is_file():
        fail(f"no LVGL checkout at {args.lvgl} — run "
             "canary-local/emulator/build.sh once to fetch the pinned one")
    tag = lvgl_tag()
    have = checkout_version(args.lvgl)
    if have != tag:
        fail(f"{args.lvgl} is LVGL {have}; build.sh pins {tag}")
    text = render(args.lvgl, tag)
    if args.check:
        if not OUT.is_file() or OUT.read_text() != text:
            print(f"STALE: {OUT.relative_to(ROOT)} — re-run "
                  "python3 firmware/scripts/gen_montserrat_metrics.py",
                  file=sys.stderr)
            return 1
        print(f"OK: {OUT.relative_to(ROOT)} matches LVGL {tag}")
        return 0
    OUT.write_text(text)
    print(f"wrote {OUT.relative_to(ROOT)} (LVGL {tag})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
