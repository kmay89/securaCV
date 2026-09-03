#!/usr/bin/env python3
"""canary-local/tools/gen_boards.py — vendor board CAD → committed GLB + boards.json.

The board viewer's generator, sibling to gen_enclosures.py. For each entry in
boards/boards.config.json it:

  1. tessellates the vendor STEP (boards/vendor/*.step) to a GLB with cascadio
     (heavy OCCT-in-wasm work — happens here, never in the browser),
  2. drops the user's mount parts by name (e.g. the Grove stand/shroud),
  3. bakes materials where the STEP export didn't carry them (Round Display),
  4. writes canary-local/boards/<id>.glb (committed, like the enclosure STLs),
  5. recomputes geometry facts from that committed GLB using the page's OWN
     loader (tools/glb_facts.mjs → assets/glb.js), so boards.json can never
     lie about the mesh the browser will show.

Committed GLBs are NOT byte-drift-gated (tessellation varies by cascadio build,
exactly as preview STLs vary by openscad build); boards.json IS gated, and
tests/boards.test.js re-derives its facts from the committed GLBs.

Local authoring tool — needs `pip install cascadio trimesh numpy` for the
vendor-STEP boards, plus `shapely manifold3d` for the procedural Waveshare
(rounded outline + boolean cutouts). CI does not run it (it verifies the
committed outputs with node only), so those extra deps stay off the CI path.
"""
import gzip
import json
import os
import shutil
import subprocess

import cascadio
import numpy as np
import trimesh
from shapely.geometry import Polygon
from trimesh.transformations import rotation_matrix
from trimesh.visual import TextureVisuals
from trimesh.visual.material import PBRMaterial

from _tooling import repo_root

REPO = repo_root()
CFG = REPO / "boards" / "boards.config.json"
VENDOR = REPO / "boards" / "vendor"
OUT_GLB_DIR = REPO / "canary-local" / "boards"
OUT_JSON = REPO / "canary-local" / "devices" / "boards.json"
FACTS = REPO / "canary-local" / "tools" / "glb_facts.mjs"


def _set_color(geom, rgb):
    geom.visual = TextureVisuals(material=PBRMaterial(baseColorFactor=[*rgb, 1.0]))


def _drop_below_y(scene, y_mm):
    """Strip any solid whose highest point sits below y_mm (raw tessellated
    millimeters, +Y up). This catches a vendor demo stand/mount that
    `merge_primitives` fuses into unnamed material buckets — the name-based
    `drop` can't see those (the merge promotes a SolidWorks feature name over
    the part name), but the printed stand sits entirely under the board, with a
    clean air gap above it, so a Y-plane cut removes it and nothing else. The
    board's own components all live above the cut. World Y = node transform ×
    vertices × 1000 (the GLB is in meters; the page's glb.js scales the same)."""
    for name in list(scene.geometry):
        nodes = scene.graph.geometry_nodes.get(name, [])
        T = scene.graph.get(nodes[0])[0] if nodes else np.eye(4)
        v = trimesh.transform_points(scene.geometry[name].vertices, T) * 1000.0
        if v[:, 1].max() < y_mm:
            scene.delete_geometry(name)


def bake_round_display(scene):
    """SolidWorks STEP export carried no per-solid colors through the
    tessellator, so assign by geometry: the Ø43 discs/ring are the black
    PCB + bezel, the 28–40 mm discs are the gray glass, everything else is a
    dark component. The shape is the vendor's exact model; only color is ours."""
    for geom in scene.geometry.values():
        diam = sorted(geom.extents)[2]  # largest extent = board diameter (meters)
        if diam > 0.040:
            _set_color(geom, (0.09, 0.09, 0.11))   # PCB face + bezel ring
        elif diam > 0.028:
            _set_color(geom, (0.21, 0.22, 0.25))   # glass / screen disc
        else:
            _set_color(geom, (0.14, 0.14, 0.16))   # connectors, socket, parts


def _pi_color(name):
    """Color a Raspberry Pi 5 solid from the vendor's own name (its STEP carried
    no colors). The model is exact; only the color is ours, keyed off the
    part label so the assignment is honest, not eyeballed geometry."""
    n = name.lower()
    if "plate_raspberry" in n or n.startswith("plate"):
        return (0.05, 0.20, 0.10)                       # FR-4 PCB green
    if any(k in n for k in ("ethernet", "usb", "hdmi", "rj45", "port", "jack", "shield")):
        return (0.72, 0.73, 0.76)                       # bright metal shells
    if any(k in n for k in ("broadcom", "rp1", "controller", "transceiv", "d9whv", "mxl", "pmic", "chip")):
        return (0.05, 0.05, 0.06)                       # black silicon
    if any(k in n for k in ("gpio", "header", "connect", "pin", "gold")):
        return (0.83, 0.67, 0.27)                       # gold header pins
    return (0.11, 0.11, 0.13)                            # dark passives / default


