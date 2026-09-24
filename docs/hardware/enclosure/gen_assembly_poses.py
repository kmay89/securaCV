#!/usr/bin/env python3
"""gen_assembly_poses.py — where every part SITS in the Lab's Assemble tab,
read off the CAD instead of typed.

    python3 gen_assembly_poses.py            # rewrite the seated poses in
                                             #   canary-local/devices/assembly.json
    python3 gen_assembly_poses.py --check    # CI gate: re-derive, diff

WHY THIS FILE EXISTS. The Assemble tab (canary-local/assets/assembly-lab.js)
puts the real printed STLs together, and every `seated` transform in
assembly.json was typed by hand against whatever revision of the case was
current that day. The cases moved and the numbers did not. Measured by
assembling the Lab's own transforms and intersecting the meshes (the fit
check's recipe, applied to the page instead of the CAD):

    device     typed                        CAD                       result
    WAP        lid at z 17.2               base_h + lid_t = 18.25     lid sunk 1.05 mm: 784 mm³ in the base
    Vision     front at z 22               base_d + lid_t = 23.38     front sunk 1.38 mm: 561 mm³
               screws at ±16.5, ±30.5      post_xy() ±17.3, ±33       2.6 mm off every post
    Sense      front at z 20.5             base_d + lid_t = 21.5      front sunk 1.0 mm: 399 mm³
               screws at ±23, ±22          post_xy() ±24.3, ±24.8     2.8 mm off every post
    Vision,    screws `irot` 180°          driven down through        shanks stood 8 mm OUT of the face
    Sense                                  the front                  with the heads buried in it
    Dash       frame turned about X        turned about Y             USB slot and chimney vents swapped ends

Every one of those parts was a correct, fit-gated mesh; only the page's
placement was wrong, and nothing gated the page. So the seated poses are now
DERIVED here: each is written as the case's own expression (the fit-check
datum, the export transform undone, post_xy(), the board datums), evaluated
by OpenSCAD through scad_probe (echo only — no CGAL, seconds), composed, and
written back into assembly.json. The choreography stays authored — explode
and insert vectors, steps, camera, colors — only WHERE A PART SITS moved
here. A part carries `"pose": "cad"` when its placement comes from this file;
canary-local/tests/assembly.test.js refuses an enclosure STL that does not.

HOW A POSE IS WRITTEN. A pose is a chain of steps applied right to left like
OpenSCAD's own nesting: ("T", "<scad vec3 expr>") translates, ("R", [a,b,c])
rotates X then Y then Z (the page's M.compose order), ("G", "<board id>")
moves a vendor board GLB from its bbox center (where the page puts it) to its
PCB datum. A device may add a `frame` chain in front of every part — the Watch
and the Dash ride their stands. An STL's own chain is always "undo the export
transform, then do the fit check's placement", written out, so a reader can
check it against the two lines of the case file it came from.

Expressions may read the part's own params as P_<name> (the lipo's P_t, the
light pipe's P_len), so a pose that depends on a part's size says so.
"""

from __future__ import annotations

import json
import math
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import scad_probe  # noqa: E402

REPO = HERE.parents[2]
ASM = REPO / "canary-local" / "devices" / "assembly.json"
ND = 3  # decimals written (the page is a viewer; 1 µm is below every tolerance)

# Vendor board GLB datums, raw GLB millimeters (the page re-centers a board
# on its bbox center at load, so a pose has to know where the PCB sits
# relative to that center). Measured off the committed GLB with trimesh, the
# page's own reading (glb.js: node tree, then ×1000):
#   center  — the GLB bbox center the page subtracts
#   datum   — the point the case positions: the PCB outline's center, on the
#             face the case measures from (the XIAO's underside; the round
#             display's back face)
# XIAO S3 Sense: the PCB solid spans x -8.67..12.28 (20.95 ≈ brd_l 21),
# z -15.00..2.78 (17.78, brd_xiao_w_measured), y -0.25..1.00; USB-C at -X,
# components +Y (boards.json `pads`). Round display: PCB y -1.8..-0.2, the
# back-side stack to -6.8 (disp_back 5.0), the trim ring to +3.2.
BOARDS = {
    "seeed_xiao_esp32s3_sense": {"center": [3.208, 6.730, -6.111], "datum": [1.805, -0.250, -6.111]},
    "seeed_round_display_xiao": {"center": [0.000, -1.800, 0.000], "datum": [0.000, -1.800, 0.000]},
}

