#!/usr/bin/env python3
"""gen_revisions.py — a revision for every printable part, and what changed.

    python3 docs/hardware/enclosure/gen_revisions.py                 # record: a new rev for every part whose shape moved
    python3 docs/hardware/enclosure/gen_revisions.py --check         # CI gate: no part moved since its last recorded rev
    python3 docs/hardware/enclosure/gen_revisions.py --check --rendered   # the same, every dev render expected on disk
    python3 docs/hardware/enclosure/gen_revisions.py --note "…"      # words for the revs this run records
    python3 docs/hardware/enclosure/gen_revisions.py --render canary_hub_din.scad   # render one case's parts, then record
    python3 docs/hardware/enclosure/gen_revisions.py --render-stale  # render only what is older than its sources, then record
    python3 docs/hardware/enclosure/gen_revisions.py --backfill      # (re)derive the committed parts' revs from git, then record
    python3 docs/hardware/enclosure/gen_revisions.py --list          # every part, its home, rev and recipe

Writes canary-local/devices/revisions.json — one entry per printable part
the catalog renders, with:

  rev        "r1", "r2", … — this PART's own count, bumped when its printed
             shape moved (not when a comment moved, not when a neighbor in
             the same file moved)
  src        eight hex chars over the comment-free, whitespace-normalized
             sources that draw it — the case file, every library it uses,
             transitively, and the -D selectors render.sh passes — so a
             reader can ask "did anything that could move this part change?"
             without OpenSCAD (the same idea as gen_stamp.py's SRC, per part)
  geom       what the rendered mesh IS: triangles, volume, area, bounding box,
             centroid and second moments (mm, mm², mm³), read off the STL
  history    every rev with its `changes`: the Customizer knobs whose literal
             moved, the volume/area/box deltas, the commits that touched its
             sources, an optional human note — and `regions`, boxes in the
             part's own coordinates (the STL frame, the print pose) where the
             new surface is not where the old one was, which the Lab's
             enclosure tab and the website showroom tint on the part

WHY. The catalog had three partial answers to "did this part change": the
7" case's build stamp (one case), the figures' `rev` (the drawing, not the
mesh) and the website's print-files sha256 (three files). Nothing said it
for the other eighty parts, nothing said WHAT changed, and nothing could
show it. A committed STL whose source moved an internal post — a feature
inside the bounding box — passed every gate (the envelope gates compare
boxes) with the old mesh still in the tree. The fingerprint here is the
mesh's own moments, so a post that moved 1 mm reads as moved.

HOW A PART IS IDENTIFIED. The recipes are render.sh's: this script runs it
with a stand-in OpenSCAD that records every `-o OUT -D … SRC` it is asked
for and renders nothing, so the list of parts, their files and their -D
selectors are whatever render.sh does today — never a second list to keep
in step. The Lab's coarse preview meshes (gen_enclosures.py RENDER_PRESETS)
are the other recipe source. A part's key is its output file:
`canary_wap_enclosure_compact_base.stl` (released, committed beside the
sources), `dev_hub_tray.stl` (in development, gitignored, rendered by CI and
by whoever runs render.sh) or `preview/canary_combo_front.stl` (the Lab's
committed coarse mesh).

WHAT THE GATE PROVES. --check (no OpenSCAD) recomputes every part's src
digest and, for every part whose STL is on disk, its geometry fingerprint,
and fails if either differs from the recorded rev — "the sources that draw
X moved since r3 was recorded" or "X's mesh moved since r3 was recorded".
In lint.yml that covers every committed STL plus every part's sources; in
enclosure.yml, right after render.sh, `--rendered` demands every dev render
too, so an in-development part cannot move unrecorded either. OpenSCAD's
STL bytes are not deterministic, so the fingerprint is compared within
tolerances (TOL below, measured: two renders of one knob differ in volume
by 8e-7 relative); triangle counts are exact for one OpenSCAD version.

RECORDING. A write run reads the STLs on disk (run render.sh first; or
--render SCAD to render one case's recipes here) and, for every part whose
fingerprint moved, records rev N+1 and works out the changes against rev N:
the previous mesh is the committed STL at the commit whose bytes carry rev
N's fingerprint (released and preview parts), or rev N's sources rendered
from a temporary worktree at the commit whose digest is rev N's `src` (dev
parts; needs OpenSCAD and that commit in history — a shallow clone gets the
numbers and no regions, and says so). Regions come from voxelizing both
surfaces at VOXEL_MM and taking what the new surface has that is not within
a cell of the old (added) and the reverse (removed); connected cells become
boxes, an added box meeting a removed one is "moved". A change below the
voxel size (a 0.3 mm wall) shows in the volume delta and not in a box, and
the entry says so. Nothing here is typed: a `--note` is the one human line.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from collections import deque
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
ENC_REL = "docs/hardware/enclosure"
OUT = REPO / "canary-local/devices/revisions.json"
OUT_REL = "canary-local/devices/revisions.json"
PREVIEW_REL = "canary-local/enclosures/preview"
RENDER_SH = HERE / "render.sh"

sys.path.insert(0, str(HERE))
import scad_probe  # noqa: E402
from gen_builder_manifest import parse_scad, DEP_RE  # noqa: E402

# Fingerprint tolerances. Two renders of the same sources agree to float32
# vertex precision; a design move is orders of magnitude larger.
TOL = {
    "bbox_mm": 0.01,          # gen_assembled_dims.py's TOL
    "abs_mm3": 0.02, "rel": 5e-6,   # volume and area: abs + rel·|value|
    "centroid_mm": 0.005,
    "moment_abs": 0.002,      # second moments and products (mm²): abs + rel·scale
    "third_abs": 0.01,        # third moments (mm³): abs + rel·(diagonal moment)^1.5
}
VOXEL_MM = 0.4               # region resolution: the material diff's voxel
MOVED_GAP = 2                # cells between an added and a removed box that are one moved feature
MAX_REGIONS = 24             # more are merged by nearest pair
MAX_COMMITS = 8
MAX_HISTORY_WALK = 300       # commits of a part's sources to search for its previous rev


# ---------------------------------------------------------------------------
# recipes — what render.sh and gen_enclosures.py render, derived
# ---------------------------------------------------------------------------

def _parse_define(v: str):
    """A -D value as render.sh spells it: "drum" -> "drum", 15 -> 15, true -> True."""
    s = v.strip()
    if s.startswith('"') and s.endswith('"'):
        return s[1:-1]
    if s in ("true", "false"):
        return s == "true"
    try:
        f = float(s)
        return int(f) if f.is_integer() and "." not in s else f
    except ValueError:
        return s


def define_token(v) -> str:
    """The reverse: a value as -D wants it."""
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    return json.dumps(v)


RECORD_RS = "\x1e"


def record_render_sh() -> list[dict]:
    """Every binstl export render.sh asks OpenSCAD for, as (out, scad, defines),
    by running it with a stand-in binary that records its arguments and
    renders nothing — so the recipe list IS render.sh, not a copy of it."""
    with tempfile.TemporaryDirectory() as td:
        fake = Path(td) / "openscad"
        log = Path(td) / "record.log"
        fake.write_text("#!/bin/sh\n"
                        'printf "%s\\036" "$@" >> "$REVISIONS_RECORD"\n'
                        'printf "\\n" >> "$REVISIONS_RECORD"\n', encoding="utf-8")
        fake.chmod(0o755)
        env = dict(os.environ, OPENSCAD=str(fake), REVISIONS_RECORD=str(log))
        r = subprocess.run(["bash", str(RENDER_SH), "--no-png"], cwd=str(HERE), env=env,
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"gen_revisions: render.sh refused to run under the recording stand-in:\n"
                     f"{r.stdout}{r.stderr}")
        # split on "\n" only: str.splitlines() also breaks on the record
        # separator (U+001E is a line boundary to Python)
        lines = log.read_text(encoding="utf-8").split("\n") if log.exists() else []
    recipes = []
    for line in lines:
        argv = [a for a in line.split(RECORD_RS) if a != ""]
        if "--export-format" not in argv:
            continue                      # the SVG templates: not parts
        if argv[argv.index("--export-format") + 1] != "binstl":
            continue
        out = argv[argv.index("-o") + 1]
        defines = {}
        for i, a in enumerate(argv):
            if a == "-D":
                k, v = argv[i + 1].split("=", 1)
                defines[k.strip()] = _parse_define(v)
        recipes.append({"key": out, "scad": argv[-1], "defines": defines,
                        "home": "dev" if out.startswith("dev_") else "released"})
    if not recipes:
        sys.exit("gen_revisions: render.sh recorded no STL exports — the stand-in never ran?")
    return recipes


GEN_ENCLOSURES = REPO / "canary-local/tools/gen_enclosures.py"
# gen_enclosures.py's _preview_jobs() adds these to `part` for every preview
# mesh (coarse curves keep the Lab's files small). Read here, not imported:
# importing that module regenerates the catalogs as a side effect.
PREVIEW_QUALITY = {"$fa": 6, "$fs": 0.8}


def preview_presets() -> dict[str, list[str]]:
    """gen_enclosures.py's RENDER_PRESETS, read off its source as a literal
    (ast, nothing executed) — the Lab's committed coarse previews."""
    import ast
    tree = ast.parse(GEN_ENCLOSURES.read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == "RENDER_PRESETS" for t in node.targets):
            return ast.literal_eval(node.value)
    sys.exit(f"gen_revisions: {GEN_ENCLOSURES.name} has no RENDER_PRESETS literal")