def bake_raspberry_pi(scene):
    """The Pi 5 STEP carries no per-solid colors and tessellates to ~12k named
    solids. Color each by its vendor name (_pi_color), then concatenate solids
    that share a color into ONE mesh per color, so the committed GLB is a
    handful of parts (like every other board) instead of 12k draw calls. Returns
    a fresh scene; transforms are baked into the vertices so the flattened mesh
    exports identically to how glb.js re-reads it."""
    buckets = {}
    for name in list(scene.geometry):
        g = scene.geometry[name].copy()
        nodes = scene.graph.geometry_nodes.get(name, [])
        if nodes:
            g.apply_transform(scene.graph.get(nodes[0])[0])
        buckets.setdefault(_pi_color(name), []).append(g)
    out = trimesh.Scene()
    for color, geoms in buckets.items():
        m = trimesh.util.concatenate(geoms)
        _set_color(m, color)
        out.add_geometry(m)
    return out


# ── procedural boards (no vendor CAD; built from a dimensional reference) ─────
# The Waveshare ESP32-S3-Touch-LCD-4.3B(-BOX) has no vendor STEP, so it is built
# here from primitives + boolean cutouts. Proportions and the exposed-feature
# layout (glossy 4.3" panel; a 16-way screw terminal along one long edge; TF +
# dual USB-C + BOOT/RESET on one short edge; status LEDs + power slide on the
# other) follow MaffooClock's published shell for the 5in sibling
# (ESP32-S3-Touch-LCD-5/5B — CC-BY-NC-SA, used as a DIMENSIONAL REFERENCE ONLY,
# not copied or redistributed) scaled to the 4.3B, cross-checked against
# firmware/boards/waveshare-esp32s3-lcd43b and the owner's photos.
#
# Frame: X = width (long edge), Y = thickness (front/screen at y=+TH → rear/IO at
# y=0). Z = height (short edge). The screen and the terminal are on OPPOSITE
# faces (as on the real board): the glass fills the front (+Y); the green
# terminal block and its screws sit on the REAR (−Y), where the field wiring
# exits, near the bottom (−Z) edge. So the Board Room's default pose looks at the
# rear (the pin side), and orbiting reveals the screen.
WS43B_W, WS43B_HT, WS43B_TH = 118.0, 79.0, 25.0   # width, height, thickness (mm)
WS43B_PITCH = 3.81                                # terminal pitch (silk-exact)
WS43B_TZ = -WS43B_HT / 2 + 11.0                   # terminal band center, Z (rear, near bottom)

# The 16 terminals in rear-silk order along +X (VIN end → DI1 end), transcribed
# from firmware/boards/waveshare-esp32s3-lcd43b/README.md ("top to bottom, per
# the rear silk"): power, I2C, CAN (L,H), RS485 (B,A), isolated I/O. The pad keys
# match boards.config.json (the three grounds are group-qualified because the
# isolated-I/O side is opto-isolated, NOT common with the 6-36 V or I2C ground).
WS43B_TERMS = [
    ("VIN", "6~36V"), ("GND-pwr", "6~36V"),
    ("VOUT", "I2C"), ("GND-i2c", "I2C"), ("SDA", "I2C"), ("SCL", "I2C"),
    ("L", "CAN"), ("H", "CAN"),
    ("B", "RS485"), ("A", "RS485"),
    ("DO0", "Isolated I/O"), ("DO1", "Isolated I/O"), ("DI COM", "Isolated I/O"),
    ("GND-io", "Isolated I/O"), ("DI0", "Isolated I/O"), ("DI1", "Isolated I/O"),
]


def _ws43b_term_x(i):
    """X (mm) of terminal i, centered on the block."""
    return (i - (len(WS43B_TERMS) - 1) / 2.0) * WS43B_PITCH


def ws43b_anchors():
    """The 16 terminal anchors (raw model mm) on the screw row — boards.json's
    pads map and pinout anchors read from the same geometry the builder lays
    down, so a flag can never point at empty space."""
    return {name: [round(_ws43b_term_x(i), 3), -13.0, round(WS43B_TZ + 2.6, 3)]
            for i, (name, _grp) in enumerate(WS43B_TERMS)}


