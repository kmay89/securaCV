#!/usr/bin/env python3
"""gen_assembled_dims.py — the ASSEMBLED envelope of every multi-part device,
measured, never summed.

    python3 gen_assembled_dims.py            # regenerate assembled_dims.json
    python3 gen_assembled_dims.py --check    # CI gate: re-measure, diff

WHY THIS FILE EXISTS. The fleet-figure ledger used to compute a multi-part
device's envelope by stacking part bounding boxes front-to-back ("depth
adds"), which ignores every lip, skirt and pocket that NESTS one part into
another. The error was not small and it was not one device:

    device               stacked     assembled (this file)
    WAP (compact)        19.6 mm     15.05 mm   (+30 %)
    Vision (XIAO)        30.5 mm     23.38 mm   (+30 %)
    Sense                27.95 mm    21.5  mm   (+30 %)
    Doorbell (on plate)  47.6 mm     ~30.2 mm   (+58 %)

Those stacked depths were the "assembled" numbers every surface displayed —
the /figures catalog, the phone and Wall turntables, the website's AR
"dimensionally-honest preview". A 30 % lie about how far a case stands off
the wall is exactly the class of falsehood the ledger exists to prevent.

HOW IT MEASURES. Each device below is rendered as the UNION of its parts in
their assembled positions — the SAME one-line placements the case files'
own `<case>_fitcheck()` modules use for the CI closing gate, so the datum
this file measures is the datum the fit gate proves. The union's bounding
box, mapped into the figure frame (scad (x, y, z) -> figure (w, h, d)), is
written to assembled_dims.json for gen_figures.mjs to read: the node-side
generator cannot run OpenSCAD, so the measurement is committed here, where
the enclosure CI already has the toolchain, and byte-gated like every other
generated catalog. Bounding boxes are deterministic even though OpenSCAD's
STL bytes are not, so --check compares numbers, not bytes.

Adding a device: add a row to DEVICES with the case's own assembled
placement (crib it from that case's fitcheck module — never invent one; a
case with no fitcheck module is measured only where its own geometry states
the seat, as the Watch Station's bezel does, and the row says where) and
rerun. gen_figures.mjs refuses a `parts:` device figure that has no row
here, so a new multi-part figure cannot fall back to the stacked lie — and a
figure declared `assembled: true` (an in-development case with no committed
STLs) reads its envelope from its row here and nowhere else.

A row may also name what the massing draws ON the face, so it is the CAD's
number rather than a retyped one: `face` (a display's aperture, centered on
the envelope — the Watch's bezel bore, the Dash's view window) and
`features` (off-center marks — the Combo's lens and radome window: each the
case's own `[cx, cy, w, h]` expression in its scad frame, recorded as a
center on the measured envelope, from its min corner, so no symmetry is
assumed). Neither is read off the cut geometry: each is the case's own
variables, the ones its cuts are drawn at, echoed back. --check re-evaluates
both with the envelope and refuses anything it cannot read back as the
recorded number — so an edit through those variables is caught, and a cut
moved without going through them is not. The features also cross to the
website: gen_builder_manifest.py --site carries them verbatim as the figure's
`features_mm` in scad/cad-dims.json, where the site's AR model of the case
places its lens and window (so a feature edit here is a carry, then that
model's regeneration, like a seam).

The render-and-parse-echo mechanics live in scad_probe.py, shared with
gen_hardware.py and gen_enclosures.py --check-previews.
"""

import json
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import scad_probe  # noqa: E402  (the shared render-and-parse-echo helper, beside this file)

OUT = HERE / "assembled_dims.json"
TOL = 0.01  # mm — bbox agreement required by --check

