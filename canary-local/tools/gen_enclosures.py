#!/usr/bin/env python3
"""canary-local/tools/gen_enclosures.py — the enclosure catalog, as data.

Reads the enclosure library's OWN sources of truth and emits
canary-local/devices/enclosures.json for the page's enclosure lab:

  1. docs/hardware/enclosure/README.md   — the variant tables ("Pick your
     variant" = print-validated sets with committed STLs; "In development"
     = render-verified designs, no STLs by policy)
  2. each *.scad                          — OpenSCAD customizer annotations
     (/* [Group] */ sections, `name = value; // comment`, enum/range
     comments) → a real parameter map, straight from the configurator

Optionally (--render, needs openscad) renders coarse PREVIEW meshes for a
curated set of in-development designs into canary-local/enclosures/preview/
so the lab can show them in 3D. These are explicitly preview meshes —
docs/hardware/enclosure keeps its "committed STLs are print-validated"
policy; nothing is written there.

Run:  python3 canary-local/tools/gen_enclosures.py [--render]
CI:   regenerates and diffs (drift gate, same idea as the emulator dist).
"""
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ENC = REPO / "docs/hardware/enclosure"
OUT_JSON = REPO / "canary-local/devices/enclosures.json"
PREVIEW_DIR = REPO / "canary-local/enclosures/preview"

# Which device each variant/design belongs to (the page groups by card).
# "family" also drives the chooser's device↔enclosure pairing.
DEVICE_OF = [
    (r"^WAP", "canary-wap"),
    (r"^Vision", "canary-vision"),
    (r"^Sense", "canary-sense"),
    (r"Watch station", "canary-display-watch"),
    (r"Dashboard display", "canary-display-dash"),
    (r"Sense bedside|Sense in-wall", "canary-sense"),
    (r"Thermal / outdoor", "canary-wap"),
    (r"Combo", "canary-vision"),
]

# Preview meshes rendered for in-development designs the device sheets
# feature. part → -D part=<...>; coarse curves keep files small.
RENDER_PRESETS = {
    "canary_watch_station.scad": ["drum", "bezel", "stand"],
    "canary_dash_display.scad": ["frame", "back", "stand"],
    "canary_combo.scad": ["back", "front"],
}

# ── Print guidance (the lab's "how to print it" cards) ───────────────────
# Global settings quoted from README.md §Suggested print settings; per-part
# orientation notes curated from the .scad sources' own comments (each part
# is MODELED in its print orientation — z=0 is the build plate, which is
# what makes the lab's plate view honest).
PRINT_SETTINGS = {
    "source": "docs/hardware/enclosure/README.md §Suggested print settings",
    "material": "PETG or ASA for heat/UV exposure; PLA only for indoor/bench",
    "gasket_material": "TPU 90–95A · 2 perimeters · 100% infill · slow",
    "layer_height_mm": 0.2,
    "walls": 3,
    "infill_pct": "20–30",
    "orientation": "parts print flat as modeled — no supports by design; lids/faces print face-down so chamfers, seats and debossed labels land on the first layers",
}

# filename-substring → note (first match wins). Sources: scad comments.
PART_NOTES = [
    ("gasket", "TPU 90–95A, 100% infill, slow — the seal is the print"),
    ("lid", "prints face-down: chamfer + deboss land on the first layers (clean bed = clean face)"),
    ("base", "prints flat, open side up — no supports"),
    ("_drum", "prints open-face-up; keyhole pockets in the back (canary_watch_station.scad)"),
    ("_bezel", "flat ring, prints face-down; seats the display disc"),
    ("station_stand", "tilted cradle prints upright — no supports"),
    ("display_frame", "prints face-down: the A-surface is your textured build plate"),
    ("display_back", "prints outer-face-down; vents + keyholes need no support"),
    ("display_stand", "prints flat; fin + rails are additive — no supports"),
    ("doorbell_face", "prints face-down without support (edge rounds off the bed)"),
    ("front", "prints face-down without support"),
    ("back", "prints flat — no supports"),
    ("bracket", "prints flat; GoPro prongs vertical for in-plane strength"),
    ("knob", "prints flat on its face"),
    ("coupon", "print FIRST — 15-minute fit tuner for your printer's tolerances"),
    ("shield", "prints as oriented; louvers are self-supporting at 45°"),
    ("tray", "prints flat — no supports"),
    ("plate", "prints flat on the wall face"),
]