def preview_recipes() -> list[dict]:
    out = []
    for scad, parts in preview_presets().items():
        for part in parts:
            out.append({"key": f"preview/{Path(scad).stem}_{part}.stl", "scad": scad,
                        "defines": {"part": part, **PREVIEW_QUALITY}, "home": "preview"})
    return out


def all_recipes() -> list[dict]:
    seen = set()
    out = []
    for r in record_render_sh() + preview_recipes():
        if r["key"] in seen:
            sys.exit(f"gen_revisions: {r['key']} is rendered twice — two recipes, one file")
        seen.add(r["key"])
        out.append(r)
    return sorted(out, key=lambda r: r["key"])


def stl_path(recipe: dict) -> Path:
    if recipe["home"] == "preview":
        return REPO / PREVIEW_REL / recipe["key"].split("/", 1)[1]
    return HERE / recipe["key"]


def stl_rel(recipe: dict) -> str:
    return stl_path(recipe).relative_to(REPO).as_posix()


# ---------------------------------------------------------------------------
# sources — the design digest, from the tree or from a commit
# ---------------------------------------------------------------------------

def strip_comments(src: str) -> str:
    """Drop // and /* */ comments, leaving string literals alone."""
    out, i, n = [], 0, len(src)
    in_str = False
    while i < n:
        ch = src[i]
        if in_str:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(src[i + 1])
                i += 2
                continue
            if ch == '"':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
            out.append(ch)
            i += 1
            continue
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def deps_of(text: str) -> list[str]:
    return [m.group(1).strip() for m in (DEP_RE.match(line) for line in text.splitlines()) if m]


def source_set(scad: str, read) -> list[tuple[str, str]]:
    """[(name, text)] for the case file and its libraries, transitively, in
    a stable order. `read(name)` returns a file's text or None."""
    order: list[tuple[str, str]] = []
    seen: set[str] = set()

    def walk(name: str) -> None:
        if name in seen:
            return
        seen.add(name)
        text = read(name)
        if text is None:
            raise FileNotFoundError(name)
        for d in deps_of(text):
            walk(d)
        order.append((name, text))

    walk(scad)
    return order


def src_digest(scad: str, defines: dict, read) -> str:
    h = hashlib.sha256()
    for name, text in source_set(scad, read):
        norm = " ".join(strip_comments(text).split())
        for piece in (name.encode(), b"\0", norm.encode(), b"\0"):
            h.update(piece)
    for k in sorted(defines):
        h.update(f"{k}={define_token(defines[k])}".encode())
        h.update(b"\0")
    return h.hexdigest()[:8]


def read_tree(name: str):
    p = HERE / name
    return p.read_text(encoding="utf-8") if p.is_file() else None


def version_of(text: str):
    for line in text.splitlines()[:3]:
        m = re.search(r"\((v\d+\.\d+(?:-\w+)?)\)", line)
        if m:
            return m.group(1)
    return None


# ---------------------------------------------------------------------------
# geometry — the fingerprint of an STL, and the mesh itself
# ---------------------------------------------------------------------------

def _read_ascii_stl(data: bytes) -> list[tuple]:
    verts = []
    for line in data.decode("ascii", errors="replace").splitlines():
        t = line.split()
        if len(t) == 4 and t[0] == "vertex":
            verts.append((float(t[1]), float(t[2]), float(t[3])))
    if not verts or len(verts) % 3:
        raise ValueError("ASCII STL with no whole facets")
    return [tuple(verts[i:i + 3]) for i in range(0, len(verts), 3)]


