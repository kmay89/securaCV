#!/usr/bin/env python3
"""scripts/tests/test_bom_pricing.py — the pricing engine's contract, pinned.

The nightly supply-chain pipeline only earns "no humans managing it" if its
edge cases are locked down: a distributor miss must demote (never silently
republish stale data as live), a seed run must never destroy fetched
history, a recovery must go quiet, and every exception kind must fire when
— and only when — its condition holds. These tests run in Repo Lints on
every PR, so the contract can't regress unnoticed.

Run:  python3 scripts/tests/test_bom_pricing.py
CI:   .github/workflows/lint.yml (Repo Lints)
"""
import importlib.util
import json
import unittest
import urllib.parse
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "bom_pricing", REPO / "scripts/bom_pricing.py")
bp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bp)


def part(mpn="MR60BHA2", **over):
    base = {"mfr": "Seeed Studio", "desc": "kit", "sourcing": "orderable",
            "seed_usd": 24.9, "sku": {"digikey": "X-ND"}, "boms": ["bom_canary_sense.csv"]}
    base.update(over)
    return {mpn: base}


def snapshot(provenance="digikey", unit=23.5, stock=100, lifecycle="Active"):
    return {"parts": {"MR60BHA2": {
        "provenance": provenance, "unit_usd": unit, "stock": stock,
        "lifecycle": lifecycle, "sku": {"digikey": "X-ND"},
        "breaks": [], "url": "u"}}}


HIT = {"provenance": "digikey", "unit_usd": 23.9, "stock": 50,
       "lifecycle": "Active", "sku_digikey": "X-ND", "breaks": [], "url": "u"}


class CarryDemote(unittest.TestCase):
    """An attempted fetch miss must demote, fire once, and recover quietly."""

    def test_attempted_miss_demotes_and_fires_no_match(self):
        old = snapshot()
        new = bp.assemble(part(), old, {}, "n", attempted={"MR60BHA2"})
        e = new["parts"]["MR60BHA2"]
        self.assertEqual(e["provenance"], "carried")
        self.assertEqual(e["unit_usd"], 23.5)  # last-known retained
        kinds = [x["kind"] for x in bp.find_exceptions(old, new)]
        self.assertEqual(kinds, ["no-match"])

    def test_repeat_miss_stays_carried_no_refire(self):
        old = snapshot("carried")
        new = bp.assemble(part(), old, {}, "n", attempted={"MR60BHA2"})
        self.assertEqual(new["parts"]["MR60BHA2"]["provenance"], "carried")
        self.assertEqual(bp.find_exceptions(old, new), [])

    def test_seed_preserves_live_and_carried_verbatim(self):
        for prov in ("digikey", "carried"):
            new = bp.assemble(part(), snapshot(prov), {}, "n", attempted=None)
            self.assertEqual(new["parts"]["MR60BHA2"]["provenance"], prov)

    def test_recovery_returns_live_silently(self):
        old = snapshot("carried")
        new = bp.assemble(part(), old, {"MR60BHA2": HIT}, "n",
                          attempted={"MR60BHA2"})
        self.assertEqual(new["parts"]["MR60BHA2"]["provenance"], "digikey")
        self.assertEqual(bp.find_exceptions(old, new), [])

    def test_never_seen_part_seeds_quietly(self):
        new = bp.assemble(part(), {}, {}, "n", attempted={"MR60BHA2"})
        self.assertEqual(new["parts"]["MR60BHA2"]["provenance"], "csv-seed")
        self.assertEqual(bp.find_exceptions({}, new), [])


