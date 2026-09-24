#!/usr/bin/env python3
"""gen_hardware.py — the hardware every committed preset needs, read off the CAD,
and the lid-rib headroom each one leaves.

    python3 gen_hardware.py            # regenerate hardware.json
    python3 gen_hardware.py --check    # CI gate: re-probe, diff, and join against the BOMs

WHY THIS FILE EXISTS. Every released case derives its hardware from the same
variables that draw the holes (canary_core_lib.scad hw_len / hw_screw /
hw_item / hw_echo) and echoes ONE line per render:

    HARDWARE — Vision doorbell: 6x M2 pan x 10 self-tap · 4x M2 pan x 6 …

Nothing read that line. The quantities a builder buys from are hand-typed in
docs/hardware/bom_*.csv, carried into the Lab by gen_enclosures.py, and no
gate compared them with the geometry — which is how the doorbell's fifth
insert drifted once already. This file renders every committed preset (the
sets render.sh cuts, plus the `screw_insert` service option the BOMs bill as
INS1), captures the echo, parses it into counted items and commits them to
hardware.json, byte-gated like every other generated catalog.

THE BOM JOIN. Fasteners only — the counted, bought, easy-to-get-wrong lines
(screws, heat-set inserts, the security screw, wall screws, the hinge bolt).
JOIN names which BOM row bills which echoed fastener, by kind and by the
spec the ROW'S OWN DESCRIPTION states (a row that says "FLAT head, 8-10mm"
joins only a flat head of 8 or 10 mm). Per set, a row's CSV qty must be at
least the echoed qty it bills; an echoed fastener no row bills is
"unbilled". Every disagreement is written into hardware.json (`bom_drift`)
so the committed file says it out loud, and --check fails on any drift that
is not in KNOWN_DRIFT (it is new) and on any KNOWN_DRIFT entry that no longer
happens (it was fixed — shrink the list). The CSVs are a human's to
correct: this generator reports, it never edits them.

THE RIB HEADROOM. Each set also records its lid rib (lid_rib_h / lid_rib_w)
and the headroom its own PLS-4 assert holds the rib to (`lid_headroom`, or
`cav_extra` on the Sense, whose assert reads that) — read from the case,
never recomputed here. DESIGN_RULES.md publishes those numbers for the
per-case rib decision (the "Lid rib proportions" table under "What is still
open" — cited by title, never by section number, so a new section above it
cannot silently retarget the citation); --check holds that table to this
ledger, so the prose cannot drift from the CAD.

Mechanics (the include-beside-the-case probe, the clean-render rule) are
scad_probe.py's, shared with gen_assembled_dims.py. The probe asks for the
echo stream only — no CGAL — so the whole ledger takes seconds.
"""

from __future__ import annotations

import csv
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import scad_probe  # noqa: E402  (the shared render-and-parse-echo helper, beside this file)

REPO = HERE.parent.parent.parent
HW = REPO / "docs/hardware"
OUT = HERE / "hardware.json"
DESIGN_RULES = HERE / "DESIGN_RULES.md"
# How every message names the rib table: by its title, not its section number.
RIB_TABLE = 'DESIGN_RULES.md "Lid rib proportions" table'

FASTENERS = ("screw", "insert", "security-screw", "wall-screw", "bolt")