def read_stl(data: bytes) -> list[tuple]:
    """Triangles of an STL as ((ax,ay,az),(bx,..),(cx,..)) tuples. Binary is
    what render.sh writes; ASCII is read too, because a few early commits of
    the catalog's history carry it and --backfill reads them."""
    if len(data) < 84:
        raise ValueError("not an STL (too short)")
    (n,) = struct.unpack_from("<I", data, 80)
    if 84 + 50 * n != len(data):
        if data[:5] == b"solid" and b"facet" in data[:600]:
            return _read_ascii_stl(data)
        raise ValueError(f"binary STL length {len(data)} does not fit {n} facets")
    tris = []
    off = 84
    for _ in range(n):
        v = struct.unpack_from("<12f", data, off)
        tris.append(((v[3], v[4], v[5]), (v[6], v[7], v[8]), (v[9], v[10], v[11])))
        off += 50
    return tris


def fingerprint(tris: list[tuple]) -> dict:
    """What the mesh IS, as numbers a re-render of the same design reproduces
    (measured: two renders of one part agree to ~1e-11 in every moment) and
    a moved feature does not: triangle count, volume, surface area, bounding
    box and its low corner, centroid, the central second moments (diagonal
    and the products of inertia xy/xz/yz) and the central third moments.
    The products are what tell a pair of holes on one diagonal from the same
    pair on the other (identical volume, area, box, centroid and diagonal
    moments); the third moments tell a feature from its mirror image."""
    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    vol6 = 0.0
    area = 0.0
    c1 = [0.0, 0.0, 0.0]
    c2 = [[0.0] * 3 for _ in range(3)]
    c3 = [0.0, 0.0, 0.0]
    for a, b, c in tris:
        for pt in (a, b, c):
            for k in range(3):
                if pt[k] < lo[k]:
                    lo[k] = pt[k]
                if pt[k] > hi[k]:
                    hi[k] = pt[k]
        # signed tetra volume with the origin, x6
        v6 = (a[0] * (b[1] * c[2] - b[2] * c[1]) + a[1] * (b[2] * c[0] - b[0] * c[2])
              + a[2] * (b[0] * c[1] - b[1] * c[0]))
        vol6 += v6
        s = [a[k] + b[k] + c[k] for k in range(3)]
        for i in range(3):
            c1[i] += v6 * s[i]
            for j in range(i, 3):       # integral of x_i x_j over the tetra, x 120/V-free
                c2[i][j] += v6 * (a[i] * a[j] + b[i] * b[j] + c[i] * c[j] + s[i] * s[j])
            ai, bi, ci = a[i], b[i], c[i]
            c3[i] += v6 * (ai ** 3 + bi ** 3 + ci ** 3 + ai * ai * (bi + ci) + bi * bi * (ai + ci)
                           + ci * ci * (ai + bi) + ai * bi * ci)
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        area += math.sqrt(nx * nx + ny * ny + nz * nz) / 2
    vol = vol6 / 6
    if abs(vol) < 1e-9:
        cen = [0.0, 0.0, 0.0]
        e2 = [[0.0] * 3 for _ in range(3)]
        e3 = [0.0, 0.0, 0.0]
    else:
        cen = [c1[k] / (4 * vol6) for k in range(3)]
        e2 = [[c2[i][j] / (20 * vol6) if j >= i else 0.0 for j in range(3)] for i in range(3)]
        e3 = [c3[i] / (20 * vol6) for i in range(3)]
    moments = [e2[k][k] - cen[k] ** 2 for k in range(3)]
    products = [e2[0][1] - cen[0] * cen[1], e2[0][2] - cen[0] * cen[2], e2[1][2] - cen[1] * cen[2]]
    third = [e3[k] - 3 * cen[k] * e2[k][k] + 2 * cen[k] ** 3 for k in range(3)]
    return {
        "triangles": len(tris),
        "volume_mm3": round(abs(vol), 3),
        "area_mm2": round(area, 3),
        "bbox_mm": [round(hi[k] - lo[k], 3) for k in range(3)],
        "lo_mm": [round(lo[k], 3) for k in range(3)],
        "centroid_mm": [round(v, 4) for v in cen],
        "moments_mm2": [round(abs(v), 4) for v in moments],
        "products_mm2": [round(v, 4) for v in products],
        "third_mm3": [round(v, 3) for v in third],
    }


def geom_equal(a: dict, b: dict) -> str | None:
    """None when two fingerprints are the same shape within TOL, else why
    not. A key one side lacks (a fingerprint recorded before the key
    existed) is not compared; write() fills it in on the next run."""
    if a["triangles"] != b["triangles"]:
        return f"triangles {a['triangles']} -> {b['triangles']}"
    for k in ("bbox_mm", "lo_mm"):
        for i in range(3):
            if abs(a[k][i] - b[k][i]) > TOL["bbox_mm"]:
                return f"{k}[{'xyz'[i]}] {a[k][i]} -> {b[k][i]}"
    for k in ("volume_mm3", "area_mm2"):
        if abs(a[k] - b[k]) > TOL["abs_mm3"] + TOL["rel"] * abs(a[k]):
            return f"{k} {a[k]} -> {b[k]}"
    for i in range(3):
        if abs(a["centroid_mm"][i] - b["centroid_mm"][i]) > TOL["centroid_mm"]:
            return f"centroid[{'xyz'[i]}] {a['centroid_mm'][i]} -> {b['centroid_mm'][i]}"
        if abs(a["moments_mm2"][i] - b["moments_mm2"][i]) > TOL["moment_abs"] + TOL["rel"] * abs(a["moments_mm2"][i]):
            return f"moments[{'xyz'[i]}] {a['moments_mm2'][i]} -> {b['moments_mm2'][i]}"
    if "products_mm2" in a and "products_mm2" in b:
        for i, ax in enumerate(("xy", "xz", "yz")):
            p, q = ax
            scale = math.sqrt(a["moments_mm2"]["xyz".index(p)] * a["moments_mm2"]["xyz".index(q)])
            if abs(a["products_mm2"][i] - b["products_mm2"][i]) > TOL["moment_abs"] + TOL["rel"] * scale:
                return f"products[{ax}] {a['products_mm2'][i]} -> {b['products_mm2'][i]}"
    if "third_mm3" in a and "third_mm3" in b:
        for i in range(3):
            scale = a["moments_mm2"][i] ** 1.5
            if abs(a["third_mm3"][i] - b["third_mm3"][i]) > TOL["third_abs"] + TOL["rel"] * scale:
                return f"third[{'xyz'[i]}] {a['third_mm3'][i]} -> {b['third_mm3'][i]}"
    return None


