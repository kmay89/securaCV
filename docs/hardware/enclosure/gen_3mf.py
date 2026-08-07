#!/usr/bin/env python3
"""Package a case's per-filament parts into ONE Bambu-readable 3MF.

    python3 gen_3mf.py tests      # ALL the test parts, laid out on one plate
    python3 gen_3mf.py coupon     # just the color + fit coupon
    python3 gen_3mf.py frame      # the whole 7" case
    python3 gen_3mf.py stick      # the hallway stick: bezel + band, back + mark
    python3 gen_3mf.py c3         # the C3 pocket case: yellow, branded lid

Renders the parts it needs with OpenSCAD, then writes a single object whose
volumes are already registered to each other and already assigned to
filaments 1 / 2 / 3.

WHY THIS EXISTS
---------------
The three color parts (body / ink / accent) only mean anything in the SAME
coordinate frame: the ink lettering sits in recesses cut into the body, and
moving one relative to the other by a millimeter puts the words beside their
own holes rather than in them.

Handing an operator three STLs and the instruction "load one, Add part → Load
the others, and do NOT re-center them" does not survive contact with a slicer.
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
import re
import subprocess
import sys
import zipfile
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
LCD7 = "canary_s3_lcd7.scad"
STICK = "canary_s3_lcd147.scad"

# A volume is (name, scad part, filament slot, source .scad, extra -D defines).
# The source is per-volume and not global because two different cases in this
# directory both expose a part called `fil_body` — keying the render cache on
# the part name alone would hand one case's plate the other case's geometry,
# which is the exact class of silent-wrong-output this file exists to prevent.
#
# Slot order IS the palette order, so a spool in the wrong slot swaps the
# lettering's colors — see PRINT COLORS.
COUPON = [("body", "coupon_body", 1, LCD7, {}),
          ("ink", "coupon_ink", 2, LCD7, {}),
          ("accent", "coupon_accent", 3, LCD7, {})]
FRAME = [("body", "fil_body", 1, LCD7, {}),
         ("ink", "fil_ink", 2, LCD7, {}),
         ("accent", "fil_accent", 3, LCD7, {})]
# The QR plaque is deliberately TWO filaments, not three: the symbol's modules
# on a body-color field. WHICH filament carries the modules is READ FROM THE
# SCAD rather than named here — canary_s3_lcd7.scad's ink_groups/accent_groups
# are the only place that decides, and a hardcoded slot in this file went stale
# the instant "qr" moved to the accent. The failure was silent and nasty: the
# scad's coupon part rendered empty, OpenSCAD wrote no file, and this packer
# shipped a body-only plaque — a scan coupon with no symbol, handed to someone
# told to scan it before committing a frame.
#
# Note this is the ONE volume list that is derived. The others may name a
# filament that the palette does not currently load: build() renders them,
# finds an empty object, drops the volume and says so out loud. That is the
# right behavior for a body/ink/accent triple, where a missing color is a fact
# about the palette. It is the WRONG behavior here, because dropping the
# modules leaves a plaque that is still printable, still two-sided, and no
# longer a scan coupon.
FIL_SLOT = {"body": 1, "ink": 2, "accent": 3}


def group_filament(group: str) -> str:
    """Which filament a back-plate group takes, per the .scad's own lists."""
    src = (HERE / LCD7).read_text(encoding="utf-8")
    for name, fil in (("accent_groups", "accent"), ("ink_groups", "ink")):
        m = re.search(rf"^{name}\s*=\s*\[([^\]]*)\]", src, re.M)
        if m and re.search(rf'"{re.escape(group)}"', m.group(1)):
            return fil
    return "body"


_QR_FIL = group_filament("qr")
QR_COUPON = [("body", "coupon_qr_body", 1, LCD7, {})] + (
    [] if _QR_FIL == "body"
    else [(_QR_FIL, "coupon_qr_fill", FIL_SLOT[_QR_FIL], LCD7, {})])

