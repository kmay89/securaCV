#!/usr/bin/env python3
"""scripts/lint_bom.py — the BOM CSVs stay machine-readable, or the PR is red.

docs/hardware/bom_*.csv is the design-intent half of the parts pipeline:
gen_enclosures.py builds the website's Build-it tables from it, and
bom_pricing.py fetches the supply-chain half keyed on its MPNs. A row a
human can read but a parser can't is a silent lie downstream — so the
schema is enforced, not suggested:

  1. HEADER — the exact flat RoHS-style schema documented in
     docs/hardware/README.md, column-for-column.
  2. ROWS — RefDes present and unique per CSV; Qty a positive integer;
     Required ∈ {Required, Optional}; Description/Manufacturer/MPN
     non-empty; UnitUSD/ExtUSD numeric; ExtUSD = Qty × UnitUSD (±1¢) or an
     intentional 0.00 (unpopulated/uncounted rows say so in their Notes).
  3. WIRED — every bom_*.csv is named in gen_enclosures.py's BOM_MAP, so a
     new device's BOM can't exist without flowing to build.json.

Summary/recipe comment rows (first cell starts with '#') are exempt — they
are parsed as build recipes, not parts.

Run:  python3 scripts/lint_bom.py
CI:   .github/workflows/lint.yml (Repo Lints)
"""
import csv
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
HW = REPO / "docs/hardware"
GEN = REPO / "canary-local/tools/gen_enclosures.py"

SCHEMA = ["Item", "RefDes", "Qty", "Required", "Category", "Description",
          "Manufacturer", "MPN", "Mouser", "DigiKey", "LCSC", "UnitUSD",
          "ExtUSD", "Lifecycle", "RoHS", "Notes"]


def lint_csv(path: Path, failures: list[str]):
    rel = path.relative_to(REPO)
    with open(path, newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    if not rows or rows[0] != SCHEMA:
        failures.append(f"{rel}: header is not the documented schema "
                        f"(docs/hardware/README.md)")
        return
    seen = set()
    for n, row in enumerate(rows[1:], start=2):
        if not row or not any(c.strip() for c in row):
            continue
        if row[0].strip().startswith("#"):
            continue  # summary/recipe comment rows
        if len(row) != len(SCHEMA):
            failures.append(f"{rel}:{n}: {len(row)} cells, expected "
                            f"{len(SCHEMA)}")
            continue
        r = dict(zip(SCHEMA, (c.strip() for c in row)))
        where = f"{rel}:{n} [{r['RefDes'] or '?'}]"
        if not r["RefDes"]:
            failures.append(f"{where}: empty RefDes")
        elif r["RefDes"] in seen:
            failures.append(f"{where}: duplicate RefDes")
        else:
            seen.add(r["RefDes"])
        if not r["Qty"].isdigit() or int(r["Qty"]) < 1:
            failures.append(f"{where}: Qty '{r['Qty']}' is not a positive "
                            f"integer")
        if r["Required"] not in ("Required", "Optional"):
            failures.append(f"{where}: Required must be Required|Optional, "
                            f"got '{r['Required']}'")
        for col in ("Description", "Manufacturer", "MPN"):
            if not r[col]:
                failures.append(f"{where}: empty {col}")
        prices = {}
        for col in ("UnitUSD", "ExtUSD"):
            try:
                prices[col] = float(r[col])
            except ValueError:
                failures.append(f"{where}: {col} '{r[col]}' is not a number")
        if len(prices) == 2 and r["Qty"].isdigit():
            expect = int(r["Qty"]) * prices["UnitUSD"]
            if prices["ExtUSD"] != 0 and abs(prices["ExtUSD"] - expect) > 0.011:
                failures.append(
                    f"{where}: ExtUSD {prices['ExtUSD']:.2f} ≠ Qty×UnitUSD "
                    f"{expect:.2f} (0.00 is allowed for intentionally "
                    f"uncounted rows)")


def main() -> int:
    failures: list[str] = []
    csvs = sorted(HW.glob("bom_*.csv"))
    if not csvs:
        print("lint_bom.py: no docs/hardware/bom_*.csv found")
        return 1
    for path in csvs:
        lint_csv(path, failures)

    gen_text = GEN.read_text(encoding="utf-8")
    for path in csvs:
        if path.name not in gen_text:
            failures.append(
                f"{path.relative_to(REPO)}: not wired into BOM_MAP in "
                f"{GEN.relative_to(REPO)} — the Build-it page won't see it")

    if failures:
        print(f"lint_bom.py: {len(failures)} problem(s):\n")
        print("\n".join(failures))
        return 1
    print(f"lint_bom.py: OK — {len(csvs)} BOM CSVs, schema-clean and wired "
          f"into the generator.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
