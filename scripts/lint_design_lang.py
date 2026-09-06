#!/usr/bin/env python3
"""lint_design_lang.py — the design-language conformance gate.

canary_core_lib.scad states the doctrine: the house look is a small set of
constants (`core_corner_r()`, `core_face_edge()`, the feature vocabulary),
and "a departure now has to type its own number AND say why." This lint is
that sentence as a build gate.

A case file's Customizer knob stays a LITERAL on purpose — a computed value
would vanish from the Customizer and from the website builder's manifest —
so the canon cannot be enforced by making cases read the functions. Instead:
every canonical parameter's default must either

  * equal the house value, or
  * carry `deviates:` in a comment on its own line, followed by the reason.

An unexplained outlier fails the build. That keeps the ledger of deviations
IN the files, next to the numbers, where the Customizer help and the next
editor will see it — not in a sidecar list that rots.

Second rule: a module the libraries own may not be redefined in a case file.
Four copies of the vent cluster had already forked once (hole size drifted on
two outdoor cases); the de-fork stays de-forked.

Run from the repo root:  python3 scripts/lint_design_lang.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENC = ROOT / "docs" / "hardware" / "enclosure"

# The canon: parameter names (both naming dialects) -> the house value and
# the core/mount function that states it. Keep this in step with the
# functions themselves — the self-check below parses the libs and fails if
# this table and the libraries disagree.
CANON = {
    "corner_r":       (3.0,  "canary_core_lib.scad",  "core_corner_r"),
    "r_out":          (3.0,  "canary_core_lib.scad",  "core_corner_r"),
    "lid_edge":       (0.8,  "canary_core_lib.scad",  "core_face_edge"),
    "lid_edge2":      (0.8,  "canary_core_lib.scad",  "core_face_edge2"),
    "foot_cham":      (0.5,  "canary_core_lib.scad",  "core_foot_cham"),
    "wall_t":         (2.0,  "canary_core_lib.scad",  "core_wall"),
    "wall":           (2.0,  "canary_core_lib.scad",  "core_wall"),
    "lp_d":           (3.0,  "canary_core_lib.scad",  "core_lightpipe_d"),
    "vent_pad_d":     (12.0, "canary_core_lib.scad",  "core_vent_pad_d"),
    "vent_pad_depth": (0.8,  "canary_core_lib.scad",  "core_vent_pad_depth"),
    "vent_ring_d":    (6.0,  "canary_core_lib.scad",  "core_vent_ring_d"),
    "vent_hole_d":    (1.0,  "canary_core_lib.scad",  "core_vent_hole_d"),
    "vent_holes":     (10,   "canary_core_lib.scad",  "core_vent_holes"),
    "tol_slide":      (0.20, "canary_core_lib.scad",  "core_tol_slide"),
    "tol_press":      (0.10, "canary_core_lib.scad",  "core_tol_press"),
    "tol_hole":       (0.30, "canary_core_lib.scad",  "core_tol_hole"),
    "kh_head_d":      (7.0,  "canary_mount_lib.scad", "mount_kh_head_d"),
}

# Modules the libraries own. A case redefining one is a fork by definition.
LIB_OWNED = [
    "vent_cluster", "core_vent_cluster", "core_lightpipe_bore",
    "soft_edge_plate", "foot_chamfer_ring", "seam_reveal_cut",
    "inner_cove_ring", "rrect", "rrect2d", "cs_cone90_cut",
    "cb_flat_cut", "cb_head_pad", "mount_keyhole_pocket", "mount_tstud",
    "port_bridge_profile2d", "port_usbc_stadium2d", "boss_tower", "wall_rib",
]

# Library files (define, never lint), and files that are not cases.
SKIP = {
    "canary_core_lib.scad", "canary_port_lib.scad", "canary_mount_lib.scad",
    "canary_snap_lib.scad", "canary_mark_lib.scad", "canary_board_lib.scad",
    "canary_color_lib.scad", "canary_vent_lib.scad", "canary_rib_lib.scad",
    "canary_panel_lib.scad", "canary_cradle_lib.scad",
    "canary_case_fitcheck.scad", "canary_s3_lcd7_fitcheck.scad",
    "canary_templates_2d.scad",
}

# A top-level assignment line, and then EVERY `name = value;` on it —
# Customizer defaults are frequently grouped (`wall_t = 2.2;  floor_t = 2.2;
# lid_t = 2.4;`), and an anchored single match would leave every value after
# the first outside the gate.
ASSIGN_LINE = re.compile(r"^[a-z_][a-z0-9_]*\s*=")
ASSIGN_EACH = re.compile(r"(?:^|(?<=;))\s*(?P<name>[a-z_][a-z0-9_]*)\s*=\s*(?P<val>-?\d+(?:\.\d+)?)\s*;")
# A deviation must SAY WHY: `deviates:` followed by at least two words. A
# bare marker is an outlier wearing a costume, not a declared decision.
DEVIATES = re.compile(r"deviates:\s*\S+\s+\S+")
FUNC = re.compile(r"function\s+(?P<fn>[a-z_][a-z0-9_]*)\(\)\s*=\s*(?P<val>-?\d+(?:\.\d+)?)\s*;")


def selfcheck():
    """The CANON table must agree with the libraries it quotes."""
    bad = []
    lib_vals = {}
    for lib in {lib for _, lib, _ in CANON.values()}:
        text = (ENC / lib).read_text()
        for m in FUNC.finditer(text):
            lib_vals[(lib, m.group("fn"))] = float(m.group("val"))
    for name, (want, lib, fn) in CANON.items():
        got = lib_vals.get((lib, fn))
        if got is None:
            bad.append(f"CANON['{name}'] quotes {lib}:{fn}() which does not exist")
        elif abs(got - float(want)) > 1e-9:
            bad.append(f"CANON['{name}'] says {want} but {lib}:{fn}() = {got} — "
                       "update the table (it mirrors the libs, it does not rule them)")
    return bad


def lint_file(path):
    problems = []
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        code = line.split("//", 1)[0]
        if ASSIGN_LINE.match(code):
            for m in ASSIGN_EACH.finditer(code):
                if m.group("name") not in CANON:
                    continue
                want, lib, fn = CANON[m.group("name")]
                got = float(m.group("val"))
                if abs(got - float(want)) > 1e-9 and not DEVIATES.search(line):
                    problems.append(
                        f"{path.name}:{lineno}: {m.group('name')} = {m.group('val')} "
                        f"but the house value is {want} ({lib}:{fn}()) — conform it, "
                        "or say why with a `deviates: <reason>` comment on the line "
                        "(a bare marker with no reason does not count)")
        mm = re.match(r"\s*module\s+([a-z_][a-z0-9_]*)\s*\(", line)
        if mm and mm.group(1) in LIB_OWNED and not DEVIATES.search(line):
            problems.append(
                f"{path.name}:{lineno}: redefines library module "
                f"'{mm.group(1)}' — a copy is a fork; call the library's, "
                "or say why with a `deviates: <reason>` comment on the line")
    return problems


def main():
    problems = selfcheck()
    for path in sorted(ENC.glob("canary_*.scad")):
        if path.name in SKIP:
            continue
        problems += lint_file(path)
    if problems:
        for p in problems:
            print(f"::error::design language: {p}")
        print(f"\ndesign language: {len(problems)} problem(s)")
        return 1
    print("design language OK — every canonical default conforms or explains itself")
    return 0


if __name__ == "__main__":
    sys.exit(main())
