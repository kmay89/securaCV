#!/usr/bin/env python3
"""Regenerate securacv_bird_glyph.svg from canary_mark_lib.scad.

    python3 gen_mark_svg.py            # regenerate the glyph
    python3 gen_mark_svg.py --check    # CI: fail if the committed SVG is stale

WHY THIS EXISTS
---------------
The repo shipped TWO birds. `canary_mark_lib.scad` is the mark as printable
geometry — the one every enclosure debosses — and `securacv_bird_glyph.svg`
was the mark as production art, shipped beside the fit coupon for engraving,
inlay and laser work. They were separate files with separate drawings, so
"exactly one bird in the line to change if the mark ever changes" was true of
the .scad and false of the repo: redrawing the mark left the SVG showing the
old one, and nothing would have said so.

So the SVG is DERIVED. This script reads the path constants straight out of the
.scad, applies the same Chaikin smoothing the .scad applies, and writes the
artwork. Change the bird in one place; the art follows or CI fails.

WHY IT PARSES THE .SCAD INSTEAD OF ASKING OPENSCAD
-------------------------------------------------
OpenSCAD can export 2D to SVG directly, which would be the obvious route and
is the wrong one for a file that gets byte-diffed: its output moves with the
OpenSCAD build, and this repo already learned that lesson about preview
meshes (see canary-local.yml — the enclosure catalog is drift-checked for its
JSON only, "preview meshes vary by openscad build"). Pure Python arithmetic
over the same numbers is reproducible on any machine, which is what a
committed generated file needs.

The parse is deliberately narrow and LOUD: it knows the exact names it wants
and raises if any of them stops matching, rather than quietly emitting a bird
with a missing wing.

THE ARTWORK
-----------
The mark is MONOLINE — one stroke weight for the outline, the C, the V and the
notepad alike — so the SVG is one group of stroked paths and nothing else. No
fills, no masks, no knock-outs: there is no silhouette to fill, because the
drawing IS the line. That also makes it importable anywhere. Engravers, laser
drivers and cutters vary wildly in what they do with masks and compound fills;
every one of them understands a stroked path.

The weight is READ OFF THE 7" CASE (`bird_h` / `bird_rib`), so the drawn art
and the molded mark are the same weight by construction rather than by two
numbers that agree today.
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "canary_mark_lib.scad"
CASE = HERE / "canary_s3_lcd7.scad"
OUT = HERE / "securacv_bird_glyph.svg"

# Rendered size on paper. The glyph is scale-free; this only sets what an
# importer sees before it scales the thing.
DOC_MM = 60.0

# The drawing, as (constant name, closed?, smoothing passes) — the SAME three
# facts mark_bird_2d() uses for each path. Listed here rather than inferred,
# because "which paths are closed" is a property of the drawing and a wrong
# guess produces a bird with a hole in it that still validates as SVG.
PATHS = [
    ("_m_head", False, 2),
    ("_m_beak", False, 0),
    ("_m_strap", False, 0),
    ("_m_pad", True, 2),
    ("_m_padclip", False, 0),
    ("_m_belly", False, 2),
    ("_m_back", False, 2),
    ("_m_tailend", False, 2),
    ("_m_wing", False, 2),
    ("_m_cee", False, 2),
    ("_m_vee", False, 0),
]
GROUPS = [("_m_legs", False, 0), ("_m_feet", False, 1)]


def _scad() -> str:
    return SRC.read_text(encoding="utf-8")


def _case_num(name: str) -> float:
    """Pull `name = <number>;` out of the 7" case — its Customizer default.

    Not duplicated as a constant here, deliberately. The first version of this
    script copied bird_h and bird_rib in, which meant --check could not see a
    re-weighted mark at all: change the case's rib and this would regenerate
    the identical old-weight SVG and pass, while the workflow step above it
    claimed the artwork matched the molded mark. A drift check that cannot
    detect the drift it names is worse than no check, because it is believed.
    """
    m = re.search(rf"^{re.escape(name)}\s*=\s*(-?[\d.]+)\s*;",
                  CASE.read_text(encoding="utf-8"), re.M)
    if not m:
        raise SystemExit(f"gen_mark_svg: {name} not found in {CASE.name}")
    return float(m.group(1))


def _num_list(name: str, depth: int) -> list:
    """Pull `name = [...];` out of the .scad and eval it as nested lists.

    depth is how many levels of brackets the value has (2 = list of points,
    3 = list of lists of points) and is checked, not assumed — a path that
    changed shape is a parse that must fail, not one that guesses.
    """
    m = re.search(rf"^{re.escape(name)}\s*=\s*(\[.*?\]);", _scad(), re.S | re.M)
    if not m:
        raise SystemExit(f"gen_mark_svg: {name} not found in {SRC.name}")
    body = m.group(1)
    if not re.fullmatch(r"[\s\[\]\d,.\-]+", body):
        raise SystemExit(f"gen_mark_svg: {name} is not a plain number list")
    val = eval(body)  # noqa: S307 — the regex above admits digits and brackets only

    def d(v):
        return 1 + d(v[0]) if isinstance(v, list) else 0

    if d(val) != depth:
        raise SystemExit(f"gen_mark_svg: {name} is {d(val)} deep, expected {depth}")
    return val


def _fn(name: str) -> float:
    """Pull `function name() = <number>;` out of the .scad."""
    m = re.search(rf"^function\s+{re.escape(name)}\(\)\s*=\s*([\d.\-]+);",
                  _scad(), re.M)
    if not m:
        raise SystemExit(f"gen_mark_svg: function {name}() not found in {SRC.name}")
    return float(m.group(1))


def _bbox_term(name: str) -> float:
    """Pull the constant out of `function mark_x0(t) = -51.2 - t/2;`."""
    m = re.search(
        rf"^function\s+{re.escape(name)}\(t\)\s*=\s*(-?[\d.]+)\s*[-+]\s*t/2;",
        _scad(), re.M)
    if not m:
        raise SystemExit(f"gen_mark_svg: function {name}(t) not found in {SRC.name}")
    return float(m.group(1))


def chaikin(pts, closed, k):
    """The .scad's _chaik/_smooth, arithmetic for arithmetic."""
    for _ in range(k):
        n = len(pts)
        out = []
        for i in range(n if closed else n - 1):
            a, b = pts[i], pts[(i + 1) % n]
            out.append([0.75 * a[0] + 0.25 * b[0], 0.75 * a[1] + 0.25 * b[1]])
            out.append([0.25 * a[0] + 0.75 * b[0], 0.25 * a[1] + 0.75 * b[1]])
        pts = out
    return pts