def _ws43b_rrect(w, h, r, n=7):
    """Centered rounded-rectangle polygon (w×h, corner radius r) in the X–Z plane."""
    hw, hh, pts = w / 2 - r, h / 2 - r, []
    for cx, cz, a0 in [(hw, hh, 0), (-hw, hh, 90), (-hw, -hh, 180), (hw, -hh, 270)]:
        for k in range(n + 1):
            a = np.radians(a0 + 90 * k / n)
            pts.append((cx + r * np.cos(a), cz + r * np.sin(a)))
    return Polygon(pts)


def _ws43b_prism_y(poly, y0, y1):
    """Extrude an X–Z polygon along Y from y0..y1 (a rounded slab facing +Y)."""
    m = trimesh.creation.extrude_polygon(poly, height=y1 - y0)   # extrudes +Z (0..h)
    m.apply_transform(rotation_matrix(np.radians(-90), [1, 0, 0]))  # Z(0..h)→Y; poly Y→world Z
    m.apply_translation([0, y0, 0])
    return m


def build_waveshare_4_3b():
    """Reverse-engineered DIMENSIONAL MODEL of the Waveshare
    ESP32-S3-Touch-LCD-4.3B(-BOX) — NOT vendor CAD. A rounded charcoal ABS shell
    (boolean-cut for the recessed 4.3" glass and the edge I/O), a glossy off IPS
    panel on the front, the green 16-way pluggable terminal on the REAR
    (pitch/order/labels exact to the firmware silk — opposite the screen, as on
    the real board), TF + dual USB-C + BOOT/RESET on one short edge, three status
    LEDs + a power slide on the other, and rear mounting bosses. See the module
    header for the reference + honesty note."""
    W, HT, TH, TZ = WS43B_W, WS43B_HT, WS43B_TH, WS43B_TZ
    CASE = (0.155, 0.163, 0.180)     # dark charcoal ABS shell
    GLASS = (0.059, 0.075, 0.102)    # off, glossy IPS panel (glossFor LENS → shines)
    BEZEL = (0.055, 0.058, 0.065)    # near-black bezel / wire-entry mouths
    GREEN = (0.243, 0.553, 0.208)    # pluggable terminal body
    GOLD = (0.831, 0.671, 0.271)     # screw metal (glossFor GOLD set)
    SILVER = (0.722, 0.729, 0.761)   # USB / TF shells (glossFor METAL set)
    BTN = (0.105, 0.110, 0.120)      # buttons / switch actuator
    LEDG, LEDA, LEDR = (0.28, 0.72, 0.34), (0.86, 0.62, 0.18), (0.82, 0.24, 0.22)
    buckets = {}

    def add(m, color):
        buckets.setdefault(color, []).append(m)

    def box(ext, c, color):
        b = trimesh.creation.box(extents=ext)
        b.apply_translation(c)
        add(b, color)

    def cyl(r, h, c, color, axis="y", n=24):
        m = trimesh.creation.cylinder(radius=r, height=h, sections=n)
        if axis == "y":
            m.apply_transform(rotation_matrix(np.pi / 2, [1, 0, 0]))
        elif axis == "x":
            m.apply_transform(rotation_matrix(np.pi / 2, [0, 1, 0]))
        m.apply_translation(c)
        add(m, color)

    # ── case body: rounded prism, screen pocket + I/O holes cut with manifold ──
    body = _ws43b_prism_y(_ws43b_rrect(W, HT, 4.0), 0.0, TH)
    cham = trimesh.creation.box(extents=[W + 4, 8, 8])            # top-front chamfer
    cham.apply_transform(rotation_matrix(np.radians(45), [1, 0, 0]))
    cham.apply_translation([0, TH + 2.0, HT / 2 + 2.0])
    body = body.difference(cham, engine="manifold")
    GW, GH, GZ = 105.0, 58.0, 0.0                                # 4.3" glass, centered on the front
    pocket = _ws43b_prism_y(_ws43b_rrect(GW + 3, GH + 3, 3.0, 5), TH - 2.0, TH + 1)
    pocket.apply_translation([0, 0, GZ])
    body = body.difference(pocket, engine="manifold")
    holes = [([6, 13.0, 4.2], [W/2 - 1, TH/2, 15]),              # TF slot (+X edge)
             ([6, 9.0, 4.5], [W/2 - 1, TH/2, 2]),                # USB-C
             ([6, 3.2, 3.2], [W/2 - 1, TH/2, -9]),               # BOOT
             ([6, 3.2, 3.2], [W/2 - 1, TH/2, -16]),              # RESET
             ([6, 2.4, 2.4], [-W/2 + 1, TH/2, 16]),              # LED (−X edge)
             ([6, 2.4, 2.4], [-W/2 + 1, TH/2, 10]),
             ([6, 2.4, 2.4], [-W/2 + 1, TH/2, 4]),
             ([6, 9.0, 5.0], [-W/2 + 1, TH/2, -12])]             # power slide slot
    for ext, c in holes:
        h = trimesh.creation.box(extents=ext)
        h.apply_translation(c)
        body = body.difference(h, engine="manifold")
    add(body, CASE)

    # ── screen: near-black bezel frame + glossy glass recessed in the pocket ──
    bez = _ws43b_prism_y(_ws43b_rrect(GW + 2.6, GH + 2.6, 3.0, 5), TH - 1.9, TH - 0.4)
    bez.apply_translation([0, 0, GZ])
    add(bez, BEZEL)
    glass = _ws43b_prism_y(_ws43b_rrect(GW, GH, 2.4, 5), TH - 1.6, TH - 0.35)
    glass.apply_translation([0, 0, GZ])
    add(glass, GLASS)

    # ── green 16-way pluggable terminal on the REAR face (−Y), near the bottom
    #    (−Z) edge — opposite the screen, where the field wiring exits the back.
    #    The block protrudes −Y; screws + wire mouths face −Y. ──
    blkw = (len(WS43B_TERMS) - 1) * WS43B_PITCH + WS43B_PITCH + 3.0
    block = _ws43b_prism_y(_ws43b_rrect(blkw, 14.0, 1.2, 3), -12.0, 2.0)
    block.apply_translation([0, 0, TZ])
    add(block, GREEN)
    for i in range(len(WS43B_TERMS)):
        x = _ws43b_term_x(i)
        box([WS43B_PITCH - 0.6, 2.2, 3.2], [x, -11.4, TZ - 3.0], BEZEL)  # wire-entry mouth
        cyl(0.95, 2.6, [x, -12.6, TZ + 2.6], GOLD, axis="y", n=16)       # screw head, faces −Y
    box([blkw - 2, 2.0, 0.6], [0, -11.4, TZ + 6.4], BEZEL)              # silk strip

    # ── I/O shells recessed in the +X edge holes ──
    box([2.4, 12.0, 3.4], [W/2 - 2.4, TH/2, 15], SILVER)   # TF card shell
    box([2.4, 8.2, 4.0], [W/2 - 2.4, TH/2, 2], SILVER)     # USB-C shell
    for z in (-9, -16):
        cyl(1.3, 2.0, [W/2 - 2.4, TH/2, z], BTN, axis="x", n=18)
    # ── status LEDs + power slide (−X edge) ──
    for z, col in [(16, LEDG), (10, LEDA), (4, LEDR)]:
        cyl(1.0, 1.6, [-W/2 + 1.6, TH/2, z], col, axis="x", n=16)
    box([2.2, 4.0, 4.2], [-W/2 + 1.8, TH/2, -12], SILVER)  # switch actuator

    # ── two rear mounting bosses (top corners of the rear) ──
    for x in (-46, 46):
        cyl(3.0, 1.4, [x, 0.7, HT/2 - 12], BEZEL, axis="y", n=20)
    # ── stylised rear product label (the real unit carries a sticker here) —
    #    a plain rounded rectangle, no fabricated text/serial/barcode ──
    STICKER = (0.70, 0.70, 0.67)
    lbl = _ws43b_prism_y(_ws43b_rrect(46, 26, 2.0, 4), -0.6, 0.2)
    lbl.apply_translation([0, 0, 6.0])
    add(lbl, STICKER)

    out = trimesh.Scene()
    for color, geoms in buckets.items():
        m = trimesh.util.concatenate(geoms)
        m.apply_scale(0.001)   # authored in mm; GLB is meters (glb.js scales ×1000)
        _set_color(m, color)
        out.add_geometry(m)
    return out


