#!/usr/bin/env python3
"""scripts/tests/test_board_design.py — the board cost model's verdict, pinned.

`cost_model.json` now emits a GO / STOP / UNRESOLVED `decision`, and a human
will read that word and decide whether to spend NRE. Two pieces of logic stand
between a quote and that word, and both are the kind of quiet heuristic this
repo has been burned by before:

  1. WHICH QUOTE APPLIES. Selection is by tier — the qty closest to (but not
     over) the basis volume — not by lowest price. PR #1460 review caught the
     price-first version: a stray 100-unit promo at $10 would outrank a real
     1000-unit quote at $11.50 and flip GO to STOP on a price nobody offered
     at that volume.

  2. WHEN THE MODEL MAY DECIDE AT ALL. A qty-1 catalog price must yield
     UNRESOLVED, never GO — even though $15.90 sits comfortably above the
     $11.32 crossover and reads like good news. That refusal is the whole
     point; a test that let it regress would let the project talk itself into
     tooling on a one-piece price.

Run:  python3 scripts/tests/test_board_design.py
CI:   .github/workflows/lint.yml (Repo Lints)
"""
import importlib.util
import json
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

spec = importlib.util.spec_from_file_location(
    "gen_board_design", REPO / "scripts/gen_board_design.py"
)
gbd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gbd)


def quotes(*rows):
    """Build the mpn -> [quote] map price_of expects."""
    out = {}
    for mpn, qty, usd, source, dated in rows:
        out.setdefault(mpn, []).append(
            {"mpn": mpn, "qty": qty, "unit_usd": usd, "source": source, "dated": dated}
        )
    return out


class QuoteSelection(unittest.TestCase):
    """price_of picks the tier that applies at the basis volume."""

    def test_deepest_tier_wins_not_cheapest(self):
        # The defect PR #1460 review found: a cheap low-volume promo must not
        # decide a high-volume program.
        q = quotes(
            ("M", 100, 10.00, "promo", "2026-08-01"),
            ("M", 1000, 11.50, "seeed", "2026-08-01"),
        )
        usd, prov, at = gbd.price_of("M", {}, None, "", 1000, q)
        self.assertEqual((usd, at), (11.50, 1000))
        self.assertEqual(prov, "quote:seeed")

    def test_monotonic_ladder_picks_the_bottom(self):
        # The ordinary case still behaves the obvious way.
        q = quotes(
            ("M", 100, 13.00, "seeed", "2026-08-01"),
            ("M", 500, 12.00, "seeed", "2026-08-01"),
            ("M", 1000, 11.00, "seeed", "2026-08-01"),
        )
        usd, _, at = gbd.price_of("M", {}, None, "", 1000, q)
        self.assertEqual((usd, at), (11.00, 1000))

    def test_quotes_above_the_basis_are_ignored(self):
        # A 5000-unit price is not available to a 1000-unit program.
        q = quotes(
            ("M", 1000, 11.50, "seeed", "2026-08-01"),
            ("M", 5000, 8.00, "seeed", "2026-08-01"),
        )
        usd, _, at = gbd.price_of("M", {}, None, "", 1000, q)
        self.assertEqual((usd, at), (11.50, 1000))

    def test_same_tier_breaks_on_recency(self):
        # An old quote must not outrank a fresh one, even if it is cheaper.
        q = quotes(
            ("M", 1000, 9.00, "stale", "2024-01-01"),
            ("M", 1000, 12.00, "fresh", "2026-08-01"),
        )
        usd, prov, _ = gbd.price_of("M", {}, None, "", 1000, q)
        self.assertEqual((usd, prov), (12.00, "quote:fresh"))

    def test_same_tier_same_date_takes_the_better_price(self):
        q = quotes(
            ("M", 1000, 12.00, "a", "2026-08-01"),
            ("M", 1000, 11.00, "b", "2026-08-01"),
        )
        usd, prov, _ = gbd.price_of("M", {}, None, "", 1000, q)
        self.assertEqual((usd, prov), (11.00, "quote:b"))

    def test_quote_outranks_a_distributor_snapshot(self):
        pricing = {"parts": {"M": {"unit_usd": 15.90, "provenance": "digikey",
                                   "breaks": [{"qty": 1, "usd": 15.90}]}}}
        q = quotes(("M", 1000, 11.50, "seeed", "2026-08-01"))
        usd, prov, _ = gbd.price_of("M", pricing, None, "", 1000, q)
        self.assertEqual((usd, prov), (11.50, "quote:seeed"))

    def test_no_quote_falls_back_to_the_snapshot_break(self):
        pricing = {"parts": {"M": {"unit_usd": 10.00, "provenance": "digikey",
                                   "breaks": [{"qty": 1, "usd": 10.00},
                                              {"qty": 500, "usd": 7.00},
                                              {"qty": 5000, "usd": 3.00}]}}}
        usd, prov, at = gbd.price_of("M", pricing, None, "", 1000, {})
        # 5000 is above the basis and must not be used.
        self.assertEqual((usd, prov, at), (7.00, "digikey", 500))


