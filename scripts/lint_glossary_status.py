#!/usr/bin/env python3
"""Pin the GLOSSARY device-line Status column to the figures.json ladder.

    python3 scripts/lint_glossary_status.py

WHY. The rule (AGENTS.md "Whether something is real or still an idea") is
that real-vs-idea verdicts are DERIVED from evidence on disk, never
hand-typed — canary-local/devices/figures.json is the ledger. The
glossary's device-line table carries a Status column for human
scannability, which makes it a hand-written copy of derived verdicts:
exactly the thing that drifts. This lint makes the copy safe — when CAD,
firmware, or catalog evidence moves a device's derived confidence, the
stale glossary cell goes red instead of quietly claiming the old status.

HOW EACH ROW IS CHECKED. ROW_MAP below classifies every row once:

  ledger rows   name -> one figures.json id (or a family prefix): the cell
                must equal the ledger's derived confidence, and a family's
                members must agree before the family verdict means anything
  hand rows     name -> a label OUTSIDE the ladder ("design" for a row with
                no figures.json entry yet, "software" for a firmware
                subsystem the ladder does not measure): the cell must be
                exactly that label, and a hand row may NEVER carry a ladder
                word — hand-typing "shipping" is the failure this exists
                to prevent

A table row missing from ROW_MAP fails (a new device row must be
classified here in the same commit), and a ROW_MAP entry missing from the
table fails too, so the two cannot drift apart silently.

Deliberate mapping notes, so nobody "fixes" them wrong later:
  - Canary Pool is NOT device.canary-poolwatch — that ledger entry is a
    different product (a pool-area camera post); the water-chemistry node
    has no ledger entry yet, hence "design".
  - Canary Sentinel appears in figures.json's own device_types.unmapped
    ("no figure for this hardware yet"), hence "design".
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GLOSSARY = ROOT / "docs/GLOSSARY.md"
FIGURES = ROOT / "canary-local/devices/figures.json"

HAND_LABELS = {"design", "software"}

# name in the table's first cell -> ("id", figure id), ("family", id prefix),
# or ("hand", label). Add a row here in the same commit that adds it to the
# table.
ROW_MAP = {
    "Canary WAP": ("id", "device.canary-wap"),
    "Canary Vision": ("id", "device.canary-vision"),
    "Canary Sense": ("id", "device.canary-sense"),
    "Canary Pool": ("hand", "design"),
    "Canary Sentinel": ("hand", "design"),
    "Canary Display": ("family", "device.canary-display-"),
    "Canary OTA": ("hand", "software"),
    "Canary Fence Guard": ("id", "device.canary-fence-guard"),
    "The Tin Can": ("hand", "design"),
    "The Night Watch": ("hand", "design"),
    "The Nightlight": ("id", "device.canary-nightlight"),
    "The Pocket Canary": ("hand", "design"),
    "The Weather / the Bond": ("hand", "design"),
}

ROW_RE = re.compile(r"^\|\s*\*\*(.+?)\*\*\s*\|\s*([a-z]+)\s*\|")
HEADER = "| Name | Status | What it is |"


def main() -> int:
    figures = json.loads(FIGURES.read_text(encoding="utf-8"))
    ladder = set(figures["ladder"]["order"])
    conf = {f["id"]: f["confidence"] for f in figures["figures"]}

    text = GLOSSARY.read_text(encoding="utf-8")
    if HEADER not in text:
        print(f"lint_glossary_status.py: the device-line table header "
              f"({HEADER!r}) is gone from docs/GLOSSARY.md — if the table "
              f"was restructured, update this lint with it", file=sys.stderr)
        return 2
    table = text.split(HEADER, 1)[1].split("\n\n", 1)[0]

    problems = []
    seen = set()
    for line in table.splitlines():
        m = ROW_RE.match(line)
        if not m:
            continue
        name, cell = m.group(1), m.group(2)
        seen.add(name)
        if name not in ROW_MAP:
            problems.append(f"row {name!r} is not classified in ROW_MAP — "
                            f"add it there in the same commit")
            continue
        kind, ref = ROW_MAP[name]
        if kind == "hand":
            if cell != ref:
                problems.append(f"{name}: cell is {cell!r}, ROW_MAP says the "
                                f"hand label {ref!r}")
            if cell in ladder:
                problems.append(f"{name}: {cell!r} is a ladder word — ladder "
                                f"verdicts are derived, never hand-typed; "
                                f"map the row to its figures.json id instead")
            continue
        ids = ([ref] if kind == "id"
               else sorted(i for i in conf if i.startswith(ref)))
        missing = [i for i in ids if i not in conf]
        if missing or not ids:
            problems.append(f"{name}: figures.json has no entry for "
                            f"{missing or [ref]} — re-map the row or make it "
                            f"a hand label")
            continue
        verdicts = {conf[i] for i in ids}
        if len(verdicts) > 1:
            problems.append(f"{name}: family {ref}* disagrees in the ledger "
                            f"({sorted(verdicts)}) — one family verdict no "
                            f"longer exists; split the row or re-map it")
        elif cell != next(iter(verdicts)):
            problems.append(f"{name}: cell says {cell!r} but figures.json "
                            f"derives {next(iter(verdicts))!r} — the evidence "
                            f"moved; update the glossary cell")
    for name in ROW_MAP:
        if name not in seen:
            problems.append(f"ROW_MAP entry {name!r} has no table row — "
                            f"remove it or restore the row")

    if problems:
        print(f"lint_glossary_status.py: {len(problems)} problem(s) — the "
              f"glossary Status column must match the derived ledger:",
              file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print(f"glossary status OK — {len(seen)} rows match "
          f"canary-local/devices/figures.json (or carry an honest "
          f"non-ladder label)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
