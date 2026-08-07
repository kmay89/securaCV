#!/usr/bin/env python3
"""Fail any STL that models a STEPPED FOOT RELIEF — a square ledge holding
the part's first millimeters in from the footprint above them.

    python3 check_foot_relief.py *.stl

WHY THIS EXISTS
---------------
Both display cases carried a knob called `lid_edge`, commented "bezel face
edge chamfer", that drew no chamfer at all: the cut was a plain
linear_extrude with no taper, so it removed a square RABBET around the whole
outline for its first 0.80 mm. Off the exported mesh, the C3 bezel's first
0.80 mm measured 25.920 x 40.870 against 27.520 x 42.470 above it.

That is three defects wearing one name, and print 4 (kmay89) found all three
by eye in the slicer before anyone found them here:

  * the ledge is an overhang. 0.80 mm of unsupported horizontal run, around
    the entire perimeter, at z = 0.80.
  * it lands on the FACE. Both bezels print face-down, so the ledge and its
    bridge scar sit on the one surface a person looks at.
  * it costs bed contact on the part that can least afford it. The C3's face
    plate is 1.2 mm thick and — since the light ring runs the full depth of
    the wall — is not joined to anything else on the plate. The rabbet took
    it from 508.15 mm2 of first-layer contact to 403.62. A fifth of the grip,
    given up by a feature that was supposed to help.

And it was redundant even read charitably: the slicer already applies
elephant-foot compensation (~0.2 mm) on the first layers. Modeling 0.80 mm
of relief on top of that is a second helping of the same correction.

WHAT THIS CHECKS
----------------
A REAL foot chamfer is fine and several cases have one (foot_chamfer_cut()
in the WAP and Vision enclosures). The difference is not "smaller at the
bottom" — a taper starts smaller too. The difference is HOW it grows: a
taper grows continuously, a rabbet holds still and then jumps by twice its
depth in a single layer. So: sample the section bounding box every STEP mm
through the first WINDOW mm and flag any single-sample growth over JUMP.

The check reads the MESH, not the source, so it catches the defect however
it was drawn — "it prints flat" stops being something a reader has to take
on trust.

WHY IT IS GIVEN AN EXPLICIT PART LIST AND NOT `*.stl`
-----------------------------------------------------
Because a flat foot is a decision about THESE parts, not a law of the
catalog, and pretending otherwise would make this a gate that lies. Run
against every STL, it also fires on:

  * bottom ROUNDOVERS, whose tangent at the plate is horizontal, so their
    first sample grows faster than any chamfer (canary_sense_back,
    the Vision back plates);
  * deliberate FLANGES and lap joints that step outward a millimeter or two
    up (dev_jbox_body, dev_s3_147_back, dev_lcd7_plug_buttons);
  * parts whose lowest facet sits a hair below z=0 (dev_watch_stand).

None of those is this defect, and none is ours to redraw on the strength of
a heuristic. Rather than carry a list of exemptions — which is how a gate
quietly stops meaning anything — the workflow names the parts that are
under the rule: the two display bezels, which print FACE-DOWN, where the
first layers are the finished front surface and a modeled foot relief is
therefore always wrong. Add a part here when you decide the rule binds it.
"""
import re
import struct
import sys

STEP = 0.05     # mm between section samples
WINDOW = 2.0    # mm above the part's lowest point that count as "the foot"
JUMP = 0.5      # mm of bbox growth in one STEP that reads as a ledge, not a taper


def triangles(path):
    data = open(path, "rb").read()
    if data[:5] == b"solid" and b"facet" in data[:2000]:      # ASCII
        v = [tuple(map(float, m.groups())) for m in
             re.finditer(rb"vertex\s+(\S+)\s+(\S+)\s+(\S+)", data)]
        return [v[i:i + 3] for i in range(0, len(v), 3)]
    n = struct.unpack("<I", data[80:84])[0]                   # binary
    return [[struct.unpack("<3f", data[84 + i*50 + 12 + j*12:
                                       84 + i*50 + 24 + j*12]) for j in range(3)]
            for i in range(n)]


def section_bbox(tris, z):
    """(width, depth) of the part's cross-section at height z, or None."""
    xs, ys = [], []
    for t in tris:
        for a in range(3):
            p, q = t[a], t[(a + 1) % 3]
            if (p[2] - z) * (q[2] - z) < 0:                   # edge crosses z
                f = (z - p[2]) / (q[2] - p[2])
                xs.append(p[0] + f*(q[0] - p[0]))
                ys.append(p[1] + f*(q[1] - p[1]))
    return (max(xs) - min(xs), max(ys) - min(ys)) if xs else None


def worst_step(tris):
    """The largest single-sample footprint jump inside the foot window."""
    z0 = min(p[2] for t in tris for p in t)
    prev, worst = None, None
    for i in range(int(WINDOW / STEP)):
        z = z0 + STEP/2 + i*STEP
        box = section_bbox(tris, z)
        if box is None:
            continue
        if prev is not None:
            grew = max(box[0] - prev[1][0], box[1] - prev[1][1])
            if grew > JUMP and (worst is None or grew > worst[0]):
                worst = (grew, prev[0], prev[1], z, box)
        prev = (z, box)
    return worst


def main(paths):
    status = 0
    for path in paths:
        worst = worst_step(triangles(path))
        if worst is None:
            continue
        grew, za, a, zb, b = worst
        print(f"::error file=docs/hardware/enclosure/{path}::stepped foot relief"
              f" — {a[0]:.3f} x {a[1]:.3f} at z={za:.3f} jumps to"
              f" {b[0]:.3f} x {b[1]:.3f} at z={zb:.3f} (+{grew:.3f} mm in"
              f" {STEP} mm). That ledge overhangs, and on a face-down part it"
              f" overhangs onto the visible face. Draw a real taper or nothing"
              f" at all — the slicer's elephant-foot compensation is the relief"
              f" this needs.")
        status = 1
    if status == 0:
        print(f"foot relief OK: {len(paths)} parts, none stepped")
    return status


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