class Verdict(unittest.TestCase):
    """The real design's decision block, driven end to end."""

    @classmethod
    def setUpClass(cls):
        cls.design = json.loads(
            (REPO / "boards/canary-witness-s3/board.json").read_text(encoding="utf-8")
        )
        cls.pricing = json.loads(
            (REPO / "docs/hardware/pricing.json").read_text(encoding="utf-8")
        )

    def model_with(self, rows):
        design = json.loads(json.dumps(self.design))
        design.setdefault("volume_quotes", {})["quotes"] = rows
        return gbd.build_cost_model(design, self.pricing)

    def dominant(self, model):
        return model["decision"]["depends_on_mpn"]

    def test_no_quote_is_unresolved_not_go(self):
        """A qty-1 catalog price above the crossover must NOT read as GO."""
        m = self.model_with([])
        self.assertEqual(m["decision"]["state"], "UNRESOLVED")
        # And the price it declined to decide on really is above the crossover —
        # i.e. the refusal is about provenance, not about the arithmetic.
        line = next(row for row in m["module_build_replaced"]["lines"]
                    if row["mpn"] == self.dominant(m))
        self.assertEqual(line["price_at_qty"], 1)
        self.assertGreater(line["unit_usd"], m["decision"]["crossover_unit_usd"])

    def test_quote_above_crossover_is_go(self):
        m = self.model_with([{"mpn": "102010469", "qty": 1000, "unit_usd": 12.40,
                              "source": "test", "dated": "2026-08-04"}])
        self.assertEqual(m["decision"]["state"], "GO")
        self.assertGreater(m["delta"]["saving_per_unit_usd"], 0)
        self.assertIsNotNone(m["delta"]["breakeven_units"])

    def test_quote_below_crossover_is_stop(self):
        m = self.model_with([{"mpn": "102010469", "qty": 1000, "unit_usd": 9.50,
                              "source": "test", "dated": "2026-08-04"}])
        self.assertEqual(m["decision"]["state"], "STOP")
        self.assertLess(m["delta"]["saving_per_unit_usd"], 0)
        self.assertIsNone(m["delta"]["breakeven_units"])

    def test_low_volume_quote_cannot_decide(self):
        """A 10-unit quote is not an answer to a 1000-unit question."""
        m = self.model_with([{"mpn": "102010469", "qty": 10, "unit_usd": 14.00,
                              "source": "test", "dated": "2026-08-04"}])
        self.assertEqual(m["decision"]["state"], "UNRESOLVED")

    def test_crossover_is_the_break_even_price(self):
        """At exactly the crossover the saving is zero — the number means what it says."""
        m = self.model_with([])
        x = m["decision"]["crossover_unit_usd"]
        at_x = self.model_with([{"mpn": "102010469", "qty": 1000, "unit_usd": x,
                                 "source": "test", "dated": "2026-08-04"}])
        self.assertAlmostEqual(at_x["delta"]["saving_per_unit_usd"], 0.0, places=2)
        self.assertEqual(at_x["decision"]["state"], "STOP")  # not strictly above


if __name__ == "__main__":
    unittest.main(verbosity=2)
