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

Local authoring tool — needs `pip install cascadio trimesh numpy`. CI does not
run it (it verifies the committed outputs with node only).
"""
import gzip
import json
import os
import shutil
import subprocess
from pathlib import Path

import cascadio
import numpy as np
import trimesh
from trimesh.visual import TextureVisuals
from trimesh.visual.material import PBRMaterial

REPO = Path(__file__).resolve().parents[2]
CFG = REPO / "boards" / "boards.config.json"
VENDOR = REPO / "boards" / "vendor"
OUT_GLB_DIR = REPO / "canary-local" / "boards"
OUT_JSON = REPO / "canary-local" / "devices" / "boards.json"
FACTS = REPO / "canary-local" / "tools" / "glb_facts.mjs"


def _set_color(geom, rgb):
    geom.visual = TextureVisuals(material=PBRMaterial(baseColorFactor=[*rgb, 1.0]))


def _drop_below_y(scene, y_mm):
    """Strip any solid whose highest point sits below y_mm (raw tessellated
    millimetres, +Y up). This catches a vendor demo stand/mount that
    `merge_primitives` fuses into unnamed material buckets — the name-based
    `drop` can't see those (the merge promotes a SolidWorks feature name over
    the part name), but the printed stand sits entirely under the board, with a
    clean air gap above it, so a Y-plane cut removes it and nothing else. The
    board's own components all live above the cut. World Y = node transform ×
    vertices × 1000 (the GLB is in metres; the page's glb.js scales the same)."""
    for name in list(scene.geometry):
        nodes = scene.graph.geometry_nodes.get(name, [])
        T = scene.graph.get(nodes[0])[0] if nodes else np.eye(4)
        v = trimesh.transform_points(scene.geometry[name].vertices, T) * 1000.0
        if v[:, 1].max() < y_mm:
            scene.delete_geometry(name)


def bake_round_display(scene):
    """SolidWorks STEP export carried no per-solid colours through the
    tessellator, so assign by geometry: the Ø43 discs/ring are the black
    PCB + bezel, the 28–40 mm discs are the grey glass, everything else is a
    dark component. The shape is the vendor's exact model; only colour is ours."""
    for geom in scene.geometry.values():
        diam = sorted(geom.extents)[2]  # largest extent = board diameter (metres)
        if diam > 0.040:
            _set_color(geom, (0.09, 0.09, 0.11))   # PCB face + bezel ring
        elif diam > 0.028:
            _set_color(geom, (0.21, 0.22, 0.25))   # glass / screen disc
        else:
            _set_color(geom, (0.14, 0.14, 0.16))   # connectors, socket, parts


def _pi_colour(name):
    """Colour a Raspberry Pi 5 solid from the vendor's own name (its STEP carried
    no colours). The model is exact; only the colour is ours, keyed off the
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
    """The Pi 5 STEP carries no per-solid colours and tessellates to ~12k named
    solids. Colour each by its vendor name (_pi_colour), then concatenate solids
    that share a colour into ONE mesh per colour, so the committed GLB is a
    handful of parts (like every other board) instead of 12k draw calls. Returns
    a fresh scene; transforms are baked into the vertices so the flattened mesh
    exports identically to how glb.js re-reads it."""
    buckets = {}
    for name in list(scene.geometry):
        g = scene.geometry[name].copy()
        nodes = scene.graph.geometry_nodes.get(name, [])
        if nodes:
            g.apply_transform(scene.graph.get(nodes[0])[0])
        buckets.setdefault(_pi_colour(name), []).append(g)
    out = trimesh.Scene()
    for colour, geoms in buckets.items():
        m = trimesh.util.concatenate(geoms)
        _set_color(m, colour)
        out.add_geometry(m)
    return out


def build_board(cfg):
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
            "source_step": "boards/vendor/" + b["source"],
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
                 "boards/vendor/README.md for provenance and licence."),
        "device_board": dev_index,
        "boards": boards,
    }
    OUT_JSON.write_text(json.dumps(doc, indent=1) + "\n")
    print(f"\nOK boards.json: {len(boards)} boards; device map {dev_index}")


if __name__ == "__main__":
    main()