def part_note(filename: str) -> str:
    low = filename.lower()
    for pat, note in PART_NOTES:
        if pat in low:
            return note
    return "prints flat as modeled — no supports by design"


def device_for(name: str) -> str | None:
    for pat, dev in DEVICE_OF:
        if re.search(pat, name):
            return dev
    return None


# ── README variant tables ────────────────────────────────────────────────
def parse_tables(md: str):
    sets = []

    def rows_of(section: str, stop: str):
        m = re.search(rf"^##+ {re.escape(section)}.*?$(.*?)(?=^## {re.escape(stop)})",
                      md, re.M | re.S)
        if not m:
            raise SystemExit(f"README section not found: {section}")
        return re.findall(r"^\|(.+)\|$", m.group(1), re.M)

    def cells(row: str):
        return [c.strip() for c in row.split("|")]

    def links(cell: str, ext: str):
        return [
            {"name": t, "file": f.lstrip("./")}
            for t, f in re.findall(rf"\[([^\]]+)\]\(\.\/([^)]+\.{ext})\)", cell)
        ]

    def preview_of(cell: str):
        m = re.search(r'src="\./([^"]+\.png)"', cell)
        return m.group(1) if m else None

    # Released: | Variant | For | Preview | Parts |
    for row in rows_of("Pick your variant", "Engineering & materials"):
        c = cells(row)
        if len(c) < 4 or c[0].startswith("-") or c[0] in ("Variant", "Design"):
            continue
        name = re.sub(r"\*+", "", c[0]).strip()
        if not name or name == "For":
            continue
        stls = links(c[3], "stl")
        if not stls:
            continue
        # The scad behind the parts: shared per family prefix.
        scad = None
        m = re.match(r"(canary_[a-z]+(?:_[a-z]+)*?)_(?:enclosure|doorbell)", stls[0]["file"])
        candidates = sorted(ENC.glob("*.scad"))
        for sc in candidates:
            if stls[0]["file"].startswith(sc.stem):
                scad = sc.name
                break
        if not scad:
            for sc in candidates:
                if m and sc.stem.startswith(m.group(1)):
                    scad = sc.name
                    break
        if not scad:
            # Family fallback: canary_sense_back.stl → canary_sense_enclosure.scad
            fam = "_".join(stls[0]["file"].split("_")[:2])
            cand = ENC / f"{fam}_enclosure.scad"
            if cand.exists():
                scad = cand.name
        sets.append({
            "id": re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-"),
            "name": name,
            "for": re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", c[1]),
            "status": "released",
            "device": device_for(name),
            "preview": preview_of(c[2]),
            "parts": stls,
            "scad": scad,
        })

    # In development: | Design | Status | Preview | Source |
    for row in rows_of("In development", "Engineering & materials"):
        c = cells(row)
        if len(c) < 4 or c[0].startswith("-") or c[0] in ("Design",):
            continue
        raw = re.sub(r"\*+", "", c[0])
        name = raw.split("—")[0].strip()
        if not name or name == "Design":
            continue
        srcs = links(c[3], "scad")
        if not srcs:
            continue
        scad = srcs[0]["file"]
        entry = {
            "id": re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-"),
            "name": name,
            "for": raw.split("—", 1)[1].strip() if "—" in raw else "",
            "status": "in-development",
            "note": re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", c[1]),
            "device": device_for(name),
            "preview": preview_of(c[2]),
            "parts": [],
            "scad": scad,
        }
        if scad in RENDER_PRESETS:
            entry["parts"] = [
                {"name": p, "file": f"preview/{Path(scad).stem}_{p}.stl",
                 "preview_mesh": True}
                for p in RENDER_PRESETS[scad]
            ]
        sets.append(entry)
    return sets