# (canary-local/tests/assembly.test.js re-measures each `center` off the
# committed GLB with glb.js, and the socket rows' pad islands below, so a
# regenerated board model cannot leave these behind silently.)
# the round display's XIAO socket: the two header rows span raw GLB x
# 0.55..18.9 (pad islands, same mesh), so the XIAO rides centered at 9.725
DISPLAY_SOCKET_X = 9.725


DEVICES = {
    # ---- WAP, battery_weather (the build the Assemble tab shows) ----------
    "canary-wap": {
        "scad": "canary_wap_enclosure.scad",
        "overrides": {"preset": '"battery_weather"', "part": '"base"'},
        "parts": {
            "base": [],
            # the bay is centered on the case axis; the cell lies on the floor
            "batt": [("T", "[batt_cx, 0, floor_t]"), ("R", [0, 0, 180])],
            # PCB underside on the standoffs at pcb_z, USB end at the -X wall
            "board": [("T", "[board_cx, board_cy, pcb_z]"), ("R", [90, 0, 0]),
                      ("G", "seeed_xiao_esp32s3_sense")],
            # the pocket opens downward from the lid's underside (lid z 0 =
            # base_h assembled): the magnet is pressed up to its closed end
            "magnet": [("T", "[board_cx + mag_dx, board_cy + mag_dy, base_h - P_h]")],
            # the pipe's face flush with the lid's outer face
            "lp": [("T", "[board_cx + lp_dx, board_cy + lp_dy, base_h + lid_t - P_len]")],
            "gasket": [("T", "[0, 0, base_h - gasket_groove]")],
            # the disc seat: 0.2 below the lid's outer face (lid(): lid_t - (cam_disc_t + 0.2))
            "disc": [("T", "[board_cx + cam_dx, board_cy + cam_dy, base_h + lid_t - (cam_disc_t + 0.2)]")],
            # wap_fitcheck: lid() at z = base_h; the STL is T(0,0,lid_t)·Rx180·lid()
            "lid": [("T", "[0, 0, base_h + lid_t]"), ("R", [180, 0, 0])],
            # shield(): prints panel-down and installs flipped about X, its
            # tubes standing on the lid's outer face
            "shield": [("T", "[0, 0, base_h + lid_t + sh_t + sh_gap]"), ("R", [180, 0, 0])],
        },
        # flat heads: the head's top (the builder's csk z = 0) is the lid's
        # outer face in face mode, the recess plane in the back (turned over)
        # in back mode — the Outdoor build the tab shows keeps face screws
        "screws": {"xy": "post_xy()",
                   "z": "e_back ? -mount_extra + bk_r : base_h + lid_t", "rot": ["e_back ? 180 : 0", 0, 0],
                   "len": "e_back ? bk_L : hw_len(lid_t, head_pad, hw_engage(screw_size))"},
        "params": {"disc": {"d": "cam_disc_d", "t": "cam_disc_t"},
                   "batt": {"t": "batt_h - 1"}},
    },
    # ---- Vision, xiao host, vision_indoor (the committed STLs it loads) ---
    "canary-vision": {
        "scad": "canary_vision_enclosure.scad",
        "overrides": {"host": '"xiao"', "preset": '"vision_indoor"', "part": '"back"'},
        "parts": {
            "back": [],
            # the Grove module rides the XIAO's socket vm_standoff off the floor
            "board": [("T", "[vm_cx, vm_cy, floor_t + vm_standoff]")],
            # the XIAO hangs stack_sock_h under it, face (USB) down, xiao_below of air
            "xiao": [("T", "[vm_cx, vm_cy - vm_l/2 + xiao_l/2, floor_t + xiao_below + P_t]"),
                     ("R", [180, 0, -90])],
            # the OV5647 board screws to the front's posts: its face sits
            # cam_post_eff under the front plate, lens on the aperture
            "cam": [("T", "[lens_x, lens_y, base_d - cam_post_eff - 2]"), ("R", [180, 0, 0])],
            # the flex runs from the module's top edge to the camera's bottom edge
            "flex": [("T", "[vm_cx, vm_cy + vm_l/2 - 3, floor_t + vm_standoff + pcb_t]")],
            "front": [("T", "[0, 0, base_d + lid_t]"), ("R", [180, 0, 0])],
        },
        # pan heads in cb_flat_cut: the seat floor is head_h under the outer face
        # screw_from "back" (the house default): driven up from the seat in
        # the back, the pan head's bearing face bk_hh above its recess; the
        # builder draws a pan head above z = 0, so it turns over
        "screws": {"xy": "post_xy()",
                   "z": "e_back ? -mount_extra + bk_r + bk_hh(screw_size, screw_head) : base_d + lid_t - head_h",
                   "rot": ["e_back ? 180 : 0", 0, 0],
                   "len": "e_back ? bk_L : hw_len(lid_t, head_pad, hw_engage(screw_size))"},
        "params": {
            "board": {"w": "vm_w", "h": "vm_l", "t": "pcb_t"},
            "xiao": {"w": "xiao_l", "h": "xiao_w"},
            "flex": {"to": "[lens_x - vm_cx, (cam_cy - cam_h/2 + 3) - (vm_cy + vm_l/2 - 3), "
                           "(base_d - cam_post_eff - 2) - (floor_t + vm_standoff + pcb_t)]"},
        },
    },
    # ---- Sense (the committed STLs) ----------------------------------------
    "canary-sense": {
        "scad": "canary_sense_enclosure.scad",
        "overrides": {"part": '"back"'},
        "parts": {
            "back": [],
            "radar": [("T", "[radar_cx, radar_cy, floor_t + radar_standoff]")],
            # the XIAO in the carrier's socket, face (USB) down toward the
            # floor, its USB end at the bottom (-Y) wall's opening
            "xiao": [("T", "[radar_cx, radar_cy - radar_l/2 + xiao_l/2, floor_t + xiao_below + P_t]"),
                     ("R", [180, 0, -90])],
            "front": [("T", "[0, 0, base_d + lid_t]"), ("R", [180, 0, 0])],
        },
        # screw_from "back" (the house default): driven up from the seat in
        # the back, the pan head's bearing face bk_hh above its recess; the
        # builder draws a pan head above z = 0, so it turns over
        "screws": {"xy": "post_xy()",
                   "z": "e_back ? -mount_extra + bk_r + bk_hh(screw_size, screw_head) : base_d + lid_t - head_h",
                   "rot": ["e_back ? 180 : 0", 0, 0],
                   "len": "e_back ? bk_L : hw_len(lid_t, head_pad, hw_engage(screw_size))"},
        "params": {
            "radar": {"w": "radar_w", "h": "radar_l", "t": "pcb_t"},
            "xiao": {"w": "xiao_l", "h": "xiao_w"},
        },
    },
    # ---- Watch Station on its stand ----------------------------------------
    # The stand echoes its own DRUM SEAT: the drum at P0, rotated [slope,0,0]
    # (drum +z along the pocket axis). Everything else rides the drum frame.
    "canary-display-watch": {
        "scad": "canary_watch_station.scad",
        "overrides": {"part": '"drum"'},
        "frame": [("T", "P0"), ("R", ["slope", 0, 0])],
        "parts": {
            "stand": "world",
            "drum": [],
            # the XIAO pins into the display's back socket, whose header rows
            # sit DISPLAY_SOCKET_X off the disc center (raw GLB x 0.55..18.9):
            # face down on its USB shell, PCB top at z_xiao0 + xiao_t, its USB
            # end toward the rim at usb_ang (the builder draws USB at -X, 180°)
            "xiao": [("T", f"[{DISPLAY_SOCKET_X}*cos(usb_ang), {DISPLAY_SOCKET_X}*sin(usb_ang), z_xiao0 + xiao_t]"),
                     ("R", [0, 0, "usb_ang - 180"]), ("R", [180, 0, 0])],
            # the display PCB's back face at z_pcb, glass out along the drum
            # axis, its socket rows (raw +X) spun onto the USB azimuth
            "display": [("T", "[0, 0, z_pcb]"), ("R", [0, 0, "usb_ang"]), ("R", [90, 0, 0]),
                        ("G", "seeed_round_display_xiao")],
            # bezel() is drawn seated at z = drum_h; the STL is bezel_print()
            "bezel": [("T", "[0, 0, drum_h + bez_t]"), ("R", [180, 0, 0])],
        },
        "params": {"xiao": {"t": "1.2"}},
    },
    # ---- Dash on its desk stand --------------------------------------------
    # stand(): the display reclines stand_ang with its back face on the fin
    # plane, which the stand derives to pass through the channel's rear edge
    # (chan_back, stand_t) — so the device's lowest back edge sits there.
    # The device frame is gen_assembled_dims.py's: back() as modeled (outer
    # face z = 0), frame() turned face-out about Y at z = back_t + frame_h.
    "canary-display-dash": {
        "scad": "canary_dash_display.scad",
        "overrides": {"part": '"back"'},
        "frame": [("T", "[0, dash_chan_back, stand_t]"), ("R", ["90 - stand_ang", 0, 0]),
                  ("T", "[0, dash_low_y, 0]")],
        "defs": ("dash_chan_back = -stand_d/2 + 16 + total_t + 2.0;\n"
                 "dash_low_y = inner_w/2 + lob_o + lob_d/2;\n"),
        "parts": {
            "stand": "world",
            "back": [],
            "frame": [("T", "[0, 0, back_t + frame_h]"), ("R", [0, 180, 0])],
            # glass front on the face's inner side, looking out (+z)
            "panel": [("T", "[0, 0, back_t + frame_h - face_t]")],
        },
        # driven from the back's outer counterbores into the frame's lobes:
        # the head's underside on the counterbore floor, shank toward the face
        "screws": {"xy": "bosses()", "z": "cb_h", "rot": [180, 0, 0], "len": None},
        "params": {"panel": {"w": "panel_l", "h": "panel_w", "glass": "glass_t", "stack": "stack_t"}},
    },
}