# figure id -> how to build the assembled union. `body` is the union in the
# case's own scad frame; `overrides` are Customizer assignments applied AFTER
# the include (an include's own defaults win over anything set before it).
# Placements are the case fitcheck modules' own, verbatim.
DEVICES = {
    # `part` is set to the part the union ALSO draws at the origin: an
    # include's top-level dispatch always renders something (an unknown part
    # falls through to the side-by-side "all" layout, which would contaminate
    # the bbox), so the probe makes that something an exact duplicate of a
    # union member — geometry the union already contains, moving no bound.
    "device.canary-wap": {
        "scad": "canary_wap_enclosure.scad",
        "overrides": {"preset": '"compact_plain"', "part": '"base"'},
        "body": "union() { base(); translate([0, 0, base_h]) lid(); }",
        "seams": "[]",   # the piston plate: base is the plate inside lid's walls — no seam crosses the side profile
        "placement": "wap_fitcheck: lid (the shell) at z = base_h; base (the plate) inside its walls at floor_t",
    },
    "device.canary-vision": {
        "scad": "canary_vision_enclosure.scad",
        "overrides": {"host": '"xiao"', "preset": '"vision_indoor"', "part": '"back"'},
        "body": "union() { back(); translate([0, 0, base_d]) front(); }",
        "seams": "[]",   # the piston plate: no seam crosses the side profile (see the Sense)
        "placement": "vision_fitcheck: front at z = base_d (the plate's front face at floor_t, inside the walls)",
    },
    "device.canary-vision-devkit": {
        "scad": "canary_vision_enclosure.scad",
        "overrides": {"host": '"devkit"', "preset": '"vision_indoor"', "part": '"back"'},
        "body": "union() { back(); translate([0, 0, base_d]) front(); }",
        "seams": "[]",   # the piston plate: no seam crosses the side profile (see the Sense)
        "placement": "vision_fitcheck: front at z = base_d (the plate's front face at floor_t, inside the walls)",
    },
    "device.canary-sense": {
        "scad": "canary_sense_enclosure.scad",
        "overrides": {"part": '"back"'},
        "body": "union() { back(); translate([0, 0, base_d]) front(); }",
        # the piston plate: the shell's walls run to the back face, the only
        # seam is the hairline around the plate ON that face — nothing crosses
        # the side profile, so the figure draws one band
        "seams": "[]",
        "placement": "sense_fitcheck: front at z = base_d (the plate's front face at floor_t, inside the walls)",
    },
    "device.canary-vision-doorbell": {
        # Mounted as it hangs: the body's blind keyhole pockets seat on the
        # plate's T-studs, back face flush on the plate front (plate_t); the
        # face rides the body exactly as doorbell_fitcheck places it.
        "scad": "canary_vision_doorbell.scad",
        "overrides": {"part": '"plate"'},
        # body() is the piston plate: its front face at z = floor_t, its back
        # face at z = -mount_extra (the pocket slab). Landing that back face
        # flush on the wall plate's front (z = wplate_t) is a lift of
        # wplate_t + mount_extra; the wall plate's T-studs bury in the pockets.
        # The hang is a slide, not a snap: the body is offered 7.5 higher
        # (pass holes over the stud heads) and dropped until it RESTS ON THE
        # WALL PLATE'S L-FOOT, whose top sits 0.5 above the wall plate's
        # bottom edge, so the resting body + face ride 0.5 up the mounting
        # axis (+y) — the assembled envelope's extra half-millimeter of height.
        "body": ("union() { plate(); translate([0, 0.5, wplate_t + mount_extra]) "
                 "{ body(); translate([0, 0, base_d]) face(); } }"),
        # visible bands from the wall out: the wall plate to wplate_t, then the
        # face's walls run all the way to the back face — the piston plate is
        # inside them, so no other seam crosses the side profile
        "seams": "[wplate_t]",
        "placement": ("doorbell_fitcheck: face (the shell) at z = base_d; body (the plate) inside its "
                      "walls at floor_t, back flush on the wall plate front (wplate_t), then the 0.5 slide "
                      "down onto the foot"),
    },
    "device.canary-display-watch": {
        # The Watch Station has no committed STLs (in development — dev_*.stl
        # is gitignored) and no fit-check module, but it does not need one to
        # be measured: the seat is stated by its own geometry. bezel() is
        # drawn in the SEATED frame — face plate z = 0..bez_t, skirt reaching
        # -skirt_dep into the bore — and its snap nubs are placed at bezel
        # z = -snap_depth precisely because "the face underside (bezel z=0)
        # rests on the drum rim (drum z=drum_h), so drum_z = drum_h +
        # bezel_z" (the nub comment in bezel()). Any other seat and the nubs
        # miss the drum's windows, so this is the only placement the snap
        # admits. Drum + bezel only: the puck as it hangs on the wall or sits
        # in the cradle — the stand is its own part, not the device's
        # envelope. This is what lets a manifest edit (disc_d, a registry
        # reference) move the published figure: the ledger re-measures the
        # case, where a typed sketch envelope silently would not.
        "scad": "canary_watch_station.scad",
        "overrides": {"part": '"drum"'},
        "body": "union() { drum(); translate([0, 0, drum_h]) bezel(); }",
        # visible bands from the back cap out: drum to its rim, bezel face beyond
        "seams": "[drum_h]",
        # the face aperture the glass shows through, centered on the drum axis
        # (bezel() cuts it as cylinder(d = bez_ap_d) at the origin)
        "face": "[bez_ap_d, bez_ap_d]",
        "placement": ("bezel() seated frame: face underside on the drum rim, bezel at z = drum_h "
                      "(the nubs' own datum, drum_z = drum_h + bezel_z)"),
    },
    "device.canary-display-dash": {
        # The Dash case (in development, no committed STLs, no fit-check
        # module), measured where its own file states the stack: the frame
        # is modeled face at z = 0 with its rear rim at frame_h, the back
        # with its OUTER (wall) face at z = 0 and its dock pads standing
        # cr_pad_h() below that ("the shadow gap the case floats off the
        # wall", canary_cradle_lib), and the file derives the assembled
        # thickness as total_t = frame_h + back_t — the number its echo
        # prints and its stand's channel is cut to; the M2 screws run from
        # the back's outer counterbores into the frame's rim lobes. So: the
        # back as modeled, the frame turned face-out (a rotation about Y, so
        # +Y stays up — the USB wall stays at the bottom) with its rim on
        # the back's inner face. The pads are in the envelope: they are how
        # far the case stands off the wall on its cradle. This replaces the
        # registry's hand-typed body_mm (113.7 x 73.6 x 16.0, which had lost
        # the corner screw lobes, the thicker back and the pads) and the
        # figure's vendor-board envelope (the Waveshare board, not the case).
        "scad": "canary_dash_display.scad",
        "overrides": {"part": '"back"'},
        "body": ("union() { back(); "
                 "translate([0, 0, back_t + frame_h]) rotate([0, 180, 0]) frame(); }"),
        # the back's frame puts z = 0 at its outer face, so the union's back
        # plane is the pad tips at -cr_pad_h(): the seams are measured from
        # there — the pad band, then the back plate, then the frame out to
        # the face
        "seams": "[cr_pad_h(), cr_pad_h() + back_t]",
        # the view window the bezel lip frames (view_l/view_w = panel less
        # 2 * bez_lip), cut as rrect2d(view_l, view_w) at the frame's origin;
        # the outline and its four corner lobes are symmetric about the same
        # origin, so the window is centered on the envelope
        "face": "[view_l, view_w]",
        "placement": ("total_t = frame_h + back_t: back as modeled (dock pads on its wall face), "
                      "frame turned face-out with its rim on the back's inner face"),
    },
    "device.canary-combo": {
        # The radar + camera Combo witness (in development, v0.1-dev: no
        # committed STLs, no fit-check module), measured where its own file
        # states the seat. back() and front() are both drawn in the ASSEMBLED
        # frame — front() is a plate at z = 0..lid_t with its nesting lip,
        # pan-head pads and camera posts hanging below z = 0 (the file flips
        # it only to print it) — and three of the file's own datums put that
        # z = 0 on the back's rim at z = base_d: the echo's thickness is
        # base_d + lid_t + mount_extra; the corner posts stop at
        # base_d - head_pad precisely because "the front carries a pad under
        # each head and the posts shorten by the same" (the pad hangs
        # head_pad below the plate); and the lid-key rib tops out at base_d
        # where the lip's slot starts. So the front rides at z = base_d, the
        # Vision case's own placement. The back carries its keyhole
        # thickening (mount_extra) below z = 0, so the union's back plane is
        # there and the seam is measured from it.
        "scad": "canary_combo.scad",
        "overrides": {"part": '"back"'},
        "body": "union() { back(); translate([0, 0, base_d]) front(); }",
        # visible bands from the wall out: the back (keyhole thickening
        # included) to its rim, the front plate beyond
        "seams": "[]",   # the piston plate: no seam crosses the side profile (the plate is inside the shell)
        # what the massing draws on the face, read from the variables front()
        # cuts at (echoed, not measured off the cut): the
        # lens aperture (cylinder(d = cam_ap_d) at lens_x, lens_y) on the
        # Vision column and the radome window (rrect2d(rad_win_x, rad_win_y)
        # at rad_cx, rad_cy) on the Sense column — the two features that
        # make this case the Combo
        "features": {
            "lens": "[lens_x, lens_y, cam_ap_d, cam_ap_d]",
            "radome": "[rad_cx, rad_cy, rad_win_x, rad_win_y]",
        },
        "placement": ("front() at z = base_d: the echo's base_d + lid_t + mount_extra, the posts "
                      "shortened by the head pads they carry, the lid-key rib to the rim"),
    },
}