# The committed sets: render.sh's presets (base part — the hardware line is a
# top-level echo, so any part gives it; the WAP shield is the exception, its
# render adds the longer screws that replace the lid's), plus one
# screw_insert=true set per case family, because INS1 is billed for exactly
# that option. `rib` marks the eight preset sets whose headroom RIB_TABLE publishes;
# `join` the kinds the BOM join reads from this set, `join_text` an optional
# filter on the item text (the shield set only adds its replacement screws).
_WAP = "canary_wap_enclosure.scad"
_VIS = "canary_vision_enclosure.scad"
_DOOR = "canary_vision_doorbell.scad"
_SENSE = "canary_sense_enclosure.scad"
SETS = [
    {"id": "wap.battery_full", "device": "canary-wap", "scad": _WAP,
     "overrides": {"preset": '"battery_full"', "part": '"base"'}, "rib": True},
    {"id": "wap.compact_plain", "device": "canary-wap", "scad": _WAP,
     "overrides": {"preset": '"compact_plain"', "part": '"base"'}, "rib": True},
    {"id": "wap.battery_weather", "device": "canary-wap", "scad": _WAP,
     "overrides": {"preset": '"battery_weather"', "part": '"base"'}, "rib": True},
    {"id": "wap.battery_weather.shield", "device": "canary-wap", "scad": _WAP,
     "overrides": {"preset": '"battery_weather"', "part": '"shield"'},
     "join_text": "REPLACES"},
    {"id": "wap.compact_plain+inserts", "device": "canary-wap", "scad": _WAP,
     "overrides": {"preset": '"compact_plain"', "part": '"base"', "screw_insert": "true"},
     "join": ("insert",)},
    {"id": "vision.xiao_indoor", "device": "canary-vision", "scad": _VIS,
     "overrides": {"host": '"xiao"', "preset": '"vision_indoor"', "part": '"back"'}, "rib": True},
    {"id": "vision.xiao_weather", "device": "canary-vision", "scad": _VIS,
     "overrides": {"host": '"xiao"', "preset": '"vision_weather"', "part": '"back"'}, "rib": True},
    {"id": "vision.devkit_indoor", "device": "canary-vision", "scad": _VIS,
     "overrides": {"host": '"devkit"', "preset": '"vision_indoor"', "part": '"back"'}, "rib": True},
    {"id": "vision.xiao_indoor+inserts", "device": "canary-vision", "scad": _VIS,
     "overrides": {"host": '"xiao"', "preset": '"vision_indoor"', "part": '"back"',
                   "screw_insert": "true"},
     "join": ("insert",)},
    {"id": "doorbell", "device": "canary-vision", "scad": _DOOR,
     "overrides": {"part": '"body"'}, "rib": True},
    {"id": "doorbell+inserts", "device": "canary-vision", "scad": _DOOR,
     "overrides": {"part": '"body"', "screw_insert": "true"}, "join": ("insert",)},
    {"id": "sense", "device": "canary-sense", "scad": _SENSE,
     "overrides": {"part": '"back"'}, "rib": True},
    {"id": "sense+inserts", "device": "canary-sense", "scad": _SENSE,
     "overrides": {"part": '"back"', "screw_insert": "true"}, "join": ("insert",)},
]

# The bound each case's own PLS-4 assert holds lid_rib_h to, and the WAP's
# battery hold-down coupling (its rib face sits at batt_h to keep the 1 mm
# swelling allowance, so its height is not free even where headroom is).
RIB_BOUND = {
    _WAP: ("lid_headroom", "batt_hold && e_battery"),
    _VIS: ("lid_headroom", "false"),
    _DOOR: ("lid_headroom", "false"),
    _SENSE: ("cav_extra", "false"),
}

# device -> (BOM csv, [(kind, pattern on the echoed item text, RefDes)]). Each
# pattern is the ROW'S description, not the echo's: a row that names one head
# and a length range joins only that; a mixed row (the Vision's SCR5) says so.
JOIN = {
    "canary-wap": ("bom_canary_wap.csv", [
        ("screw", r"^M2 flat x (8|10) self-tap", "SCR3"),          # FLAT head, 8-10mm
        ("insert", r"^M2 heat-set insert 3\.5 OD x 4\b", "INS1"),   # 3.5mm OD x 4.0mm
        ("wall-screw", r"^#6 pan wall screw \(keyholes\)", "SCR4"),  # keyhole mount
    ]),
    "canary-vision": ("bom_canary_vision.csv", [
        ("screw", r"^M2 (pan|flat) x (6|8|10) self-tap", "SCR5"),  # 6-10mm, pan + per-case head
        ("insert", r"^M2 heat-set insert 3\.5 OD x 4\b", "INS1"),
        ("security-screw", r"^M2 x 10 security screw", "SCR8"),
        ("bolt", r"^M5 x 25 bolt \+ nut", "SCR6"),
        ("wall-screw", r"^#8 ", "SCR7"),                            # #8 / M4 countersunk (bracket)
    ]),
    "canary-sense": ("bom_canary_sense.csv", [
        ("screw", r"^M2 flat x (8|10) self-tap", "SCR3"),          # FLAT head, 8-10mm
        ("insert", r"^M2 heat-set insert 3\.5 OD x 4\b", "INS1"),
        ("wall-screw", r"^#6 pan wall screw \(keyholes\)", "SCR4"),  # keyhole mount
    ]),
}

