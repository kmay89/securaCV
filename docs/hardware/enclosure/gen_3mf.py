#!/usr/bin/env python3
"""Package the 7" case's per-filament parts into ONE Bambu-readable 3MF.

    python3 gen_3mf.py tests      # ALL the test parts, laid out on one plate
    python3 gen_3mf.py coupon     # just the colour + fit coupon
    python3 gen_3mf.py frame      # the whole case

Renders the parts it needs with OpenSCAD, then writes a single object whose
volumes are already registered to each other and already assigned to
filaments 1 / 2 / 3.

WHY THIS EXISTS
---------------
The three colour parts (body / ink / accent) only mean anything in the SAME
coordinate frame: the ink lettering sits in recesses cut into the body, and
moving one relative to the other by a millimetre puts the words beside their
own holes rather than in them.

Handing an operator three STLs and the instruction "load one, Add part → Load
the others, and do NOT re-centre them" does not survive contact with a slicer.
Loaded as separate OBJECTS — which is what File → Open does — Bambu Studio
auto-arranges them across the plate, correctly and fatally. That happened on
the first real attempt. An instruction that must be obeyed for the output to
be correct is a design defect, not a documentation problem; this script
removes the instruction.

TWO THINGS THAT BIT ON THE WAY, both worth keeping:

1. DO NOT DEDUPLICATE VERTICES ACROSS PARTS. The inlays sit exactly in their
   recesses and the bezel abuts the body, so the two surfaces meet at
   identical coordinates. Welding those shared vertices gives edges with FOUR
   incident triangles, and Bambu Studio rejects the file — "4227 non-manifold
   edges" — even though each part is watertight on its own. Each volume keeps
   its own vertices; the check at the bottom of build() enforces it.

2. BAMBU WANTS COMPONENTS, NOT TRIANGLE RANGES. An older PrusaSlicer
   convention puts every volume in one mesh and describes them as index
   ranges in the config. Bambu ignores that and shows a single part. Each
   volume must be its OWN <object>, assembled by a parent <object> holding
   <components>. Both config dialects are written, since PrusaSlicer and
   OrcaSlicer read the other one.

The "not from Bambu Lab, load geometry and color data only" dialog on open is
expected and harmless — it means the filament assignment was read.
"""
import struct
import subprocess
import sys
import zipfile
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "canary_s3_lcd7.scad"

# name -> (scad part, filament slot). Slot order IS the palette order, so a
# spool in the wrong slot swaps the lettering's colours — see PRINT COLOURS.
COUPON = [("body", "coupon_body", 1),
          ("ink", "coupon_ink", 2),
          ("accent", "coupon_accent", 3)]
FRAME = [("body", "fil_body", 1),
         ("ink", "fil_ink", 2),
         ("accent", "fil_accent", 3)]
# The QR plaque is deliberately TWO filaments, not three. The symbol is ink
# modules on a body-colour field, and the accent must never land on a finder
# pattern — a three-slot version of this would only offer a way to break it.
QR_COUPON = [("body", "coupon_qr_body", 1),
             ("ink", "coupon_qr_ink", 2)]

# A "set" is a list of OBJECTS. Each object is (name, volumes, plate centre).
# Volumes within one object are parts of it and stay registered to each other;
# separate objects are independent and get their own place on the plate.
#
# The "tests" plate is the whole pre-flight in one job, cheapest check first:
# the ring proves the outline, the coupon proves colour and corner fit, the
# QR plaque proves the symbol actually scans, and the corner gauge proves the
# screw pattern against the real panel. Four questions, one job, and every one
# of them is cheaper to answer here than on a committed frame.
SETS = {
    # SPLIT BY FILAMENT COUNT, not by theme — and this is the single biggest
    # time saving in the file, worth more than any arrangement of parts.
    #
    # A tool change anywhere on a plate builds a purge tower, and the tower is
    # built up to the height of the LAST change. The colour coupon's bezel band
    # is at print z 22.9 … 23.5 — the very top — so one plate holding it makes
    # the slicer raise ~23 mm of tower to service a 0.6 mm band, and every
    # single-filament part sharing that plate waits through it. On the first
    # combined plate the two dimension gauges, which need no tool change at
    # all, were hostage to exactly that.
    #
    # So the gauges print alone, with no tower and no purge, and they print
    # FIRST — the ring gauge is the cheapest thing that can tell you the whole
    # outline is wrong, and nobody should spend a three-filament print to find
    # that out. `tests` writes both files in that order.
    "gauges": [
        ("ring gauge",    [("ring", "ring_gauge", 1)],   (128, 180)),
        ("corner gauge",  [("corner", "frame_gauge", 1)], (128, 60)),
    ],
    "colour": [
        ("colour coupon", COUPON,                        (100, 150)),
        ("QR coupon",     QR_COUPON,                     (60, 80)),
    ],
    "coupon": [("colour coupon", COUPON, (128, 128))],
    "qr":     [("QR coupon", QR_COUPON, (128, 128))],
    "frame":  [("frame", FRAME, (128, 128))],
}
BED = 256.0          # P2S build plate, mm square
PLATE_MARGIN = 4.0   # keep parts off the very edge

