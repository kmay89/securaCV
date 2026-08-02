#!/usr/bin/env python3
"""Derive a printable solid bird silhouette from the SecuraCV glyph artwork.

WHY THIS EXISTS
---------------
The brand glyph (art/securacv_bird_glyph.svg) is LINE ART: every stroke is a
thin filled ribbon, averaging 0.084 mm wide even at the 60 mm badge size and
0.013 mm at the ~9 mm a coupon station can host. No 0.4 mm nozzle lays that
down, so embossing the artwork verbatim yields a blank. What the fit coupon
needs is the bird's *outline*, filled — a solid mark whose survival is worth
testing.

The artwork's own geometry hands us that outline. With fill-rule evenodd the
big contours are: a disc (the surrounding ring's outer edge) and a second
contour that is "the disc with the bird knocked out of it". The bird is the
knocked-out hole, so its boundary is exactly the run of that contour which
does NOT lie on the disc edge. This script isolates that run, trims the thin
perch sliver off the bottom (a ~0.2 mm wisp that would not print), and emits
the result as an inline OpenSCAD polygon.

WHY INLINE
----------
The fit coupon is CARRIED to the website as a single standalone .scad
(gen_builder_manifest.py --site copies that one file and nothing else), so it
may not `include <>` a second file or `import()` an SVG — either would leave
the web builder with a dependency it cannot resolve.

Usage:
    python3 gen_bird_glyph.py art/securacv_bird_glyph.svg
    python3 gen_bird_glyph.py art/securacv_bird_glyph.svg --into FILE.scad
    python3 gen_bird_glyph.py art/securacv_bird_glyph.svg --into FILE.scad --check

Without --into the OpenSCAD block goes to stdout. With it, the block between
the GENERATED-BEGIN/END bird_glyph markers in FILE is rewritten in place, so
the points are never hand-transcribed. --check verifies the file is already up
to date (exit 1 if not) instead of writing.

Points come out normalised to a 1 mm-tall box centred on the origin, so the
coupon scales them with a single `glyph_h` and the data never changes when the
printed size does. Y is flipped: SVG counts down the page, OpenSCAD counts up.
"""
import math
import re
import sys

BEGIN = "// GENERATED-BEGIN bird_glyph"
END = "// GENERATED-END bird_glyph"

ON_EDGE = 30.0    # art units: a contour point this close to the disc edge is
                  # riding the ring, not tracing the bird
TRIM_PCT = 5.0    # trim this much off the bottom — kills the perch sliver
                  # (raises the thinnest solid neck from 25 to 34 art units)


def subpaths(svg_path: str) -> list[list[tuple[float, float]]]:
    d = open(svg_path, encoding="utf-8").read()
    paths = re.findall(r'<path[^>]*\sd="([^"]+)"', d)
    if not paths:
        sys.exit(f"{svg_path}: no <path> found")
    stray = set(re.findall(r"[A-Za-z]", " ".join(paths))) - set("MLZmlz")
    if stray:
        sys.exit(f"path uses curve/arc commands {sorted(stray)}; only M/L/Z "
                 "straight segments are supported — flatten the art first")
    out = []
    for path in paths:
        for chunk in path.split("M"):
            if not chunk.strip():
                continue
            pts = [(float(x), float(y)) for x, y in
                   re.findall(r"(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)", chunk)]
            if len(pts) >= 3:
                out.append(pts)
    return out


def area(poly) -> float:
    a = 0.0
    for i in range(len(poly)):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % len(poly)]
        a += x1 * y2 - x2 * y1
    return abs(a) / 2


def seg_dist(p, a, b) -> float:
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy) / L))
    return math.dist(p, (ax + t * dx, ay + t * dy))


def dist_to_poly(p, poly) -> float:
    return min(seg_dist(p, poly[i], poly[(i + 1) % len(poly)])
               for i in range(len(poly)))


def extract_bird(polys) -> list[tuple[float, float]]:
    """The two largest contours are the disc and the disc-minus-bird; the bird
    is the part of the latter that does not ride the disc edge."""
    ranked = sorted(polys, key=area, reverse=True)
    disc, cut = ranked[0], None
    for cand in ranked[1:]:
        # the disc-minus-bird contour spans essentially the whole disc bbox
        dx = [p[0] for p in cand]
        dy = [p[1] for p in cand]
        ex = [p[0] for p in disc]
        ey = [p[1] for p in disc]
        if (max(dx) - min(dx)) > 0.9 * (max(ex) - min(ex)) and \
           (max(dy) - min(dy)) > 0.9 * (max(ey) - min(ey)):
            cut = cand
            break
    if cut is None:
        sys.exit("could not identify the disc-minus-bird contour in the art")

    on_edge = [dist_to_poly(p, disc) < ON_EDGE for p in cut]
    n = len(cut)
    best_len, best_start, start = 0, 0, None
    for k in range(2 * n):
        if not on_edge[k % n]:
            if start is None:
                start = k
        else:
            if start is not None and best_len < k - start <= n:
                best_len, best_start = k - start, start
            start = None
    if not best_len:
        sys.exit("bird contour not found — check ON_EDGE against the artwork")
    return [cut[(best_start + j) % n] for j in range(best_len)]