# Disagreements between the CAD's echo and the BOM CSVs that are KNOWN and
# await a human (the CSV's owner) — found by this gate's first run, reported,
# not edited. A new one fails --check; so does one that stops happening.
KNOWN_DRIFT = {
    "doorbell+inserts|short INS1",
    "sense|unbilled #6 pan wall screw (bracket)",
    "sense|unbilled M2 pan x 10 self-tap",
    "sense|unbilled M5 x 25 bolt + nut (hinge; Vision knob/bracket parts)",
    "vision.devkit_indoor|unbilled #6 pan wall screw (bracket)",
    "vision.xiao_indoor|unbilled #6 pan wall screw (bracket)",
    "vision.xiao_weather|unbilled #6 pan wall screw (bracket)",
    "vision.xiao_weather|unbilled #6 pan wall screw (keyholes)",
    "wap.battery_weather|unbilled M3 flat-head wall screw x 12 (pierce the anti-lift "
    "knockouts after hanging; 90° seat)",
    "wap.battery_weather.shield|unbilled M2 flat x 16 self-tap — REPLACES the lid screws "
    "when the shield is fitted",
}

_HW_LINE = re.compile(r'^"HARDWARE — (.+?): (.*)"$')
_ITEM = re.compile(r"^(\d+)x (.+)$")
_SCREW = re.compile(r"^M(\d+(?:\.\d+)?) (pan|flat) x (\d+) (self-tap|machine \(into the inserts\))")
_INSERT = re.compile(r"^M(\d+(?:\.\d+)?) heat-set insert (\S+) OD x (\S+)")
_BOLT = re.compile(r"^M\d+(?:\.\d+)? x \d+ bolt")


def classify(what: str) -> tuple[str, str]:
    """(kind, spec) of one echoed item's text (the part after `<qty>x `)."""
    m = _SCREW.match(what)
    if m:
        return "screw", f"M{m[1]} {m[2]} x {m[3]} {m[4]}"
    m = _INSERT.match(what)
    if m:
        return "insert", f"M{m[1]} heat-set insert {m[2]} OD x {m[3]}"
    if "security screw" in what:
        return "security-screw", what.split(",")[0]
    if "wall screw" in what:
        return "wall-screw", what
    if _BOLT.match(what):
        return "bolt", what.split(" (")[0]
    for needle, kind in (("O-ring", "o-ring"), ("TPU gasket", "gasket"), ("vent patch", "vent"),
                         ("light pipe", "light-pipe"), ("disc magnet", "magnet"),
                         ("clear disc", "window"), ("button", "button"),
                         ("(print part=", "printed-part")):
        if needle in what:
            return kind, what
    return "other", what


def parse_line(payload: str) -> tuple[str, list[dict]]:
    """`"HARDWARE — <case>: <n>x <item> · …"` (an ECHO payload, quotes
    included) -> (case name, items). Refuses anything it cannot read — a
    count it cannot parse is a count it would silently drop."""
    m = _HW_LINE.match(payload)
    if not m:
        raise ValueError(f"not a HARDWARE line: {payload!r}")
    items = []
    for raw in m[2].split(" · "):
        im = _ITEM.match(raw.strip())
        if not im:
            raise ValueError(f"HARDWARE item without a count: {raw!r}")
        kind, spec = classify(im[2])
        items.append({"qty": int(im[1]), "kind": kind, "spec": spec, "text": im[2]})
    return m[1], items


def probe_set(s: dict) -> dict:
    bound, batt = RIB_BOUND[s["scad"]]
    body = (f'echo("RIB", [lid_ribs ? 1 : 0, lid_rib_h, lid_rib_w, {bound}, '
            f'({batt}) ? 1 : 0]);')
    try:
        res = scad_probe.probe(f"hardware_{s['id']}", s["scad"], s["overrides"], body,
                               export="echo", root=HERE)
        line = next((e for e in res.echoes if e.startswith('"HARDWARE — ')), None)
        if line is None:
            raise scad_probe.ProbeError(f"{s['id']}: no HARDWARE echo\n{res.diag}")
        rib = scad_probe.echo_numbers(res, "RIB", s["id"])
        case, items = parse_line(line)
    except (scad_probe.ProbeError, ValueError) as e:
        sys.exit(f"gen_hardware: {e}")
    rec = {
        "device": s["device"],
        "scad": s["scad"],
        "overrides": {k: v.strip('"') for k, v in s["overrides"].items()},
        "case": case,
        "items": items,
    }
    if s.get("rib"):
        ribs, h, w, headroom, hold = rib
        rec["lid_rib"] = {
            "ribs": bool(ribs), "h": h, "w": w, "headroom": headroom, "bound": bound,
            "slack": round(headroom - h, 3), "batt_hold": bool(hold),
        }
    return rec