PROCEDURAL_BUILDERS = {"waveshare_4_3b": build_waveshare_4_3b}


def build_procedural(cfg):
    """Build a board that has no vendor CAD — the mesh comes from a Python
    builder (PROCEDURAL_BUILDERS) instead of a tessellated STEP. Exports the
    committed GLB and recomputes facts with the page's own loader, same as the
    STEP path, so boards.json still can't lie about the mesh."""
    out = OUT_GLB_DIR / (cfg["id"] + ".glb")
    builder = PROCEDURAL_BUILDERS[cfg["builder"]]
    builder().export(str(out))
    facts = json.loads(subprocess.check_output(["node", str(FACTS), str(out)]))
    return out, facts


def build_board(cfg):
    if cfg.get("source") == "procedural":
        return build_procedural(cfg)
    src = VENDOR / cfg["source"]
    if not src.exists():
        raise FileNotFoundError(f"vendor CAD missing: {src}")
    tmp = OUT_GLB_DIR / (cfg["id"] + ".tmp.glb")
    out = OUT_GLB_DIR / (cfg["id"] + ".glb")
    # Vendor STEPs are gzipped in-repo (boards/vendor/*.step.gz); cascadio needs
    # a plain path, so decompress to a scratch .step for this run only. The
    # try/finally guarantees both scratch files are removed even on failure.
    work_step = None
    try:
        step_path = src
        if src.suffix == ".gz":
            work_step = OUT_GLB_DIR / (cfg["id"] + ".src.step")
            with gzip.open(src, "rb") as fi, open(work_step, "wb") as fo:
                shutil.copyfileobj(fi, fo)
            step_path = work_step
        cascadio.step_to_glb(
            str(step_path), str(tmp),
            tol_linear=cfg["tol_linear"], tol_angular=cfg["tol_angular"],
            merge_primitives=cfg.get("merge_primitives", True),
        )
        scene = trimesh.load(str(tmp), process=False)

        drop = [d.lower() for d in cfg.get("drop", [])]
        if drop and hasattr(scene, "geometry"):
            for name in list(scene.geometry):
                if any(d in name.lower() for d in drop):
                    scene.delete_geometry(name)

        if cfg.get("drop_below_y") is not None and hasattr(scene, "geometry"):
            _drop_below_y(scene, cfg["drop_below_y"])

        if cfg.get("materials") == "round-display":
            bake_round_display(scene)
        elif cfg.get("materials") == "raspberry-pi":
            scene = bake_raspberry_pi(scene)

        scene.export(str(out))
    finally:
        tmp.unlink(missing_ok=True)
        if work_step is not None:
            work_step.unlink(missing_ok=True)

    facts = json.loads(subprocess.check_output(["node", str(FACTS), str(out)]))
    return out, facts