# Objects whose volumes are ALL required — an empty one is a hard error here,
# not a dropped volume with a note.
#
# The general rule (drop it, say so, carry on) is right for the color coupon:
# a palette that puts no ink on that object is a real configuration, and the
# operator just loads one fewer spool. It is exactly wrong for the QR coupon,
# whose entire job is to be SCANNED. Drop its ink and you package a blank
# body-colored plaque, print it, and learn nothing — while the run log says
# "EMPTY: the palette puts no ink on this object", which reads like a palette
# decision rather than a broken test.
#
# That happened: turning the back plate's `qr_back` off also stopped the coupon
# drawing a symbol, because the coupon intersects the frame's ink and the frame
# had none. The .scad fix is `qr_draw` (the coupon carries the symbol whether or
# not the plate does); this is the second lock, so the next way it breaks is
# caught by the packager rather than by a printed part that will not scan.
# "c3 lid" is here for the same reason, one step removed. Its mark volume is
# TYPE, and type has a dependency nothing else on these plates has: a font.
# On a machine without DejaVu Sans, text() yields nothing, the volume renders
# empty, and the drop-and-carry-on rule would package a blank yellow lid and
# say "the palette puts no ink on this object" — which reads like a palette
# decision rather than a missing font. The whole point of this plate is a lid
# with the brand ON it, so an empty mark is a hard error.
# (A lid_back="keyhole" build genuinely has no mark. That is a different
# product configuration and wants its own set — it must not be quietly
# packaged as the branded one.)
REQUIRE_ALL = {"QR coupon", "c3 lid"}

# ── The hallway stick (Waveshare ESP32-S3-LCD-1.47) ────────────────────────
# Two objects, three filaments, and the whole reason it is worth packaging:
# both objects have a second color that lives INSIDE a recess in the first.
#
# band_clear = 0 on the band is not a tweak, it is the co-print mode the .scad
# documents. The slot is always cut at full size; only the loose INSERT is
# shrunk, so a band rendered at the default 0.10 would print 0.2 mm smaller
# than its own pocket in every direction and leave the AMS to bridge a gap
# that should not exist. At 0 the band exactly fills the slot it was cut from
# and fuses to the walls, which is what an AMS is for.
STICK_BEZEL = [("body", "bezel", 1, STICK, {}),
               ("band", "fil_light", 3, STICK, {"band_clear": "0"})]
STICK_BACK = [("body", "fil_body", 1, STICK, {}),
              ("mark", "fil_accent", 2, STICK, {})]

# ── The C3 pocket case (Waveshare ESP32-C3-LCD-1.47) ───────────────────────
# Same ROLE convention as the stick — slot 1 body, slot 2 mark, slot 3 light
# — so an operator who has printed one case loads the other without
# re-learning the slots. What changed with the yellow colorway is the SPOOLS
# in those roles, not the roles:
#     slot 1 BODY  = YELLOW  (bezel and lid both — the case is yellow)
#     slot 2 MARK  = BLACK   (the "Canary" wordmark on the lid's back)
#     slot 3 LIGHT = WHITE   (the band)
# The first three prints ran slot 1 BLACK / slot 2 YELLOW; slots 1 and 2 now
# swap spools. Both objects are two-color now: the bezel's band fuses in and
# the lid's wordmark fills its deboss, both at zero clearance — the co-print
# doctrine the .scad documents for the band, applied to type as well.
#
# headers="pillars" is pinned on every volume: the plate prints the case for
# the board AS WAVESHARE SHIPS IT (brass corner pillars on, headers not
# soldered). Pinned rather than inherited so the plate cannot silently change
# meaning if the .scad's default ever moves. Stripped or soldered-header
# boards: re-run with the define edited — the geometry is one word away, and
# a 3MF that guessed would fit exactly one of the three boards silently.
C3 = "canary_c3_lcd147.scad"
C3_BEZEL = [("body", "bezel", 1, C3, {"headers": '"pillars"'}),
            ("band", "light", 3, C3, {"headers": '"pillars"',
                                      "band_clear": "0"})]