# ── .scad customizer annotations ─────────────────────────────────────────
def parse_scad(path: Path):
    text = path.read_text(errors="replace")
    lines = text.splitlines()

    # Header description: leading // block, first two content lines.
    header = []
    for ln in lines:
        if ln.startswith("//"):
            t = ln.lstrip("/ ").rstrip()
            if t and not set(t) <= {"=", "-", "="}:
                header.append(t)
        elif ln.strip():
            break
    title = header[0] if header else path.stem

    groups = []
    cur = None
    for ln in lines:
        g = re.match(r"^/\*\s*\[(.+?)\]\s*(?:—|-)?\s*(.*?)\s*\*/", ln)
        if g:
            cur = {"name": g.group(1).strip(), "note": g.group(2).strip(" -—*/"),
                   "params": []}
            groups.append(cur)
            continue
        p = re.match(
            r"^(\w+)\s*=\s*([^;]+);\s*(?://\s*(.*))?$", ln)
        if p and cur is not None:
            name, val, comment = p.group(1), p.group(2).strip(), (p.group(3) or "").strip()
            if name.startswith("$"):
                continue
            enum = None
            rng = None
            em = re.match(r'^\[((?:"[^"]*"(?:\s*,\s*)?)+)\]', comment)
            if em:
                enum = re.findall(r'"([^"]*)"', em.group(1))
                comment = comment[em.end():].strip()
            rm = re.match(r"^\[(-?[\d.]+):(-?[\d.]+):(-?[\d.]+)\]", comment)
            if rm:
                rng = [float(rm.group(1)), float(rm.group(2)), float(rm.group(3))]
                comment = comment[rm.end():].strip()
            cur["params"].append({
                "name": name, "default": val.strip('"'),
                **({"enum": enum} if enum else {}),
                **({"range": rng} if rng else {}),
                **({"comment": comment} if comment else {}),
            })
    groups = [g for g in groups if g["params"]]
    return {"title": title, "groups": groups}


# ── preview mesh rendering (openscad; coarse curves, binary STL) ─────────
def render_previews():
    PREVIEW_DIR.mkdir(parents=True, exist_ok=True)
    for scad, parts in RENDER_PRESETS.items():
        for part in parts:
            out = PREVIEW_DIR / f"{Path(scad).stem}_{part}.stl"
            cmd = [
                "openscad", "-o", str(out),
                "-D", f'part="{part}"',
                "-D", "$fa=6", "-D", "$fs=0.8",
                "--export-format", "binstl",
                str(ENC / scad),
            ]
            print("render:", out.name)
            subprocess.run(cmd, check=True, capture_output=True)


def main():
    md = (ENC / "README.md").read_text(errors="replace")
    sets = parse_tables(md)
    scads = {}
    for s in sets:
        if s["scad"] and s["scad"] not in scads:
            scads[s["scad"]] = parse_scad(ENC / s["scad"])
    for s in sets:
        for p in s["parts"]:
            p["print_note"] = part_note(p["file"])
            p["material"] = ("TPU 90–95A" if "gasket" in p["file"].lower()
                             else "PETG / ASA (PLA indoors)")
    data = {
        "generated_by": "canary-local/tools/gen_enclosures.py",
        "source": "docs/hardware/enclosure",
        "print_settings": PRINT_SETTINGS,
        "sets": sets,
        "scads": scads,
    }
    if "--render" in sys.argv:
        render_previews()
    OUT_JSON.write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n")
    released = sum(1 for s in sets if s["status"] == "released")
    print(f"OK: {len(sets)} sets ({released} released, {len(sets)-released} in-dev), "
          f"{len(scads)} scads → {OUT_JSON.relative_to(REPO)}")


if __name__ == "__main__":
    main()


# ═════════════════════════════════════════════════════════════════════════
# Build-it data (BOM / assembly / SBOM) → devices/build.json
#
# Same philosophy as the enclosure catalog: parse the sources maintainers
# already edit (docs/hardware/bom_*.csv, the enclosure README's Assembly
# sections, sbom/README.md), emit JSON, drift-gate in CI. The page can
# then say "how to build it" without a second copy that rots.
# ═════════════════════════════════════════════════════════════════════════
import csv

BUILD_JSON = REPO / "canary-local/devices/build.json"
HW = REPO / "docs/hardware"

BOM_MAP = [
    # (csv, device_id, refdes_prefix or None). A prefix keeps rows whose
    # RefDes starts with it PLUS unprefixed shared rows (e.g. PSU1) — the
    # display CSV interleaves W-* (watch) and D-* (dash) lines.
    ("bom_canary_wap.csv", "canary-wap", None),
    ("bom_canary_vision.csv", "canary-vision", None),
    ("bom_canary_sense.csv", "canary-sense", None),
    ("bom_canary_display.csv", "canary-display-watch", "W-"),
    ("bom_canary_display.csv", "canary-display-dash", "D-"),
]