# THE PURGE TOWER NEEDS A PLACE TO STAND, and a packer that ignores it has not
# finished packing. A multi-filament plate always grows one, the slicer
# auto-places it, and if every open pocket is small it wedges the tower against
# a part and warns "Prime Tower is too close to others, collisions may be
# caused". That is what the tests plate did on its first real slice: objects
# arranged only against each OTHER, leaving no deliberate gap.
#
# PER SET, not one constant. The first version of this was a single global
# zone, generalised from the small tests plate — and it immediately rejected
# the frame, which is 197 x 115 on a 256 bed and cannot leave a 102 x 110
# pocket anywhere. There is no one rectangle that suits both a plate of small
# coupons and a plate holding a part that nearly fills the bed; the zone is a
# property of the LAYOUT, so it lives with the layout.
#
# Sized generously where there is room: a tall three-filament tower wants more
# footprint than people expect, and unused bed is free.
TOWER_ZONES = {
    "colour": (150.0, 8.0, 252.0, 118.0),   # right of the coupons
    "coupon": (150.0, 8.0, 252.0, 118.0),
    "qr":     (150.0, 8.0, 252.0, 118.0),
    # The frame spans y 70.5 … 185.5, so the tower goes in the clear strip
    # BELOW it — the only place on the plate big enough once the case is down.
    "frame":  (40.0, 8.0, 216.0, 64.0),
}


def _sources_mtime() -> float:
    """Newest mtime across everything that can change a part's geometry."""
    return max(p.stat().st_mtime for p in SRC.parent.glob("*.scad")
               if not p.name.startswith("_"))


def render(part: str, out: Path) -> Path:
    """Export one part to binary STL, failing loudly on any diagnostic.

    The cache is mtime-checked against the sources, and that check is not
    optional book-keeping — without it this script silently ships the wrong
    plate. A bare `if out.exists()` reuses whatever an earlier run left in the
    working directory, so editing the .scad and re-running produces a 3MF built
    from the PREVIOUS geometry with no warning anywhere. It bit for real: a
    coupon margin changed, the plate rebuilt "successfully", and every part
    came from before the edit — including the build stamp debossed into it,
    which is the one feature whose whole job is to make that detectable.
    Same doctrine as the website's committed .glb files: a generator you can
    run without regenerating is a generator that lies.

    DO NOT "improve" this into a byte comparison of the STL. OpenSCAD's CGAL
    export does not emit triangles in a stable ORDER between runs: re-rendering
    an unchanged part produces a file that differs from the first byte of the
    second triangle onward while describing the identical solid — same triangle
    count, same volume to four decimals, same bounding box, same set of
    triangles. Measured, not assumed: a `cmp` against a known-good part
    reported "differs" and the shapes were equal. A byte check here would
    declare every cached part stale forever and re-render the whole plate on
    every run. Compare mtimes, or compare geometry — never bytes.
    """
    if out.exists() and out.stat().st_mtime >= _sources_mtime():
        return out
    out.unlink(missing_ok=True)
    r = subprocess.run(
        ["openscad", "--export-format", "binstl", "-o", str(out),
         "-D", f'part="{part}"', str(SRC)],
        capture_output=True, text=True)
    diag = [ln for ln in (r.stderr or "").splitlines()
            if "ERROR" in ln or "WARNING" in ln]
    if diag or not out.exists():
        raise SystemExit(f"render of {part} failed:\n" + "\n".join(diag[:5]))
    return out


def read_stl(path: Path):
    with open(path, "rb") as f:
        f.read(80)
        n = struct.unpack("<I", f.read(4))[0]
        return [struct.unpack("<12f", f.read(50)[:48])[3:12] for _ in range(n)]


def mesh(path: Path):
    """Indexed mesh, deduplicated WITHIN this part only (see header note 1)."""
    verts, vidx, tris = [], {}, []
    for t in read_stl(path):
        ids = []
        for k in ((t[0], t[1], t[2]), (t[3], t[4], t[5]), (t[6], t[7], t[8])):
            k = (round(k[0], 6), round(k[1], 6), round(k[2], 6))
            if k not in vidx:
                vidx[k] = len(verts)
                verts.append(k)
            ids.append(vidx[k])
        if len(set(ids)) == 3:          # drop degenerates, keep winding
            tris.append(ids)
    edges = Counter()
    for t in tris:
        for i in range(3):
            edges[tuple(sorted((t[i], t[(i + 1) % 3])))] += 1
    bad = sum(1 for c in edges.values() if c != 2)
    if bad:
        raise SystemExit(f"{path.name}: {bad} edges not shared by exactly two "
                         "faces — the slicer will reject this")
    return verts, tris