# The lid is a THREE-filament part now: yellow plate, black wordmark plus the
# egg's outer ring, white egg shell. No new spool — slot 3 is already loaded
# for the bezel's light band, and this plate already changes tool for the
# wordmark. The shell rides along on a color change that was happening anyway.
C3_LID = [("lid", "lid", 1, C3, {"headers": '"pillars"'}),
          ("mark", "mark", 2, C3, {"headers": '"pillars"'}),
          ("shell", "shell", 3, C3, {"headers": '"pillars"'})]

# A "set" is a list of OBJECTS. Each object is (name, volumes, plate center).
# Volumes within one object are parts of it and stay registered to each other;
# separate objects are independent and get their own place on the plate.
#
# The "tests" plate is the whole pre-flight in one job, cheapest check first:
# the ring proves the outline, the coupon proves color and corner fit, the
# QR plaque proves the symbol actually scans, and the corner gauge proves the
# screw pattern against the real panel. Four questions, one job, and every one
# of them is cheaper to answer here than on a committed frame.
SETS = {
    # SPLIT BY FILAMENT COUNT, not by theme — and this is the single biggest
    # time saving in the file, worth more than any arrangement of parts.
    #
    # A tool change anywhere on a plate builds a purge tower, and the tower is
    # built up to the height of the LAST change. The color coupon's bezel band
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
        ("ring gauge",   [("ring", "ring_gauge", 1, LCD7, {})],   (128, 180)),
        ("corner gauge", [("corner", "frame_gauge", 1, LCD7, {})], (128, 60)),
    ],
    "color": [
        ("color coupon", COUPON,                        (100, 150)),
        ("QR coupon",     QR_COUPON,                     (60, 80)),
    ],
    "coupon": [("color coupon", COUPON, (128, 128))],
    "qr":     [("QR coupon", QR_COUPON, (128, 128))],
    "frame":  [("frame", FRAME, (128, 128))],
    # Both halves of the stick on one plate: it is a 25 x 41 mm part, the pair
    # is under 20 g, and splitting it across two jobs would build the purge
    # tower twice for no reason.
    "stick": [("stick bezel", STICK_BEZEL, (100, 150)),
              ("stick back",  STICK_BACK,  (100, 95))],
    # Both halves of the C3 case on one plate, same argument as the stick:
    # 25 x 41 mm parts, the pair is light, and a second job would build the
    # purge tower twice for no reason.
    "c3": [("c3 bezel", C3_BEZEL, (100, 150)),
           ("c3 lid",   C3_LID,   (100, 95))],
    # THE TPU FITMENTS, two of each — a spare is worth more than a second job.
    # All six are ONE material, so this plate changes tool exactly never: no
    # purge tower, no tower zone, and every volume on slot 1. That is the whole
    # reason they get their own plate rather than riding along with a rigid
    # one; a soft part sharing a plate with a color change waits through the
    # tower and comes off strung.
    #
    # ⚠️ THE SLOT IS NOT THE POINT — THE FEED PATH IS. Slot 1 here means "the
    # first filament of this plate", and what that spool has to be is decided
    # by durometer, not by this file: 90-95 A must come off an EXTERNAL holder
    # (it buckles in the AMS's long PTFE path and jams the hub), while Bambu's
    # stiffer "TPU for AMS" 68 D feeds the AMS fine and is too hard to be a
    # good leash. Both print this plate; they do not print it equally well.
    # See bambu_p2s_bringup.md §0, and the leash note at port_tether.
    #
    # Layout: the two covers stand side by side on the left (they are the tall
    # parts, 20 x 48), the four stadium fitments stack down the right in the
    # order you install them — grommets first, then the blanks that fill
    # whichever exit you did not use. Spacing is deliberately loose. TPU strings
    # between close parts and a wipe that clips a neighbor drags it off the
    # plate, which on a 14 g print costs more than the bed space does.
    #
    # ALL FOUR FITMENTS, and that is the point of naming the set "tpu" rather
    # than listing the three somebody happened to ask for. The frame cuts a
    # BOOT/RESET window, so a plate that omits plug_buttons leaves the case
    # with an open hole and no way to press the buttons through it — and it
    # does so silently, because nothing downstream knows what the operator
    # meant to print. A set named for a MATERIAL has to carry everything made
    # of it; anything less is a checklist that quietly loses an item.
    "tpu": [
        ("SD cover 1",    [("tpu", "plug_sd",      1, LCD7, {})], (80, 128)),
        ("SD cover 2",    [("tpu", "plug_sd",      1, LCD7, {})], (112, 128)),
        ("grommet 1",     [("tpu", "grommet_usb",  1, LCD7, {})], (150, 145)),
        ("grommet 2",     [("tpu", "grommet_usb",  1, LCD7, {})], (150, 125)),
        ("port blank 1",  [("tpu", "plug_port",    1, LCD7, {})], (150, 105)),
        ("port blank 2",  [("tpu", "plug_port",    1, LCD7, {})], (150, 85)),
        ("button plug 1", [("tpu", "plug_buttons", 1, LCD7, {})], (195, 145)),
        ("button plug 2", [("tpu", "plug_buttons", 1, LCD7, {})], (195, 118)),
    ],
}
# The file each set writes. Named per CASE, not per set, so two cases' plates
# can never overwrite each other.
OUTPUT = {"gauges": "lcd7_gauges", "color": "lcd7_color",
          "coupon": "lcd7_coupon", "qr": "lcd7_qr", "frame": "lcd7_frame",
          "stick": "stick_case", "c3": "c3_case", "tpu": "lcd7_tpu"}