def measure(fig_id, spec):
    # The shared probe (scad_probe.py): include the case beside the case files,
    # apply the overrides after it, draw the union, echo the seams (and the
    # face aperture, where the row names one) — and refuse a dirty render
    # rather than measure it.
    face_echo = "\necho(\"FACE\", {face});".format(face=spec["face"]) if "face" in spec else ""
    features = spec.get("features", {})
    feature_echo = "".join(f"\necho(\"FEATURE_{name}\", {expr});" for name, expr in features.items())
    try:
        res = scad_probe.probe(
            f"assembled_{fig_id}", spec["scad"], spec["overrides"],
            "{body}\necho(\"SEAMS\", {seams});{face}{features}".format(
                body=spec["body"], seams=spec["seams"], face=face_echo, features=feature_echo),
            root=HERE,
        )
        seams = scad_probe.echo_numbers(res, "SEAMS", fig_id)
        face = scad_probe.echo_numbers(res, "FACE", fig_id) if "face" in spec else None
        marks = {name: scad_probe.echo_numbers(res, f"FEATURE_{name}", fig_id) for name in features}
    except scad_probe.ProbeError as e:
        sys.exit(f"gen_assembled_dims: {e}")
    if face is not None and len(face) != 2:
        sys.exit(f"gen_assembled_dims: {fig_id} face must echo [w, h], got {face}")
    for name, got in marks.items():
        if len(got) != 4:
            sys.exit(f"gen_assembled_dims: {fig_id} feature {name!r} must echo [cx, cy, w, h], got {got}")
    x, y, z = res.bbox
    # scad frame -> figure frame (the massing's 'scad-wall'): w = x, h = y, d = z
    extra = {}
    if face is not None:
        # the aperture on the outer face (scad x, y -> figure w, h), centered
        # on the envelope: what the massing draws the glass in, so a panel
        # or bezel-lip edit moves the drawn window as well as the outline
        extra["face_fig_mm"] = {"w": face[0], "h": face[1]}
    if marks:
        # each feature's center on the outer face (scad x, y -> figure x, z),
        # from the measured envelope's min corner (res.lo), and its extent
        lo_x, lo_y = res.lo[0], res.lo[1]
        extra["features_fig_mm"] = {
            name: {"x": round(cx - lo_x, 3), "z": round(cy - lo_y, 3), "w": w, "h": h}
            for name, (cx, cy, w, h) in sorted(marks.items())
        }
        # A feature this file cannot place on the face is refused, never
        # written: a non-finite number (JSON cannot spell it and --check could
        # never call it equal), a zero or negative extent, or a mark that does
        # not lie on the measured face — an expression in the wrong frame, or
        # the wrong variable, lands exactly there.
        for name, f in extra["features_fig_mm"].items():
            if not all(math.isfinite(v) for v in f.values()) or f["w"] <= 0 or f["h"] <= 0:
                sys.exit(f"gen_assembled_dims: {fig_id} feature {name!r} is not a mark on a face: {f}")
            if (f["x"] - f["w"] / 2 < -TOL or f["x"] + f["w"] / 2 > x + TOL
                    or f["z"] - f["h"] / 2 < -TOL or f["z"] + f["h"] / 2 > y + TOL):
                sys.exit(f"gen_assembled_dims: {fig_id} feature {name!r} {f} does not lie on the "
                         f"measured {x} x {y} face — check its expression's frame")
    return {
        "scad": spec["scad"],
        "overrides": {k: v.strip('"') for k, v in spec["overrides"].items()},
        "placement": spec["placement"],
        "mm_scad": [x, y, z],
        "fig": {"w": x, "d": z, "h": y},
        # part-to-part transitions along the assembled depth (fig d), from the
        # case's own datums: the massing draws each part's VISIBLE band
        # between consecutive seams, so the drawn stack nests as built
        "seams_fig_d": seams,
        **extra,
    }