# ---------------------------------------------------------------------------
# regions — where the material changed, as boxes on the part
# ---------------------------------------------------------------------------
#
# A solid-volume diff: both meshes are voxelized as MATERIAL (inside / outside,
# by ray parity along z through each column of a VOXEL_MM grid), and a voxel
# whose center is solid in one and empty in the other is changed. Connected
# changed voxels become boxes: `added` (material the new rev has), `removed`
# (material it lost), and `moved` where an added and a removed box sit within
# MOVED_GAP voxels of each other. Material, not surface: a ring of 1.6 mm holes
# drilled through a 2 mm face is a removed region here, where a surface-distance
# diff saw nothing (the hole walls lie within a cell of the old face).
#
# The grid is offset by an irrational fraction of a voxel so the round-number
# planes a CAD part is made of (z = 1.2, x = 20.0) never pass through a voxel
# center or a column: a render's 1e-4 mm noise cannot flip a voxel, and a ray
# never grazes an axis-aligned edge.

GRID_OFFSET = 0.1373              # of a voxel; anything not a short fraction
MIN_REGION_VOXELS = 3             # a smaller blob is a sampling artifact


def _column_hits(tris: list[tuple], res: float, ox: float, oy: float) -> dict:
    """{(i, j): [(z, entering), …]} — every crossing of a +z ray through the
    center of column (i, j) with the mesh. entering = the face looks down."""
    cols: dict = {}
    for a, b, c in tris:
        ux, uy = b[0] - a[0], b[1] - a[1]
        vx, vy = c[0] - a[0], c[1] - a[1]
        det = ux * vy - uy * vx           # 2x the projected (signed) area
        if abs(det) < 1e-12:
            continue                      # a vertical face: no ray crosses it
        entering = det < 0                # projected winding: a down-facing face (normal z < 0)
        x0, x1 = min(a[0], b[0], c[0]), max(a[0], b[0], c[0])
        y0, y1 = min(a[1], b[1], c[1]), max(a[1], b[1], c[1])
        i0, i1 = math.ceil((x0 - ox) / res - 0.5), math.floor((x1 - ox) / res - 0.5)
        j0, j1 = math.ceil((y0 - oy) / res - 0.5), math.floor((y1 - oy) / res - 0.5)
        for i in range(i0, i1 + 1):
            px = ox + (i + 0.5) * res
            for j in range(j0, j1 + 1):
                py = oy + (j + 0.5) * res
                wx, wy = px - a[0], py - a[1]
                s1 = (wx * vy - wy * vx) / det     # barycentric coordinates of the column center
                s2 = (ux * wy - uy * wx) / det
                if s1 < 0 or s2 < 0 or s1 + s2 > 1:
                    continue
                z = a[2] + s1 * (b[2] - a[2]) + s2 * (c[2] - a[2])
                cols.setdefault((i, j), []).append((z, entering))
    return cols


def _intervals(hits: list) -> list[tuple[float, float]] | None:
    """Solid intervals along one column, or None when the crossings do not
    pair (an open mesh here): that column is left out of the diff, never guessed."""
    hits = sorted(hits)
    if len(hits) % 2:
        return None
    out = []
    depth, start = 0, 0.0
    for z, entering in hits:
        if entering:
            if depth == 0:
                start = z
            depth += 1
        else:
            depth -= 1
            if depth == 0:
                out.append((start, z))
            if depth < 0:
                return [(hits[k][0], hits[k + 1][0]) for k in range(0, len(hits), 2)]   # parity fallback
    return out if depth == 0 else [(hits[k][0], hits[k + 1][0]) for k in range(0, len(hits), 2)]


def _solid_voxels(iv: list[tuple[float, float]], res: float, oz: float) -> set[int]:
    ks = set()
    for z0, z1 in iv:
        for k in range(math.ceil((z0 - oz) / res - 0.5), math.floor((z1 - oz) / res - 0.5) + 1):
            ks.add(k)
    return ks


_NEIGHBORS = [(dx, dy, dz) for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1)
              if (dx, dy, dz) != (0, 0, 0)]


def components(cells: set) -> list[set]:
    left = set(cells)
    comps = []
    while left:
        seed = left.pop()
        comp = {seed}
        q = deque([seed])
        while q:
            x, y, z = q.popleft()
            for dx, dy, dz in _NEIGHBORS:
                nb = (x + dx, y + dy, z + dz)
                if nb in left:
                    left.remove(nb)
                    comp.add(nb)
                    q.append(nb)
        comps.append(comp)
    return comps


def _box_of(comp: set, res: float, origin: tuple) -> list[float]:
    xs = [c[0] for c in comp]
    ys = [c[1] for c in comp]
    zs = [c[2] for c in comp]
    return [round(origin[0] + min(xs) * res, 2), round(origin[1] + min(ys) * res, 2),
            round(origin[2] + min(zs) * res, 2),
            round(origin[0] + (max(xs) + 1) * res, 2), round(origin[1] + (max(ys) + 1) * res, 2),
            round(origin[2] + (max(zs) + 1) * res, 2)]


def _overlap(a: list, b: list) -> bool:
    return all(a[i] <= b[i + 3] and b[i] <= a[i + 3] for i in range(3))


def _union(a: list, b: list) -> list:
    return [min(a[i], b[i]) for i in range(3)] + [max(a[i + 3], b[i + 3]) for i in range(3)]


def _vol(b: list) -> float:
    return max(0.0, b[3] - b[0]) * max(0.0, b[4] - b[1]) * max(0.0, b[5] - b[2])