def bbox(verts):
    xs = [v[0] for v in verts]; ys = [v[1] for v in verts]
    return min(xs), max(xs), min(ys), max(ys)


def build(setname: str) -> Path:
    groups, oid = [], 0
    for gname, vols, centre in SETS[setname]:
        meshes = []
        for _n, part, slot in vols:
            oid += 1
            v, t = mesh(render(part, HERE / f"_3mf_{part}.stl"))
            meshes.append((oid, _n, slot, v, t))
        # the group's own extent, so the plate offset centres the WHOLE object
        allv = [p for m in meshes for p in m[3]]
        x0, x1, y0, y1 = bbox(allv)
        off = (centre[0] - (x0 + x1) / 2, centre[1] - (y0 + y1) / 2)
        foot = (x0 + off[0], x1 + off[0], y0 + off[1], y1 + off[1])
        print(f"  {gname:14} {len(meshes)} part(s), "
              f"{x1-x0:6.1f} x {y1-y0:5.1f} mm  at ({centre[0]}, {centre[1]})")
        for m in meshes:
            print(f"      {m[1]:7} {len(m[4]):>6} triangles  filament {m[2]}")
        groups.append((gname, meshes, off, foot))

    # A plate whose parts overlap is a wasted print, and the slicer will happily
    # take it. Check here instead.
    for i in range(len(groups)):
        for j in range(i + 1, len(groups)):
            a, b = groups[i][3], groups[j][3]
            if a[0] < b[1] and b[0] < a[1] and a[2] < b[3] and b[2] < a[3]:
                raise SystemExit(f"plate layout: '{groups[i][0]}' overlaps "
                                 f"'{groups[j][0]}' — move a centre in SETS")
    for g in groups:
        f = g[3]
        if (f[0] < PLATE_MARGIN or f[1] > BED - PLATE_MARGIN
                or f[2] < PLATE_MARGIN or f[3] > BED - PLATE_MARGIN):
            raise SystemExit(f"plate layout: '{g[0]}' runs off the {BED:.0f} mm "
                             f"bed at x {f[0]:.1f}..{f[1]:.1f} "
                             f"y {f[2]:.1f}..{f[3]:.1f}")

    # ...and out of the tower's zone, for any plate that actually changes tool.
    zone = TOWER_ZONES.get(setname)
    multi = len({s for g in groups for (_o, _n, s, _v, _t) in g[1]}) > 1
    if multi and zone is None:
        raise SystemExit(
            f"plate layout: set '{setname}' changes filament but declares no "
            f"purge-tower zone. Add one to TOWER_ZONES — the tower is not "
            f"optional, and leaving it nowhere to stand is how you get "
            f"'Prime Tower is too close to others'.")
    if multi:
        tx0, ty0, tx1, ty1 = zone
        for g in groups:
            f = g[3]
            if f[0] < tx1 and tx0 < f[1] and f[2] < ty1 and ty0 < f[3]:
                raise SystemExit(
                    f"plate layout: '{g[0]}' sits in the purge tower's zone "
                    f"(x {tx0:.0f}..{tx1:.0f}, y {ty0:.0f}..{ty1:.0f}) — move a "
                    f"centre in SETS. The tower is not optional on a "
                    f"multi-filament plate; leaving it nowhere to stand is how "
                    f"you get 'Prime Tower is too close to others'.")
        print(f"  purge tower zone kept clear: x {tx0:.0f}..{tx1:.0f}, "
              f"y {ty0:.0f}..{ty1:.0f}  "
              f"({tx1-tx0:.0f} x {ty1-ty0:.0f} mm)")

    objs = [m for g in groups for m in g[1]]

    ident = "1 0 0 0 1 0 0 0 1 0 0 0"
    res, items, bcfg, pcfg = [], [], [], []

    for oid, _n, _s, verts, tris in objs:
        vs = "\n".join(f'    <vertex x="{x:.5f}" y="{y:.5f}" z="{z:.5f}"/>'
                       for x, y, z in verts)
        ts = "\n".join(f'    <triangle v1="{a}" v2="{b}" v3="{c}"/>'
                       for a, b, c in tris)
        res.append(f'  <object id="{oid}" type="model">\n   <mesh>\n'
                   f'    <vertices>\n{vs}\n    </vertices>\n'
                   f'    <triangles>\n{ts}\n    </triangles>\n'
                   f'   </mesh>\n  </object>')

    # One ASSEMBLY per group, and one build item carrying that group's plate
    # offset — so an object's volumes travel together and stay registered,
    # while separate objects are independently placed.
    for gi, (gname, meshes, off, _foot) in enumerate(groups):
        aid = 100 + gi
        comps = "\n".join(
            f'    <component objectid="{m[0]}" transform="{ident}"/>'
            for m in meshes)
        res.append(f'  <object id="{aid}" type="model">\n   <components>\n'
                   f'{comps}\n   </components>\n  </object>')
        xf = f"1 0 0 0 1 0 0 0 1 {off[0]:.4f} {off[1]:.4f} 0"
        items.append(f'  <item objectid="{aid}" transform="{xf}"/>')
        bcfg.append(
            f' <object id="{aid}">\n'
            f'  <metadata key="name" value="{gname}"/>\n'
            '  <metadata key="extruder" value="1"/>\n'
            + "".join(f'  <part id="{m[0]}" subtype="normal_part">\n'
                      f'   <metadata key="name" value="{m[1]}"/>\n'
                      f'   <metadata key="extruder" value="{m[2]}"/>\n'
                      '  </part>\n' for m in meshes)
            + ' </object>')
        pcfg.append(
            f' <object id="{aid}">\n'
            f'  <metadata type="object" key="name" value="{gname}"/>\n'
            + "".join(f'  <volume firstid="0" lastid="{len(m[4]) - 1}">\n'
                      f'   <metadata type="volume" key="name" value="{m[1]}"/>\n'
                      f'   <metadata type="volume" key="extruder" '
                      f'value="{m[2]}"/>\n  </volume>\n' for m in meshes)
            + ' </object>')

    model = ('<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" '
             'xml:lang="en-US" xmlns="http://schemas.microsoft.com/'
             '3dmanufacturing/core/2015/02">\n <resources>\n'
             + "\n".join(res)
             + '\n </resources>\n <build>\n' + "\n".join(items)
             + '\n </build>\n</model>')

    name = f"lcd7_{setname}"
    bambu = ('<?xml version="1.0" encoding="UTF-8"?>\n<config>\n'
             + "\n".join(bcfg) + '\n</config>')
    prusa = ('<?xml version="1.0" encoding="UTF-8"?>\n<config>\n'
             + "\n".join(pcfg) + '\n</config>')

    ct = ('<?xml version="1.0" encoding="UTF-8"?>\n<Types xmlns="http://'
          'schemas.openxmlformats.org/package/2006/content-types">\n'
          '<Default Extension="rels" ContentType="application/vnd.'
          'openxmlformats-package.relationships+xml"/>\n'
          '<Default Extension="model" ContentType="application/vnd.ms-package.'
          '3dmanufacturing-3dmodel+xml"/>\n</Types>')
    rels = ('<?xml version="1.0" encoding="UTF-8"?>\n<Relationships xmlns='
            '"http://schemas.openxmlformats.org/package/2006/relationships">\n'
            '<Relationship Target="/3D/3dmodel.model" Id="rel-1" Type="http://'
            'schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>\n'
            '</Relationships>')

    out = HERE / f"{name}.3mf"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", ct)
        z.writestr("_rels/.rels", rels)
        z.writestr("3D/3dmodel.model", model)
        z.writestr("Metadata/model_settings.config", bambu)
        z.writestr("Metadata/Slic3r_PE_model.config", prusa)
    return out