# ---- 4×4 matrix helpers (row-major lists; the page's compose order) --------

def _eye():
    return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def _mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def _t(v):
    m = _eye()
    m[0][3], m[1][3], m[2][3] = v
    return m


def _rx(d):
    c, s = math.cos(math.radians(d)), math.sin(math.radians(d))
    return [[1, 0, 0, 0], [0, c, -s, 0], [0, s, c, 0], [0, 0, 0, 1]]


def _ry(d):
    c, s = math.cos(math.radians(d)), math.sin(math.radians(d))
    return [[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]]


def _rz(d):
    c, s = math.cos(math.radians(d)), math.sin(math.radians(d))
    return [[c, -s, 0, 0], [s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]


def _rot(r):
    return _mul(_mul(_rx(r[0]), _ry(r[1])), _rz(r[2]))


def _apply(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) + m[i][3] for i in range(3)]


def _decompose(m):
    """pos + [a, b, c] with R = Rx(a)·Ry(b)·Rz(c) — the page's M.compose."""
    b = math.degrees(math.asin(max(-1.0, min(1.0, m[0][2]))))
    if abs(abs(m[0][2]) - 1) < 1e-9:  # gimbal: fold the X/Z pair into a
        a = math.degrees(math.atan2(m[2][1], m[1][1]))
        c = 0.0
    else:
        a = math.degrees(math.atan2(-m[1][2], m[2][2]))
        c = math.degrees(math.atan2(-m[0][1], m[0][0]))
    return [m[0][3], m[1][3], m[2][3]], [a, b, c]


