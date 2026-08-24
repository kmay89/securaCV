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
    # board-specific firmware-display cases → their display device page (not
    # universal, or they'd show on every WAP/Vision/Sense/hub page)
    (r"7. touch dashboard case", "canary-display-dash"),   # ESP32-S3-Touch-LCD-7
    (r"touch watch-display", "canary-display-watch"),      # ESP32-S3-Touch-LCD-1.69
    (r"C6 display pocket", "canary-display-watch"),        # ESP32-C6-LCD-1.47 (glance)
    (r"Sense bedside|Sense in-wall", "canary-sense"),
    (r"Thermal / outdoor", "canary-wap"),
    (r"Combo", "canary-vision"),
]

# .scad files in the enclosure folder that are TOOLING, not products — they
# render no printable part and have no row in README.md's variant tables.
# enclosures.json builds its scad map from those tables and so skips them
# naturally; catalog.json globs the folder, so it has to be told. Without this
# a verification harness shows up in the catalog as a product with no variants,
# and catalog.test.js fails it for not being in the enclosures.json inventory.
NON_PRODUCT_SCADS = {
    "canary_s3_lcd7_fitcheck.scad",   # 7" bezel/tray assembly-interference check
    "canary_s3_lcd7_qr.scad",         # generated help-QR bit matrix (gen_qr.py)
    "canary_vent_lib.scad",           # brand vent pattern library — shapes, not a part
    "canary_cradle_lib.scad",         # the wall dock's INTERFACE — studs, clips and
                                      # the pockets that swallow them. Each case
                                      # exports its own part="cradle" sized to its
                                      # back; the library itself prints nothing.
    "canary_mark_lib.scad",           # THE BIRD — the shared mark, geometry not a part
    "canary_panel_lib.scad",          # the PANEL REGISTRY — describes the boards a
                                      # case is built AROUND, not a thing you print
    "canary_mark_lib.scad",           # the HOUSE MARK — the bird as printable
                                      # geometry for other parts to wear, not a
                                      # part in its own right
    "canary_s3_lcd7_stamp.scad",      # generated build stamp (gen_stamp.py)
    "canary_core_lib.scad",           # shared 2D/3D helpers + process floors —
                                      # the catalog's geometry vocabulary, not a part
    "canary_mount_lib.scad",          # the stud/keyhole hanging interface — the
                                      # CONTRACT every case and mount mates through
    "canary_snap_lib.scad",           # snap-fit engineering (strain budgets, the
                                      # derived-window rule) — math, not a part
    "canary_port_lib.scad",           # connector opening profiles + the insertion-
                                      # length gate — cuts other parts make
    "canary_board_lib.scad",          # the BOARD REGISTRY — measured module
                                      # dimensions cases are built AROUND
    "canary_color_lib.scad",          # the COLORWAY registry — spool palettes,
                                      # not a part
}

