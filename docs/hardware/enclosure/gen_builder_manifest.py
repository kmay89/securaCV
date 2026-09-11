#!/usr/bin/env python3
"""Generate the web *enclosure builder* manifest from the OpenSCAD sources.

The securacv.com "builder" page lets people tweak the released Canary cases
with simple dropdowns and render STLs in the browser (OpenSCAD compiled to
WebAssembly). This script is the single source of truth for what that page
shows: it parses the Customizer annotations already present in the curated
.scad files (groups, option lists, ranges, per-parameter help text) and
emits a JSON manifest, plus — with --site — the carried copies for the
website repo (js/builder-data.js and byte-identical /scad sources,
sha256-pinned so the site's CI can detect drift from these sources). The
--site run also carries the REFERENCE_SCADS (sources the site's AR models
trace but the builder does not offer) and distills the fleet-figures
ledger into scad/cad-dims.json, the envelope numbers the website's model
tests pin the AR previews against.

Usage:
  ./gen_builder_manifest.py                 # (re)write builder_manifest.json
  ./gen_builder_manifest.py --check         # CI: fail if manifest is stale
  ./gen_builder_manifest.py --site DIR      # also write the website carries
                                            # (DIR = website checkout root)
  ./gen_builder_manifest.py --site DIR --check
                                            # write nothing; name every carry
                                            # in DIR whose bytes are stale

A run writes only what changed: builder_manifest.json is left untouched when
its bytes are already current (so a --site run touches nothing in this tree
it did not mean to), and each website carry is written only when its bytes
differ. `--site DIR --check` regenerates every carry in memory — the .scad
copies (models, their use/include deps, REFERENCE_SCADS), scad/colorways.json,
scad/cad-dims.json and js/builder-data.js — and byte-compares each against
DIR, printing the stale or missing paths and exiting 1 on any. It is the
monorepo-side gate the carry never had: the website's weekly carry job and
its sha256 pins caught a stale copy a week late, and the only way to ask
"is the site current?" used to be a write.

WHAT cad-dims.json CARRIES (distill_cad_dims). Per device/part figure: the
envelope, confidence and dims_source the website's model tests already pin,
plus — for the released multi-part devices — `seams_mm`, the assembled
part-to-part seams along the figure's depth exactly as the ledger measured
them (unrounded, so the AR generators can place a lid or a face at the real
seam instead of a hand-typed meter constant), and `knobs`, the board and
module dimensions the case is built around: the device manifests' cad.params
(devices/<slug>/device.json), resolved through gen_cad_params.py's own
loader so a registry reference is already a number, merged per figure the
way that generator merges them per case (a shared key two manifests disagree
on fails there, once, and so fails here). A figure no params-bearing
manifest draws gets no `knobs` key — the doorbell today. Top-level
`board_registry` ({id: {evidence, l, t, w}}) and `board_facts`
({brd_<name>: value}) carry canary_board_lib.scad's rows and measured facts,
evidence rung included, so page copy that says "40 × 20 module" or "measured"
can be pinned to the registry instead of retyped. Every new object is keyed
in sorted order, and every key that existed before these did is emitted
byte-for-byte as before.

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
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "builder_manifest.json"
# gen_cad_params.py (same directory) imports parse_scad from this module and
# is imported back, lazily, by distill_cad_dims() — so the two cannot import
# each other at module load. HERE on sys.path lets that lazy import resolve
# whether this file runs as a script, is imported by a test harness or by
# the manifest linter.
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

# Numbers appearing in curated hint prose — see the truth gate in
# build_manifest(). Matches a bare decimal, optionally followed by a unit,
# and deliberately not a number embedded in an identifier.
HINT_NUM = re.compile(r"(?<![\w.])(\d+(?:\.\d+)?)\s*(?:mm|°|%)?")

# ---------------------------------------------------------------------------
# Curation: which cases the web builder offers, and which parameters are
# front-and-center ("simple") vs. tucked into the Advanced accordion.
# Parameters not listed in `simple` still appear, grouped, under Advanced.
# `preset_controls` are grayed out while the preset dropdown != "custom"
# (the .scad's preset table overrides those checkboxes — same as desktop).
#
# The microcopy fields keep the page self-explanatory; all are validated
# against the parsed sources so they can't name things that don't exist:
#   about      — 1-2 sentences shown when the case is selected
#   print_plan — what to actually print, and in which material
#   part_info  — one line per part option (must cover every option)
#   labels     — friendly display names for the simple controls
#   hints      — help text overriding a source comment that's too terse
#   choices    — plain-language text for raw enum values (value stays as-is)
#   units      — display unit for sliders when it isn't millimeters
# ---------------------------------------------------------------------------

CURATED = [
    {
        "id": "wap",
        "file": "canary_wap_enclosure.scad",
        "name": "Canary WAP",
        "tagline": "The pocket witness case — XIAO ESP32-S3, optional camera, "
                   "battery, GPS and weather sealing.",
        "about": "A pocket-sized box for the XIAO ESP32-S3 witness build. "
                 "Tick only the peripherals actually on your bench — each one "
                 "adds its bay, port or window, and the case grows or shrinks "
                 "to fit.",
        "print_plan": "Print the Base and Lid in PETG. Going outdoors? Pick "
                      "the Outdoor preset, then also print the Gasket in TPU "
                      "— plus the Shield and Tray if it will sit in the sun.",
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
        "part_info": {
            "all": "Every part in place, for looking around — pick a single "
                   "part when you want an STL to print.",
            "base": "The tub the boards and battery clip into. Prints flat, "
                    "open side up, no supports.",
            "lid": "Snaps onto the base; prints face-down. Your options "
                   "carve its windows, vents and ports.",
            "coupon": "A one-clip fit tester for this case — print it before "
                      "a full lid to dial in the snap.",
            "gasket": "The soft seal ring for weather builds — print in TPU, "
                      "and switch Weather seal ON first or there is nothing "
                      "to render.",
            "shield": "A sun shield that sits over sealed outdoor builds and "
                      "keeps direct sun off the case.",
            "tray": "A little desiccant tray for sealed builds — silica gel "
                    "keeps the inside dry through the seasons.",
        },
        "labels": {
            "part": "Part to print", "preset": "Quick preset",
            "opt_camera": "Camera (XIAO Sense)", "opt_buzzer": "Buzzer",
            "opt_led": "Status LED", "opt_battery": "Battery bay",
            "opt_gps": "GPS module", "opt_tamper": "Tamper magnet",
            "opt_touch": "Touch pad", "opt_antenna": "External antenna",
            "opt_seal": "Weather seal", "opt_mount": "Wall mount",
            "mount_style": "Mount style", "lid_edge": "Lid edge chamfer",
        },
        "hints": {},
        "choices": {
            "preset": {
                "custom": "Custom — pick the options yourself",
                "battery_full": "Battery build — camera, GPS, buzzer, the lot (indoor)",
                "compact_plain": "Compact — smallest case, buzzer + LED only",
                "battery_weather": "Outdoor — battery build, sealed and wall-mounted",
            },
            "mount_style": {
                "keyhole": "Keyholes — screws in the wall, case slots on",
                "tabs": "Screw tabs — four ears, screwed straight down",
                "both": "Both — keyholes and tabs",
            },
        },
        "units": {"lid_edge": "mm"},
    },
    {
        "id": "vision",
        "file": "canary_vision_enclosure.scad",
        "name": "Canary Vision",
        "tagline": "The camera case — Grove Vision AI V2 or DevKit host, "
                   "GoPro-compatible hinge, optional rain hood.",
        "about": "The camera witness for a wall or eave: Grove Vision AI V2 "
                 "with a stacked XIAO (recommended) or a Grove-cabled DevKit. "
                 "The GoPro-style hinge aims it; the options weather it.",
        "print_plan": "Print the Back and Front in PETG — plus the Bracket "
                      "and Knob if you use the hinge mount. The Outdoor "
                      "preset adds a rain hood and the TPU Gasket.",
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
            "hood": "Rain hood",
        },
        "part_info": {
            "all": "Every part in place, for looking around — pick a single "
                   "part when you want an STL to print.",
            "back": "The wall-side shell. Boards clip in; the hinge knuckle "
                    "or keyholes grow from its back.",
            "front": "The face with the lens aperture and clear-disc seat — "
                     "screws into the back with 4× M2.",
            "gasket": "The soft seal ring for weather builds — print in TPU; "
                      "switch Weather seal ON first.",
            "bracket": "The wall bracket the hinge clicks into — four screws "
                       "into the wall, then aim and lock.",
            "knob": "The thumbscrew that locks the hinge at your angle.",
            "hood": "The rain & glare hood — its own part: press it into the "
                    "groove on the front and bond it. Switch the hood ON first.",
        },
        "labels": {
            "part": "Part to print", "preset": "Quick preset",
            "host": "Brain board", "opt_led": "Status LED",
            "opt_vent": "GORE vent", "opt_tamper": "Tamper magnet",
            "opt_hood": "Rain & glare hood", "opt_seal": "Weather seal",
            "opt_mount": "Mounting", "mount_style": "Mount style",
        },
        "hints": {},
        "choices": {
            "preset": {
                "custom": "Custom — pick the options yourself",
                "vision_indoor": "Indoor — LED and hinge, no sealing",
                "vision_weather": "Outdoor — sealed, vented and hooded",
            },
            "host": {
                "xiao": "Stacked XIAO (recommended)",
                "devkit": "DevKitM-1 over a Grove cable",
            },
            "mount_style": {
                "hinge": "GoPro-style hinge — aim it after mounting",
                "keyhole": "Keyholes — flat and flush on the wall",
                "both": "Both — hinge and keyholes",
            },
        },
        "units": {},
    },
    {
        "id": "doorbell",
        "file": "canary_vision_doorbell.scad",
        "name": "Canary Vision Doorbell",
        "tagline": "Wyze/Ring form factor for the Vision stack — sealed by "
                   "default, wedge plates for aiming down the approach.",
        "about": "The Vision stack in a Ring/Wyze-sized doorbell: camera, "
                 "lit button, hidden security screw. It ships sealed by "
                 "default — doorbells live outside.",
        "print_plan": "Print the Body, Face and Plate in PETG and the Gasket "
                      "in TPU. Aiming down a porch or across a corner? Set "
                      "the wedge angles before you print the Plate.",
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
        "part_info": {
            "all": "Every part in place, for looking around — pick a single "
                   "part when you want an STL to print.",
            "body": "The main shell the module stack lives in — it hangs on "
                    "the plate's T-studs and locks with the hidden screw.",
            "face": "The visible front: lens aperture, button hole, and the "
                    "gasket groove behind.",
            "plate": "The wall plate with the T-studs. The wedge angles live "
                     "here — the case aims wherever the plate points.",
            "gasket": "The soft TPU ring that seals face to body against "
                      "the weather.",
        },
        "labels": {
            "part": "Part to print", "opt_seal": "Weather seal",
            "opt_vent": "GORE vent", "opt_led": "Extra light pipe",
            "opt_tamper": "Tamper magnet",
            "plate_wedge": "Aim down", "plate_wedge_x": "Aim left / right",
        },
        "hints": {
            "plate_wedge": "Tilts the whole case downward — porches and "
                           "walk-ups. 15° is the safe maximum.",
            "plate_wedge_x": "Turns the case toward the approach — corner "
                             "installs. Negative aims left.",
        },
        "choices": {},
        "units": {"plate_wedge": "°", "plate_wedge_x": "°"},
    },
    {
        "id": "sense",
        "file": "canary_sense_enclosure.scad",
        "name": "Canary Sense",
        "tagline": "The 60 GHz radar radome case — MR60BHA2/FDA2 kit with a "
                   "stacked XIAO C6. The front window stays thin and flat.",
        "about": "The radar witness — Seeed's MR60 kit with a stacked XIAO "
                 "C6 behind a thin, flat radome window the 60 GHz beam "
                 "passes through. Nothing may sit in front of the antenna: "
                 "no labels, no ribs, no metal.",
        "print_plan": "Print the Back and Front in PETG — the Front IS the "
                      "radome, so keep its window one clean membrane. "
                      "Fall-detection builds mount flat on the ceiling via "
                      "keyholes; add the TPU Gasket only for sealed builds.",
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
        "part_info": {
            "all": "Every part in place, for looking around — pick a single "
                   "part when you want an STL to print.",
            "back": "The mounting shell — the radar carrier clips in with "
                    "the XIAO hanging beneath it.",
            "front": "The radome face. The window over the antenna stays "
                     "thin, flat and empty — that's the physics working.",
            "gasket": "The soft TPU seal ring — switch Weather seal ON "
                      "first; indoor ceilings rarely need it.",
            "bracket": "Wall bracket for the hinge mount — same part the "
                       "Vision case uses.",
            "knob": "The hinge thumbscrew — same part the Vision case uses.",
        },
        "labels": {
            "part": "Part to print", "radar": "Radar kit",
            "opt_led": "Status LED", "opt_lux": "Light-sensor window",
            "opt_vent": "GORE vent", "opt_tamper": "Tamper magnet",
            "opt_seal": "Weather seal", "opt_mount": "Mounting",
            "mount_style": "Mount style", "radome_t": "Radome thickness",
        },
        # radome_t deliberately carries NO hint. It used to, and the hint said
        # "1.0 mm is the proven default" beside a control whose own slider
        # starts at 1.3 and whose model asserts >= 1.3, because 0.7-1.1 is the
        # quarter-wave band that reflects ~20 % of the beam back into the
        # antenna. A first timer who followed the hint got an aborted render;
        # one who trusted it over the assert would have thinned the wall in a
        # slicer and built a radar that quietly does not work. The source
        # comment is better than the hint was and is already shown.
        "hints": {},
        "choices": {
            "radar": {
                "bha2": "MR60BHA2 — breathing & presence (wall or bedside)",
                "fda2": "MR60FDA2 — fall detection (flat on the ceiling)",
            },
            "mount_style": {
                "hinge": "GoPro-style hinge — aim the beam",
                "keyhole": "Keyholes — flush on the wall or ceiling",
                "both": "Both — hinge and keyholes",
            },
        },
        "units": {"radome_t": "mm"},
    },
    {
        "id": "coupon",
        "file": "canary_fit_coupon.scad",
        "name": "Fit coupon",
        "tagline": "The first-print calibration plate. Every fit used across "
                   "the fleet's cases, as labeled test stations — print "
                   "this first, tune the three tolerances, reuse everywhere.",
        "about": "Every fit the fleet's cases use, gathered on one branded "
                 "test plate: the Canary mark embossed in uniform domed strokes, "
                 "the WAP's two-sided snap-clip channel, "
                 "click-retained keyholes, magnet and light-pipe "
                 "pockets, insert bores, the lid lip, the USB-C port "
                 "opening, a −/0/+ screw-pilot ladder, and the embossed/"
                 "debossed wordmarks every case's branding uses. A tight or "
                 "loose station tells you which number to change — once, "
                 "for every case after.",
        "print_plan": "Print the Base in PETG first — it's small next to "
                      "any case. The Mate "
                      "tests the keyholes (slide to the click) and its "
                      "edge tongue tests the lid-lip channel; the "
                      "Strip is the TPU gasket-squeeze test. Adjust the "
                      "three tolerances and reprint until stations fit.",
        # glyph_rib, not emblem_rib. The EMBLEM station was retired
        # (emblem_h = 0) and the builder went on promoting its stroke knob to
        # the front page — a control that moves no geometry, with a hint
        # telling a newcomer how to react to a print they will never get,
        # while the GLYPH station the coupon's own header calls "the one that
        # matters most" had no label, no hint and no place in the simple list.
        "simple": ["part", "tol_slide", "tol_press", "tol_hole", "kh_click",
                   "glyph_rib"],
        "preset_param": None,
        "preset_controls": [],
        "part_labels": {
            "all": "Base + mate + strip",
            "base": "Base plate (rigid)",
            "mate": "Mate — studs + slide tongue",
            "strip": "Gasket bar — print in TPU",
        },
        "part_info": {
            "all": "Base, mate and strip laid out together — in practice "
                   "print them separately (the strip is TPU).",
            "base": "The station plate: clip, pocket, bore, port and "
                    "wordmark tests, each labeled with the parameter it "
                    "exercises.",
            "mate": "T-studs that hang in the base's keyholes and click "
                    "past the retention detent, plus a bottom-edge tongue "
                    "for the slide channel — the stud face stays flat so "
                    "the hang test seats fully.",
            "strip": "A soft TPU bar for the gasket groove — tests the "
                     "squeeze that seals the weather builds.",
        },
        "labels": {
            "part": "Part to print", "tol_slide": "Sliding fits",
            "tol_press": "Press fits", "tol_hole": "Screw holes",
            "kh_click": "Keyhole click",
            "glyph_show": "Bird on the bed face",
            "glyph_h": "Bird height",
            "glyph_rib": "Bird stroke",
            "glyph_depth": "Bird deboss depth",
        },
        "hints": {
            "tol_slide": "Parts that slide or snap: lid lips, board clips, "
                         "keyholes. Loose station → smaller number.",
            "tol_press": "Parts pushed in to stay: magnets, light pipes. "
                         "They should seat firmly by thumb.",
            "tol_hole": "Self-tapping M2 pilots: threads should bite "
                        "without splitting the post.",
            "kh_click": "The keyhole retention bump. Mate won't slide past "
                        "it → smaller number; slides back off too easily → "
                        "bigger. 0 removes the click.",
            "glyph_show": "Debosses the Canary into the face that prints "
                          "against the bed. Every case A-surface in this "
                          "catalog prints face-down, so this is the station "
                          "that tells you what your plate and first layer do "
                          "to a visible surface.",
            "glyph_rib": "The stroke width the bird is drawn at, and the "
                         "number this station tests. Blobbed and closed up → "
                         "smaller; broken or missing → bigger. No feature is "
                         "drawn thinner than this, so one number decides the "
                         "whole mark.",
            "glyph_depth": "How deep the bird is sunk. Too shallow and one "
                           "bridged layer closes it back up; this is the same "
                           "depth every case label uses, so what happens here "
                           "happens on your case.",
        },
        "choices": {},
        "units": {"tol_slide": "mm", "tol_press": "mm", "tol_hole": "mm",
                  "kh_click": "mm", "glyph_rib": "mm", "glyph_depth": "mm"},
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
# `use <lib.scad>` / `include <lib.scad>` — the file's OpenSCAD dependencies.
DEP_RE = re.compile(r"^\s*(?:use|include)\s*<([^>]+)>")


def scad_deps(path: Path) -> list[str]:
    """Ordered, de-duplicated list of the .scad libraries a model `use`s or
    `include`s, transitively. Names are relative to the enclosure sources
    (HERE) — the flat layout the website /scad carries + the browser worker
    both use — so a model that `use <canary_mark_lib.scad>` cannot render in
    the in-browser builder unless that library is carried alongside it.
    """
    result: list[str] = []
    seen: set[str] = set()

    def walk(p: Path) -> None:
        for raw in p.read_text(encoding="utf-8").splitlines():
            m = DEP_RE.match(raw)
            if not m:
                continue
            dep = m.group(1).strip()
            if dep in seen:
                continue
            seen.add(dep)
            dep_path = HERE / dep
            if not dep_path.is_file():
                sys.exit(f"{p.name}: use/include <{dep}> was not found next to "
                         f"the OpenSCAD sources — cannot carry it to the site")
            walk(dep_path)        # a dependency's own deps must be carried too
            result.append(dep)

    walk(path)
    return result


def _num(s: str):
    f = float(s)
    return int(f) if f.is_integer() and "." not in s else f


def parse_scad(path: Path, with_lines: bool = False) -> list[dict]:
    """Return the file's Customizer groups with their literal parameters.

    with_lines=True additionally records each parameter's 1-based source
    line as `"line"`. gen_cad_params.py asks for it so the knobs a device
    manifest may own and the knobs this parser accepts are one set by
    construction — the rewrite lands on the very line the parser took the
    literal from, and nothing else scans the file. The default output
    (and so builder_manifest.json) is unchanged.
    """
    groups: list[dict] = []
    group = {"name": "Parameters", "params": []}
    in_block_comment = False
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
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
            if with_lines:
                param["line"] = lineno
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
        part_options = by_name["part"].get("options", [])
        for part in spec["part_labels"]:
            if part not in part_options:
                sys.exit(f"{spec['file']}: part label '{part}' is not a "
                         f"part option — update gen_builder_manifest.py")
        # microcopy must cover every part and only name real things
        for part in part_options:
            if part not in spec["part_info"]:
                sys.exit(f"{spec['file']}: part '{part}' has no part_info "
                         f"line — every option needs one")
        for field in ("labels", "hints", "units"):
            for name in spec[field]:
                if name not in by_name:
                    sys.exit(f"{spec['file']}: {field} names unknown "
                             f"parameter '{name}'")
        # A hint may not restate a number the source owns. The checks above
        # only ever asserted that microcopy NAMES real things — so a hint
        # could name only real parameters and still lie about every one of
        # them, which is exactly what happened: the Sense's radome hint sold
        # "1.0 mm is the proven default" beside a slider that starts at 1.3,
        # for the one control where the wrong number is a radar that quietly
        # does not work. Prose is free to explain; numbers belong to the .scad.
        for name, text in spec["hints"].items():
            p = by_name[name]
            if p.get("type") != "number":
                continue
            said = {float(x) for x in HINT_NUM.findall(text)}
            # The source owns its default, its slider bounds, AND any number
            # its own comment states — a documented sentinel like "0 = no
            # click" is the source speaking, not the hint inventing.
            owned = {float(p[k]) for k in ("default", "min", "max", "step")
                     if isinstance(p.get(k), (int, float))}
            owned |= {float(x) for x in HINT_NUM.findall(p.get("desc") or "")}
            lying = said - owned
            if lying:
                sys.exit(
                    f"{spec['file']}: the hint for '{name}' names "
                    f"{sorted(lying)}, which is not its default/min/step/max "
                    f"{sorted(owned)} — a hint may not restate a number the "
                    f"source owns. Delete the number, or fix the source.")
        for name, mapping in spec["choices"].items():
            opts = by_name.get(name, {}).get("options")
            if not opts:
                sys.exit(f"{spec['file']}: choices for '{name}' but it has "
                         f"no option list")
            for value in mapping:
                if value not in opts:
                    sys.exit(f"{spec['file']}: choices for '{name}' name "
                             f"unknown value '{value}'")
        # Carry-and-pin the model's `use`/`include` libraries the same way the
        # model itself is pinned (file + sha256), so the site can render a case
        # whose source pulls in a shared library (e.g. the coupon's
        # canary_mark_lib.scad) and CI can detect drift in that library too.
        dep_pins = [
            {"file": dep,
             "sha256": hashlib.sha256((HERE / dep).read_bytes()).hexdigest()}
            for dep in scad_deps(src)
        ]
        model_entry = {
            **{k: spec[k] for k in ("id", "file", "name", "tagline", "about",
                                    "print_plan", "simple", "preset_param",
                                    "preset_controls", "part_labels",
                                    "part_info", "labels", "hints",
                                    "choices", "units")},
            "sha256": hashlib.sha256(src.read_bytes()).hexdigest(),
            "groups": groups,
        }
        if dep_pins:
            model_entry["deps"] = dep_pins
        models.append(model_entry)
    return {
        "comment": "GENERATED by gen_builder_manifest.py — do not edit. "
                   "Drives the securacv.com enclosure builder page.",
        "colorways": parse_colorways(),
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

# Sources carried and sha256-pinned WITHOUT appearing in the builder's model
# list. The website's AR/product models (securacv_website scripts/make-*.mjs)
# trace their dimensions to these files, so they ride the same
# carry-and-pin contract as the builder models — before this list existed,
# canary_watch_station.scad had been hand-copied into the site's /scad with
# no pin, no generator and no test: an edit here left the copy silently
# stale forever.
REFERENCE_SCADS = ["canary_watch_station.scad"]

# The colorway registry (canary_color_lib.scad) — parsed here so the palette
# crosses to every non-scad consumer from its one home: the manifest embeds
# it for the builder UI, and --site carries scad/colorways.json for the AR
# variant picker and the showroom finishes. Values are lowercase sRGB hex;
# consumers own their conversions (the scad's color() takes sRGB 0..1, glTF
# baseColorFactor wants linear).
COLOR_LIB = HERE / "canary_color_lib.scad"
_CW_ROW = re.compile(
    r'\[\s*"([a-z]+)"\s*,\s*"([^"]+)"\s*,\s*"([0-9a-f]{6})"\s*,'
    r'\s*"([0-9a-f]{6})"\s*,\s*"([0-9a-f]{6})"\s*,\s*"([^"]*)"\s*\]',
    re.S)


def parse_colorways() -> list[dict]:
    src = COLOR_LIB.read_text(encoding="utf-8")
    start = src.index("CW_REGISTRY = [")
    end = src.index("];", start)
    region = src[start:end]
    rows = _CW_ROW.findall(region)
    # Every row must parse, not just some: findall() would silently drop a
    # row that drifts from the literal shape (an id with a digit, an
    # uppercase hex) and this generator would publish an INCOMPLETE palette
    # while OpenSCAD kept the missing colorway — the worst kind of quiet
    # fork. Row starts are counted independently of the row regex, so a
    # malformed row is a build failure here, never a shorter palette.
    row_starts = len(re.findall(r'\[\s*"', region))
    if not rows or len(rows) != row_starts:
        sys.exit(f"canary_color_lib.scad: parsed {len(rows)} of {row_starts} "
                 "CW_REGISTRY rows — a row has drifted from the literal "
                 '[id, name, hex, hex, hex, note] shape (lowercase id and '
                 "hex; keep each row a plain literal)")
    return [{"id": r[0], "name": r[1], "body": r[2], "ink": r[3],
             "light": r[4], "note": re.sub(r"\s+", " ", r[5]).strip()}
            for r in rows]


# The distilled CAD-dimensions ledger the website's model tests pin against.
# Distilled from canary-local/devices/figures.json — whose envelopes are read
# from the committed STL bounding boxes at generation time, never retyped —
# so the site's "dimensionally-honest preview" claim chains back to the same
# meshes people print. Regenerated by --site alongside the scad carries.
REPO = HERE.parent.parent.parent
FIGURES_JSON = REPO / "canary-local" / "devices" / "figures.json"
DEVICES_DIR = REPO / "devices"


def knobs_by_figure(devices_dir: Path | None = None,
                    repo: Path | None = None) -> dict[str, dict[str, object]]:
    """{figure id: {knob: resolved value}} — every device manifest's cad.params,
    joined to the figure it draws, sorted by knob.

    The values are gen_cad_params.load_params()'s: a registry reference is
    already the registry's number, and a shared key two manifests disagree on
    has already failed there, naming both — the merge is not repeated here.
    Only the manifests that draw a figure contribute to it (the devkit's
    dk_l / dk_w / stack_h reach device.canary-vision-devkit, not
    device.canary-vision, though both name one case file); a manifest with no
    `figure`, or with nothing in cad.params, contributes nothing. Exits 1 when
    the manifests do not agree with their cases (gen_cad_params.py --check),
    so the ledger never publishes a knob the released STLs were not rendered
    from, and when two manifests drawing one figure name two case files."""
    import gen_cad_params as gcp   # lazy: it imports parse_scad from here
    devices_dir = devices_dir or DEVICES_DIR
    repo = repo or REPO
    problems = gcp.check(devices_dir, repo)
    if problems:
        sys.exit("cad-dims.json carries the manifests' cad.params as the cases' knobs, but "
                 f"they disagree ({len(problems)} problem(s)):\n  " + "\n  ".join(problems)
                 + "\nrun python3 docs/hardware/enclosure/gen_cad_params.py (--check) first")
    owned, _ = gcp.load_params(devices_dir, repo)
    drawn: dict[str, list[tuple[str, str]]] = {}      # figure -> [(slug, cad.scad)]
    for path in sorted(devices_dir.glob("*/device.json")):
        m = json.loads(path.read_text(encoding="utf-8"))
        cad = m.get("cad") or {}
        if not cad.get("params") or not m.get("figure"):
            continue
        drawn.setdefault(m["figure"], []).append((m.get("slug") or path.parent.name,
                                                  cad["scad"]))
    out: dict[str, dict[str, object]] = {}
    for fig_id in sorted(drawn):
        scads = sorted({scad for _, scad in drawn[fig_id]})
        if len(scads) > 1:
            who = ", ".join(f"devices/{s} ({Path(c).name})" for s, c in drawn[fig_id])
            sys.exit(f"figure {fig_id} is drawn by manifests naming different case files — "
                     f"{who} — one figure's knobs come from one case")
        slugs = {s for s, _ in drawn[fig_id]}
        knobs = {k: o.value for k, o in owned.get(scads[0], {}).items() if slugs & set(o.slugs)}
        if knobs:
            out[fig_id] = {k: knobs[k] for k in sorted(knobs)}
    return out


def distill_cad_dims(figures_json: Path | None = None, devices_dir: Path | None = None,
                     repo: Path | None = None) -> dict:
    """Reduce the fleet-figures ledger to what the website's 3D models need:
    per device/part envelope, confidence, and where the dims came from —
    plus the assembled seams, the manifest-owned knobs and the board
    registry the site's generators and copy would otherwise hand-type.

    Provenance travels per figure, because the ledger is mixed: a
    dims_source of "stl" is read from committed STL bounding boxes,
    "board-cad" from a vendor board mesh — and "sketch" is a massing
    ESTIMATE for an in-development design, carried so the site can show
    the design honestly, never as a measured fact. The sketch_note rides
    along for exactly that reason; a consumer that treats a sketch figure
    like an stl one is misreading the ledger, not this file.

    `seams_mm` is the ledger's assembled.seams_fig_d verbatim — the depths
    (figure d) at which one part ends and the next begins, measured by
    gen_assembled_dims.py and not rounded here: the AR models place a seam
    in meters and the page rounds for copy, so a rounded seam would move a
    model that is byte-reproducible today. `knobs` and the two board tables
    are documented on knobs_by_figure() and in the module docstring. The
    optional arguments point a test at a scratch tree; the defaults are this
    repository."""
    import gen_cad_params as gcp   # lazy: it imports parse_scad from here
    repo = repo or REPO
    ledger = json.loads((figures_json or FIGURES_JSON).read_text(encoding="utf-8"))
    knobs = knobs_by_figure(devices_dir, repo)
    try:
        registry = gcp.parse_board_registry(repo / gcp.BOARD_LIB_REL)
    except gcp.RegistryError as e:
        sys.exit(f"cad-dims.json carries the board registry, which did not parse: {e}")
    figures = {}
    for fig in ledger["figures"]:
        if fig.get("role") not in ("device", "part"):
            continue
        env = fig.get("envelope_mm")
        if not env:
            continue
        entry = {
            "role": fig["role"],
            "confidence": fig["confidence"],
            "dims_source": fig["dims_source"],
            "envelope_mm": {k: env[k] for k in ("w", "h", "d")},
        }
        if fig.get("sketch_note"):
            entry["sketch_note"] = fig["sketch_note"]
        seams = (fig.get("assembled") or {}).get("seams_fig_d")
        if seams:
            entry["seams_mm"] = list(seams)
        if fig["id"] in knobs:
            entry["knobs"] = knobs.pop(fig["id"])
        figures[fig["id"]] = entry
    if knobs:
        # A params-bearing manifest names a figure this ledger does not carry
        # (no envelope, or not a device/part) — its knobs would vanish silently.
        sys.exit("cad.params with no ledger home: the manifest(s) drawing "
                 f"{', '.join(sorted(knobs))} own knobs, but the fleet-figures ledger carries no "
                 "device/part envelope for that figure — regenerate figures.json "
                 "(gen_figures.mjs) or fix the manifest's `figure`")
    return {
        "comment": "GENERATED by gen_builder_manifest.py --site — do not edit. "
                   "Distilled from securaCV's fleet-figures ledger "
                   "(canary-local/devices/figures.json). Provenance is PER "
                   "FIGURE via dims_source: \"stl\" envelopes are read from "
                   "committed STL bounding boxes, \"board-cad\" from vendor "
                   "board meshes, and \"sketch\" figures are massing estimates "
                   "for in-development designs (sketch_note says more) — "
                   "honest previews, not measurements. The website's model "
                   "tests pin the AR models to these numbers and surface the "
                   "dims_source so page copy keeps saying which is which.",
        "spec": ledger.get("spec"),
        "board_registry": {
            rid: {"evidence": row.status, "l": row.dims["l"], "t": row.dims["t"],
                  "w": row.dims["w"]}
            for rid, row in sorted(registry.rows.items())
        },
        "board_facts": {name: fact.value for name, fact in sorted(registry.facts.items())},
        "figures": {k: figures[k] for k in sorted(figures)},
    }


SITE_HEADER = """\
/* GENERATED FILE — DO NOT EDIT BY HAND.
 * Canary enclosure builder manifest, generated from the parametric OpenSCAD
 * sources in the main repo (securaCV: docs/hardware/enclosure) by
 * gen_builder_manifest.py --site. Regenerate there — local edits will be
 * overwritten. The byte-identical .scad carries live in /scad and are
 * sha256-pinned here so tests/builder-facts.test.mjs can detect drift.
 */