def main():
    cfg = json.loads(CFG.read_text())
    OUT_GLB_DIR.mkdir(parents=True, exist_ok=True)
    boards = {}
    for b in cfg["boards"]:
        out, facts = build_board(b)
        rel = os.path.relpath(out, REPO / "canary-local").replace("\\", "/")
        boards[b["id"]] = {
            "name": b["name"], "vendor": b["vendor"], "mpn": b.get("mpn"),
            "devices": b["devices"], "glb": rel,
            # procedural boards (built from photos/spec) have no vendor STEP
            **({} if b["source"] == "procedural" else {"source_step": "boards/vendor/" + b["source"]}),
            "dims_mm": facts["dims_mm"], "triangles": facts["triangles"],
            "parts": facts["parts"], "materials": facts["materials"],
            "pose": b["pose"],
            # pads (full castellation map) and per-row anchor/anchors ride along
            # verbatim — raw GLB mm, authored from tools/pin_anchors.mjs islands
            **({"pads": b["pads"]} if "pads" in b else {}),
            "pinout": b["pinout"], "blurb": b["blurb"],
            "doc": b.get("doc"), "provenance": b["provenance"],
        }
        print(f"OK {b['id']}: {facts['dims_mm']} mm · {facts['triangles']:,} tris · "
              f"{facts['parts']} parts · {len(facts['materials'])} materials → {rel}")

    # device -> [board_id, ...] in config order (primary board first). A device
    # can carry more than one board (e.g. the Watch is a plain XIAO stacked in
    # the Round Display); board-lab.js renders a picker when the list has >1.
    dev_index = {}
    for bid, e in boards.items():
        for d in e["devices"]:
            dev_index.setdefault(d, []).append(bid)

    doc = {
        "generated_by": "canary-local/tools/gen_boards.py",
        "note": ("Vendor board CAD (boards/vendor/*.step) tessellated to a committed "
                 "GLB by cascadio; geometry facts recomputed from the committed mesh by "
                 "the page's own loader (tools/glb_facts.mjs → assets/glb.js). See "
                 "boards/vendor/README.md for provenance and license."),
        "device_board": dev_index,
        "boards": boards,
    }
    OUT_JSON.write_text(json.dumps(doc, indent=1) + "\n")
    print(f"\nOK boards.json: {len(boards)} boards; device map {dev_index}")


if __name__ == "__main__":
    main()