class ExceptionKinds(unittest.TestCase):
    def test_out_of_stock_fires_on_live_zero(self):
        old = snapshot()
        hit = dict(HIT, stock=0)
        new = bp.assemble(part(), old, {"MR60BHA2": hit}, "n",
                          attempted={"MR60BHA2"})
        kinds = [x["kind"] for x in bp.find_exceptions(old, new)]
        self.assertIn("out-of-stock", kinds)

    def test_price_jump_fires_over_15pct_only(self):
        old = snapshot(unit=20.0)
        calm = bp.assemble(part(), old, {"MR60BHA2": dict(HIT, unit_usd=22.0)},
                           "n", attempted={"MR60BHA2"})
        self.assertEqual([x["kind"] for x in bp.find_exceptions(old, calm)], [])
        spike = bp.assemble(part(), old, {"MR60BHA2": dict(HIT, unit_usd=24.0)},
                            "n", attempted={"MR60BHA2"})
        self.assertIn("price-jump",
                      [x["kind"] for x in bp.find_exceptions(old, spike)])

    def test_price_jump_needs_real_money_not_just_percent(self):
        # A $0.02 passive doubling to $0.04 is 100% but $0.02 — never an
        # exception. The noise floor keeps the queue meaningful.
        old = snapshot(unit=0.02)
        doubled = bp.assemble(part(), old,
                              {"MR60BHA2": dict(HIT, unit_usd=0.04)},
                              "n", attempted={"MR60BHA2"})
        self.assertEqual(
            [x["kind"] for x in bp.find_exceptions(old, doubled)], [])

    def test_lifecycle_fires_on_nrnd(self):
        old = snapshot()
        hit = dict(HIT, lifecycle="Not For New Designs")
        new = bp.assemble(part(), old, {"MR60BHA2": hit}, "n",
                          attempted={"MR60BHA2"})
        self.assertIn("lifecycle",
                      [x["kind"] for x in bp.find_exceptions(old, new)])

    def test_generic_parts_never_flag(self):
        p = part(sourcing="generic")
        old = snapshot()
        new = bp.assemble(p, old, {}, "n", attempted={"MR60BHA2"})
        self.assertEqual(bp.find_exceptions(old, new), [])


class GenericDetection(unittest.TestCase):
    def test_rules(self):
        self.assertTrue(bp.is_generic("Generic", "USB-C-DATA-1M"))
        self.assertTrue(bp.is_generic("SecuraCV", "canary_wap_enclosure.scad"))
        self.assertTrue(bp.is_generic("Seeed/Generic", "A / B"))  # spaced MPN
        self.assertFalse(bp.is_generic("Seeed Studio", "MR60BHA2"))
        self.assertFalse(bp.is_generic("Littelfuse", "59140-1-S-02-A"))


class RealData(unittest.TestCase):
    """The committed CSVs and snapshot stay coherent with the engine."""

    def test_read_boms_covers_all_csvs(self):
        parts = bp.read_boms()
        self.assertGreater(len(parts), 50)
        self.assertIn("MR60BHA2", parts)
        self.assertEqual(parts["MR60BHA2"]["sourcing"], "orderable")
        self.assertEqual(parts["USB-C-DATA-1M"]["sourcing"], "generic")
        self.assertTrue(all(m.strip() for m in parts))

