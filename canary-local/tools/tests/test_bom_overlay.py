#!/usr/bin/env python3
"""Host tests for the live supply-chain overlay in gen_enclosures.py.

The overlay is the join between the nightly distributor snapshot
(docs/hardware/pricing.json) and the Build-it catalog (build.json). Its
contract is narrow and worth pinning, because every failure mode here is
silent: a row that shows stale numbers as live, or a link that points
somewhere we never verified, looks exactly like a healthy row.

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

# Import the generator from the sibling tools/ dir regardless of cwd.
TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import gen_enclosures as ge  # noqa: E402

DK = "https://www.digikey.com/en/products/detail/seeed/102010469/1"


def part(**over):
    base = {"provenance": "digikey", "unit_usd": 13.99, "stock": 4213,
            "url": DK}
    base.update(over)
    return base


class LiveOverlay(unittest.TestCase):

    def test_verified_row_carries_price_stock_source_and_link(self):
        live = ge.live_overlay(part())
        self.assertEqual(live, {"unit_usd": 13.99, "stock": 4213,
                                "src": "digikey", "url": DK})

    def test_seeded_row_has_no_overlay(self):
        self.assertIsNone(ge.live_overlay(part(provenance="csv-seed")))
        self.assertIsNone(ge.live_overlay(None))
        self.assertIsNone(ge.live_overlay({}))

    def test_carried_row_never_republishes_as_live(self):
        """A part that stopped matching keeps its numbers in the snapshot for
        reference, but must not reappear here — and so cannot keep a link to
        a listing that may no longer exist."""
        self.assertIsNone(ge.live_overlay(part(provenance="carried")))

    def test_priceless_row_has_no_overlay(self):
        self.assertIsNone(ge.live_overlay(part(unit_usd=None)))

    def test_missing_url_is_simply_no_link(self):
        for bad in (None, "", "   "):
            live = ge.live_overlay(part(url=bad))
            self.assertNotIn("url", live)
            self.assertEqual(live["unit_usd"], 13.99)  # the row still works

    def test_untrusted_url_is_dropped_not_rendered(self):
        """pricing.json is a committed file a human can edit; the generator
        re-checks rather than trusting what it reads."""
        for bad in ("javascript:alert(1)", "http://www.digikey.com/x",
                    "https://evil.example/x", "//evil.example/x",
                    "https://www.mouser.com/x"):  # right shape, wrong source
            live = ge.live_overlay(part(url=bad))
            self.assertNotIn("url", live, f"accepted {bad!r}")

    def test_mouser_source_takes_mouser_links(self):
        live = ge.live_overlay(part(provenance="mouser",
                                    url="https://www.mouser.com/ProductDetail/x"))
        self.assertEqual(live["src"], "mouser")
        self.assertEqual(live["url"], "https://www.mouser.com/ProductDetail/x")


class CommittedCatalog(unittest.TestCase):

    def test_every_live_link_in_build_json_is_https(self):
        """Whatever the nightly run committed, no row ships a non-https href."""
        import json
        build = json.loads(ge.BUILD_JSON.read_text(encoding="utf-8"))
        for dev, d in (build.get("devices") or {}).items():
            for r in ((d.get("bom") or {}).get("rows") or []):
                url = (r.get("live") or {}).get("url")
                if url is not None:
                    self.assertTrue(url.startswith("https://"),
                                    f"{dev} {r.get('ref')}: {url!r}")


if __name__ == "__main__":
    unittest.main(verbosity=1)