def trim_bottom(poly, pct):
    """Sutherland-Hodgman clip against y <= ycut (SVG y grows downward)."""
    ys = [p[1] for p in poly]
    ycut = max(ys) - (max(ys) - min(ys)) * pct / 100.0
    out = []
    for i in range(len(poly)):
        a, b = poly[i], poly[(i + 1) % len(poly)]
        a_in, b_in = a[1] <= ycut, b[1] <= ycut
        if a_in:
            out.append(a)
        if a_in != b_in:
            t = (ycut - a[1]) / (b[1] - a[1])
            out.append((a[0] + t * (b[0] - a[0]), ycut))
    return out


def normalise(pts):
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    cx = (min(xs) + max(xs)) / 2
    cy = (min(ys) + max(ys)) / 2
    h = max(ys) - min(ys)
    return [((x - cx) / h, -(y - cy) / h) for x, y in pts]


def thinnest_neck(poly) -> float:
    """Narrowest solid pinch, in the polygon's own units. Distances whose
    midpoint falls outside the shape are gaps (between toes, say), not necks,
    and are excluded — they fuse when printed instead of vanishing."""
    def inside(pt):
        x, y = pt
        c = False
        for i in range(len(poly)):
            x1, y1 = poly[i]
            x2, y2 = poly[(i + 1) % len(poly)]
            if ((y1 > y) != (y2 > y)) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
                c = not c
        return c

    m = len(poly)
    best = float("inf")
    for i in range(m):
        p = poly[i]
        for j in range(m):
            if min(abs(i - j), m - abs(i - j)) <= 12:
                continue
            a, b = poly[j], poly[(j + 1) % m]
            d = seg_dist(p, a, b)
            if d >= best:
                continue
            ax, ay = a
            dx, dy = b[0] - ax, b[1] - ay
            L = dx * dx + dy * dy
            t = 0.0 if L == 0 else max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy) / L))
            q = (ax + t * dx, ay + t * dy)
            if inside(((p[0] + q[0]) / 2, (p[1] + q[1]) / 2)):
                best = d
    return best


def render_block(pts, neck_mm_at_9) -> str:
    xs = [p[0] for p in pts]
    out = [BEGIN,
           f"// {len(pts)} points, normalised to a 1 mm-tall box centred on",
           f"// the origin (width {max(xs) - min(xs):.4f} at that height).",
           f"// Thinnest solid neck is {neck_mm_at_9:.2f} mm at glyph_h = 9 —",
           "// the wing and tail tapers. They round off at a 0.4 mm nozzle",
           "// rather than vanishing; how much they round IS the test.",
           "function bird_glyph_pts() = ["]
    for i in range(0, len(pts), 4):
        out.append("    " + ", ".join(f"[{x:.4f},{y:.4f}]"
                                      for x, y in pts[i:i + 4]) + ",")
    out += ["];", END]
    return "\n".join(out)


def main() -> None:
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    check = "--check" in args
    args = [a for a in args if a != "--check"]
    into = None
    if "--into" in args:
        i = args.index("--into")
        into = args[i + 1]
        args = args[:i] + args[i + 2:]
    if len(args) != 1:
        sys.exit(__doc__)

    bird = trim_bottom(extract_bird(subpaths(args[0])), TRIM_PCT)
    height = max(p[1] for p in bird) - min(p[1] for p in bird)
    neck = thinnest_neck(bird) / height * 9.0
    block = render_block(normalise(bird), neck)

    if into is None:
        print(block)
        return

    src = open(into, encoding="utf-8").read()
    if BEGIN not in src or END not in src:
        sys.exit(f"{into}: GENERATED-BEGIN/END bird_glyph markers not found")
    head, rest = src.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    new = head + block + tail
    if check:
        if new != src:
            sys.exit(f"{into}: bird glyph block is stale — rerun without --check")
        print(f"{into}: bird glyph block is up to date")
        return
    if new != src:
        open(into, "w", encoding="utf-8").write(new)
        print(f"wrote bird glyph block ({len(block.splitlines())} lines) into {into}")
    else:
        print(f"{into}: bird glyph block already up to date")


if __name__ == "__main__":
    main()