assert set(OUTPUT) == set(SETS), "every set needs an output name"
# Volume tuples grew a source and a defines dict when a second case moved in
# here, and a set written inline (rather than through one of the named lists
# above) is easy to miss. This is cheap and it fails at import, not four
# minutes into a CGAL render — which is how the miss was actually found.
for _set, _groups in SETS.items():
    for _gname, _vols, _center in _groups:
        for _v in _vols:
            assert len(_v) == 5, (
                f"{_set}/{_gname}: volume {_v!r} is not "
                "(name, part, slot, source, defines)")
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
# zone, generalized from the small tests plate — and it immediately rejected
# the frame, which is 197 x 115 on a 256 bed and cannot leave a 102 x 110
# pocket anywhere. There is no one rectangle that suits both a plate of small
# coupons and a plate holding a part that nearly fills the bed; the zone is a
# property of the LAYOUT, so it lives with the layout.
#
# Sized generously where there is room: a tall three-filament tower wants more
# footprint than people expect, and unused bed is free.
TOWER_ZONES = {
    "color": (150.0, 8.0, 252.0, 118.0),   # right of the coupons
    "coupon": (150.0, 8.0, 252.0, 118.0),
    "qr":     (150.0, 8.0, 252.0, 118.0),
    # The frame spans y 70.5 … 185.5, so the tower goes in the clear strip
    # BELOW it — the only place on the plate big enough once the case is down.
    "frame":  (40.0, 8.0, 216.0, 64.0),
    # The stick is tiny and both objects sit left of x=115, so the tower gets
    # the whole right half — a three-filament tower on a part this small
    # purges far more plastic than the part itself weighs, and cramping it is
    # the one way to make a 20 g print fail.
    "stick":  (140.0, 40.0, 250.0, 210.0),
    # Same layout as the stick's plate, same reasoning: both objects sit left
    # of x ≈ 115, so the tower gets the whole right half of the bed.
    "c3":     (140.0, 40.0, 250.0, 210.0),
}


def _sources_mtime() -> float:
    """Newest mtime across everything that can change a part's geometry."""
    return max(p.stat().st_mtime for p in HERE.glob("*.scad")
               if not p.name.startswith("_"))