def read_bom(name: str) -> dict[str, int]:
    """RefDes -> Qty for every counted row of one BOM CSV."""
    out = {}
    with open(HW / name, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            ref, qty = (row.get("RefDes") or "").strip(), (row.get("Qty") or "").strip()
            if ref and qty.isdigit():
                out[ref] = int(qty)
    return out


def join(sets: dict, boms: dict | None = None) -> tuple[dict, list[dict]]:
    """Per set: {RefDes: {"bom": n, "echo": m}} for the rows it bills, plus the
    drift list (short rows, unbilled fasteners). `boms` maps csv name ->
    {RefDes: qty} (a test hands in fixtures; default reads docs/hardware)."""
    boms = boms if boms is not None else {}
    table, drift = {}, []
    for sid, rec in sets.items():
        spec = next(s for s in SETS if s["id"] == sid)
        csv_name, rules = JOIN[rec["device"]]
        bom = boms.get(csv_name)
        if bom is None:
            bom = boms[csv_name] = read_bom(csv_name)
        kinds = spec.get("join", FASTENERS)
        need: dict[str, int] = {}
        for it in rec["items"]:
            if it["kind"] not in kinds:
                continue
            if spec.get("join_text") and spec["join_text"] not in it["text"]:
                continue
            ref = next((r for k, pat, r in rules if k == it["kind"] and re.search(pat, it["text"])),
                       None)
            if ref is None:
                drift.append({"set": sid, "csv": csv_name, "what": f"unbilled {it['text']}",
                              "detail": f"{it['qty']}x {it['text']} — no row in {csv_name} bills it"})
                continue
            need[ref] = need.get(ref, 0) + it["qty"]
        rows = {}
        for ref, n in sorted(need.items()):
            have = bom.get(ref)
            rows[ref] = {"bom": have, "echo": n}
            if have is None or have < n:
                drift.append({"set": sid, "csv": csv_name, "what": f"short {ref}",
                              "detail": f"{csv_name} {ref} qty {have} < {n} the CAD echoes"})
        table[sid] = rows
    drift.sort(key=lambda d: (d["set"], d["what"]))
    return table, drift


def build() -> dict:
    sets = {s["id"]: probe_set(s) for s in sorted(SETS, key=lambda s: s["id"])}
    table, drift = join(sets)
    for sid, rows in table.items():
        sets[sid]["bom"] = rows
    return {
        "generated_by": "docs/hardware/enclosure/gen_hardware.py",
        "note": ("The hardware each committed preset needs, parsed from the case's own "
                 "HARDWARE echo (canary_core_lib hw_echo — derived from the knobs that draw "
                 "the holes), and the lid-rib headroom its own assert holds the rib to. "
                 "`bom` joins the fasteners against docs/hardware/bom_*.csv; `bom_drift` "
                 "lists every disagreement, for the CSV's owner to resolve — this file "
                 "never edits a BOM."),
        "sets": sets,
        "bom_drift": drift,
    }


# RIB_TABLE rows: | `<set>` | <h> | <headroom> | `<bound>` | <slack> | …
_RULE_ROW = re.compile(r"^\s*\| `([a-z0-9_.+-]+)` \| ([0-9.]+) \| ([0-9.]+) \| `(\w+)` \| ([0-9.]+) \|",
                       re.M)


def check_design_rules(sets: dict) -> list[str]:
    text = DESIGN_RULES.read_text(encoding="utf-8")
    rows = {m[1]: (float(m[2]), float(m[3]), m[4], float(m[5])) for m in _RULE_ROW.finditer(text)}
    bad = []
    want = {sid: r["lid_rib"] for sid, r in sets.items() if "lid_rib" in r}
    for sid, rib in sorted(want.items()):
        got = rows.get(sid)
        if got is None:
            bad.append(f"{RIB_TABLE} has no headroom row for `{sid}`")
            continue
        h, headroom, bound, slack = got
        if (abs(h - rib["h"]) > 0.005 or abs(headroom - rib["headroom"]) > 0.005
                or bound != rib["bound"] or abs(slack - rib["slack"]) > 0.005):
            bad.append(f"{RIB_TABLE}: `{sid}` says rib {h} / headroom {headroom} "
                       f"({bound}) / slack {slack}; the CAD says {rib['h']} / {rib['headroom']} "
                       f"({rib['bound']}) / {rib['slack']}")
    for sid in sorted(set(rows) - set(want)):
        bad.append(f"{RIB_TABLE} has a headroom row for `{sid}`, which is not a rib set here")
    return bad


def stale_message(have: dict, fresh: dict) -> str:
    """Name what moved, by source: a set whose echoed hardware (or rib) changed
    is the CAD's; a set whose only change is its `bom` join (or a drift line) is
    a BOM CSV's. Sending a reader to the .scad for a CSV edit wastes their time."""
    old, new = have.get("sets", {}), fresh["sets"]
    cad, bom = [], []
    for sid in sorted(set(old) | set(new)):
        a, b = old.get(sid), new.get(sid)
        if a == b:
            continue
        if a is None or b is None or {k: v for k, v in a.items() if k != "bom"} != \
                {k: v for k, v in b.items() if k != "bom"}:
            cad.append(sid)
        else:
            bom.append(sid)
    # A drift line can move with no set's `bom` moving (a row's detail text);
    # name the sets whose drift lines differ, still as the BOM's.
    was = {json.dumps(d, sort_keys=True): d["set"] for d in have.get("bom_drift") or []}
    now = {json.dumps(d, sort_keys=True): d["set"] for d in fresh["bom_drift"]}
    bom = sorted(set(bom) | {was.get(k) or now[k] for k in set(was) ^ set(now)} - set(cad))
    parts = []
    if cad:
        parts.append(f"the CAD's hardware moved ({', '.join(cad)})")
    if bom:
        parts.append(f"a BOM quantity or row moved ({', '.join(bom)})")
    if not parts:
        parts.append("its text moved")
    return "hardware.json is stale — " + "; ".join(parts) + "; regenerate"


def main() -> int:
    fresh = build()
    text = json.dumps(fresh, indent=1, ensure_ascii=False) + "\n"
    keys = {f"{d['set']}|{d['what']}" for d in fresh["bom_drift"]}
    new = sorted(keys - KNOWN_DRIFT)
    gone = sorted(KNOWN_DRIFT - keys)
    bad = []
    for sid, rec in fresh["sets"].items():
        rib = rec.get("lid_rib")
        if rib and rib["ribs"] and rib["slack"] < 0:
            bad.append(f"{sid}: lid rib {rib['h']} reaches past its {rib['headroom']} headroom")
    bad += [f"BOM drift not seen before (a human fixes the CSV or the CAD; never this list "
            f"alone): {k}" for k in new]
    bad += [f"BOM drift no longer happens — take it out of KNOWN_DRIFT: {k}" for k in gone]
    bad += check_design_rules(fresh["sets"])
    if "--check" in sys.argv:
        if not OUT.exists():
            bad.insert(0, "hardware.json is missing — run gen_hardware.py")
        elif OUT.read_text(encoding="utf-8") != text:
            bad.insert(0, stale_message(json.loads(OUT.read_text(encoding="utf-8")), fresh))
        for b in bad:
            print(f"::error::gen_hardware: {b}")
        if bad:
            return 1
        print(f"hardware.json OK ({len(fresh['sets'])} sets, {len(keys)} known BOM drift entries)")
        return 0
    OUT.write_text(text, encoding="utf-8")
    for sid, rec in fresh["sets"].items():
        n = sum(i["qty"] for i in rec["items"] if i["kind"] in FASTENERS)
        rib = rec.get("lid_rib")
        extra = f", rib slack {rib['slack']}" if rib else ""
        print(f"  {sid}: {len(rec['items'])} items, {n} fasteners{extra}")
    for d in fresh["bom_drift"]:
        print(f"  BOM drift — {d['set']}: {d['detail']}")
    print(f"wrote {OUT}")
    for b in bad:
        print(f"::warning::gen_hardware: {b}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
