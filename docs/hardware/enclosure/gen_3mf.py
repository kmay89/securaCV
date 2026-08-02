#!/usr/bin/env python3
"""Package the 7" case's per-filament parts into ONE Bambu-readable 3MF.

    python3 gen_3mf.py coupon     # the colour + fit coupon
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
SETS = {
    "coupon": [("body", "coupon_body", 1),
               ("ink", "coupon_ink", 2),
               ("accent", "coupon_accent", 3)],
    "frame":  [("body", "fil_body", 1),
               ("ink", "fil_ink", 2),
               ("accent", "fil_accent", 3)],
}


def render(part: str, out: Path) -> Path:
    """Export one part to binary STL, failing loudly on any diagnostic."""
    if out.exists():
        return out
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


def build(setname: str) -> Path:
    parts = SETS[setname]
    objs = []
    for oid, (name, part, slot) in enumerate(parts, start=1):
        stl = render(part, HERE / f"_3mf_{part}.stl")
        v, t = mesh(stl)
        print(f"  {name:7} object {oid}  {len(t):>6} triangles  filament {slot}")
        objs.append((oid, name, slot, v, t))

    res = []
    for oid, _n, _s, verts, tris in objs:
        vs = "\n".join(f'    <vertex x="{x:.5f}" y="{y:.5f}" z="{z:.5f}"/>'
                       for x, y, z in verts)
        ts = "\n".join(f'    <triangle v1="{a}" v2="{b}" v3="{c}"/>'
                       for a, b, c in tris)
        res.append(f'  <object id="{oid}" type="model">\n   <mesh>\n'
                   f'    <vertices>\n{vs}\n    </vertices>\n'
                   f'    <triangles>\n{ts}\n    </triangles>\n'
                   f'   </mesh>\n  </object>')
    ident = "1 0 0 0 1 0 0 0 1 0 0 0"
    comps = "\n".join(f'    <component objectid="{o[0]}" transform="{ident}"/>'
                      for o in objs)
    res.append(f'  <object id="99" type="model">\n   <components>\n{comps}\n'
               f'   </components>\n  </object>')

    model = ('<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" '
             'xml:lang="en-US" xmlns="http://schemas.microsoft.com/'
             '3dmanufacturing/core/2015/02">\n <resources>\n'
             + "\n".join(res) +
             f'\n </resources>\n <build>\n  <item objectid="99" '
             f'transform="{ident}"/>\n </build>\n</model>')

    name = f"lcd7_{setname}"
    bambu = ('<?xml version="1.0" encoding="UTF-8"?>\n<config>\n'
             f' <object id="99">\n  <metadata key="name" value="{name}"/>\n'
             '  <metadata key="extruder" value="1"/>\n'
             + "".join(f'  <part id="{o}" subtype="normal_part">\n'
                       f'   <metadata key="name" value="{n}"/>\n'
                       f'   <metadata key="extruder" value="{s}"/>\n  </part>\n'
                       for o, n, s, _v, _t in objs)
             + ' </object>\n</config>')
    prusa = ('<?xml version="1.0" encoding="UTF-8"?>\n<config>\n'
             f' <object id="99">\n  <metadata type="object" key="name" '
             f'value="{name}"/>\n'
             + "".join(f'  <volume firstid="0" lastid="{len(t) - 1}">\n'
                       f'   <metadata type="volume" key="name" value="{n}"/>\n'
                       f'   <metadata type="volume" key="extruder" '
                       f'value="{s}"/>\n  </volume>\n'
                       for _o, n, s, _v, t in objs)
             + ' </object>\n</config>')

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
    for _name, part, _slot in parts:
        (HERE / f"_3mf_{part}.stl").unlink(missing_ok=True)
    return out


def main() -> int:
    which = sys.argv[1] if len(sys.argv) > 1 else "coupon"
    if which not in SETS:
        print(f"usage: gen_3mf.py [{' | '.join(SETS)}]", file=sys.stderr)
        return 2
    print(f"packaging {which}:")
    out = build(which)
    print(f"OK {out.name}  {out.stat().st_size / 1e6:.2f} MB")
    print("  open it directly — three parts, already on filaments 1/2/3.")
    print("  Add the filament slots in Bambu Studio FIRST, or there is "
          "nothing for parts 2 and 3 to point at.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