class ProductUrlSafety(unittest.TestCase):
    """A fetched product URL becomes an href. Only https at that distributor.

    This is the one field that crosses from a third-party response into
    something a visitor clicks, so the check lives at the fetch boundary and
    a rejection is always "no link", never a broken or hostile one.
    """

    def test_accepts_distributor_https(self):
        for src, url in (
                ("digikey", "https://www.digikey.com/en/products/detail/x"),
                ("digikey", "https://digikey.com/short"),
                ("mouser", "https://www.mouser.com/ProductDetail/x"),
        ):
            self.assertEqual(bp.safe_product_url(url, src), url)

    def test_joins_root_relative_path(self):
        self.assertEqual(
            bp.safe_product_url("/en/products/detail/x", "digikey"),
            "https://www.digikey.com/en/products/detail/x")

    def test_rejects_hostile_and_malformed(self):
        for src, url in (
                ("digikey", "javascript:alert(1)"),
                ("digikey", "http://www.digikey.com/x"),      # not https
                ("digikey", "https://evil.example/x"),         # wrong host
                ("digikey", "https://notdigikey.com/x"),       # suffix trick
                ("digikey", "https://digikey.com.evil.io/x"),  # subdomain trick
                ("digikey", "//evil.example/x"),               # protocol-rel
                ("digikey", "https://user@digikey.com/x"),     # userinfo
                ("digikey", "https://evil.example@digikey.com/x"),
                ("digikey", "https://www.mouser.com/x"),       # wrong source
                ("mouser", "https://www.digikey.com/x"),
                ("digikey", ""),
                ("digikey", "   "),
                ("digikey", None),
                ("digikey", 42),
                ("bogus-src", "https://www.digikey.com/x"),
        ):
            self.assertIsNone(bp.safe_product_url(url, src),
                              f"{src} accepted {url!r}")

    def test_rejects_urls_python_and_a_browser_parse_differently(self):
        """The parser-differential class, refused rather than modeled.

        urlsplit reports the host of `https://evil.example\\@digikey.com/x`
        as digikey.com (backslash is an ordinary character, so the authority
        runs to the last "@"); a browser normalizes the backslash to "/" and
        navigates to evil.example. Validating with one parser and publishing
        the raw string for the other is the bug, so any character the two can
        disagree about disqualifies the URL outright.
        """
        for url in ("https://evil.example\\@digikey.com/x",
                    "https://evil.example\\.digikey.com/x",
                    "https://digikey.com\\@evil.example/x",
                    "https://digikey.com/x\twith-tab",
                    "https://digikey.com/x\nwith-newline",
                    "https://digikey.com/x\rwith-cr",
                    "https://digikey.com/x with-space"):
            self.assertIsNone(bp.safe_product_url(url, "digikey"),
                              f"accepted {url!r}")
        # Pin the differential itself, so a future stdlib change is visible.
        self.assertEqual(
            urllib.parse.urlsplit("https://evil.example\\@digikey.com/x").hostname,
            "digikey.com", "urlsplit stopped disagreeing — recheck this rule")

    def test_lookup_drops_a_hostile_url_from_the_snapshot(self):
        """The parser must sanitize, not just the helper — pinned end to end."""
        res = {"Products": [{
            "ManufacturerProductNumber": "MR60BHA2",
            "ProductUrl": "javascript:alert(1)",
            "QuantityAvailable": 5,
            "ProductStatus": {"Status": "Active"},
            "ProductVariations": [{
                "DigiKeyProductNumber": "X-ND",
                "MinimumOrderQuantity": 1,
                "StandardPricing": [{"BreakQuantity": 1, "UnitPrice": 23.9}],
            }],
        }]}
        hit = bp._parse_digikey(res, "MR60BHA2")
        self.assertIsNone(hit["url"])
        self.assertEqual(hit["unit_usd"], 23.9)  # the rest still lands


class SnapshotSchema(unittest.TestCase):

    def test_committed_snapshot_schema(self):
        snap = json.loads((REPO / "docs/hardware/pricing.json").read_text())
        self.assertEqual(snap["generated_by"], "scripts/bom_pricing.py")
        self.assertIn("as_of", snap)
        for mpn, e in snap["parts"].items():
            for key in ("mfr", "desc", "sourcing", "seed_usd", "unit_usd",
                        "provenance", "stock", "lifecycle", "sku", "breaks",
                        "url", "boms"):
                self.assertIn(key, e, f"{mpn} missing {key}")
            self.assertIn(e["sourcing"], ("orderable", "generic"))
            self.assertIn(e["provenance"],
                          ("digikey", "mouser", "carried", "csv-seed"))
            self.assertTrue(e["boms"], f"{mpn} belongs to no BOM")


if __name__ == "__main__":
    unittest.main(verbosity=1)
