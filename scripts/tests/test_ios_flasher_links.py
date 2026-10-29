#!/usr/bin/env python3
"""The iPhone app's flasher link, held to the flasher it points at.

The phone never flashes (iOS has no USB serial). A blank display's
walkthrough starts with "Put SecuraCV on it — from a computer": the phone
hands a computer the in-browser flasher's link, focused on one product with
`?product=<id>` (canary-local/assets/flash-core.js `preferredProductId`).
That id lives in Swift (`FlasherLinks` in ios/Shared/SetupGuide.swift), and
there is no Swift toolchain on the Linux CI runners, so this test reads the
source and holds it to the truth on disk:

  * every product id `FlasherLinks` names is a real product in the
    flasher's catalog (canary-local/devices/flash.json) — rename the product
    and this fails instead of the link silently opening an unfocused list;
  * the id passes the flasher's own deep-link filter (`^[a-z0-9-]+$`), so
    the focus hint is honored rather than dropped;
  * the link's page is the flasher page this tree publishes
    (canary-local/flash.html), served from the repo's GitHub Pages root;
  * the Glance AMOLED's id is the catalog entry for the Waveshare
    ESP32-S3-Touch-AMOLED-2.41 board, the board the walkthrough names.

It says what the source says, not that it compiles; `ios-selfheal.yml`
compiles it and SetupGuideTests runs the same URL through Foundation.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SWIFT = ROOT / "ios" / "Shared" / "SetupGuide.swift"
CATALOG = ROOT / "canary-local" / "devices" / "flash.json"
FLASH_PAGE = ROOT / "canary-local" / "flash.html"
PAGES_ROOT = "https://kmay89.github.io/securaCV/"


def _flasher_block(src: str) -> str:
    m = re.search(r"enum FlasherLinks \{(.*?)\n\}", src, re.S)
    if not m:
        raise AssertionError("ios/Shared/SetupGuide.swift has no `enum FlasherLinks`")
    return m.group(1)


def _string_constants(block: str) -> dict[str, str]:
    return dict(re.findall(r'static let (\w+)\s*=\s*"([^"]*)"', block))


class FlasherLinksTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.block = _flasher_block(SWIFT.read_text(encoding="utf-8"))
        cls.consts = _string_constants(cls.block)
        catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
        cls.products = {p["id"]: p for p in catalog["products"]}

    def test_every_named_product_is_in_the_flasher_catalog(self) -> None:
        m = re.search(r"static let products\s*=\s*\[([^\]]*)\]", self.block)
        self.assertIsNotNone(m, "FlasherLinks.products lists the ids it names")
        names = [n.strip() for n in m.group(1).split(",") if n.strip()]
        self.assertTrue(names, "FlasherLinks.products is not empty")
        for name in names:
            self.assertIn(name, self.consts, f"{name} is a string constant in FlasherLinks")
            pid = self.consts[name]
            self.assertIn(pid, self.products, f"{pid} is a product in flash.json")
            self.assertRegex(pid, r"^[a-z0-9-]+$", "the flasher's deep-link filter keeps it")

    def test_the_glance_amoled_id_is_the_waveshare_241_board(self) -> None:
        pid = self.consts.get("glanceAMOLEDProduct")
        self.assertIsNotNone(pid)
        product = self.products[pid]
        self.assertEqual(product["board_label"], "Waveshare ESP32-S3-Touch-AMOLED-2.41")
        self.assertEqual(product["name"], "Canary Glance AMOLED")

    def test_the_link_is_this_trees_flasher_page(self) -> None:
        url = self.consts.get("webFlasher")
        self.assertIsNotNone(url)
        self.assertTrue(url.startswith(PAGES_ROOT), url)
        rel = url[len(PAGES_ROOT):]
        self.assertEqual(ROOT / rel, FLASH_PAGE, "the link names canary-local/flash.html")
        self.assertTrue(FLASH_PAGE.is_file())

    def test_the_flasher_still_honors_the_product_focus(self) -> None:
        core = (ROOT / "canary-local" / "assets" / "flash-core.js").read_text(encoding="utf-8")
        self.assertIn('get("product")', core, "flash-core.js reads ?product=")


if __name__ == "__main__":
    unittest.main()