def regions_between(old: list[tuple], new: list[tuple], cell: float = VOXEL_MM) -> list[dict]:
    """Boxes (part coordinates, mm) where `new` has material `old` did not
    (added), the reverse (removed), and the two meeting (moved). Deterministic
    for the same two meshes; the same mesh twice gives none."""
    lo = [math.inf] * 3
    for tri in old + new:
        for p in tri:
            for k in range(3):
                lo[k] = min(lo[k], p[k])
    origin = tuple(v - cell * (1 + GRID_OFFSET) for v in lo)
    ho = _column_hits(old, cell, origin[0], origin[1])
    hn = _column_hits(new, cell, origin[0], origin[1])
    added: set = set()
    removed: set = set()
    for col in set(ho) | set(hn):
        io = _intervals(ho.get(col, []))
        inn = _intervals(hn.get(col, []))
        if io is None or inn is None:
            continue
        so = _solid_voxels(io, cell, origin[2])
        sn = _solid_voxels(inn, cell, origin[2])
        for k in sn - so:
            added.add((col[0], col[1], k))
        for k in so - sn:
            removed.add((col[0], col[1], k))
    regs = []
    for kind, cells in (("added", added), ("removed", removed)):
        for comp in components(cells):
            if len(comp) < MIN_REGION_VOXELS:
                continue
            regs.append({"kind": kind, "box": _box_of(comp, cell, origin), "cells": len(comp)})

    # merge boxes that meet: the same kind stays what it is; an added box
    # within MOVED_GAP voxels of a removed one is a feature that MOVED
    def near(a: dict, b: dict) -> bool:
        gap = 0.0 if a["kind"] == b["kind"] else MOVED_GAP * cell
        ga = [a["box"][k] - gap for k in range(3)] + [a["box"][k + 3] + gap for k in range(3)]
        return _overlap(ga, b["box"])

    merged = True
    while merged and len(regs) > 1:
        merged = False
        for i in range(len(regs)):
            for j in range(i + 1, len(regs)):
                if near(regs[i], regs[j]):
                    ki, kj = regs[i]["kind"], regs[j]["kind"]
                    regs[i] = {"kind": ki if ki == kj else "moved",
                               "box": _union(regs[i]["box"], regs[j]["box"]),
                               "cells": regs[i]["cells"] + regs[j]["cells"]}
                    del regs[j]
                    merged = True
                    break
            if merged:
                break
    while len(regs) > MAX_REGIONS:                 # cap by merging the pair whose union grows least
        best = None
        for i in range(len(regs)):
            for j in range(i + 1, len(regs)):
                grow = _vol(_union(regs[i]["box"], regs[j]["box"])) - _vol(regs[i]["box"]) - _vol(regs[j]["box"])
                if best is None or grow < best[0]:
                    best = (grow, i, j)
        _, i, j = best
        ki, kj = regs[i]["kind"], regs[j]["kind"]
        regs[i] = {"kind": ki if ki == kj else "moved", "box": _union(regs[i]["box"], regs[j]["box"]),
                   "cells": regs[i]["cells"] + regs[j]["cells"]}
        del regs[j]
    regs.sort(key=lambda r: (-r["cells"], r["box"]))
    return regs


# ---------------------------------------------------------------------------
# git — where a previous rev's bytes and sources live
# ---------------------------------------------------------------------------

def git(*args: str, binary: bool = False, check: bool = True):
    r = subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=not binary)
    if check and r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {(r.stderr if not binary else r.stderr.decode())}")
    return r.stdout if r.returncode == 0 else None


def git_head() -> str:
    return (git("rev-parse", "HEAD") or "").strip()


def git_show_bytes(sha: str, rel: str):
    return git("show", f"{sha}:{rel}", binary=True, check=False)


def git_show_text(sha: str, rel: str):
    data = git_show_bytes(sha, rel)
    return data.decode("utf-8") if data is not None else None


def commits_touching(rels: list[str], n: int = MAX_HISTORY_WALK) -> list[str]:
    out = git("log", f"-n{n}", "--format=%H", "--", *rels, check=False) or ""
    return out.split()


def find_previous_stl(recipe: dict, want: dict) -> tuple[bytes | None, str]:
    """The committed STL bytes carrying the previous rev's fingerprint, and
    where they came from ("stl@<sha7>") — else (None, why)."""
    rel = stl_rel(recipe)
    tried = 0
    for sha in commits_touching([rel]):
        data = git_show_bytes(sha, rel)
        if data is None:
            continue
        tried += 1
        try:
            fp = fingerprint(read_stl(data))
        except ValueError:
            continue
        if geom_equal(want, fp) is None:
            return data, f"stl@{sha[:7]}"
        if tried >= 40:
            break
    return None, ("no committed revision of this STL carries the previous fingerprint "
                  "(history too shallow, or the mesh was never committed as recorded)")


def find_previous_sources(recipe: dict, want_src: str) -> tuple[str | None, str]:
    """The commit whose sources digest to the previous rev's `src`, else why."""
    scad = recipe["scad"]
    rels = [f"{ENC_REL}/{n}" for n, _ in source_set(scad, read_tree)]
    for sha in commits_touching(rels):
        def read_at(name: str, sha=sha):
            return git_show_text(sha, f"{ENC_REL}/{name}")
        try:
            d = src_digest(scad, recipe["defines"], read_at)
        except FileNotFoundError:
            continue
        if d == want_src:
            return sha, f"sources@{sha[:7]}"
    return None, ("no commit in history carries the previous rev's sources "
                  "(a shallow clone, or the sources were never committed as recorded)")


def render_at_commit(sha: str, recipe: dict) -> list[tuple]:
    """Render a recipe from a temporary worktree at `sha` (the libraries it
    uses come from the same commit)."""
    with tempfile.TemporaryDirectory() as td:
        wt = Path(td) / "wt"
        git("worktree", "add", "--detach", str(wt), sha)
        try:
            src = wt / ENC_REL / recipe["scad"]
            defines = {k: define_token(v) for k, v in recipe["defines"].items()}
            keep = Path(td) / "prev.stl"
            scad_probe.render(src, defines, keep=keep, label=f"{recipe['key']} @ {sha[:7]}")
            return read_stl(keep.read_bytes())
        finally:
            git("worktree", "remove", "--force", str(wt), check=False)


def _knobs_of_text(text: str) -> dict:
    with tempfile.NamedTemporaryFile("w", suffix=".scad", delete=False, encoding="utf-8") as tf:
        tf.write(text)
        tmp = Path(tf.name)
    try:
        return {p["name"]: p["default"] for g in parse_scad(tmp) for p in g["params"]}
    finally:
        tmp.unlink(missing_ok=True)


def knob_diff(scad: str, prev_sha: str, new_sha: str | None = None) -> list[dict]:
    """Customizer knobs whose literal default differs between the case file at
    `prev_sha` and at `new_sha` (the working tree when None)."""
    old_text = git_show_text(prev_sha, f"{ENC_REL}/{scad}")
    if old_text is None:
        return []
    old = _knobs_of_text(old_text)
    if new_sha is None:
        new = {p["name"]: p["default"] for g in parse_scad(HERE / scad) for p in g["params"]}
    else:
        new_text = git_show_text(new_sha, f"{ENC_REL}/{scad}")
        new = _knobs_of_text(new_text) if new_text is not None else {}
    out = []
    for name in sorted(set(old) | set(new)):
        if name not in old:
            out.append({"knob": name, "to": new[name]})
        elif name not in new:
            out.append({"knob": name, "from": old[name]})
        elif old[name] != new[name]:
            out.append({"knob": name, "from": old[name], "to": new[name]})
    return out[:40]


def commits_since(scad: str, prev_sha: str, new_sha: str = "HEAD", extra: tuple[str, ...] = ()) -> list[str]:
    """`<sha7> <subject>` of the commits between two revs that touched the
    part's sources (the tree's dependency list) or `extra` paths (its
    committed STL, for a backfilled rev), newest first, at most MAX_COMMITS."""
    rels = [f"{ENC_REL}/{n}" for n, _ in source_set(scad, read_tree)] + list(extra)
    out = git("log", f"-n{MAX_COMMITS}", "--format=%h %s", f"{prev_sha}..{new_sha}", "--", *rels,
              check=False) or ""
    return [line for line in out.splitlines() if line.strip()]


