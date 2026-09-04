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
placement (crib it from that case's fitcheck module — never invent one) and
rerun. gen_figures.mjs refuses a `parts:` device figure that has no row
here, so a new multi-part figure cannot fall back to the stacked lie.
"""

import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
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
        "seams": "[base_h]",
        "placement": "wap_fitcheck: lid at z = base_h",
    },
    "device.canary-vision": {
        "scad": "canary_vision_enclosure.scad",
        "overrides": {"host": '"xiao"', "preset": '"vision_indoor"', "part": '"back"'},
        "body": "union() { back(); translate([0, 0, base_d]) front(); }",
        "seams": "[base_d]",
        "placement": "vision_fitcheck: front at z = base_d",
    },
    "device.canary-vision-devkit": {
        "scad": "canary_vision_enclosure.scad",
        "overrides": {"host": '"devkit"', "preset": '"vision_indoor"', "part": '"back"'},
        "body": "union() { back(); translate([0, 0, base_d]) front(); }",
        "seams": "[base_d]",
        "placement": "vision_fitcheck: front at z = base_d",
    },
    "device.canary-sense": {
        "scad": "canary_sense_enclosure.scad",
        "overrides": {"part": '"back"'},
        "body": "union() { back(); translate([0, 0, base_d]) front(); }",
        "seams": "[base_d]",
        "placement": "sense_fitcheck: front at z = base_d",
    },
    "device.canary-vision-doorbell": {
        # Mounted as it hangs: the body's blind keyhole pockets seat on the
        # plate's T-studs, back face flush on the plate front (plate_t); the
        # face rides the body exactly as doorbell_fitcheck places it.
        "scad": "canary_vision_doorbell.scad",
        "overrides": {"part": '"plate"'},
        # body() carries its keyhole-pocket thickening at z = -kh_extra, so
        # landing that back face flush on the plate front (z = plate_t) is a
        # lift of plate_t + kh_extra; the plate's T-studs bury in the pockets.
        "body": ("union() { plate(); translate([0, 0, plate_t + kh_extra]) "
                 "{ body(); translate([0, 0, base_d]) face(); } }"),
        # visible bands from the wall out: plate to plate_t (the studs bury in
        # the body's pockets), body to its front rim, face to the outer plane
        "seams": "[plate_t, plate_t + kh_extra + base_d]",
        "placement": "doorbell_fitcheck: face at z = base_d; body back flush on plate front (T-studs in pockets)",
    },
}


def stl_bbox(path):
    raw = path.read_bytes()
    (n,) = struct.unpack_from("<I", raw, 80)
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    off = 84
    for _ in range(n):
        # 12 floats: normal + 3 vertices; then a u16 attribute
        vals = struct.unpack_from("<12f", raw, off)
        for v in range(3):
            for a in range(3):
                c = vals[3 + v * 3 + a]
                if c < lo[a]:
                    lo[a] = c
                if c > hi[a]:
                    hi[a] = c
        off += 50
    return [round(hi[a] - lo[a], 3) for a in range(3)]


def measure(fig_id, spec):
    probe = "include <{scad}>\n{ov}\n{body}\necho(\"SEAMS\", {seams});\n".format(
        scad=spec["scad"],
        ov="\n".join(f"{k} = {v};" for k, v in spec["overrides"].items()),
        body=spec["body"],
        seams=spec["seams"],
    )
    # The probe must sit BESIDE the case files: OpenSCAD resolves `include`
    # relative to the including file, and the cases include the shared libs
    # the same way.
    with tempfile.TemporaryDirectory() as td:
        src = HERE / f".tmp_assembled_{fig_id.replace('.', '_')}.scad"
        out = Path(td) / "probe.stl"
        src.write_text(probe)
        try:
            r = subprocess.run(
                ["openscad", "--export-format", "binstl", "-o", str(out), str(src)],
                cwd=HERE, capture_output=True, text=True,
            )
        finally:
            src.unlink(missing_ok=True)
        diag = (r.stdout or "") + (r.stderr or "")
        if "ERROR" in diag or "WARNING" in diag or not out.exists():
            sys.exit(f"gen_assembled_dims: {fig_id}: dirty render, nothing measured\n{diag}")
        m = __import__("re").search(r'ECHO: "SEAMS", \[([0-9., ]+)\]', diag)
        if not m:
            sys.exit(f"gen_assembled_dims: {fig_id}: seam echo missing\n{diag}")
        seams = [round(float(v), 3) for v in m.group(1).split(",")]
        x, y, z = stl_bbox(out)
    # scad frame -> figure frame (the massing's 'scad-wall'): w = x, h = y, d = z
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
    }


def build():
    return {
        "generated_by": "docs/hardware/enclosure/gen_assembled_dims.py",
        "note": ("Assembled outer envelopes, measured off the union of each "
                 "device's committed parts in their fit-checked assembled "
                 "positions. gen_figures.mjs reads these for multi-part device "
                 "figures instead of stacking part depths, which overstates "
                 "any nesting assembly. Regenerate after re-exporting any STL "
                 "these unions include."),
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
                        "— an STL moved; regenerate and re-run gen_figures.mjs"
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
