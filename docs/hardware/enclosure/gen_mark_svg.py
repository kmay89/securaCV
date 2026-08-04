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

So the SVG is DERIVED now. This script reads the path constants straight out
of the .scad, applies the same Chaikin smoothing the .scad applies, and writes
the artwork. Change the bird in one place; the art follows or CI fails.

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
The FILLED form — the silhouette solid, with the eye and the wing knocked back
out of it — because that is the brand art's own construction and what the 7"
case prints in accent filament. Stroke weight is the case's own line weight
expressed in design units, so the drawn shape matches the molded one.

TWO GROUPS, NOT A MASK. The knock-outs are a second group painted in #fff over
a first group painted in #000, which is exactly the two-filament part: the
mark in accent, the eye and the wing line in body color. An SVG <mask> would
be the tidier way to say "transparent hole", and it is the wrong one here —
this file's job is to be imported by engravers, laser drivers and cutters, and
mask support in that software ranges from partial to absent. Two flat groups
import as two flat groups everywhere. Recolor them to the real filaments.
"""
import math
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "canary_mark_lib.scad"
OUT = HERE / "securacv_bird_glyph.svg"

# The stroke, in DESIGN units. The 7" case draws the mark 32 mm tall with a
# 2.0 mm rib, and mark_span() is 110 — so 2.0 / (32/110) = 6.875. Written as
# that division rather than as 6.875 so the two cannot drift apart silently.
CASE_MARK_H = 32.0
CASE_MARK_RIB = 2.0

# Rendered size on paper. The glyph is scale-free; this only sets what an
# importer sees before it scales the thing.
DOC_MM = 60.0


def _scad() -> str:
    return SRC.read_text(encoding="utf-8")


def _num_list(name: str, depth: int) -> list:
    """Pull `name = [...];` out of the .scad and eval it as nested lists.

    depth is how many levels of brackets the value has (2 = list of points,
    3 = list of lists of points) and is checked, not assumed — a path that
    changed shape is a parse that must fail, not one that guesses.
    """
    src = _scad()
    m = re.search(rf"^{re.escape(name)}\s*=\s*(\[.*?\]);", src, re.S | re.M)
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
    """Pull the constant out of `function mark_x0(t) = -50.0 - t/2;`."""
    m = re.search(rf"^function\s+{re.escape(name)}\(t\)\s*=\s*(-?[\d.]+)\s*[-+]\s*t/2;",
                  _scad(), re.M)
    if not m:
        raise SystemExit(f"gen_mark_svg: function {name}(t) not found in {SRC.name}")
    return float(m.group(1))


def chaikin(pts, closed, k=3):
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


def beak_shapes(p0, p1, r0, r1):
    """The .scad's _gtaper as SVG: two circles plus their outer tangent quad.

    _gtaper hulls a circle at each end; the hull of two circles is exactly
    that. Overlapping shapes in one fill group union, so no boolean is needed.
    """
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    dist = math.hypot(dx, dy)
    if dist <= abs(r0 - r1):
        raise SystemExit("gen_mark_svg: beak circles nest — no external tangent")
    # Angle from the center line to each external tangent point.
    a = math.atan2(dy, dx)
    b = math.acos((r0 - r1) / dist)
    quad = [(p0[0] + r0 * math.cos(a + b), p0[1] + r0 * math.sin(a + b)),
            (p1[0] + r1 * math.cos(a + b), p1[1] + r1 * math.sin(a + b)),
            (p1[0] + r1 * math.cos(a - b), p1[1] + r1 * math.sin(a - b)),
            (p0[0] + r0 * math.cos(a - b), p0[1] + r0 * math.sin(a - b))]
    pts = " ".join(f"{f(x)},{f(-y)}" for x, y in quad)
    return [f'<circle cx="{f(p0[0])}" cy="{f(-p0[1])}" r="{f(r0)}"/>',
            f'<circle cx="{f(p1[0])}" cy="{f(-p1[1])}" r="{f(r1)}"/>',
            f'<polygon points="{pts}"/>']


def build() -> str:
    rib = CASE_MARK_RIB / (CASE_MARK_H / _fn("mark_span"))

    body = chaikin(_num_list("_g_body", 2), True)
    wing = chaikin(_num_list("_g_wing", 2), True)
    tailu = _num_list("_g_tailu", 2)      # unsmoothed in the .scad — see there
    taill = _num_list("_g_taill", 2)
    beak = _num_list("_g_beak", 2)
    eye = _num_list("_g_eye", 1)
    legs = _num_list("_g_legs", 3)
    feet = _num_list("_g_feet", 3)
    eye_d = max(_fn("_g_eye_d"), rib * 1.4)
    beak_root = _fn("_g_beak_root")

    x0 = _bbox_term("mark_x0") - rib / 2
    x1 = _bbox_term("mark_x1") + rib / 2
    y0 = _bbox_term("mark_y0") - rib / 2
    y1 = _bbox_term("mark_y1") + rib / 2
    # Air around the box so a round cap sitting exactly on it is not clipped
    # by a viewer that rounds the viewBox the other way — and so the beak tip,
    # which is the closest thing to an edge, does not READ as clipped either.
    pad = 2.0
    vb = (x0 - pad, -(y1 + pad), (x1 - x0) + 2 * pad, (y1 - y0) + 2 * pad)

    caps = 'stroke-linecap="round" stroke-linejoin="round"'

    # The mark. Strokes carry the rib; the beak does NOT — _gtaper hulls two
    # circles whose diameters ARE the finished width, so stroking it as well
    # would inflate the beak by half a rib all round and push its tip out
    # through the viewBox. (It did, on the first generated file.)
    solid = [f'<path d="{path_d(body, True)}"/>',
             f'<path d="{path_d(tailu + taill[-2::-1], True)}"/>',
             f'<g stroke="none">{"".join(beak_shapes(beak[0], beak[1], beak_root / 2, rib / 2))}</g>']
    for seg in legs + feet:
        solid.append(f'<path fill="none" d="{path_d(seg, False)}"/>')

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{f(vb[0])} {f(vb[1])}'
        f' {f(vb[2])} {f(vb[3])}" width="{f(DOC_MM)}mm" height='
        f'"{f(DOC_MM * vb[3] / vb[2])}mm">',
        "  <title>SecuraCV Bird Glyph</title>",
        "  <desc>The Canary house mark. GENERATED from "
        "canary_mark_lib.scad by gen_mark_svg.py — edit the paths there, not "
        "here. Scalable outline geometry for embossing, engraving, inlay, "
        "stamping, or laser cutting. Two groups, one per filament: #000 is the "
        "mark, #fff is what the mark's own color is knocked out of it.</desc>",
        f'  <g id="securacv-bird-mark" fill="#000" stroke="#000"'
        f' stroke-width="{f(rib)}" {caps}>',
    ]
    lines += [f"    {s}" for s in solid]
    lines += [
        "  </g>",
        f'  <g id="securacv-bird-knockout" fill="#fff" stroke="#fff"'
        f' stroke-width="{f(rib)}" {caps}>',
        f'    <path fill="none" d="{path_d(wing, True)}"/>',
        f'    <circle stroke="none" cx="{f(eye[0])}" cy="{f(-eye[1])}"'
        f' r="{f(eye_d / 2)}"/>',
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