def main() -> int:
    which = sys.argv[1] if len(sys.argv) > 1 else "coupon"
    # "tests" is the whole pre-flight, and it is deliberately TWO files in a
    # deliberate order rather than one plate — see the note on SETS.
    if which == "tests":
        for i, part in enumerate(("gauges", "colour"), 1):
            print(f"packaging {part}  (plate {i} of 2):")
            print(f"OK {build(part).name}")
        print("\n  Print lcd7_gauges.3mf FIRST: one filament, no tool change,")
        print("  no purge tower. The ring gauge on it is the cheapest thing")
        print("  that can tell you the whole outline is wrong, and nobody")
        print("  should spend a three-filament print to find that out.")
        print("  Then lcd7_colour.3mf, which needs all three slots loaded.")
        return 0
    if which not in SETS:
        print(f"usage: gen_3mf.py [tests | {' | '.join(SETS)}]", file=sys.stderr)
        return 2
    print(f"packaging {which}:")
    out = build(which)
    print(f"OK {out.name}  {out.stat().st_size / 1e6:.2f} MB")
    print("  open it directly — already positioned and already on their "
          "filaments.")
    print("  Add the filament SLOTS in Bambu Studio first: with one slot "
          "loaded there is")
    print("  nothing for parts 2 and 3 to point at, and it reads as 'the "
          "parts are missing'.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