def render(part: str, out: Path, src: str, defines: dict) -> Path:
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
    cmd = ["openscad", "--export-format", "binstl", "-o", str(out),
           "-D", f'part="{part}"']
    for k, v in defines.items():
        cmd += ["-D", f"{k}={v}"]
    r = subprocess.run(cmd + [str(HERE / src)], capture_output=True, text=True)
    raw = r.stderr or ""
    diag = [ln for ln in raw.splitlines()
            if "ERROR" in ln or "WARNING" in ln]
    # An EMPTY part is not a failed part, and telling them apart matters.
    # A filament's share of an object is empty whenever the palette simply
    # does not put that color on that object — the color coupon has no INK
    # on it once ink_groups is just ["qr"], because the coupon clip
    # deliberately does not reach the QR. That is a correct palette, not a
    # broken render, and the fix is NOT to invent a token ink feature so the
    # packager stays happy: it is to drop the volume and say so. Caller
    # decides whether an object with no volumes left is fatal.
    # Match against RAW stderr, not the diagnostic list: OpenSCAD prints
    # "Current top level object is empty." with NO "WARNING"/"ERROR" prefix,
    # so it never survives the filter above. Keying on the filtered list is
    # exactly the bug this comment exists to stop being re-introduced — it
    # looked right and turned every empty part back into a hard abort.
    # Empty is: no file, no diagnostics at all, and the marker present.
    if not out.exists() and not diag and "top level object is empty" in raw:
        return None
    if diag or not out.exists():
        raise SystemExit(f"render of {part} failed:\n"
                         + "\n".join(diag[:5] or raw.splitlines()[-3:]))
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