# Live supply-chain overlay (scripts/bom_pricing.py → docs/hardware/pricing.json).
# A committed input, so the drift gate stays deterministic; absent file = no
# overlay, the CSVs' indicative prices stand alone.
PRICING_PATH = HW / "pricing.json"
PRICING = (json.loads(PRICING_PATH.read_text(encoding="utf-8"))
           if PRICING_PATH.exists() else {})


def parse_bom(name, prefix=None):
    rows = []
    req_total = 0.0
    full_total = 0.0
    req_live = 0.0
    full_live = 0.0
    parts = PRICING.get("parts") or {}
    with open(HW / name, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if not r.get("RefDes"):
                continue
            if prefix is not None:
                ref = r["RefDes"]
                mine = ref.startswith(prefix)
                shared = "-" not in ref  # unprefixed rows (PSU1…) serve both
                if not (mine or shared):
                    continue
            try:
                ext = float(r.get("ExtUSD") or 0)
            except ValueError:
                ext = 0.0
            required = (r.get("Required") or "").strip().lower() == "required"
            # Distributor-verified price, when the nightly snapshot has one;
            # the CSV's indicative ExtUSD is the fallback for the live totals.
            part = parts.get(r.get("MPN", ""))
            live = None
            qty = r.get("Qty", "1").strip()
            if (part and part.get("provenance") in ("digikey", "mouser")
                    and part.get("unit_usd") is not None and qty.isdigit()):
                live = {
                    "unit_usd": part["unit_usd"],
                    "stock": part.get("stock"),
                    "src": part["provenance"],
                }
            ext_live = (live["unit_usd"] * int(qty)) if live else ext
            if required:
                req_total += ext
                req_live += ext_live
            full_total += ext
            full_live += ext_live
            rows.append({
                "ref": r["RefDes"],
                "qty": r.get("Qty", "1"),
                "required": required,
                "category": r.get("Category", ""),
                "desc": r.get("Description", ""),
                "mpn": r.get("MPN", ""),
                "mfr": r.get("Manufacturer", ""),
                "usd": ext,
                "notes": r.get("Notes", ""),
                **({"live": live} if live else {}),
            })
    return {
        "source": f"docs/hardware/{name}",
        "rows": rows,
        "required_usd": round(req_total, 2),
        "full_usd": round(full_total, 2),
        **({"required_usd_live": round(req_live, 2),
            "full_usd_live": round(full_live, 2),
            "pricing_as_of": PRICING.get("as_of")} if PRICING else {}),
    }


def parse_assembly(md):
    """Every '## Assembly' block → numbered steps; device inferred from the
    block's own vocabulary (deterministic keywords, tested)."""
    out = {}
    for m in re.finditer(r"^## Assembly\s*$(.*?)(?=^## |\Z)", md, re.M | re.S):
        body = m.group(1)
        steps = [re.sub(r"\s+", " ", s).strip()
                 for s in re.findall(r"^\d+\.\s+(.*?)(?=^\d+\.|\Z)", body, re.M | re.S)]
        steps = [re.sub(r"\*+", "", s) for s in steps if s]
        if not steps:
            continue
        text = body.lower()
        if "ov5647" in text or "grove" in text or "lens" in text:
            dev = "canary-vision"
        elif "lipo" in text or "wire channel" in text or "magnet" in text:
            dev = "canary-wap"
        elif "radar" in text or "mr60" in text:
            dev = "canary-sense"
        else:
            continue
        out[dev] = {
            "source": "docs/hardware/enclosure/README.md §Assembly",
            "steps": steps,
        }
    return out


SBOM_INFO = {
    "note": "Software Bill of Materials: CycloneDX 1.5 JSON, generated in CI "
            "on every main push (sbom.yml) — Rust kernel, Node tools, and the "
            "ESP32 firmware stack (esp-idf, FreeRTOS, mbedtls, lwip, cJSON…). "
            "Download from the SBOM Generation workflow's artifacts.",
    "source": "sbom/README.md",
    "link": "https://github.com/kmay89/securaCV/actions/workflows/sbom.yml",
}


def build_main():
    md = (ENC / "README.md").read_text(errors="replace")
    assembly = parse_assembly(md)
    devices = {}
    for name, dev_id, prefix in BOM_MAP:
        devices.setdefault(dev_id, {})["bom"] = parse_bom(name, prefix)
    for d, a in assembly.items():
        devices.setdefault(d, {})["assembly"] = a
    data = {
        "generated_by": "canary-local/tools/gen_enclosures.py",
        "sbom": SBOM_INFO,
        **({"pricing": {
            "source": "docs/hardware/pricing.json",
            "as_of": PRICING.get("as_of"),
            "note": "Nightly distributor snapshot (scripts/bom_pricing.py); "
                    "rows without a live match keep the CSV's indicative price.",
        }} if PRICING else {}),
        "devices": devices,
    }
    BUILD_JSON.write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n")
    print(f"OK build.json: {len(devices)} devices, "
          f"assembly for {sorted(assembly)}")


build_main()


# ═════════════════════════════════════════════════════════════════════════
# Workshop data (the production-workshop journey) → devices/workshop.json
#
# The Tesla-configurator promise, kept honest: every option, package,
# consequence, BOM link and firmware flag below is PARSED from — or
# VERIFIED against — the sources maintainers already edit:
#
#   · .scad customizer groups        → the options and their consequences
#     (the `// LiPo -> battery bay (enlarges the case)` comments ARE the
#      checklist copy — written by the enclosure's own author)
#   · README "example presets" table → packages, with rendered outer dims
#   · README variant tables (sets)   → each package's committed STL parts
#   · docs/hardware/bom_*.csv        → option ↔ RefDes links + build recipes
#     (the CSV's own `# ... build (REF+REF+…)` comment rows)
#   · firmware/configs/*/config.h    → FEATURE_* flags per flavor
#   · template_*.svg                 → 1:1 paper drill templates
#
# OPTION_LINKS is the one curated mapping (which RefDes serves which
# checkbox) — and every ref/flag it names is verified to exist in the
# parsed BOM/configs, so a renamed part or flag fails the drift gate
# instead of silently lying on the page.
# ═════════════════════════════════════════════════════════════════════════

WORKSHOP_JSON = REPO / "canary-local/devices/workshop.json"
FWCONF = REPO / "firmware/configs"

# device id → (configs project dir, flavors to publish)
FW_FLAVORS = {
    "canary-wap": ("canary-wap", ["default", "mobile"]),
    "canary-vision": ("canary-vision", ["default"]),
    "canary-sense": ("canary-sense", ["default", "wellbeing"]),
    "canary-display-watch": ("canary-display", ["watch"]),
    "canary-display-dash": ("canary-display", ["dash"]),
}

# (scad, option) → BOM RefDes + firmware flags. Refs justified by the CSV
# descriptions (e.g. M1 = "L76K GNSS receiver module" ↔ opt_gps = "L76K
# GPS module"); alternates ride along so the page can show them.
OPTION_LINKS = {
    ("canary_wap_enclosure.scad", "opt_camera"): {
        "bom": ["CW1", "ADH1"], "fw": ["FEATURE_CAMERA_PEEK"]},
    ("canary_wap_enclosure.scad", "opt_buzzer"): {
        "bom": ["BZ1", "R1", "Q1", "Dfb1"], "fw": ["FEATURE_CHIRP"]},
    ("canary_wap_enclosure.scad", "opt_led"): {
        "bom": ["DLED1", "R2", "C1", "LP1"], "fw": []},
    ("canary_wap_enclosure.scad", "opt_battery"): {
        "bom": ["BT1", "BT1-ALT1", "BT1-ALT2"], "fw": []},
    ("canary_wap_enclosure.scad", "opt_gps"): {
        "bom": ["M1"], "fw": ["FEATURE_GNSS"]},
    ("canary_wap_enclosure.scad", "opt_tamper"): {
        "bom": ["SW2", "SW2-ALT", "MAG1"], "fw": ["FEATURE_TAMPER_GPIO"]},
    ("canary_wap_enclosure.scad", "opt_touch"): {
        "bom": ["TP1", "R6"], "fw": []},
    ("canary_wap_enclosure.scad", "opt_antenna"): {
        "bom": ["ANT1"], "fw": []},
    ("canary_wap_enclosure.scad", "opt_seal"): {
        "bom": ["FIL1", "PLUG1", "VENT1", "ADH1"], "fw": []},
    ("canary_wap_enclosure.scad", "opt_mount"): {
        "bom": ["SCR4"], "fw": []},
    ("canary_watch_station.scad", "opt_batt"): {
        "bom": ["W-BT1"], "fw": []},
    # Sense options: LED + lux are ON the MR60BHA2 kit board (no separate
    # BOM row); the enclosure options link to bom_canary_sense.csv refs.
    ("canary_sense_enclosure.scad", "opt_led"): {
        "bom": [], "fw": ["FEATURE_STATUS_LED"]},
    ("canary_sense_enclosure.scad", "opt_lux"): {
        "bom": [], "fw": ["FEATURE_AMBIENT_LIGHT"]},
    ("canary_sense_enclosure.scad", "opt_vent"): {"bom": [], "fw": []},
    ("canary_sense_enclosure.scad", "opt_tamper"): {
        "bom": ["SW2", "MAG1"], "fw": []},
    ("canary_sense_enclosure.scad", "opt_seal"): {
        "bom": ["FIL1"], "fw": []},
    ("canary_sense_enclosure.scad", "opt_mount"): {
        "bom": ["SCR4"], "fw": []},
}

# README preset-table name → variant-set id (both must exist; verified).
WAP_PRESET_SETS = {
    "battery_full": "wap-battery",
    "compact_plain": "wap-compact",
    "battery_weather": "wap-weather",
}

# "What's on it" keyword → option (parsing the README's own words).
PRESET_KEYWORDS = [
    ("camera", "opt_camera", True), ("no camera", "opt_camera", False),
    ("buzzer", "opt_buzzer", True), ("led", "opt_led", True),
    ("lipo", "opt_battery", True), ("gps", "opt_gps", True),
    ("tamper", "opt_tamper", True), ("seal", "opt_seal", True),
    ("mount", "opt_mount", True),
]

# 1:1 paper drill templates (rendered by render.sh from
# canary_templates_2d.scad; print at 100% — the 20 mm calibration square
# in the corner must measure exactly 20 mm).
TEMPLATES = {
    "template_studs.svg": {
        "label": "Keyhole / T-stud pair",
        "devices": ["canary-wap", "canary-sense"],
        "note": "generic two-stud pattern — set the stud gap per case",
    },
    "template_bracket.svg": {
        "label": "Wall-bracket screw + keyhole pattern",
        "devices": ["canary-vision", "canary-sense"],
        "note": "mirrors the Vision/Sense wall bracket defaults",
    },
    "template_doorbell.svg": {
        "label": "Doorbell plate: screws + cable oval + outline",
        "devices": ["canary-vision"],
        "note": "mirrors the doorbell plate defaults",
    },
}


def parse_features(project: str, flavor: str):
    path = FWCONF / project / flavor / "config.h"
    feats = {}
    for ln in path.read_text(errors="replace").splitlines():
        m = re.match(r"^#define\s+(FEATURE_\w+)\s+(\d)\s*(?://\s*(.*))?$", ln)
        if m:
            feats[m.group(1)] = {
                "on": m.group(2) == "1",
                **({"note": m.group(3).strip()} if m.group(3) else {}),
            }
    if not feats:
        raise SystemExit(f"workshop: no FEATURE_* flags parsed from {path}")
    return feats


def parse_recipes(csv_name: str):
    """The BOM CSV's own `# <name> (REF+REF+…)` summary rows — named build
    recipes with their indicative subtotal and note, maintained in the CSV."""
    recipes = []
    with open(HW / csv_name, newline="", encoding="utf-8") as f:
        for row in csv.reader(f):
            if not row or not row[0].startswith("#"):
                continue
            m = re.match(r"^#\s*(.+?)\s*\(([^)]+)\)\s*$", row[0])
            if not m or "+" not in m.group(2):
                continue
            usd = next((c for c in row[1:] if re.fullmatch(r"\d+\.\d\d", c)), None)
            note = next((c for c in reversed(row[1:]) if c and c != usd), "")
            recipes.append({
                "label": m.group(1), "formula": m.group(2),
                **({"usd": float(usd)} if usd else {}),
                **({"note": note} if note else {}),
            })
    return recipes


def scad_options(scads: dict, scad: str, bom_rows_by_dev: dict, dev: str,
                 features_all: dict):
    """opt_* booleans + their enum companions from the parsed customizer
    groups, enriched with verified BOM/firmware links."""
    out = []
    parsed = scads.get(scad)
    if not parsed:
        return out
    refs = {r["ref"] for r in bom_rows_by_dev.get(dev, [])}
    flags = set()
    for fl in features_all.get(dev, {}).values():
        flags |= set(fl)
    for g in parsed["groups"]:
        has_opt = any(p["name"].startswith("opt_") for p in g["params"])
        if not has_opt:
            continue
        for p in g["params"]:
            is_opt = p["name"].startswith("opt_")
            is_enum_companion = "enum" in p and p["name"] in ("mount_style",)
            if not (is_opt or is_enum_companion):
                continue
            comment = p.get("comment", "")
            label, _, consequence = comment.partition("->")
            link = OPTION_LINKS.get((scad, p["name"]), {"bom": [], "fw": []})
            for ref in link["bom"]:
                if ref not in refs:
                    raise SystemExit(
                        f"workshop: {scad}:{p['name']} names BOM ref {ref} "
                        f"absent from {dev}'s parsed BOM — fix OPTION_LINKS "
                        f"or the CSV")
            for f in link["fw"]:
                if f not in flags:
                    raise SystemExit(
                        f"workshop: {scad}:{p['name']} names {f} absent "
                        f"from {dev}'s parsed config.h flags")
            out.append({
                "id": p["name"],
                "group": g["name"].split("—")[0].split(" you have")[0].strip(),
                "label": label.strip() or p["name"],
                "consequence": consequence.strip(),
                "default": p["default"] == "true",
                **({"enum": p["enum"]} if "enum" in p else {}),
                **({"bom": link["bom"]} if link["bom"] else {}),
                **({"fw": link["fw"]} if link["fw"] else {}),
            })
    return out


def wap_packages(md: str, sets_by_id: dict):
    """README 'The three committed example presets' table → packages with
    real rendered dims, option vectors parsed from its own words, and the
    variant set's committed STL parts."""
    m = re.search(r"^The three committed example presets:\s*$(.*?)(?=^## |\Z)",
                  md, re.M | re.S)
    if not m:
        raise SystemExit("workshop: README preset table not found")
    pkgs = []
    for row in re.findall(r"^\|(.+)\|$", m.group(1), re.M):
        c = [x.strip() for x in row.split("|")]
        if len(c) < 3 or c[0].startswith("-") or c[0] in ("Preset",):
            continue
        name = re.sub(r"\*+", "", c[0]).strip()
        if name not in WAP_PRESET_SETS:
            continue
        set_id = WAP_PRESET_SETS[name]
        st = sets_by_id.get(set_id)
        if not st:
            raise SystemExit(f"workshop: preset {name} maps to missing set {set_id}")
        contents = re.sub(r"\*+", "", c[2]).strip()
        low = contents.lower()
        opts = {}
        base = low.split("+")[0]
        if name == "battery_weather" and "battery_full" in low:
            # "battery_full + gasket seal + …" — inherit, then add
            opts = dict(pkgs[[p["id"] for p in pkgs].index("battery_full")]["options"])
        for kw, opt, val in PRESET_KEYWORDS:
            if kw in low:
                # a mention always wins (battery_weather inherits camera=True
                # from battery_full, then "+ gasket seal" flips seal on);
                # "no camera" is listed after "camera" so negation lands last
                opts[opt] = val
        for o in ("opt_camera", "opt_buzzer", "opt_led", "opt_battery",
                  "opt_gps", "opt_tamper", "opt_seal", "opt_mount",
                  "opt_touch", "opt_antenna"):
            opts.setdefault(o, False)
        pkgs.append({
            "id": name,
            "label": name.replace("_", " "),
            "set": set_id,
            "dims_mm": re.sub(r"\*+", "", c[1]).strip(),
            "contents": contents,
            "options": opts,
            "parts": st["parts"],
            **({"preview": st["preview"]} if st.get("preview") else {}),
        })
    if len(pkgs) != 3:
        raise SystemExit(f"workshop: expected 3 wap presets, parsed {len(pkgs)}")
    return pkgs


def sets_as_packages(sets, dev, exclude=()):
    """Devices without a preset table: each released variant set IS a
    package (committed, print-validated); in-dev sets ride along marked."""
    out = []
    for s in sets:
        if s["device"] != dev or s["id"] in exclude or not s["parts"]:
            continue
        out.append({
            "id": s["id"],
            "label": s["name"],
            "set": s["id"],
            "contents": s.get("for") or s.get("note", ""),
            "status": s["status"],
            "parts": s["parts"],
            **({"preview": s["preview"]} if s.get("preview") else {}),
        })
    return out


def workshop_main():
    md = (ENC / "README.md").read_text(errors="replace")
    sets = parse_tables(md)
    sets_by_id = {s["id"]: s for s in sets}
    scads = {}
    for s in sets:
        if s["scad"] and s["scad"] not in scads:
            scads[s["scad"]] = parse_scad(ENC / s["scad"])

    bom_rows = {}
    for name, dev_id, prefix in BOM_MAP:
        bom_rows[dev_id] = parse_bom(name, prefix)["rows"]

    features = {}
    for dev, (project, flavors) in FW_FLAVORS.items():
        features[dev] = {fl: parse_features(project, fl) for fl in flavors}

    for f, t in TEMPLATES.items():
        if not (ENC / f).exists():
            raise SystemExit(f"workshop: template {f} missing from {ENC}")

    fasteners = re.search(r"\*\*Fasteners\*\*\s*\|\s*([^|]+)\|", md)

    devices = {
        "canary-wap": {
            "scad": "canary_wap_enclosure.scad",
            "options": scad_options(scads, "canary_wap_enclosure.scad",
                                    bom_rows, "canary-wap", features),
            "packages": wap_packages(md, sets_by_id),
            "default_package": "battery_full",
            "addons": [{
                "id": "solar_thermal_kit",
                "label": "Solar / thermal outdoor kit",
                "set": "thermal-outdoor-kit",
                "when": "opt_seal",
                "blurb": "solar radiation shield + desiccant tray — for a "
                         "case that lives in the sun",
                "parts": sets_by_id["thermal-outdoor-kit"]["parts"],
            }],
            "coupon": sets_by_id["wap-clip-coupon"]["parts"][0],
            "always": ([{"label": "Fasteners",
                         "note": re.sub(r"\*+", "", fasteners.group(1)).strip()}]
                       if fasteners else []),
            "recipes": parse_recipes("bom_canary_wap.csv"),
            "templates": ["template_studs.svg"],
        },
        "canary-vision": {
            "scad": "canary_vision_enclosure.scad",
            "options": [],
            "packages": sets_as_packages(sets, "canary-vision",
                                         exclude=("vision-mount-kit",)),
            "addons": [{
                "id": "vision_mount_kit",
                "label": "Wall bracket + GoPro knob",
                "set": "vision-mount-kit",
                "blurb": "bracket mount kit for any Vision case",
                "parts": sets_by_id["vision-mount-kit"]["parts"],
            }],
            "recipes": parse_recipes("bom_canary_vision.csv"),
            "templates": ["template_bracket.svg", "template_doorbell.svg"],
        },
        "canary-sense": {
            "scad": "canary_sense_enclosure.scad",
            "options": scad_options(scads, "canary_sense_enclosure.scad",
                                    bom_rows, "canary-sense", features),
            "packages": sets_as_packages(sets, "canary-sense"),
            "templates": ["template_bracket.svg"],
        },
        "canary-display-watch": {
            "scad": "canary_watch_station.scad",
            "options": scad_options(scads, "canary_watch_station.scad",
                                    bom_rows, "canary-display-watch", features),
            "packages": sets_as_packages(sets, "canary-display-watch"),
        },
        "canary-display-dash": {
            "scad": "canary_dash_display.scad",
            "options": [],
            "packages": sets_as_packages(sets, "canary-display-dash"),
        },
    }
    for dev in devices:
        devices[dev]["firmware"] = {
            "project": FW_FLAVORS[dev][0],
            "source": f"firmware/configs/{FW_FLAVORS[dev][0]}",
            "flavors": features[dev],
        }

    data = {
        "generated_by": "canary-local/tools/gen_enclosures.py",
        "sources": [
            "docs/hardware/enclosure/README.md",
            "docs/hardware/enclosure/*.scad (customizer annotations)",
            "docs/hardware/bom_*.csv (rows + build-recipe comments)",
            "firmware/configs/*/config.h (FEATURE_* flags)",
        ],
        "calibration_note": "print templates at 100% / \"actual size\" — the "
                            "20 mm calibration square must measure exactly 20 mm",
        "devices": devices,
        "templates": TEMPLATES,
    }
    WORKSHOP_JSON.write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n")
    n_opts = sum(len(d["options"]) for d in devices.values())
    n_pkgs = sum(len(d["packages"]) for d in devices.values())
    print(f"OK workshop.json: {len(devices)} devices, {n_pkgs} packages, "
          f"{n_opts} linked options")


workshop_main()