"""


def site_carries(manifest: dict) -> dict[str, bytes]:
    """Every file a --site run writes, as {path relative to the website root:
    bytes}, in write order. Pure: nothing is read from the website and nothing
    is written anywhere, so write_site() and check_site() cannot disagree
    about what a carry is — one computes it, the other compares it."""
    carries: dict[str, bytes] = {}
    carried: set[str] = set()
    for model in manifest["models"]:
        for name in [model["file"], *(d["file"] for d in model.get("deps", []))]:
            if name in carried:
                continue
            carries[f"scad/{name}"] = (HERE / name).read_bytes()
            carried.add(name)
    # reference sources (carried + pinned, not offered in the builder UI) —
    # the AR model generators trace these, so they get the same drift gate
    reference = []
    for name in REFERENCE_SCADS:
        for dep in [name, *scad_deps(HERE / name)]:
            if dep in carried:
                continue
            carries[f"scad/{dep}"] = (HERE / dep).read_bytes()
            carried.add(dep)
            reference.append(
                {"file": dep,
                 "sha256": hashlib.sha256((HERE / dep).read_bytes()).hexdigest()})
    # the colorway registry as a standalone carry (the AR variant picker
    # fetches it; tiny, so the page need not import the whole manifest)
    cw_text = json.dumps({
        "comment": "GENERATED by gen_builder_manifest.py --site — do not edit. "
                   "The colorway registry from canary_color_lib.scad: lowercase "
                   "sRGB hex per role (body / ink / light). Consumers convert "
                   "explicitly (glTF wants linear).",
        "colorways": parse_colorways(),
    }, indent=2, ensure_ascii=False) + "\n"
    carries["scad/colorways.json"] = cw_text.encode("utf-8")
    # the distilled CAD-dimensions ledger + its pin
    dims_text = json.dumps(distill_cad_dims(), indent=2, ensure_ascii=False) + "\n"
    carries["scad/cad-dims.json"] = dims_text.encode("utf-8")
    site_data = dict(manifest)
    site_data["reference_scads"] = reference
    site_data["cad_dims"] = {
        "file": "scad/cad-dims.json",
        "sha256": hashlib.sha256(dims_text.encode("utf-8")).hexdigest(),
    }
    site_data["colorways_carry"] = {
        "file": "scad/colorways.json",
        "sha256": hashlib.sha256(cw_text.encode("utf-8")).hexdigest(),
    }
    data = SITE_HEADER + "export const BUILDER = " + \
        json.dumps(site_data, indent=2, ensure_ascii=False) + ";\n"
    carries["js/builder-data.js"] = data.encode("utf-8")
    return carries


def check_site(carries: dict[str, bytes], site: Path) -> list[str]:
    """The carries (site_carries()) whose bytes in `site` differ from what
    --site would write, as `<relative path> (stale)` / `(missing)` lines, in
    write order; [] means the website checkout is current. Writes nothing."""
    stale: list[str] = []
    for rel, data in carries.items():
        dst = site / rel
        if not dst.is_file():
            stale.append(f"{rel} (missing)")
        elif dst.read_bytes() != data:
            stale.append(f"{rel} (stale)")
    return stale


def write_site(carries: dict[str, bytes], site: Path) -> list[str]:
    """Write every carry (site_carries()) whose bytes differ — a current file
    is not touched — and return the relative paths written."""
    (site / "scad").mkdir(exist_ok=True)
    written: list[str] = []
    for rel, data in carries.items():
        dst = site / rel
        if dst.is_file() and dst.read_bytes() == data:
            continue
        dst.write_bytes(data)
        written.append(rel)
    return written


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="verify builder_manifest.json is up to date (CI); with --site, also "
                         "verify every carry in DIR — write nothing either way")
    ap.add_argument("--site", metavar="DIR", type=Path,
                    help="also write the website carries into checkout DIR")
    args = ap.parse_args(argv)

    if args.site and not (args.site / "js").is_dir():
        sys.exit(f"{args.site} does not look like the website checkout")
    manifest = build_manifest()
    text = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    current = MANIFEST.exists() and MANIFEST.read_text(encoding="utf-8") == text
    if args.check:
        if not current:
            sys.exit("builder_manifest.json is stale — rerun "
                     "gen_builder_manifest.py")
        print("builder_manifest.json is up to date")
    elif current:
        print(f"{MANIFEST.name} is unchanged — not rewritten")
    else:
        MANIFEST.write_text(text, encoding="utf-8")
        print(f"wrote {MANIFEST}")
    if not args.site:
        return 0
    carries = site_carries(manifest)
    total = len(carries)
    if args.check:
        stale = check_site(carries, args.site)
        if stale:
            print(f"{len(stale)} of {total} website carries are not current in {args.site}:")
            for line in stale:
                print(f"  ✗ {line}")
            sys.exit("rerun gen_builder_manifest.py --site "
                     f"{args.site} (then the site's model generators, if cad-dims.json moved)")
        print(f"website carries in {args.site} are up to date ({total} files)")
        return 0
    written = write_site(carries, args.site)
    if written:
        for rel in written:
            print(f"wrote {args.site / rel}")
        print(f"{len(written)} of {total} website carries written to {args.site} "
              f"({total - len(written)} already current)")
    else:
        print(f"website carries in {args.site} are already current ({total} files) — "
              f"nothing written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
