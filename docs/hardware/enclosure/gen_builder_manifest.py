#!/usr/bin/env python3
"""Generate the web *enclosure builder* manifest from the OpenSCAD sources.

The securacv.com "builder" page lets people tweak the released Canary cases
with simple dropdowns and render STLs in the browser (OpenSCAD compiled to
WebAssembly). This script is the single source of truth for what that page
shows: it parses the Customizer annotations already present in the curated
.scad files (groups, option lists, ranges, per-parameter help text) and
emits a JSON manifest, plus — with --site — the carried copies for the
website repo (js/builder-data.js and byte-identical /scad sources,
sha256-pinned so the site's CI can detect drift from these sources).

Usage:
  ./gen_builder_manifest.py                 # (re)write builder_manifest.json
  ./gen_builder_manifest.py --check         # CI: fail if manifest is stale
  ./gen_builder_manifest.py --site DIR      # also write the website carries
                                            # (DIR = website checkout root)

Parsing rules (mirrors the OpenSCAD Customizer):
  * only top-level `name = <literal>;` assignments count (numbers, strings,
    true/false); computed values like `e_seal = opt_seal;` are skipped
  * scanning stops at the first top-level `module` definition
  * `/* [Group] */` starts a group; a `[Hidden]` group is skipped
  * a trailing `// comment` is the parameter's help text; a final
    `// ["a","b"]` or `// [min:step:max]` bracket is its options/range
  * `$fa`-style special variables are skipped (the builder's quality
    dropdown handles those itself)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "builder_manifest.json"

# ---------------------------------------------------------------------------
# Curation: which cases the web builder offers, and which parameters are
# front-and-centre ("simple") vs. tucked into the Advanced accordion.
# Parameters not listed in `simple` still appear, grouped, under Advanced.
# `preset_controls` are greyed out while the preset dropdown != "custom"
# (the .scad's preset table overrides those checkboxes — same as desktop).
# ---------------------------------------------------------------------------

CURATED = [
    {
        "id": "wap",
        "file": "canary_wap_enclosure.scad",
        "name": "Canary WAP",
        "tagline": "The pocket witness case — XIAO ESP32-S3, optional camera, "
                   "battery, GPS and weather sealing.",
        "simple": ["part", "preset", "opt_camera", "opt_buzzer", "opt_led",
                   "opt_battery", "opt_gps", "opt_tamper", "opt_touch",
                   "opt_antenna", "opt_seal", "opt_mount", "mount_style",
                   "lid_edge"],
        "preset_param": "preset",
        "preset_controls": ["opt_camera", "opt_buzzer", "opt_led",
                            "opt_battery", "opt_gps", "opt_tamper",
                            "opt_touch", "opt_antenna", "opt_seal",
                            "opt_mount"],
        "part_labels": {
            "all": "Assembled preview (not for printing)",
            "base": "Base — the tub",
            "lid": "Lid",
            "coupon": "Clip-fit coupon (print first)",
            "gasket": "Gasket ring — print in TPU",
            "shield": "Solar radiation shield",
            "tray": "Desiccant tray",
        },
    },
    {
        "id": "vision",
        "file": "canary_vision_enclosure.scad",
        "name": "Canary Vision",
        "tagline": "The camera case — Grove Vision AI V2 or DevKit host, "
                   "GoPro-compatible hinge, optional rain hood.",
        "simple": ["part", "preset", "host", "opt_led", "opt_vent",
                   "opt_tamper", "opt_hood", "opt_seal", "opt_mount",
                   "mount_style"],
        "preset_param": "preset",
        "preset_controls": ["opt_led", "opt_buzzer", "opt_vent", "opt_tamper",
                            "opt_hood", "opt_seal", "opt_mount",
                            "mount_style"],
        "part_labels": {
            "all": "Assembled preview (not for printing)",
            "back": "Back shell",
            "front": "Front face",
            "gasket": "Gasket ring — print in TPU",
            "bracket": "Wall bracket",
            "knob": "Hinge knob",
        },
    },
    {
        "id": "doorbell",
        "file": "canary_vision_doorbell.scad",
        "name": "Canary Vision Doorbell",
        "tagline": "Wyze/Ring form factor for the Vision stack — sealed by "
                   "default, wedge plates for aiming down the approach.",
        "simple": ["part", "opt_seal", "opt_vent", "opt_led", "opt_tamper",
                   "plate_wedge", "plate_wedge_x"],
        "preset_param": None,
        "preset_controls": [],
        "part_labels": {
            "all": "Assembled preview (not for printing)",
            "body": "Body",
            "face": "Face",
            "plate": "Wall plate",
            "gasket": "Gasket ring — print in TPU",
        },
    },
    {
        "id": "sense",
        "file": "canary_sense_enclosure.scad",
        "name": "Canary Sense",
        "tagline": "The 60 GHz radar radome case — MR60BHA2/FDA2 kit with a "
                   "stacked XIAO C6. The front window stays thin and flat.",
        "simple": ["part", "radar", "opt_led", "opt_lux", "opt_vent",
                   "opt_tamper", "opt_seal", "opt_mount", "mount_style",
                   "radome_t"],
        "preset_param": None,
        "preset_controls": [],
        "part_labels": {
            "all": "Assembled preview (not for printing)",
            "back": "Back shell",
            "front": "Front face — the radome",
            "gasket": "Gasket ring — print in TPU",
            "bracket": "Wall bracket",
            "knob": "Hinge knob",
        },
    },
    {
        "id": "coupon",
        "file": "canary_fit_coupon.scad",
        "name": "Fit coupon",
        "tagline": "The ~25-minute calibration print. Every fit used across "
                   "the fleet's cases, as labelled test stations — print "
                   "this first, tune the three tolerances, reuse everywhere.",
        "simple": ["part", "tol_slide", "tol_press", "tol_hole"],
        "preset_param": None,
        "preset_controls": [],
        "part_labels": {
            "all": "Base + mate + strip",
            "base": "Base plate (rigid)",
            "mate": "Mate — studs + slide rib",
            "strip": "Gasket bar — print in TPU",
        },
    },
]

# ---------------------------------------------------------------------------
# .scad Customizer parsing
# ---------------------------------------------------------------------------

GROUP_RE = re.compile(r"/\*\s*\[([^\]]+)\]")  # trailing text/newlines allowed
ASSIGN_RE = re.compile(
    r"(?:^|;)\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
    r"(\"(?:[^\"\\]|\\.)*\"|true|false|-?\d+\.?\d*)\s*(?=;)"
)
ENUM_RE = re.compile(r"\[\s*\"[^\]]*\]")          # ["a","b",...]
RANGE_RE = re.compile(r"\[\s*(-?\d+\.?\d*)\s*:\s*(-?\d+\.?\d*)"
                      r"(?:\s*:\s*(-?\d+\.?\d*))?\s*\]")  # [min:step:max]


def _num(s: str):
    f = float(s)
    return int(f) if f.is_integer() and "." not in s else f


def parse_scad(path: Path) -> list[dict]:
    """Return the file's Customizer groups with their literal parameters."""
    groups: list[dict] = []
    group = {"name": "Parameters", "params": []}
    in_block_comment = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        if re.match(r"^module\s", raw):
            break
        line = raw
        if in_block_comment:
            if "*/" not in line:
                continue
            line = line.split("*/", 1)[1]
            in_block_comment = False
        m = GROUP_RE.search(line)
        if m:
            if group["params"]:
                groups.append(group)
            group = {"name": m.group(1).strip(), "params": []}
            if "*/" not in line[m.end():]:
                in_block_comment = True
            continue
        if "/*" in line:
            before, after = line.split("/*", 1)
            if "*/" in after:
                line = before + after.split("*/", 1)[1]
            else:
                in_block_comment = True
                line = before
        code, comment = (line.split("//", 1) + [""])[:2]
        if group["name"].strip().lower() == "hidden":
            continue
        assigns = ASSIGN_RE.findall(" ;" + code.replace(";", "; ;"))
        # (the padding lets the regex anchor every `name =` after a `;`)
        if not assigns:
            continue
        for name, value in assigns:
            if name.startswith("$"):
                continue
            param: dict = {"name": name}
            if value in ("true", "false"):
                param["type"] = "bool"
                param["default"] = value == "true"
            elif value.startswith('"'):
                param["type"] = "string"
                param["default"] = json.loads(value)
            else:
                param["type"] = "number"
                param["default"] = _num(value)
            if len(assigns) == 1 and comment.strip():
                desc = comment.strip()
                em = ENUM_RE.search(desc)
                if em and param["type"] == "string":
                    try:
                        opts = json.loads(em.group(0).replace("'", '"'))
                        param["options"] = opts
                        desc = (desc[:em.start()] + desc[em.end():]).strip()
                    except ValueError:
                        pass
                else:
                    rm = None
                    for rm_c in RANGE_RE.finditer(desc):
                        rm = rm_c            # use the LAST bracket on the line
                    if rm and param["type"] == "number":
                        a, b, c = rm.group(1), rm.group(2), rm.group(3)
                        param["min"] = _num(a)
                        if c is None:
                            param["max"] = _num(b)
                        else:
                            param["step"] = _num(b)
                            param["max"] = _num(c)
                        desc = (desc[:rm.start()] + desc[rm.end():]).strip()
                    desc = re.sub(r"\s*//\s*$", "", desc)  # bracket left a bare //
                param["desc"] = re.sub(r"\s+", " ", desc).strip(" -—")
            group["params"].append(param)
    if group["params"]:
        groups.append(group)
    return groups