# ---------------------------------------------------------------------------
# the ledger
# ---------------------------------------------------------------------------

def load_ledger() -> dict:
    if OUT.exists():
        return json.loads(OUT.read_text(encoding="utf-8"))
    return {"parts": {}}


def today() -> str:
    return dt.date.today().isoformat()


def history_entry(n: int, recorded: str, src, geom: dict, version, changes: dict | None, *,
                  recorded_on: str | None, introduced_in: str | None) -> dict:
    """One rev. `introduced_in` is the commit whose tree first carries this
    mesh — known for a rev --backfill read out of git, null for a rev recorded
    from the working tree (its mesh lands in the commit that records it, which
    does not exist yet). `recorded_on` is the HEAD it was recorded on top of."""
    return {"rev": f"r{n}", "recorded": recorded, "version": version, "src": src, "geom": geom,
            "recorded_on": recorded_on, "introduced_in": introduced_in, "changes": changes}


def part_from_history(recipe: dict, hist: list[dict], src: str, version) -> dict:
    last = hist[-1]
    return {
        "scad": recipe["scad"],
        "defines": recipe["defines"],
        "home": recipe["home"],
        "file": stl_rel(recipe),
        "version": version,
        "rev": last["rev"],
        "recorded": last["recorded"],
        "src": src,
        "geom": last["geom"],
        "history": hist,
    }


def part_record(recipe: dict, prev: dict | None, src: str, geom: dict, version, head: str,
                changes: dict | None) -> dict:
    hist = list(prev["history"]) if prev else []
    hist.append(history_entry(len(hist) + 1, today(), src, geom, version, changes,
                              recorded_on=head or None, introduced_in=None))
    return part_from_history(recipe, hist, src, version)


def delta(a: float, b: float, nd: int = 3) -> float:
    return round(b - a, nd)


def change_numbers(prev_geom: dict, new_geom: dict) -> dict:
    return {
        "volume_mm3": delta(prev_geom["volume_mm3"], new_geom["volume_mm3"]),
        "area_mm2": delta(prev_geom["area_mm2"], new_geom["area_mm2"]),
        "bbox_mm": [delta(prev_geom["bbox_mm"][i], new_geom["bbox_mm"][i]) for i in range(3)],
        "triangles": new_geom["triangles"] - prev_geom["triangles"],
    }


REGIONS_NOTE = (f"below the {VOXEL_MM} mm voxel: no voxel center changed between solid and empty "
                "— see the volume and area deltas")


def previous_mesh(recipe: dict, prev_part: dict, want_regions: bool,
                  prev_sha: str | None, src_where: str) -> tuple[list | None, str]:
    """The previous rev's triangles and where they came from, or (None, why)."""
    last = prev_part["history"][-1]
    if recipe["home"] in ("released", "preview"):
        sha = last.get("introduced_in")
        if sha:
            data = git_show_bytes(sha, stl_rel(recipe))
            if data is not None:
                tris = read_stl(data)
                if geom_equal(last["geom"], fingerprint(tris)) is None:
                    return tris, f"stl@{sha[:7]}"
        data, where = find_previous_stl(recipe, last["geom"])
        return (read_stl(data), where) if data is not None else (None, f"unavailable: {where}")
    if prev_sha is None:
        return None, f"unavailable: {src_where}"
    if not want_regions:
        return None, f"{src_where} (regions skipped: --no-regions)"
    try:
        return render_at_commit(prev_sha, recipe), f"rendered {src_where}"
    except scad_probe.ProbeError as e:
        return None, f"unavailable: the previous sources did not render cleanly ({str(e).splitlines()[0]})"


def changes_for(recipe: dict, prev_part: dict, new_tris: list[tuple], note: str | None,
                want_regions: bool) -> dict:
    """What moved between the part's recorded rev and the mesh on disk."""
    ch: dict = {**change_numbers(prev_part["geom"], fingerprint(new_tris)),
                "knobs": [], "commits": [], "previous": None, "regions": None, "note": note}
    prev_sha, src_where = find_previous_sources(recipe, prev_part["history"][-1]["src"] or "")
    if prev_sha:
        ch["knobs"] = knob_diff(recipe["scad"], prev_sha)
        ch["commits"] = commits_since(recipe["scad"], prev_sha)
    old_tris, ch["previous"] = previous_mesh(recipe, prev_part, want_regions, prev_sha, src_where)
    if old_tris is not None:
        ch["regions"] = regions_between(old_tris, new_tris)
        if not ch["regions"]:
            ch["regions_note"] = REGIONS_NOTE
    return ch


def backfill_history(recipe: dict) -> list[dict]:
    """A committed part's revisions read out of git: every commit of its STL
    whose mesh is a different SHAPE from the one before it is a rev (a
    re-render of the same design is not), dated by that commit, with what
    moved worked out against the previous rev's committed bytes. Its `src` is
    the part's sources at that commit (null when a library it uses today did
    not exist yet)."""
    rel = stl_rel(recipe)
    out = git("log", "--reverse", "--format=%H %cs", "--", rel, check=False) or ""
    hist: list[dict] = []
    prev = None                        # (sha, tris, fingerprint)
    for line in out.splitlines():
        sha, date = line.split()
        data = git_show_bytes(sha, rel)
        if data is None:
            continue                   # the commit deleted it
        try:
            tris = read_stl(data)
        except ValueError:
            continue
        fp = fingerprint(tris)
        if prev and geom_equal(prev[2], fp) is None:
            continue

        def read_at(name: str, sha: str = sha):
            return git_show_text(sha, f"{ENC_REL}/{name}")

        try:
            src = src_digest(recipe["scad"], recipe["defines"], read_at)
        except FileNotFoundError:
            src = None
        scad_text = read_at(recipe["scad"])
        changes = None
        if prev:
            changes = {**change_numbers(prev[2], fp),
                       "knobs": knob_diff(recipe["scad"], prev[0], sha),
                       "commits": commits_since(recipe["scad"], prev[0], sha, extra=(rel,)),
                       "previous": f"stl@{prev[0][:7]}", "note": None,
                       "why": geom_equal(prev[2], fp),
                       "regions": regions_between(prev[1], tris)}
            if not changes["regions"]:
                changes["regions_note"] = REGIONS_NOTE
        hist.append(history_entry(len(hist) + 1, date, src, fp, version_of(scad_text or ""), changes,
                                  recorded_on=sha, introduced_in=sha))
        prev = (sha, tris, fp)
    return hist