def _numbers_moved(fresh: dict, got, keys: tuple[str, ...]) -> bool:
    """True unless `got` is a dict of exactly `keys`, each a number within TOL
    of `fresh`'s. A committed value this cannot read as a number (a string, a
    bool, NaN, the wrong shape) is never "equal"."""
    if not isinstance(got, dict) or set(got) != set(keys):
        return True
    return any(not isinstance(got[k], (int, float)) or isinstance(got[k], bool)
               or not abs(fresh[k] - got[k]) <= TOL for k in keys)


def face_moved(fresh, got) -> bool:
    """True unless the committed face aperture is the measured one (to TOL) —
    or both are absent. A committed value this cannot read as a number is
    never "equal"."""
    if fresh is None or got is None:
        return (fresh is None) != (got is None)
    return _numbers_moved(fresh, got, ("w", "h"))


def features_moved(fresh, got) -> bool:
    """True unless the committed face features are the measured ones — the
    same names, each center and extent within TOL — or both are absent. Same
    refusal as face_moved: nothing it cannot read as the measurement passes."""
    if fresh is None or got is None:
        return (fresh is None) != (got is None)
    if not isinstance(got, dict) or set(got) != set(fresh):
        return True
    return any(_numbers_moved(fresh[name], got[name], ("x", "z", "w", "h")) for name in fresh)