def build(setname: str) -> tuple:
    """Returns (path, used_slots) — the slots are what actually SURVIVED the
    render, not what SETS listed. An empty volume is dropped here, so the
    table over-reports: for the two-color palette it still names slot 2."""
    groups, oid, used_slots = [], 0, set()
    for gname, vols, center in SETS[setname]:
        meshes, dropped = [], []
        for _n, part, slot, src, defs in vols:
            tag = "".join(f"_{k}{v}" for k, v in sorted(defs.items()))
            stl = render(part, HERE / f"_3mf_{Path(src).stem}_{part}{tag}.stl",
                         src, defs)
            if stl is None:            # this color is not on this object
                if gname in REQUIRE_ALL:
                    raise SystemExit(
                        f"'{gname}': the {_n} volume (part=\"{part}\") rendered "
                        f"EMPTY, and this object needs all of its volumes — a "
                        f"QR coupon with no symbol on it is a test that cannot "
                        f"fail. Check that the symbol is drawn for this part "
                        f"(see qr_draw in {src}).")
                dropped.append((_n, slot))
                continue
            oid += 1
            v, t = mesh(stl)
            meshes.append((oid, _n, slot, v, t))
        if not meshes:
            raise SystemExit(
                f"'{gname}': every filament volume is empty — there is "
                "nothing to print. Check the palette groups.")
        # the group's own extent, so the plate offset centers the WHOLE object
        allv = [p for m in meshes for p in m[3]]
        x0, x1, y0, y1 = bbox(allv)
        # ...and lift the whole group so nothing starts below the plate. An
        # accent volume typically dips a hair into the body it unions with (a
        # boolean needs the overlap), which puts it below z=0 once the part is
        # already lying face-down. The lift is per GROUP, never per volume —
        # leveling volumes independently is exactly how you shear an inlay
        # out of its recess.
        zlift = -min(p[2] for p in allv)
        off = (center[0] - (x0 + x1) / 2, center[1] - (y0 + y1) / 2,
               zlift if zlift > 0 else 0.0)
        foot = (x0 + off[0], x1 + off[0], y0 + off[1], y1 + off[1])
        print(f"  {gname:14} {len(meshes)} part(s), "
              f"{x1-x0:6.1f} x {y1-y0:5.1f} mm  at ({center[0]}, {center[1]})")
        for m in meshes:
            print(f"      {m[1]:7} {len(m[4]):>6} triangles  filament {m[2]}")
            used_slots.add(m[2])
        # Loud, never silent: a filament missing from a plate changes what the
        # operator has to load, and a color coupon that quietly stopped
        # rehearsing a color is worse than one that never claimed to.
        for _n, slot in dropped:
            print(f"      {_n:7} {'—':>6} EMPTY: the palette puts no {_n} on "
                  f"this object, so filament slot {slot} is not used here")
        groups.append((gname, meshes, off, foot))

    # A plate whose parts overlap is a wasted print, and the slicer will happily
    # take it. Check here instead.
    for i in range(len(groups)):
        for j in range(i + 1, len(groups)):
            a, b = groups[i][3], groups[j][3]
            if a[0] < b[1] and b[0] < a[1] and a[2] < b[3] and b[2] < a[3]:
                raise SystemExit(f"plate layout: '{groups[i][0]}' overlaps "
                                 f"'{groups[j][0]}' — move a center in SETS")
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
                    f"center in SETS. The tower is not optional on a "
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
        xf = f"1 0 0 0 1 0 0 0 1 {off[0]:.4f} {off[1]:.4f} {off[2]:.4f}"
        items.append(f'  <item objectid="{aid}" transform="{xf}"/>')
        bcfg.append(
            f' <object id="{aid}">\n'
            f'  <metadata key="name" value="{gname}"/>\n'
            # The OBJECT-level extruder follows the group's FIRST volume, not a
            # hard-coded 1. Bambu Studio applies the object value to any object
            # it treats as single-part — which is exactly what the C3 case's
            # lid is — so a hard-coded 1 silently painted the yellow lid black.
            # kmay89's print 2 did not get a yellow lid; this line is why.
            # Multi-volume objects are unaffected either way (their per-part
            # values below override), so first-volume is always right.
            f'  <metadata key="extruder" value="{meshes[0][2]}"/>\n'
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

    name = OUTPUT[setname]
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
    return out, sorted(used_slots)


def main() -> int:
    which = sys.argv[1] if len(sys.argv) > 1 else "coupon"
    # "tests" is the whole pre-flight, and it is deliberately TWO files in a
    # deliberate order rather than one plate — see the note on SETS.
    if which == "tests":
        for i, part in enumerate(("gauges", "color"), 1):
            print(f"packaging {part}  (plate {i} of 2):")
            print(f"OK {build(part)[0].name}")
        print("\n  Print lcd7_gauges.3mf FIRST: one filament, no tool change,")
        print("  no purge tower. The ring gauge on it is the cheapest thing")
        print("  that can tell you the whole outline is wrong, and nobody")
        print("  should spend a three-filament print to find that out.")
        print("  Then lcd7_color.3mf, which needs all three slots loaded.")
        return 0
    if which not in SETS:
        print(f"usage: gen_3mf.py [tests | {' | '.join(SETS)}]", file=sys.stderr)
        return 2
    print(f"packaging {which}:")
    out, slots = build(which)
    print(f"OK {out.name}  {out.stat().st_size / 1e6:.2f} MB")
    print("  open it directly — already positioned and already on their "
          "filaments.")
    # Name the slots this plate ACTUALLY uses. "parts 2 and 3" was right only
    # while every object was three-filament; the two-color palette uses slots
    # 1 and 3, so a fixed sentence sends the operator to load the wrong spool
    # into the one slot the plate does not touch.
    named = (", ".join(str(s) for s in slots[:-1]) + f" and {slots[-1]}"
             if len(slots) > 1 else str(slots[0]))
    # ...and only give the load-the-slots warning when there IS more than one.
    # On a single-material plate the old text read "add filament SLOTS 1 first:
    # with one slot loaded there is nothing for the other volumes to point at",
    # which describes a hazard that cannot occur and names slots that do not
    # exist. Advice that is wrong on a plate is worse than no advice, because
    # the operator cannot tell which half of the sentence to believe.
    if len(slots) > 1:
        print(f"  Add filament SLOTS {named} in Bambu Studio first: with one")
        print("  slot loaded there is nothing for the other volumes to point")
        print("  at, and it reads as 'the parts are missing'.")
    else:
        print(f"  ONE material, one slot ({named}) — no tool change anywhere on")
        print("  this plate, so no purge tower and nothing to remap. Whatever")
        print("  is loaded in that slot is what the whole plate prints in.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