def build_manifest() -> dict:
    models = []
    for spec in CURATED:
        src = HERE / spec["file"]
        groups = parse_scad(src)
        by_name = {p["name"]: p for g in groups for p in g["params"]}
        for want in spec["simple"] + spec["preset_controls"]:
            if want not in by_name:
                sys.exit(f"{spec['file']}: curated parameter '{want}' "
                         f"not found — update gen_builder_manifest.py")
        for part in spec["part_labels"]:
            if part not in by_name["part"].get("options", []):
                sys.exit(f"{spec['file']}: part label '{part}' is not a "
                         f"part option — update gen_builder_manifest.py")
        models.append({
            **{k: spec[k] for k in ("id", "file", "name", "tagline", "simple",
                                    "preset_param", "preset_controls",
                                    "part_labels")},
            "sha256": hashlib.sha256(src.read_bytes()).hexdigest(),
            "groups": groups,
        })
    return {
        "comment": "GENERATED by gen_builder_manifest.py — do not edit. "
                   "Drives the securacv.com enclosure builder page.",
        "quality": {
            "note": "the builder's draft mode overrides the sources' "
                    "$fa=3/$fs=0.4 for faster preview renders",
            "draft": {"$fa": 8, "$fs": 1.2},
        },
        "models": models,
    }


