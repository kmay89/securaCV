#!/usr/bin/env python3
"""gen_templates.py — the 1:1 paper drill templates, rendered and held to the parts they drill for.

    python3 docs/hardware/enclosure/gen_templates.py           # render template_*.svg from canary_templates_2d.scad
    python3 docs/hardware/enclosure/gen_templates.py --check   # CI: committed SVGs current, mirrored numbers true

A paper template is a promise about a part that is not on the paper: tape it
to the wall, drill where it says, and the plate's screws land in the holes.
Two things can break that promise, and both happened:

  1. THE SVG IS OLDER THAN ITS SOURCE. render.sh writes template_*.svg, but
     nothing compared the committed files with a fresh render, so when the
     doorbell grew 50 mm in v0.7 the committed doorbell template stayed the
     v0.6 drawing (82 x 135 mm against a 172.7 mm plate) for three weeks with
     every gate green. --check renders each template and compares bytes:
     OpenSCAD's 2D SVG export is deterministic for one version (2021.01,
     what CI installs), unlike its STL bytes.
  2. THE TEMPLATE'S NUMBERS ARE TYPED COPIES. canary_templates_2d.scad
     cannot `use` a case's variables (use imports modules and functions
     only), so its [Doorbell] and [Bracket] knobs restate the parts' own.
     --check asks OpenSCAD what each part says (an echo through scad_probe,
     no CGAL) and fails on any knob that stopped equaling its source.

Run --check before render.sh in CI: render.sh rewrites the SVGs in the
checkout, after which a byte compare would always pass.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import scad_probe  # noqa: E402
from gen_builder_manifest import parse_scad  # noqa: E402

SRC = "canary_templates_2d.scad"
# mode -> committed file (render.sh's own list: svg template_<mode>.svg -D mode="<mode>")
MODES = {"studs": "template_studs.svg", "bracket": "template_bracket.svg",
         "doorbell": "template_doorbell.svg"}
# template knob -> (source case, the expression that says the same thing there, -D overrides)
MIRRORS = {
    "db_out_x": ("canary_vision_doorbell.scad", "out_x", {"part": '"plate"'}),
    "db_out_y": ("canary_vision_doorbell.scad", "out_y", {"part": '"plate"'}),
    "db_r": ("canary_vision_doorbell.scad", "rr", {"part": '"plate"'}),
    "db_screw_d": ("canary_vision_doorbell.scad", "plate_screw_d", {"part": '"plate"'}),
    "db_screw_inset_x": ("canary_vision_doorbell.scad", "plate_screw_inset()[0]", {"part": '"plate"'}),
    "db_screw_inset_y": ("canary_vision_doorbell.scad", "plate_screw_inset()[1]", {"part": '"plate"'}),
    "db_exit_dx": ("canary_vision_doorbell.scad", "exit_cx", {"part": '"plate"'}),
    "db_exit_cy": ("canary_vision_doorbell.scad", "exit_cy", {"part": '"plate"'}),
    "db_exit_w": ("canary_vision_doorbell.scad", "usb_exit_w", {"part": '"plate"'}),
    "db_exit_h": ("canary_vision_doorbell.scad", "usb_exit_h", {"part": '"plate"'}),
    "db_exit_slide": ("canary_vision_doorbell.scad", "mount_dt_travel()", {"part": '"plate"'}),
    "br_x": ("canary_vision_enclosure.scad", "br_x", {"part": '"bracket"'}),
    "br_y": ("canary_vision_enclosure.scad", "br_y", {"part": '"bracket"'}),
    "br_screw_d": ("canary_vision_enclosure.scad", "br_screw_d", {"part": '"bracket"'}),
}
TOL = 1e-6


def render_svg(mode: str, out: Path) -> None:
    """One template to SVG, judged by scad_probe's clean-render rule (an
    ERROR or WARNING in the log means nothing was drawn). Not through
    scad_probe.render, which measures an STL's box after every export."""
    cmd = [scad_probe.OPENSCAD, "-o", str(out), "-D", f'mode="{mode}"', str(HERE / SRC)]
    out.unlink(missing_ok=True)
    try:
        r = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
    except OSError as err:
        raise scad_probe.ProbeError(f"template {mode}: OpenSCAD is not runnable ({err.strerror})") from None
    diag = (r.stdout or "") + (r.stderr or "")
    if scad_probe.DIRTY.search(diag) or not out.exists():
        raise scad_probe.ProbeError(f"template {mode}: dirty render, nothing drawn\n{diag}")


def template_knobs() -> dict:
    return {p["name"]: p["default"] for g in parse_scad(HERE / SRC) for p in g["params"]}


def mirror_problems() -> list[str]:
    knobs = template_knobs()
    bad = []
    by_src: dict[tuple, list[str]] = {}
    for knob, (scad, expr, ov) in MIRRORS.items():
        if knob not in knobs:
            bad.append(f"{SRC}: no literal knob {knob} — MIRRORS names it")
            continue
        by_src.setdefault((scad, tuple(sorted(ov.items()))), []).append(knob)
    for (scad, ov), names in by_src.items():
        body = "\n".join(f'echo("M_{n}", {MIRRORS[n][1]});' for n in names)
        try:
            r = scad_probe.probe(f"tpl_{Path(scad).stem}", scad, dict(ov), body, export="echo")
        except scad_probe.ProbeError as e:
            bad.append(str(e).splitlines()[0])
            continue
        for n in names:
            try:
                (val,) = scad_probe.echo_numbers(r, f"M_{n}", n)
            except (scad_probe.ProbeError, ValueError) as e:
                bad.append(f"{n}: {e}")
                continue
            if abs(val - float(knobs[n])) > TOL:
                bad.append(f"{SRC}: {n} = {knobs[n]}, but {scad} says {MIRRORS[n][1]} = {val} "
                           f"— the paper would drill for a part that no longer exists; update the knob, then "
                           f"python3 docs/hardware/enclosure/gen_templates.py")
    return bad


def check() -> list[str]:
    bad = mirror_problems()
    with tempfile.TemporaryDirectory() as td:
        for mode, name in MODES.items():
            fresh = Path(td) / name
            try:
                render_svg(mode, fresh)
            except scad_probe.ProbeError as e:
                bad.append(str(e).splitlines()[0])
                continue
            have = HERE / name
            if not have.is_file():
                bad.append(f"{name}: missing — run gen_templates.py")
            elif have.read_bytes() != fresh.read_bytes():
                bad.append(f"{name}: not what {SRC} renders today — the committed drawing is stale; "
                           f"run python3 docs/hardware/enclosure/gen_templates.py and commit it")
    return bad


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="verify; write nothing")
    args = ap.parse_args(argv)
    if args.check:
        bad = check()
        for b in bad:
            print(f"::error::{b}")
        if bad:
            return 1
        print(f"templates OK: {len(MODES)} SVGs are what {SRC} renders, {len(MIRRORS)} mirrored numbers "
              f"equal the parts they copy")
        return 0
    bad = mirror_problems()
    for b in bad:
        print(f"::error::{b}")
    if bad:
        return 1
    for mode, name in MODES.items():
        render_svg(mode, HERE / name)
        print(f"wrote {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