# Preview meshes rendered for in-development designs the device sheets
# feature. part → -D part=<...>; coarse curves keep files small.
RENDER_PRESETS = {
    "canary_watch_station.scad": ["drum", "bezel", "stand"],
    "canary_dash_display.scad": ["frame", "back", "stand", "cradle"],
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
    ("display_back", "prints PADS-UP (inner face on the bed) — the dock pads and their pocket bridges are the last layers, not the first"),
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

# The snapshot's URL checker, imported rather than re-implemented — two
# copies of a security rule is how one of them quietly stops matching.
sys.path.insert(0, str(REPO / "scripts"))
from bom_pricing import safe_product_url  # noqa: E402


def live_overlay(part):
    """The per-row `live` object, or None when the row isn't distributor-verified.

    Only "digikey"/"mouser" provenance counts as live: a part demoted to
    "carried" keeps its last-known numbers in the snapshot for reference but
    must never be republished here as fresh — and so it never carries a link
    to a listing that may no longer exist.
    """
    if not part or part.get("provenance") not in ("digikey", "mouser"):
        return None
    if part.get("unit_usd") is None:
        return None
    src = part["provenance"]
    live = {
        "unit_usd": part["unit_usd"],
        "stock": part.get("stock"),
        "src": src,
    }
    # Re-checked on the way out, not just on the way in: the snapshot is a
    # committed file a human can edit. A URL that doesn't pass is simply
    # absent, and the row renders exactly as it did before links existed.
    url = safe_product_url(part.get("url"), src)
    if url:
        live["url"] = url
    return live


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
            qty = r.get("Qty", "1").strip()
            live = live_overlay(part) if qty.isdigit() else None
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
                # a real manufacturer part number a distributor can resolve
                # (drives the Build-it page's order-the-parts copy panel)
                "orderable": bool(part) and part["sourcing"] == "orderable",
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


def parse_recipes(csv_name: str):
    """The BOM CSV's own `# <name> (REF+REF+…)` summary rows — named build
    recipes with their indicative subtotal and note, maintained in the CSV.
    (Defined here, above build_main, because both the Build-it presets and
    the workshop read them.)"""
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


def resolve_recipe(formula, refs):
    """A recipe formula → the explicit optional-ref list it names, or None
    when any token is prose ('LiPo', 'sealed input') rather than a RefDes.
    'req' means the required rows (the page always includes those), and
    simple ranges expand (SCR5-7 → SCR5, SCR6, SCR7). Resolved recipes
    become one-click build presets on the Build-it page; prose ones stay
    display-only in the workshop."""
    out = []
    for tok in (t.strip() for t in formula.split("+")):
        if not tok or tok.lower() == "req":
            continue
        if tok in refs:
            out.append(tok)
            continue
        m = re.match(r"^([A-Za-z]+)(\d+)-(\d+)$", tok)
        if m:
            span = [f"{m.group(1)}{n}"
                    for n in range(int(m.group(2)), int(m.group(3)) + 1)]
            if span and all(r in refs for r in span):
                out.extend(span)
                continue
        return None
    return out


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
        bom = parse_bom(name, prefix)
        # The CSV's own recipe rows, resolved to refs → one-click presets.
        # Only optional refs matter (required rows are always in a build).
        opt_refs = {r["ref"] for r in bom["rows"] if not r["required"]}
        all_refs = {r["ref"] for r in bom["rows"]}
        presets = []
        for rec in parse_recipes(name):
            refs = resolve_recipe(rec["formula"], all_refs)
            if refs is None:
                continue
            presets.append({
                "label": rec["label"],
                "refs": [r for r in refs if r in opt_refs],
                **({"usd": rec["usd"]} if "usd" in rec else {}),
                **({"note": rec["note"]} if "note" in rec else {}),
            })
        if presets:
            bom["recipes"] = presets
        devices.setdefault(dev_id, {})["bom"] = bom
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


# ── option classification (shared by the workshop transform and the catalog) ──
# The audience split, the EXPLICIT signal that replaces the leaky `opt_` name
# prefix. A user option is any boolean param (default true/false) or option-enum
# (mount_style) that isn't a variant selector, the `part` render selector, or a
# structural/manufacturing engineering toggle. Both scad_options (workshop.json)
# and catalog_options (catalog.json) classify with these, so the workshop can't
# silently diverge from the manifest.
VARIANT_SELECTOR_PARAMS = {
    "preset", "host", "radar", "stack", "model", "variant", "mode", "mount",
    "headers",  # C6 display: stripped board vs as-shipped (headers + pillars)
    "panel_variant",  # 7" dashboard: the -7 and the -7B, half a turn apart
    # C3 pocket case: WHICH BOARD, not which option. usb_c is the
    # ESP32-C3-LCD-1.47 (receptacle on the PCB back); usb_a is the
    # ESP32-S3-LCD-1.47, whose PCB ends in a series-A male plug. Same outline,
    # same panel, same buttons — a different connector and therefore a
    # different case. It belongs here rather than in OPTION_ENUM_PARAMS for
    # exactly that reason: a buyer does not choose it, their board does.
    "port",
}
OPTION_ENUM_PARAMS = {
    "mount_style", "battery", "side_exit",
    # 7" dashboard palette. These are string enums rather than bools, and the
    # classifier below only counts bools by default — so when vent_accent (a
    # bool) became vent_ring_color (body/ink/accent), the option silently
    # vanished from the workshop instead of gaining two settings. They are
    # exactly the kind of choice a buyer makes, so they are named here.
    "bezel_color", "vent_ring_color", "qr_style",
    # C3 pocket case: what the lid's back carries — the Canary wordmark or
    # the keyhole mount. Same trap as vent_ring_color above, sprung a second
    # time: this replaced a bool (opt_keyhole) with a string enum, and the
    # control disappeared from the workshop rather than becoming two
    # settings. A buyer choosing "branded back" over "hangs on a screw" is
    # exactly the kind of choice this set is for.
    "lid_back",
}
ENGINEERING_OPTIONS = {
    "lid_ribs", "kh_lock", "hinge_teeth", "bracket_tripod", "screw_insert",
    "board_clips", "usb_cover",
}


def is_user_option(p: dict) -> bool:
    name = p["name"]
    if name == "part" or name in VARIANT_SELECTOR_PARAMS:
        return False
    if name in ENGINEERING_OPTIONS:
        return False
    is_bool = str(p.get("default", "")).lower() in ("true", "false")
    return is_bool or name in OPTION_ENUM_PARAMS


def scad_options(scads: dict, scad: str, bom_rows_by_dev: dict, dev: str,
                 features_all: dict):
    """The device's USER options from the parsed customizer groups — classified
    by `is_user_option` (audience), NOT the `opt_` name prefix — enriched with
    verified BOM/firmware links. Structural/engineering toggles and variant
    selectors are excluded; a non-`opt_` user bool (term_open, vent_back, …) on a
    configurable device would surface here just the same."""
    out = []
    parsed = scads.get(scad)
    if not parsed:
        return out
    refs = {r["ref"] for r in bom_rows_by_dev.get(dev, [])}
    flags = set()
    for fl in features_all.get(dev, {}).values():
        flags |= set(fl)
    for g in parsed["groups"]:
        for p in g["params"]:
            if not is_user_option(p):
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
                "audience": "user",
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


# ═════════════════════════════════════════════════════════════════════════
# Catalog manifest (the 3-tier Product → Variant → Option model) →
# devices/catalog.json
#
# This is Phase 2 of docs/hardware/enclosure/CATALOG_ARCHITECTURE.md: make
# the manifest that doc describes REAL and runnable, generated from the same
# sources the other emitters already parse — never hand-authored, so it
# can't drift from the SCADs (guarded by the same CI diff gate).
#
# What it adds over enclosures.json's flat sets[]:
#   · one PRODUCT per .scad, with its whole customizer inventory classified
#     by the six axes (§2 of the doc): variant-selectors vs options vs the
#     render `part` selector vs engineering/tuning knobs
#   · the ⚠ "invisible" enum axes (host/radar/stack/model/variant/mode/mount)
#     surfaced as first-class VARIANT SELECTORS instead of dropped
#   · committed sets become VARIANTS (their own parts/meshes/preview), and a
#     variant may override the product `scad` — the schema half of §7 row 1
#     (display family collapses across files; no current set triggers it yet)
#   · env rating as a FIELD with a `verified` flag (never badge a target as
#     achieved — field case stays untested per field_ratings.md)
#   · a shared, orthogonal FIT tier bound to the coupon, not per-file copy
#   · sideways alternatives (the "which one do I pick?" links)
#
# Every curated overlay (ENV/ALTERNATIVES/OPTION_DEPS) is verified against
# the parsed sources at gen time — a renamed scad/option fails the build
# instead of silently lying, the same discipline as OPTION_LINKS above.
# ═════════════════════════════════════════════════════════════════════════

CATALOG_JSON = REPO / "canary-local/devices/catalog.json"

# The canonical id for a case that fits no single device (axis 1). The doc's
# §4 identity contract replaces the nullable `device` link with this, so a
# universal record joins by device id on the same path as a device-specific one.
# (VARIANT_SELECTOR_PARAMS / OPTION_ENUM_PARAMS / ENGINEERING_OPTIONS — the
# option classification — are defined above scad_options, shared by both
# transforms.)
UNIVERSAL_ID = "_universal"

# The selector vector that produces each committed variant (axis 3 — the doc's
# `selects{}`), keyed by set id. Every param/value is verified against the
# product's own variant_axes at gen time (a typo fails the build). Sets absent
# here have no discrete selector (accessories, single in-dev previews) and emit
# an empty `selects`.
VARIANT_SELECTS = {
    "wap-compact": {"preset": "compact_plain"},
    "wap-battery": {"preset": "battery_full"},
    "wap-weather": {"preset": "battery_weather"},
    "vision-xiao-indoor": {"host": "xiao", "preset": "vision_indoor"},
    "vision-xiao-weather": {"host": "xiao", "preset": "vision_weather"},
    "vision-devkit-indoor": {"host": "devkit", "preset": "vision_indoor"},
    "sense-radome": {"radar": "bha2"},  # the MR60BHA2 radome
    # The dash ships docked. Without a pinned value this set's `selects` is
    # empty, and matchVariant() skips empty-select variants on purpose — so a
    # user who moved the mount selector and put it back on "cradle" would be
    # told their configuration is custom, with no way back to the committed
    # variant that it is byte-for-byte.
    "dashboard-display-case": {"mount": "cradle"},
}

# Env rating as data (axis: environment) — read from each model's OWN source,
# not a table here. Each rated `.scad` carries a machine-readable header line:
#   // @env cer=2 ip="~IP54" basis="weather preset"
# so the rating lives next to the geometry it describes and can't drift from it.
# CER levels are the field_ratings.md ladder (1–4). `verified` is ALWAYS False:
# a rating is design intent, EARNED per printed unit by the field_ratings.md
# test protocol, never asserted from the CAD.
CER_LEVELS = {1, 2, 3, 4}  # docs/hardware/enclosure/field_ratings.md
_ENV_KEYS = {"ip", "mil", "basis", "note", "env"}
# A key=value token: value is a properly TERMINATED double-quoted string or a
# bare run that does not start with a quote. This is strict on purpose — see
# parse_env: the whole annotation must be consumed by these tokens, so a typo
# (`cer 2`, a stray word, an unterminated quote) fails the build instead of
# being silently dropped into a truthy-but-empty rating.
_ENV_TOKEN = re.compile(r'(\w+)=("[^"]*"|[^"\s]\S*)')


def parse_env(scad_text: str):
    """A model's environment rating from its `// @env …` header line, or None
    when the model declares none. Values may be bare or double-quoted; `cer`
    coerces to int and is validated against the CER ladder; `verified` is forced
    False (honesty invariant). The parser is STRICT: any text the token grammar
    can't consume, or an annotation that yields no rating field, fails the build
    — a mistyped rating must never regenerate to a silent, empty env."""
    m = re.search(r"^\s*//\s*@env\b(.*)$", scad_text, re.M)
    if not m:
        return None
    body = m.group(1).strip()
    out, pos = {}, 0
    while pos < len(body):
        tm = _ENV_TOKEN.match(body, pos)
        if not tm:
            raise SystemExit(
                f"catalog: malformed @env — expected key=value near "
                f"'{body[pos:pos + 40]}'")
        k, v = tm.group(1), tm.group(2)
        val = v[1:-1] if v.startswith('"') else v
        if k == "cer":
            try:
                cer = int(val)
            except ValueError:
                raise SystemExit(f"catalog: @env cer='{val}' is not an integer")
            if cer not in CER_LEVELS:
                raise SystemExit(
                    f"catalog: @env cer={cer} is not a field_ratings.md CER "
                    f"level {sorted(CER_LEVELS)}")
            out["cer"] = cer
        elif k in _ENV_KEYS:
            out[k] = val
        else:
            raise SystemExit(f"catalog: @env unknown key '{k}='")
        pos = tm.end()
        while pos < len(body) and body[pos].isspace():
            pos += 1
    if not out:
        raise SystemExit(f"catalog: @env line has no rating fields: '{body}'")
    out["verified"] = False
    return out

# Sideways "which one do I pick?" links (axis 6 — alternative/remix). NOT a
# step in the narrowing funnel. Keyed by scad → list of alternative scads;
# every target is verified to exist below.
ALTERNATIVES = {
    # outdoor witness — printed pod vs bought IP67 box vs sealed field case
    "canary_field_case.scad": [
        "canary_hammond_chassis.scad", "canary_relay_solar.scad"],
    "canary_hammond_chassis.scad": [
        "canary_field_case.scad", "canary_relay_solar.scad"],
    "canary_relay_solar.scad": [
        "canary_field_case.scad", "canary_hammond_chassis.scad"],
    # watch-display board — the three ways "which display board" is encoded
    "canary_s3_touch169.scad": [
        "canary_c6_display.scad", "canary_watch_station.scad"],
    "canary_c6_display.scad": [
        "canary_s3_touch169.scad", "canary_watch_station.scad"],
    "canary_watch_station.scad": [
        "canary_s3_touch169.scad", "canary_c6_display.scad"],
    # dashboard display
    "canary_s3_lcd7.scad": ["canary_dash_display.scad"],
    "canary_dash_display.scad": ["canary_s3_lcd7.scad"],
}

# Option dependency graph (axis 4 carries requires/excludes). Minimal and
# verified: each `requires`/`excludes` names another option on the SAME
# scad; `requires_parts` names a `part` enum value the scad exposes. Keyed by
# (scad, option).
OPTION_DEPS = {
    ("canary_wap_enclosure.scad", "opt_seal"): {"requires_parts": ["gasket"]},
    ("canary_vision_enclosure.scad", "opt_seal"): {
        "requires_parts": ["gasket"], "requires": ["opt_vent"]},
    ("canary_sense_enclosure.scad", "opt_seal"): {
        "requires_parts": ["gasket"], "requires": ["opt_vent"]},
}

# The six axes, so the manifest is self-describing (mirrors §2 of the doc).
AXES_LEGEND = [
    {"n": 1, "axis": "use-case / device",
     "role": "filters — what the user has/wants to sense"},
    {"n": 2, "axis": "product / case family",
     "role": "the base geometry — one designed enclosure (≈ one .scad)"},
    {"n": 3, "axis": "variant / flavor",
     "role": "discrete; changes the printed part set (preset/host/model/…)"},
    {"n": 4, "axis": "option",
     "role": "a toggle/enum inside a variant; may carry requires/excludes"},
    {"n": 5, "axis": "fit",
     "role": "print-tolerance tier — orthogonal, belongs to the printer"},
    {"n": 6, "axis": "remix / alternative",
     "role": "a sideways 'see also', never a step in the funnel"},
]


def _tol_defaults(parsed) -> dict:
    """The tol_* print-fit knobs' defaults from a parsed scad (the fit tier
    is shared, so we read one representative product, not per-file copies)."""
    out = {}
    for g in parsed["groups"]:
        for p in g["params"]:
            if p["name"].startswith("tol_"):
                try:
                    out[p["name"]] = float(p["default"])
                except ValueError:
                    out[p["name"]] = p["default"]
    return out


def _is_bool_param(p) -> bool:
    return str(p.get("default", "")).lower() in ("true", "false")


def catalog_options(parsed, scad: str):
    """Every boolean param + option-enum (mount_style) → a catalog option,
    classified by an explicit `audience` tag (user vs engineering) rather than
    the leaky `opt_` prefix — so non-prefixed user booleans (term_open,
    vent_back, bez_on, tie_slots, screws, …) are surfaced and structural
    toggles (lid_ribs, hinge_teeth, …) are captured-but-tagged, not dropped.
    Enriched with BOM/fw links (verified via OPTION_LINKS) and the
    requires/excludes graph. Excludes the `part` render selector and the
    variant-selector enums (those are axis 3, emitted separately)."""
    opts = []
    for g in parsed["groups"]:
        for p in g["params"]:
            name = p["name"]
            if name == "part" or name in VARIANT_SELECTOR_PARAMS:
                continue
            is_opt_enum = name in OPTION_ENUM_PARAMS
            is_bool = _is_bool_param(p)
            if not (is_bool or is_opt_enum):
                continue  # numeric/tuning knob → counted as engineering below
            audience = "engineering" if name in ENGINEERING_OPTIONS else "user"
            comment = p.get("comment", "")
            label, _, consequence = comment.partition("->")
            link = OPTION_LINKS.get((scad, name), {"bom": [], "fw": []})
            dep = OPTION_DEPS.get((scad, name), {})
            opts.append({
                "id": name,
                "type": "enum" if "enum" in p else "bool",
                "label": (label.strip() or name),
                "consequence": consequence.strip(),
                "audience": audience,
                **({"enum": p["enum"]} if "enum" in p else {}),
                **({"default": p["default"] == "true"} if is_bool
                   else {"default": p["default"]}),
                **({"bom": link["bom"]} if link["bom"] else {}),
                **({"fw": link["fw"]} if link["fw"] else {}),
                **({"requires": dep["requires"]} if dep.get("requires") else {}),
                **({"excludes": dep["excludes"]} if dep.get("excludes") else {}),
                **({"requires_parts": dep["requires_parts"]}
                   if dep.get("requires_parts") else {}),
            })
    return opts


def variant_axes_of(parsed):
    """The discrete part-set selectors (axis 3) surfaced from the customizer —
    including the ⚠ invisible ones the workshop transform drops today."""
    axes = []
    for g in parsed["groups"]:
        for p in g["params"]:
            if p["name"] in VARIANT_SELECTOR_PARAMS and "enum" in p:
                axes.append({
                    "param": p["name"],
                    "values": p["enum"],
                    "default": p["default"],
                    "audience": "user",
                    **({"note": p["comment"]} if p.get("comment") else {}),
                })
    return axes


def render_parts_of(parsed):
    """The `part` enum values (minus the 'all' meta) — the printable pieces a
    configurator lists per product. Not a user variant axis."""
    for g in parsed["groups"]:
        for p in g["params"]:
            if p["name"] == "part" and "enum" in p:
                return [v for v in p["enum"] if v != "all"]
    return []


def variant_from_set(s: dict, product_scad: str) -> dict:
    """A committed set → a first-class variant. Records the `selects` vector
    that produces it (so a picker can translate the variant back into a SCAD
    render), normalizes the universal device link, and carries its own scad
    only when it differs from the product default (the display-family collapse
    hook; no current set triggers it, but the schema is ready)."""
    meshes = [
        {"name": p.get("name", ""), "file": p["file"],
         **({"preview_mesh": True} if p.get("preview_mesh") else {})}
        for p in s["parts"]
    ]
    return {
        "id": s["id"],
        "name": s["name"],
        "for": s.get("for") or s.get("note", ""),
        "status": s["status"],
        "device": s.get("device") or UNIVERSAL_ID,
        "selects": VARIANT_SELECTS.get(s["id"], {}),
        **({"scad": s["scad"]} if s.get("scad") and s["scad"] != product_scad
           else {}),
        **({"preview": s["preview"]} if s.get("preview") else {}),
        "parts": meshes,
    }


def catalog_main():
    md = (ENC / "README.md").read_text(errors="replace")
    sets = parse_tables(md)
    scad_files = sorted(p.name for p in ENC.glob("*.scad")
                        if p.name not in NON_PRODUCT_SCADS)
    scads = {name: parse_scad(ENC / name) for name in scad_files}
    # Environment rating parsed from each model's own `// @env` header line.
    env_by_scad = {name: parse_env((ENC / name).read_text(errors="replace"))
                   for name in scad_files}

    # Verify every curated overlay points at real sources (fail-fast).
    for scad, alts in ALTERNATIVES.items():
        if scad not in scads:
            raise SystemExit(f"catalog: ALTERNATIVES key {scad} is not a scad")
        for a in alts:
            if a not in scads:
                raise SystemExit(
                    f"catalog: ALTERNATIVES[{scad}] names missing scad {a}")
    for (scad, opt), dep in OPTION_DEPS.items():
        parsed = scads.get(scad)
        if not parsed:
            raise SystemExit(f"catalog: OPTION_DEPS names missing scad {scad}")
        names = {p["name"] for g in parsed["groups"] for p in g["params"]}
        if opt not in names:
            raise SystemExit(
                f"catalog: OPTION_DEPS ({scad},{opt}) — option not in the scad")
        part_vals = set(render_parts_of(parsed))
        for rp in dep.get("requires_parts", []):
            if rp not in part_vals:
                raise SystemExit(
                    f"catalog: OPTION_DEPS ({scad},{opt}) requires_parts "
                    f"'{rp}' — not a `part` value of {scad}")
        for req in dep.get("requires", []) + dep.get("excludes", []):
            if req not in names:
                raise SystemExit(
                    f"catalog: OPTION_DEPS ({scad},{opt}) references '{req}' "
                    f"— not an option on {scad}")

    sets_by_scad = {}
    for s in sets:
        sets_by_scad.setdefault(s.get("scad"), []).append(s)

    # Verify each VARIANT_SELECTS entry against the product's own variant axes
    # (the selector param must exist on that scad and the value be in its enum).
    set_to_scad = {s["id"]: s.get("scad") for s in sets}
    for set_id, sel in VARIANT_SELECTS.items():
        scad = set_to_scad.get(set_id)
        if not scad or scad not in scads:
            raise SystemExit(
                f"catalog: VARIANT_SELECTS names unknown set '{set_id}'")
        axes = {a["param"]: a["values"] for a in variant_axes_of(scads[scad])}
        for param, val in sel.items():
            if param not in axes:
                raise SystemExit(
                    f"catalog: VARIANT_SELECTS[{set_id}] param '{param}' is not "
                    f"a variant axis of {scad}")
            if val not in axes[param]:
                raise SystemExit(
                    f"catalog: VARIANT_SELECTS[{set_id}] {param}='{val}' not in "
                    f"{scad}'s enum {axes[param]}")

    products = []
    for scad in scad_files:
        parsed = scads[scad]
        my_sets = sets_by_scad.get(scad, [])
        # Canonical device ids; universal (nullable) sets normalize to _universal.
        devices = sorted({s.get("device") or UNIVERSAL_ID for s in my_sets})
        family = (device_for(parsed["title"])
                  or (devices[0] if devices and devices != [UNIVERSAL_ID] else None)
                  or "universal")
        options = catalog_options(parsed, scad)
        vaxes = variant_axes_of(parsed)
        variants = [variant_from_set(s, scad) for s in my_sets]
        released = sum(1 for v in variants if v["status"] == "released")

        # Coverage honesty: how many raw customizer knobs are NOT surfaced as
        # user variant-axes/options (engineering/tuning) — dropped on purpose,
        # counted so the manifest never pretends it captured everything.
        surfaced = ({a["param"] for a in vaxes} | {o["id"] for o in options}
                    | {"part"})
        eng = sum(1 for g in parsed["groups"] for p in g["params"]
                  if p["name"] not in surfaced)

        products.append({
            "id": scad[:-5].removeprefix("canary_"),  # stable, unique
            "scad": scad,
            "title": parsed["title"],
            "family": family,
            "device_compat": devices,
            "class": "primary" if variants else "accessory",
            **({"env": env_by_scad[scad]} if env_by_scad.get(scad) else {}),
            "fit": "standard",  # → the shared tier (top-level `fit`)
            "variant_axes": vaxes,
            "variants": variants,
            "render_parts": render_parts_of(parsed),
            "options": options,
            "engineering_param_count": eng,
            "alternatives": [a[:-5].removeprefix("canary_")
                             for a in ALTERNATIVES.get(scad, [])],
            "remix_of": None,  # no builds.json source yet — honest null
            "counts": {
                "variants": len(variants), "released": released,
                "options": len(options),
                "user_options": sum(1 for o in options
                                    if o["audience"] == "user"),
                "variant_axes": len(vaxes),
            },
        })

    # The shared, orthogonal fit tier (axis 5) — one tier, bound to the
    # UNIVERSAL coupon (canary_fit_coupon.scad: slide/press/hole/gasket/screw/
    # insert), not the WAP-only snap-clip coupon and not per-file copy. Its mesh
    # isn't committed yet, so we point at the scad and omit coupon_part rather
    # than pair the scad with a mismatched STL.
    coupon_scad = "canary_fit_coupon.scad"
    coupon_set = next((s for s in sets if s.get("scad") == coupon_scad), None)
    fit = {
        "tiers": [{
            "id": "standard",
            "note": "one shared print-fit tier; calibrate to your printer with "
                    "the coupon, then reuse the offsets across every case",
            "defaults": _tol_defaults(scads["canary_wap_enclosure.scad"]),
        }],
        "coupon_scad": coupon_scad,
        **({"coupon_variant": coupon_set["id"]} if coupon_set else {}),
        **({"coupon_part": coupon_set["parts"][0]}
           if coupon_set and coupon_set.get("parts") else {}),
    }

    data = {
        "generated_by": "canary-local/tools/gen_enclosures.py",
        "spec": "docs/hardware/enclosure/CATALOG_ARCHITECTURE.md",
        "note": "Phase-2 stub: the 3-tier Product→Variant→Option manifest, "
                "generated (not hand-authored) from the SCAD customizer "
                "inventory + README variant tables. Phase 3+ (unify the "
                "pickers, guided funnel, showroom) reads from here.",
        "axes": AXES_LEGEND,
        "fit": fit,
        "products": products,
    }
    CATALOG_JSON.write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n")
    prim = sum(1 for p in products if p["class"] == "primary")
    nvar = sum(p["counts"]["variants"] for p in products)
    nax = sum(p["counts"]["variant_axes"] for p in products)
    nopt = sum(p["counts"]["options"] for p in products)
    print(f"OK catalog.json: {len(products)} products ({prim} primary), "
          f"{nvar} variants, {nax} variant-axes, {nopt} options "
          f"→ {CATALOG_JSON.relative_to(REPO)}")


catalog_main()