def _num(x):
    r = round(x, ND)
    return 0 if r == 0 else (int(r) if r == int(r) else r)


def _ang(x):
    r = round(x % 360.0, ND) % 360.0
    return _num(r)


# ---- evaluation ------------------------------------------------------------

def _exprs(dev):
    """Every scad expression the device's poses need, tagged for the echo."""
    out = {}

    def take(steps, tag):
        for i, (kind, arg) in enumerate(steps):
            if kind == "T":
                out[f"{tag}_{i}"] = arg
            elif kind == "R":
                for j, a in enumerate(arg):
                    if isinstance(a, str):
                        out[f"{tag}_{i}_{j}"] = a

    take(dev.get("frame", []), "F")
    for pid, steps in dev["parts"].items():
        if isinstance(steps, list):
            take(steps, f"P{pid}")
    for pid, ps in dev.get("params", {}).items():
        for k, e in ps.items():
            out[f"Q{pid}_{k}"] = e
    sc = dev.get("screws")
    if sc:
        out["Sxy"] = sc["xy"]
        out["Sz"] = sc["z"]
        if sc.get("len"):
            out["Slen"] = sc["len"]
        for j, a in enumerate(sc.get("rot") or []):
            if isinstance(a, str):
                out[f"Srot_{j}"] = a
    return out


def _parse(val: str):
    v = val.strip()
    if v in ("true", "false"):
        return v == "true"
    try:
        return json.loads(v)
    except ValueError:
        return None  # undef, a string, a range — nothing a pose can use


def _evaluate(dev_id, dev, asm_parts):
    exprs = _exprs(dev)
    # the parts' own params, readable as P_<name> (a pose that depends on a
    # part's size reads the size the page will draw)
    pdefs = []
    for p in asm_parts:
        for k, v in (p.get("params") or {}).items():
            if isinstance(v, (int, float)):
                pdefs.append((p["id"], k, v))
    body = dev.get("defs", "")
    lines = []
    for tag, e in exprs.items():
        pid = tag[1:].split("_")[0] if tag[0] in "PQ" else None
        lets = ", ".join(f"P_{k} = {v}" for (i, k, v) in pdefs if i == pid)
        lines.append(f'echo(POSE_{tag} = {"let (" + lets + ") " if lets else ""}{e});')
    body += "\n".join(lines)
    res = scad_probe.probe(f"asm_{dev_id}", dev["scad"], dev["overrides"], body, export="echo")
    vals = {}
    for e in res.echoes:
        m = re.match(r"POSE_(\S+) = (.*)$", e)
        if m:
            vals[m.group(1)] = _parse(m.group(2))
    missing = [t for t in exprs if t not in vals or vals[t] is None]
    if missing:
        raise scad_probe.ProbeError(f"{dev_id}: no value for {missing} (undef in the case?)")
    return vals