# ---------------------------------------------------------------------------
# Website carries
# ---------------------------------------------------------------------------

SITE_HEADER = """\
/* GENERATED FILE — DO NOT EDIT BY HAND.
 * Canary enclosure builder manifest, generated from the parametric OpenSCAD
 * sources in the main repo (securaCV: docs/hardware/enclosure) by
 * gen_builder_manifest.py --site. Regenerate there — local edits will be
 * overwritten. The byte-identical .scad carries live in /scad and are
 * sha256-pinned here so tests/builder-facts.test.mjs can detect drift.
 */
"""


def write_site(manifest: dict, site: Path) -> None:
    scad_dir = site / "scad"
    scad_dir.mkdir(exist_ok=True)
    for model in manifest["models"]:
        shutil.copyfile(HERE / model["file"], scad_dir / model["file"])
    data = SITE_HEADER + "export const BUILDER = " + \
        json.dumps(manifest, indent=2, ensure_ascii=False) + ";\n"
    (site / "js" / "builder-data.js").write_text(data, encoding="utf-8")
    print(f"wrote {site / 'js' / 'builder-data.js'} and "
          f"{len(manifest['models'])} .scad carries to {scad_dir}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="verify builder_manifest.json is up to date (CI)")
    ap.add_argument("--site", metavar="DIR", type=Path,
                    help="also write the website carries into checkout DIR")
    args = ap.parse_args()

    manifest = build_manifest()
    text = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    if args.check:
        if not MANIFEST.exists() or MANIFEST.read_text(encoding="utf-8") != text:
            sys.exit("builder_manifest.json is stale — rerun "
                     "gen_builder_manifest.py")
        print("builder_manifest.json is up to date")
    else:
        MANIFEST.write_text(text, encoding="utf-8")
        print(f"wrote {MANIFEST}")
    if args.site:
        if not (args.site / "js").is_dir():
            sys.exit(f"{args.site} does not look like the website checkout")
        write_site(manifest, args.site)


if __name__ == "__main__":
    main()