def build():
    return {
        "generated_by": "docs/hardware/enclosure/gen_assembled_dims.py",
        "note": ("Assembled outer envelopes, measured off the union of each "
                 "device's parts rendered from its case source in their "
                 "assembled positions (the fit-checked ones where the case has "
                 "a fit check; `placement` says which). gen_figures.mjs reads "
                 "these for multi-part device figures instead of stacking part "
                 "depths, which overstates any nesting assembly, and for "
                 "in-development figures declared `assembled` instead of a "
                 "typed sketch. Regenerate after any edit to the cases these "
                 "unions include."),
        "devices": {fig_id: measure(fig_id, spec) for fig_id, spec in sorted(DEVICES.items())},
    }


def main():
    fresh = build()
    text = json.dumps(fresh, indent=1, ensure_ascii=False) + "\n"
    if "--check" in sys.argv:
        if not OUT.exists():
            sys.exit("gen_assembled_dims: assembled_dims.json is missing — run the generator")
        have = json.loads(OUT.read_text())
        for fig_id, spec in fresh["devices"].items():
            got = have.get("devices", {}).get(fig_id)
            if not got:
                sys.exit(f"gen_assembled_dims: {fig_id} missing from assembled_dims.json — regenerate")
            for a, (m, n) in enumerate(zip(spec["mm_scad"], got.get("mm_scad", [0, 0, 0]))):
                if abs(m - n) > TOL:
                    sys.exit(
                        f"gen_assembled_dims: {fig_id} axis {a}: measured {m} vs committed {n} "
                        "— the CAD moved (a case edit or an STL re-export); regenerate and re-run "
                        "gen_figures.mjs"
                    )
            # The seams are consumed data too (the massings draw each part's
            # visible band between them), and a datum like base_d can move
            # WITHOUT moving the union's outer box — so a bbox-only check
            # would bless stale bands. Compare them with the same tolerance.
            fresh_seams = spec["seams_fig_d"]
            got_seams = got.get("seams_fig_d", [])
            if len(fresh_seams) != len(got_seams) or any(
                abs(m - n) > TOL for m, n in zip(fresh_seams, got_seams)
            ):
                sys.exit(
                    f"gen_assembled_dims: {fig_id} seams: measured {fresh_seams} vs committed "
                    f"{got_seams} — an assembly datum moved; regenerate and re-run gen_figures.mjs"
                )
            # And everything else in the record (fig frame, placement prose,
            # overrides) must match what this generator would write, so the
            # committed file can never describe a different assembly than the
            # one measured.
            # The face aperture is consumed the same way (the massing draws
            # the glass in it) — present exactly where the row names one.
            fresh_face, got_face = spec.get("face_fig_mm"), got.get("face_fig_mm")
            if face_moved(fresh_face, got_face):
                sys.exit(
                    f"gen_assembled_dims: {fig_id} face: measured {fresh_face} vs committed "
                    f"{got_face} — the face aperture moved; regenerate and re-run gen_figures.mjs"
                )
            # ...and so are the off-center face features (the massing draws
            # the Combo's lens and radome window at them)
            fresh_feat, got_feat = spec.get("features_fig_mm"), got.get("features_fig_mm")
            if features_moved(fresh_feat, got_feat):
                sys.exit(
                    f"gen_assembled_dims: {fig_id} features: measured {fresh_feat} vs committed "
                    f"{got_feat} — a face feature moved; regenerate and re-run gen_figures.mjs"
                )
            for key in ("scad", "overrides", "placement", "fig"):
                if spec[key] != got.get(key):
                    sys.exit(
                        f"gen_assembled_dims: {fig_id} field '{key}' drifted from the committed "
                        "record — regenerate"
                    )
        stray = set(have.get("devices", {})) - set(fresh["devices"])
        if stray:
            sys.exit(f"gen_assembled_dims: stray committed rows: {sorted(stray)}")
        print(f"assembled_dims.json OK ({len(fresh['devices'])} devices)")
    else:
        OUT.write_text(text)
        for fig_id, spec in fresh["devices"].items():
            w, h, d = spec["fig"]["w"], spec["fig"]["h"], spec["fig"]["d"]
            print(f"  {fig_id}: {w} x {h} x {d} mm assembled")
        print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