def _chain(steps, tag, vals):
    m = _eye()
    for i, (kind, arg) in enumerate(steps):
        if kind == "T":
            m = _mul(m, _t(vals[f"{tag}_{i}"]))
        elif kind == "R":
            r = [vals[f"{tag}_{i}_{j}"] if isinstance(a, str) else a for j, a in enumerate(arg)]
            m = _mul(m, _rot(r))
        elif kind == "G":
            b = BOARDS[arg]
            m = _mul(m, _t([b["center"][k] - b["datum"][k] for k in range(3)]))
    return m


def derive(asm):
    """The assembly.json these CAD poses imply (a deep copy, poses rewritten)."""
    out = json.loads(json.dumps(asm))
    for dev_id, dev in DEVICES.items():
        parts = out["devices"][dev_id]["parts"]
        vals = _evaluate(dev_id, dev, parts)
        F = _chain(dev.get("frame", []), "F", vals)
        byid = {p["id"]: p for p in parts}
        for pid, steps in dev["parts"].items():
            p = byid.get(pid)
            if p is None:
                raise SystemExit(f"{dev_id}: assembly.json has no part '{pid}'")
            m = _eye() if steps == "world" else _mul(F, _chain(steps, f"P{pid}", vals))
            pos, rot = _decompose(m)
            seated = {"pos": [_num(x) for x in pos]}
            if any(abs(_ang(r)) > 0 for r in rot):
                seated["rot"] = [_ang(r) for r in rot]
            p["seated"] = seated
            p["pose"] = "cad"
            glb = [a for (k, a) in (steps if isinstance(steps, list) else []) if k == "G"]
            if glb:
                # written beside the pose so the Lab test can re-measure the
                # committed GLB with the page's own loader: a regenerated or
                # replaced board model that moved its center fails there
                p["glb_datum"] = {k: [_num(x) for x in v] for k, v in BOARDS[glb[0]].items()}
        for pid, ps in dev.get("params", {}).items():
            p = byid[pid]
            p.setdefault("params", {})
            for k in ps:
                v = vals[f"Q{pid}_{k}"]
                p["params"][k] = [_num(x) for x in v] if isinstance(v, list) else _num(v)
        sc = dev.get("screws")
        if sc:
            p = byid["screws"]
            xy = vals["Sxy"]
            inst, rot0 = [], None
            for q in xy:
                m = _mul(F, _t([q[0], q[1], vals["Sz"]]))
                if sc["rot"]:
                    m = _mul(m, _rot([vals[f"Srot_{j}"] if isinstance(a, str) else a
                                      for j, a in enumerate(sc["rot"])]))
                pos, rot = _decompose(m)
                inst.append([_num(x) for x in pos])
                rot0 = rot
            p["instances"] = inst
            p.pop("iz", None)
            if any(abs(_ang(r)) > 0 for r in rot0):
                p["irot"] = [_ang(r) for r in rot0]
            else:
                p.pop("irot", None)
            if sc.get("len"):
                p.setdefault("params", {})["len"] = _num(vals["Slen"])
            p["pose"] = "cad"
            if len(inst) != p.get("qty", len(inst)):
                raise SystemExit(f"{dev_id}: the CAD has {len(inst)} screws, assembly.json qty {p.get('qty')}")
    return out


def main(argv):
    check = "--check" in argv
    asm = json.loads(ASM.read_text(encoding="utf-8"))
    new = derive(asm)
    text = json.dumps(new, indent=1, ensure_ascii=False) + "\n"
    old = ASM.read_text(encoding="utf-8")
    if check:
        if text != old:
            print("assembly.json's seated poses are stale against the CAD — run "
                  "python3 docs/hardware/enclosure/gen_assembly_poses.py", file=sys.stderr)
            return 1
        print("assembly poses: current")
        return 0
    if text != old:
        ASM.write_text(text, encoding="utf-8")
        print(f"wrote {ASM.relative_to(REPO)}")
    else:
        print("assembly poses: already current")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