# ---------------------------------------------------------------------------
# check / write / list
# ---------------------------------------------------------------------------

def tree_state(recipes: list[dict]) -> dict:
    """key -> (recipe, src, geom-or-None, version) from the working tree."""
    st = {}
    for r in recipes:
        src = src_digest(r["scad"], r["defines"], read_tree)
        p = stl_path(r)
        geom = None
        if p.is_file():
            geom = fingerprint(read_stl(p.read_bytes()))
        st[r["key"]] = (r, src, geom, version_of(read_tree(r["scad"]) or ""))
    return st


def check(rendered: bool) -> list[str]:
    bad = []
    if not OUT.exists():
        return [f"{OUT_REL}: missing — run {ENC_REL}/gen_revisions.py"]
    led = load_ledger()
    parts = led.get("parts", {})
    recipes = all_recipes()
    st = tree_state(recipes)
    for key, (r, src, geom, version) in st.items():
        e = parts.get(key)
        if not e:
            bad.append(f"{key}: no revision recorded — run {ENC_REL}/gen_revisions.py")
            continue
        if e["scad"] != r["scad"] or e["defines"] != r["defines"]:
            bad.append(f"{key}: its recipe moved ({e['scad']} {e['defines']} -> {r['scad']} {r['defines']}) "
                       f"— run {ENC_REL}/gen_revisions.py")
        if e["src"] != src:
            bad.append(f"{key}: the sources that draw it moved since {e['rev']} was recorded "
                       f"(src {e['src']} -> {src}) — run {ENC_REL}/gen_revisions.py --render-stale")
        if geom is None:
            if r["home"] != "dev":
                bad.append(f"{key}: {stl_rel(r)} is missing")
            elif rendered:
                bad.append(f"{key}: {stl_rel(r)} was not rendered — render.sh renders it (--rendered expects it)")
            continue
        why = geom_equal(e["geom"], geom)
        if why:
            bad.append(f"{key}: the mesh moved since {e['rev']} was recorded ({why}) "
                       f"— run {ENC_REL}/gen_revisions.py to record the next rev")
        if e.get("version") != version:
            bad.append(f"{key}: {r['scad']} is {version}, the ledger says {e.get('version')} "
                       f"— run {ENC_REL}/gen_revisions.py")
        hist = e.get("history") or []
        if not hist or hist[-1]["rev"] != e["rev"] or [h["rev"] for h in hist] != [f"r{i+1}" for i in range(len(hist))]:
            bad.append(f"{key}: history is not r1..{e['rev']} in order — regenerate, never hand-edit")
    for key in parts:
        if key not in st:
            bad.append(f"{key}: in the ledger but nothing renders it — run {ENC_REL}/gen_revisions.py")
    return bad


def sources_mtime(scad: str) -> float:
    return max((HERE / name).stat().st_mtime for name, _ in source_set(scad, read_tree))


def render_one(r: dict, out: Path) -> None:
    defines = {k: define_token(v) for k, v in r["defines"].items()}
    try:
        scad_probe.render(HERE / r["scad"], defines, keep=out, label=r["key"])
    except scad_probe.ProbeError as e:
        sys.exit(f"gen_revisions: {e}")


def is_stale(r: dict, old_parts: dict) -> bool:
    """A part whose mesh on disk cannot be the design its sources draw now:
    no mesh at all, or one rendered before those sources last changed while
    they differ from what its recorded rev was drawn from."""
    p = stl_path(r)
    if not p.is_file():
        return True
    prev = old_parts.get(r["key"])
    if prev is not None and prev["src"] == src_digest(r["scad"], r["defines"], read_tree):
        return False
    return p.stat().st_mtime < sources_mtime(r["scad"])


def render_stale(recipes: list[dict], old_parts: dict) -> list[str]:
    """--render-stale: render exactly the stale parts (is_stale). A dev
    render goes into place; a committed STL is replaced only when its SHAPE
    moved — a re-render of the same design leaves the committed bytes alone
    (and marks them current), so git shows real changes and not OpenSCAD's
    byte noise."""
    todo = [r for r in recipes if is_stale(r, old_parts)]
    placed = []
    with tempfile.TemporaryDirectory() as td:
        for i, r in enumerate(todo, 1):
            tmp = Path(td) / "part.stl"
            print(f"render [{i}/{len(todo)}]: {r['key']}")
            render_one(r, tmp)
            dest = stl_path(r)
            dest.parent.mkdir(parents=True, exist_ok=True)
            if r["home"] == "dev" or not dest.is_file():
                shutil.move(str(tmp), dest)
                placed.append(r["key"])
                continue
            fresh = fingerprint(read_stl(tmp.read_bytes()))
            if geom_equal(fingerprint(read_stl(dest.read_bytes())), fresh):
                shutil.move(str(tmp), dest)
                placed.append(r["key"])
            else:
                os.utime(dest)         # the same shape: the committed bytes ARE current
    print(f"gen_revisions: rendered {len(todo)} stale part(s); {len(placed)} written into place")
    return placed


