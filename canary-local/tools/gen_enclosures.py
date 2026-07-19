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
    ("bom_canary_display.csv", "canary-display-watch", "W-"),
    ("bom_canary_display.csv", "canary-display-dash", "D-"),
]


def parse_bom(name, prefix=None):
    rows = []
    req_total = 0.0
    full_total = 0.0
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
            if required:
                req_total += ext
            full_total += ext
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
            })
    return {
        "source": f"docs/hardware/{name}",
        "rows": rows,
        "required_usd": round(req_total, 2),
        "full_usd": round(full_total, 2),
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
    # honest gaps, stated
    devices.setdefault("canary-sense", {})["bom_note"] = (
        "Sense BOM pending — parts list lives in firmware/projects/canary-sense/README.md.")
    data = {
        "generated_by": "canary-local/tools/gen_enclosures.py",
        "sbom": SBOM_INFO,
        "devices": devices,
    }
    BUILD_JSON.write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n")
    print(f"OK build.json: {len(devices)} devices, "
          f"assembly for {sorted(assembly)}")


build_main()