def f(v: float) -> str:
    """Fixed 3 decimals, no negative zero — the whole file must be stable."""
    s = f"{v:.3f}"
    return "0.000" if s == "-0.000" else s


def path_d(pts, closed):
    """SVG path data. Design y is up, SVG y is down, so y is negated here."""
    d = f"M {f(pts[0][0])} {f(-pts[0][1])}"
    for p in pts[1:]:
        d += f" L {f(p[0])} {f(-p[1])}"
    return d + " Z" if closed else d


def build() -> str:
    rib = _case_num("bird_rib") / (_case_num("bird_h") / _fn("mark_span"))

    paths = [path_d(chaikin(_num_list(n, 2), closed, k), closed)
             for n, closed, k in PATHS]
    for name, closed, k in GROUPS:
        paths += [path_d(chaikin(seg, closed, k), closed)
                  for seg in _num_list(name, 3)]
    eye = _num_list("_m_eye", 1)
    eye_d = max(_fn("_m_eye_d"), rib)

    x0 = _bbox_term("mark_x0") - rib / 2
    x1 = _bbox_term("mark_x1") + rib / 2
    y0 = _bbox_term("mark_y0") - rib / 2
    y1 = _bbox_term("mark_y1") + rib / 2
    # Air around the box so a round cap sitting exactly on it is not clipped by
    # a viewer that rounds the viewBox the other way — and so the beak's tip,
    # which is the closest thing to an edge, does not READ as clipped either.
    pad = 2.0
    vb = (x0 - pad, -(y1 + pad), (x1 - x0) + 2 * pad, (y1 - y0) + 2 * pad)

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{f(vb[0])} {f(vb[1])}'
        f' {f(vb[2])} {f(vb[3])}" width="{f(DOC_MM)}mm" height='
        f'"{f(DOC_MM * vb[3] / vb[2])}mm">',
        "  <title>SecuraCV Bird Glyph</title>",
        "  <desc>The Canary house mark: a monoline bird carrying a notepad, "
        "with a C spiralled into its wing and a V nested in its tail. GENERATED "
        "from canary_mark_lib.scad by gen_mark_svg.py — edit the paths there, "
        "not here. One stroke weight throughout, taken from the 7\" case, for "
        "embossing, engraving, inlay, stamping or laser cutting.</desc>",
        f'  <g id="securacv-bird" fill="none" stroke="#000"'
        f' stroke-width="{f(rib)}" stroke-linecap="round"'
        ' stroke-linejoin="round">',
    ]
    lines += [f'    <path d="{d}"/>' for d in paths]
    lines += [
        f'    <circle fill="#000" stroke="none" cx="{f(eye[0])}"'
        f' cy="{f(-eye[1])}" r="{f(eye_d / 2)}"/>',
        "  </g>",
        "</svg>",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    svg = build()
    if "--check" in sys.argv:
        have = OUT.read_text(encoding="utf-8") if OUT.exists() else None
        if have != svg:
            print(f"{OUT.name} is stale — run: python3 {Path(__file__).name}",
                  file=sys.stderr)
            return 1
        print(f"{OUT.name} matches {SRC.name}")
        return 0
    OUT.write_text(svg, encoding="utf-8")
    print(f"wrote {OUT.name} ({len(svg)} bytes) from {SRC.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