def write(note: str | None, want_regions: bool, backfill: bool = False, rerender: bool = False) -> int:
    led = load_ledger()
    old_parts = dict(led.get("parts", {}))
    recipes = all_recipes()
    st = tree_state(recipes)
    head = git_head()
    if backfill:
        n_revs = 0
        for key, (r, _src, _geom, _version) in st.items():
            if r["home"] not in ("released", "preview"):
                continue
            hist = backfill_history(r)
            if hist:
                old_parts[key] = part_from_history(r, hist, hist[-1]["src"], hist[-1]["version"])
                n_revs += len(hist)
                print(f"backfill: {key}: {len(hist)} rev(s) from git")
        print(f"gen_revisions: backfilled {n_revs} revision(s) from git history")
    if rerender:
        render_stale(recipes, old_parts)
        st = tree_state(recipes)
    # A mesh rendered BEFORE its sources last changed cannot be the design the
    # sources draw now; recording it would stamp the old shape with the new
    # src. Refused, by name, before anything is written (--render-stale, or
    # render.sh, renders them).
    stale = [key for key, (r, _src, geom, _v) in st.items() if geom is not None and is_stale(r, old_parts)]
    if stale:
        sys.exit(f"gen_revisions: {len(stale)} mesh(es) are older than the sources that draw them, so they "
                 f"are not the design those sources describe: {', '.join(stale[:8])}"
                 f"{' …' if len(stale) > 8 else ''}\n  render them first: ./render.sh --no-png, or "
                 f"gen_revisions.py --render-stale")
    new_parts = {}
    recorded, bumped, kept = [], [], 0
    missing = []
    for key, (r, src, geom, version) in st.items():
        if geom is None:
            missing.append(key)
            continue
        prev = old_parts.get(key)
        if prev is None:
            new_parts[key] = part_record(r, None, src, geom, version, head, None)
            recorded.append(key)
            continue
        why = geom_equal(prev["geom"], geom)
        if why is None:
            # same shape: carry the history, refresh what can move without a rev
            p = dict(prev)
            p.update({"scad": r["scad"], "defines": r["defines"], "home": r["home"], "file": stl_rel(r),
                      "version": version, "src": src})
            if set(geom) - set(prev["geom"]):
                # a fingerprint recorded before a key existed: fill it in from
                # this same shape (never rewritten otherwise — no float churn)
                p["geom"] = {**geom, **prev["geom"]}
                hist = [dict(h) for h in prev["history"]]
                hist[-1]["geom"] = {**geom, **hist[-1]["geom"]}
                p["history"] = hist
            new_parts[key] = p
            kept += 1
            continue
        tris = read_stl(stl_path(r).read_bytes())
        ch = changes_for(r, prev, tris, note, want_regions)
        ch["why"] = why
        new_parts[key] = part_record(r, prev, src, geom, version, head, ch)
        bumped.append(key)
    if missing:
        devs = [k for k in missing if st[k][0]["home"] == "dev"]
        others = [k for k in missing if st[k][0]["home"] != "dev"]
        if others:
            sys.exit("gen_revisions: committed meshes missing from the tree: " + ", ".join(others))
        unrecorded = [k for k in devs if k not in old_parts]
        if unrecorded:
            sys.exit(f"gen_revisions: {len(unrecorded)} dev part(s) have no render and no recorded rev: "
                     f"{', '.join(unrecorded[:8])} — run render.sh or --render-stale")
        print(f"gen_revisions: {len(devs)} dev part(s) not rendered, left as recorded: "
              + ", ".join(devs[:6]) + (" …" if len(devs) > 6 else ""))
        for k in devs:
            new_parts[k] = old_parts[k]
    dropped = sorted(k for k in old_parts if k not in new_parts and k not in st)
    led = {
        "generated_by": f"{ENC_REL}/gen_revisions.py",
        "note": ("GENERATED — do not edit. One revision per printable part: rev bumps when the part's "
                 "printed shape moved (fingerprint within `tolerance`), src is the digest of the sources "
                 "that draw it, history[].changes says what moved — knobs, deltas, commits, and `regions`, "
                 "boxes in the STL's own coordinates where the new surface is not where the old one was, "
                 "for a viewer to tint. The recipes are render.sh's and gen_enclosures.py's, recorded "
                 "from the scripts themselves; a committed part's earlier revs are read out of git "
                 "(introduced_in names the commit). Provenance, not authenticity: a rev proves which "
                 "design a mesh is, never who printed it."),
        "spec": f"{ENC_REL}/README.md#part-revisions-what-changed-and-where",
        "tolerance": TOL,
        "voxel_mm": VOXEL_MM,
        "parts": {k: new_parts[k] for k in sorted(new_parts)},
    }
    OUT.write_text(json.dumps(led, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {OUT_REL}: {len(new_parts)} parts — {len(recorded)} first recorded, "
          f"{len(bumped)} bumped, {kept} unchanged" + (f", {len(dropped)} dropped" if dropped else ""))
    for k in bumped:
        e = new_parts[k]
        ch = e["history"][-1]["changes"]
        regs = ch.get("regions")
        print(f"  {k}: {e['rev']} ({ch['why']}; ΔV {ch['volume_mm3']:+} mm³, "
              f"{len(ch['knobs'])} knob(s), regions: {'n/a — ' + ch['previous'] if regs is None else len(regs)})")
    return 0


def render_recipes(target: str, recipes: list[dict]) -> None:
    """--render: one case's recipes (by .scad or by key), through scad_probe."""
    todo = [r for r in recipes if r["scad"] == target or r["key"] == target]
    if not todo:
        sys.exit(f"gen_revisions: --render {target}: no recipe renders that (see --list)")
    for r in todo:
        out = stl_path(r)
        out.parent.mkdir(parents=True, exist_ok=True)
        print(f"render: {r['key']}")
        render_one(r, out)


def list_parts() -> None:
    led = load_ledger().get("parts", {})
    for r in all_recipes():
        e = led.get(r["key"])
        d = " ".join(f"{k}={define_token(v)}" for k, v in r["defines"].items())
        print(f"{r['key']:48} {r['home']:8} {(e or {}).get('rev', '—'):4} {(e or {}).get('recorded', ''):10} "
              f"{r['scad']} {d}")


def find_part(scad: str, defines: dict, ledger: dict | None = None) -> tuple[str, dict] | None:
    """A ledger entry by recipe — what gen_print_files.py asks for a part it
    renders itself. `defines` as -D tokens ('"drum"') or as values."""
    led = ledger or load_ledger()
    want = {k: (_parse_define(v) if isinstance(v, str) else v) for k, v in defines.items()}
    for key, e in led.get("parts", {}).items():
        if e["scad"] == scad and e["defines"] == want and e["home"] != "preview":
            return key, e
    return None


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="verify the ledger against the tree; write nothing")
    ap.add_argument("--rendered", action="store_true",
                    help="with --check: every dev render is expected on disk (after render.sh)")
    ap.add_argument("--note", metavar="TEXT", help="words for the revisions this run records")
    ap.add_argument("--no-regions", action="store_true",
                    help="record a dev part's new rev without rendering its previous sources (no regions)")
    ap.add_argument("--render", metavar="SCAD|KEY", help="render one case's recipes into the tree first")
    ap.add_argument("--render-stale", action="store_true",
                    help="render exactly the parts whose sources moved since their recorded rev, then record")
    ap.add_argument("--backfill", action="store_true",
                    help="(re)derive every committed part's revisions from git history, then record")
    ap.add_argument("--list", action="store_true", help="every part, its home, rev and recipe")
    args = ap.parse_args(argv)
    if args.list:
        list_parts()
        return 0
    if args.check:
        bad = check(args.rendered)
        for b in bad:
            print(f"::error::{b}")
        if bad:
            print(f"revisions: {len(bad)} problem(s) — see above")
            return 1
        n = len(load_ledger().get("parts", {}))
        print(f"revisions OK: {n} parts, every recorded rev is the tree's")
        return 0
    if args.render:
        render_recipes(args.render, all_recipes())
    return write(args.note, not args.no_regions, backfill=args.backfill, rerender=args.render_stale)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
