#!/usr/bin/env python3
"""scripts/gen_board_design.py — the board's cost case is computed, never typed.

`boards/<id>/board.json` is design intent: parts (MPN-keyed), connectivity,
and fabrication assumptions with a stated basis for every number. This
generator joins that intent against the two things already true on disk —
the live supply-chain snapshot (docs/hardware/pricing.json, written by
scripts/bom_pricing.py) and the module build's own BOM CSV — and emits:

  boards/<id>/cost_model.json   what the carrier costs, what the module build
                                it replaces costs, the per-unit delta, the NRE,
                                the break-even unit count, and — the part that
                                matters — how much of that answer is
                                distributor-verified versus estimated.
  boards/<id>/connections.csv   the connectivity intent as a flat table, one
                                row per (net, part, pin), importable as a
                                netlist starting point in any EDA tool.

Two rules this generator exists to enforce:

  1. NOBODY TYPES A SAVING. "The custom board saves ~$15" is the kind of claim
     that is right when written and wrong three months later. Here it falls out
     of the snapshot; when the XIAO's price moves, so does the break-even, and
     the number on the page moves with it.

  2. ESTIMATES ARE LABELED, NOT LAUNDERED. Every part carries a provenance:
     `digikey`/`mouser` (a distributor resolved this MPN), `csv-seed` (an
     indicative price from a BOM CSV), or `estimate` (a human's stated
     assumption with a `basis` string). The output reports the split in
     dollars, so "we think it costs $12" is always accompanied by "and 62% of
     that is an estimate." A cost model that hides its own confidence is how
     a project talks itself into tooling it cannot afford.

What this does NOT do is emit a schematic or a KiCad netlist with physical
pin numbers. board.json's `nets` are keyed by signal name and GPIO on purpose:
ESP32-S3-WROOM-1 package pin numbers come off the datasheet during schematic
capture and are checked by the EDA tool's own ERC. Generating them from
memory here would produce exactly the confident-but-wrong artifact the rest of
this repo is built to prevent. See docs/hardware/flagship_board_program.md.

Run:    python3 scripts/gen_board_design.py
Check:  python3 scripts/gen_board_design.py --check   (CI: byte-diff gate)
"""
import csv
import io
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BOARDS = REPO / "boards"
HW = REPO / "docs/hardware"
PRICING = HW / "pricing.json"

# Provenance values that mean "a distributor resolved this MPN this run".
VERIFIED = {"digikey", "mouser"}


def load_pricing() -> dict:
    if not PRICING.exists():
        return {"parts": {}, "as_of": None}
    return json.loads(PRICING.read_text(encoding="utf-8"))


def bom_rows(csv_name: str) -> dict:
    """RefDes -> row, for the module build we are comparing against.

    Recipe/summary rows (first cell starts with '#') and blank RefDes rows are
    skipped — they are build presets, not parts. Matches lint_bom.py's rule.
    """
    out = {}
    path = HW / csv_name
    with path.open(encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            item = (row.get("Item") or "").strip()
            ref = (row.get("RefDes") or "").strip()
            if not ref or item.startswith("#"):
                continue
            out[ref] = row
    return out


def quotes_of(design: dict) -> dict:
    """MPN -> list of human-entered volume quotes from board.json.

    A distributor snapshot is what a catalog publishes; a quote is what a
    supplier actually said. For the one question this model exists to answer —
    does the module stay above the crossover price at the volume we would buy?
    — the catalog cannot answer it (Digi-Key publishes no break for the XIAO at
    all) and a quote can. So quotes outrank snapshots, and they are the only
    place in this pipeline where a human types a price on purpose.
    """
    out: dict = {}
    for q in design.get("volume_quotes", {}).get("quotes", []):
        out.setdefault(q["mpn"], []).append(q)
    return out


def price_of(mpn: str, pricing: dict, fallback_usd, fallback_basis: str, qty_basis: int,
             quotes: dict | None = None):
    """Best price for an MPN at the model's volume, with honest provenance.

    Returns (unit_usd, provenance, break_qty). `break_qty` is the volume the
    price actually applies at — 1 when the distributor published no break, and
    None for local estimates (which are already quoted at the model's volume).

    This is the single most important honesty knob in the model. Comparing a
    qty-1 distributor price for the module we are replacing against reel
    estimates for the parts replacing it OVERSTATES the saving, and it does so
    invisibly. So we take the deepest published break at or below the basis
    volume, and hand back the volume it came from — the caller reports every
    line that could not be matched, and in which direction that biases the
    answer.
    """
    # A real quote at or below the basis volume beats anything a catalog says.
    #
    # Selection is by TIER, not by price. Among quotes at or below the basis
    # volume, take the one whose qty is closest to it — that is the tier that
    # actually applies at the volume we would buy. Picking the cheapest instead
    # lets a stray 100-unit promo decide a 1000-unit program: a $10 hundred-off
    # would override a $11.50 thousand-quote and flip GO to STOP on a price
    # nobody offered at that volume. Ties break on the most recent `dated`
    # (an old quote must not outrank a fresh one), then on the lower price.
    def rank(q):
        return (int(q["qty"]), str(q.get("dated", "")), -float(q["unit_usd"]))

    applicable = [q for q in (quotes or {}).get(mpn, []) if int(q["qty"]) <= qty_basis]
    best_quote = max(applicable, key=rank) if applicable else None
    if best_quote is not None:
        return (
            float(best_quote["unit_usd"]),
            f"quote:{best_quote['source']}",
            int(best_quote["qty"]),
        )

    entry = (pricing.get("parts") or {}).get(mpn)
    if entry and entry.get("unit_usd") is not None:
        prov = entry.get("provenance") or "csv-seed"
        best = float(entry["unit_usd"])
        best_qty = 1
        for brk in entry.get("breaks") or []:
            try:
                bq, bu = int(brk["qty"]), float(brk["usd"])
            except (KeyError, TypeError, ValueError):
                continue
            if bq <= qty_basis and bu < best:
                best, best_qty = bu, bq
        return best, prov, best_qty
    if fallback_usd is None:
        return None, "unpriced", None
    return float(fallback_usd), fallback_basis, None


def money(x: float) -> float:
    return round(x + 1e-9, 4)


def build_cost_model(design: dict, pricing: dict) -> dict:
    fab = design["fab"]
    qty = int(fab["qty_basis"])
    quotes = quotes_of(design)

    # ── The carrier: parts on (and shipped with) the new board ──────────────
    def line_for(ref: str, spec: dict, est_key: str) -> dict:
        unit, prov, brk = price_of(
            spec["mpn"], pricing, spec.get(est_key), "estimate", qty, quotes
        )
        if unit is None:
            raise SystemExit(
                f"gen_board_design.py: {ref} ({spec['mpn']}) has no price in "
                f"pricing.json and no {est_key} in board.json"
            )
        qty_n = int(spec.get("qty", 1))
        return {
            "ref": ref,
            "mpn": spec["mpn"],
            "qty": qty_n,
            "unit_usd": money(unit),
            "ext_usd": money(unit * qty_n),
            "provenance": prov,
            "verified": prov in VERIFIED,
            "price_at_qty": brk,
            "volume_matched": brk is None or brk >= qty,
            "basis": spec.get("basis") if prov not in VERIFIED else None,
        }

    lines = [line_for(p["ref"], p, "est_unit_usd") for p in design["parts"]]
    lines += [
        line_for(f"(off-board) {o['mpn']}", o, "est_unit_usd")
        for o in design.get("off_board_parts", {}).get("refs", [])
    ]

    parts_usd = sum(line["ext_usd"] for line in lines)
    verified_usd = sum(line["ext_usd"] for line in lines if line["verified"])

    pcb_usd = float(fab["pcb_unit_usd"])
    asm_usd = float(fab["assembly_unit_usd"])
    carrier_unit_usd = parts_usd + pcb_usd + asm_usd

    # ── The module build being replaced: only the rows the carrier absorbs ──
    cmp_spec = design["compare_to"]
    rows = bom_rows(cmp_spec["csv"])
    absorbed, missing = [], []
    for ref in cmp_spec["absorbs"]:
        row = rows.get(ref)
        if row is None:
            missing.append(ref)
            continue
        mpn = (row.get("MPN") or "").strip()
        seed = (row.get("UnitUSD") or "").strip() or None
        unit, prov, brk = price_of(mpn, pricing, seed, "csv-seed", qty, quotes)
        n = int((row.get("Qty") or "1").strip() or 1)
        absorbed.append(
            {
                "ref": ref,
                "mpn": mpn,
                "qty": n,
                "unit_usd": money(unit),
                "ext_usd": money(unit * n),
                "provenance": prov,
                "verified": prov in VERIFIED,
                "price_at_qty": brk,
                "volume_matched": brk is None or brk >= qty,
                "why": cmp_spec.get("absorbs_rationale", {}).get(ref),
            }
        )
    if missing:
        raise SystemExit(
            "gen_board_design.py: compare_to.absorbs names RefDes rows that are "
            f"not in {cmp_spec['csv']}: {', '.join(missing)}. The CSV changed "
            "under the design — fix `absorbs` rather than the CSV."
        )

    module_unit_usd = sum(a["ext_usd"] for a in absorbed)
    module_verified_usd = sum(a["ext_usd"] for a in absorbed if a["verified"])

    saving_unit_usd = module_unit_usd - carrier_unit_usd

    # Parts a design choice REMOVES rather than integrates, where the repo's own
    # sources disagree about whether the module build needs them at all. These
    # stay out of the headline on purpose: an unresolved question must not
    # silently inflate a saving. Reported as upside, priced from the same rows.
    contested = []
    for ref, spec in (cmp_spec.get("contested_eliminations") or {}).items():
        if ref.startswith("_"):
            continue
        row = rows.get(ref)
        if row is None:
            raise SystemExit(
                f"gen_board_design.py: contested_eliminations names {ref}, which "
                f"is not in {cmp_spec['csv']}"
            )
        mpn = (row.get("MPN") or "").strip()
        seed = (row.get("UnitUSD") or "").strip() or None
        unit, prov, brk = price_of(mpn, pricing, seed, "csv-seed", qty, quotes)
        n = int((row.get("Qty") or "1").strip() or 1)
        contested.append(
            {
                "ref": ref,
                "mpn": mpn,
                "required_in_csv": (row.get("Required") or "").strip(),
                "usd": money(unit * n),
                "provenance": prov,
                "claim": spec.get("claim"),
                "dispute": spec.get("the_repo_disagrees_with_itself"),
                "how_to_settle_it": spec.get("how_to_settle_it"),
            }
        )
    contested_usd = sum(c["usd"] for c in contested)

    # ── NRE and the break-even that falls out of it ─────────────────────────
    # Unique parts drive the PCBA setup fee, so derive the count from the parts
    # list rather than trusting a hand-typed number that drifts as parts land.
    unique_parts = sum(int(p.get("unique_values", 1)) for p in design["parts"])
    nre_usd = (
        float(fab["nre_design_usd"])
        + float(fab["pcba_setup_usd"])
        + float(fab["pcba_placement_usd"]) * unique_parts
    )
    if saving_unit_usd > 0:
        breakeven_units = int(-(-nre_usd // saving_unit_usd))  # ceil
    else:
        breakeven_units = None

    total_priced = parts_usd + module_unit_usd
    total_verified = verified_usd + module_verified_usd
    verified_share = (total_verified / total_priced) if total_priced else 0.0

    # Every line the snapshot could only price at a volume below the model's
    # basis. These bias the saving UPWARD when they sit on the replaced side
    # (we pay a low-volume price for the thing we are dropping) and DOWNWARD
    # when they sit on the carrier side.
    unmatched_replaced = [a for a in absorbed if not a["volume_matched"]]
    unmatched_carrier = [row for row in lines if not row["volume_matched"]]

    # The one question this model can answer with no invented data at all:
    # how far can the dominant replaced part fall in price before the carrier
    # stops being cheaper? Everything else on the replaced side is held at its
    # current modeled price, so this is a floor on that part alone.
    dominant = max(absorbed, key=lambda a: a["ext_usd"]) if absorbed else None
    sensitivity = None
    decision = None
    if dominant is not None:
        rest = module_unit_usd - dominant["ext_usd"]
        crossover = (carrier_unit_usd - rest) / max(dominant["qty"], 1)
        # The model's own verdict. A quote at the basis volume settles it; a
        # qty-1 catalog price cannot, and must not be allowed to read as if it
        # had. GO/STOP is only claimed when the price it rests on is real.
        # May this price decide a `qty`-unit program at all? The two sources
        # have different semantics and both traps point the same way — toward
        # a GO nobody earned:
        #
        #  · a distributor break at N means "N or more", so any N in (1, qty]
        #    genuinely applies at qty. N == 1 is just list price, not a volume
        #    price, and is the case this whole refusal exists for.
        #  · a QUOTE at N means "we priced N units". Below the basis it is only
        #    an upper bound on what we would really pay, and on the replaced
        #    side an upper bound overstates the module's cost — which flatters
        #    the carrier. So a low-volume quote is not an answer either.
        at = dominant["price_at_qty"]
        quoted = dominant["provenance"].startswith("quote:")
        if at is None:
            decidable = False
        elif quoted:
            decidable = at >= qty
        else:
            decidable = 1 < at <= qty

        # Compare against the SAME rounded crossover that gets reported, or
        # quoting the published number back lands a hair above it and reads GO
        # at what is actually exact break-even.
        crossover_reported = money(crossover)
        if decidable:
            verdict_state = "GO" if dominant["unit_usd"] > crossover_reported else "STOP"
            verdict_why = (
                f"{dominant['mpn']} is priced at ${dominant['unit_usd']:.2f} at qty "
                f"{at} ({dominant['provenance']}), "
                f"{'above' if verdict_state == 'GO' else 'at or below'} the "
                f"${crossover_reported:.2f} crossover."
            )
        else:
            verdict_state = "UNRESOLVED"
            verdict_why = (
                f"{dominant['mpn']} is priced at ${dominant['unit_usd']:.2f} at qty "
                f"{at} ({dominant['provenance']}), which cannot answer a {qty}-unit "
                "question: "
                + (
                    f"a quote for {at} units is only an upper bound on the "
                    f"{qty}-unit price, and on the replaced side an upper bound "
                    "flatters the carrier."
                    if quoted
                    else "that is a catalog price for one piece, not the price we "
                    "would pay."
                )
                + f" Add a quote at qty {qty} to board.json `volume_quotes` and "
                "this becomes GO or STOP automatically."
            )
        decision = {
            "state": verdict_state,
            "why": verdict_why,
            "crossover_unit_usd": crossover_reported,
            "depends_on_mpn": dominant["mpn"],
            "how_to_resolve": design.get("volume_quotes", {}).get("how_to_get_one"),
        }

        sensitivity = {
            "question": (
                f"How cheap would {dominant['mpn']} have to get before this "
                f"carrier stops paying for itself?"
            ),
            "dominant_replaced_mpn": dominant["mpn"],
            "modeled_unit_usd": dominant["unit_usd"],
            "modeled_at_qty": dominant["price_at_qty"],
            "crossover_unit_usd": money(crossover),
            "verdict": (
                f"The carrier is cheaper while {dominant['mpn']} costs more than "
                f"${money(crossover):.2f}/unit. It is modeled at "
                f"${dominant['unit_usd']:.2f} at qty {dominant['price_at_qty']}. "
                "This is the number to re-check against a real volume quote "
                "before committing NRE — it needs no estimate to compute, only "
                "the price of one part."
            ),
        }

    return {
        "generated_by": "scripts/gen_board_design.py",
        "board": design["id"],
        "confidence": design["confidence"],
        "pricing_as_of": pricing.get("as_of"),
        "qty_basis": qty,
        "carrier": {
            "parts_usd": money(parts_usd),
            "pcb_usd": money(pcb_usd),
            "assembly_usd": money(asm_usd),
            "unit_usd": money(carrier_unit_usd),
            "lines": lines,
        },
        "module_build_replaced": {
            "csv": cmp_spec["csv"],
            "unit_usd": money(module_unit_usd),
            "lines": absorbed,
        },
        "contested_upside": {
            "note": (
                "Excluded from saving_per_unit_usd on purpose. Each line is a "
                "part the carrier's design choice removes, where this repo's "
                "own sources disagree about whether the module build needed it. "
                "Settle them on a bench, then move them into `absorbs` or drop "
                "them — do not let them drift into the headline unresolved."
            ),
            "usd_per_unit": money(contested_usd),
            "saving_if_all_upheld_usd": money(saving_unit_usd + contested_usd),
            "lines": contested,
        },
        "delta": {
            "saving_per_unit_usd": money(saving_unit_usd),
            "nre_usd": money(nre_usd),
            "nre_breakdown": {
                "design_usd": money(float(fab["nre_design_usd"])),
                "pcba_setup_usd": money(float(fab["pcba_setup_usd"])),
                "unique_parts": unique_parts,
                "unique_part_placement_usd": money(
                    float(fab["pcba_placement_usd"]) * unique_parts
                ),
            },
            "breakeven_units": breakeven_units,
            "breakeven_note": (
                "Units that must be built before the carrier has paid back its own "
                "NRE. It excludes certification entirely — the Part 15B SDoC in "
                "board.json `certification` is owed whether or not this board "
                "exists, because it attaches to shipping a radio, not to which "
                "PCB the radio sits on."
            )
            if breakeven_units is not None
            else "No break-even: the carrier does not cost less than the module build.",
        },
        "decision": decision,
        "sensitivity": sensitivity,
        "confidence_accounting": {
            "note": (
                "Share of the compared dollars that a distributor actually "
                "resolved this snapshot, versus a human's stated estimate. Read "
                "the saving with this number next to it."
            ),
            "priced_usd": money(total_priced),
            "distributor_verified_usd": money(total_verified),
            "estimated_usd": money(total_priced - total_verified),
            "verified_share": round(verified_share, 4),
            "unverified_mpns": sorted(
                {
                    line["mpn"]
                    for line in lines + absorbed
                    if not line["verified"]
                }
            ),
            "volume_mismatch": {
                "note": (
                    f"Lines the snapshot could only price below the {qty}-unit "
                    "basis. On the replaced side this OVERSTATES the saving "
                    "(we credit ourselves a low-volume price for a part we are "
                    "dropping); on the carrier side it understates it. Any line "
                    "here is a volume quote waiting to be got."
                ),
                "replaced_side_overstates_by_at_most_usd": money(
                    sum(a["ext_usd"] for a in unmatched_replaced)
                ),
                "replaced_side_mpns": sorted(
                    {a["mpn"] for a in unmatched_replaced}
                ),
                "carrier_side_mpns": sorted(
                    {row["mpn"] for row in unmatched_carrier if row["provenance"] in VERIFIED}
                ),
            },
        },
    }


def build_connections(design: dict) -> str:
    out = [["net", "net_class", "signal", "gpio", "ref", "pin", "note"]]
    pins = {**design["pin_map"], **design["pin_map_unenforced"]}
    for group in ("power", "signals"):
        for net in design["nets"][group]:
            sig = net.get("signal")
            gpio = pins.get(sig, {}).get("gpio", "") if sig else ""
            for node in net["nodes"]:
                ref, _, pin = node.partition(".")
                out.append(
                    [
                        net["net"],
                        net["class"],
                        sig or "",
                        gpio if gpio != "" else "",
                        ref,
                        pin,
                        net.get("note", ""),
                    ]
                )
    # Real CSV quoting, not string-joining: notes carry commas today and would
    # carry a double quote the first time someone quotes pins.h in one. A
    # hand-rolled writer corrupts that silently, which is the worst way for a
    # netlist to be wrong.
    buf = io.StringIO()
    csv.writer(buf, lineterminator="\n").writerows(out)
    return buf.getvalue()


def emit(path: Path, text: str, check: bool, stale: list) -> None:
    old = path.read_text(encoding="utf-8") if path.exists() else None
    if check:
        if old != text:
            stale.append(str(path.relative_to(REPO)))
        return
    path.write_text(text, encoding="utf-8")
    print(f"  wrote {path.relative_to(REPO)}")


def main() -> int:
    check = "--check" in sys.argv
    pricing = load_pricing()
    stale: list = []

    designs = sorted(BOARDS.glob("*/board.json"))
    if not designs:
        print("gen_board_design.py: no boards/*/board.json found")
        return 0

    for design_path in designs:
        design = json.loads(design_path.read_text(encoding="utf-8"))
        out_dir = design_path.parent
        if not check:
            print(f"{design['id']}:")

        model = build_cost_model(design, pricing)
        emit(
            out_dir / "cost_model.json",
            json.dumps(model, indent=2, ensure_ascii=False) + "\n",
            check,
            stale,
        )
        emit(out_dir / "connections.csv", build_connections(design), check, stale)

        if not check:
            d = model["delta"]
            c = model["confidence_accounting"]
            print(
                f"  carrier ${model['carrier']['unit_usd']:.2f}/unit vs "
                f"${model['module_build_replaced']['unit_usd']:.2f} replaced "
                f"-> saves ${d['saving_per_unit_usd']:.2f}/unit, "
                f"break-even {d['breakeven_units']} units "
                f"({c['verified_share']*100:.0f}% distributor-verified)"
            )

    if check and stale:
        print("gen_board_design.py --check: these are stale, re-run the generator:")
        for s in stale:
            print(f"  {s}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
