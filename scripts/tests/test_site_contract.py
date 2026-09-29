"""canary-local/devices/site_contract.json — the website's runtime contract on
this tree — names files the site fetches live (showroom STLs, board GLBs,
figure SVGs, the device catalogs). Every path must exist and every pattern
must match, or the site 404s in a visitor's browser. The file is carried from
the website by gen_builder_manifest.py --site (see its reverse-carry block);
these tests read the committed copy."""
from __future__ import annotations

import json
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CONTRACT = REPO / "canary-local" / "devices" / "site_contract.json"


class SiteContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not CONTRACT.is_file():
            raise unittest.SkipTest(f"{CONTRACT.relative_to(REPO)} not carried yet "
                                    "(gen_builder_manifest.py --site <website>)")
        cls.data = json.loads(CONTRACT.read_text(encoding="utf-8"))

    def test_shape(self):
        self.assertEqual(self.data["carried_from"], "securacv_website/upstream-contract.json")
        self.assertIsInstance(self.data["paths"], list)
        self.assertIsInstance(self.data["patterns"], list)
        self.assertTrue(self.data["paths"] or self.data["patterns"], "an empty contract gates nothing")
        self.assertEqual(self.data["paths"], sorted(set(self.data["paths"])))
        self.assertEqual(self.data["patterns"], sorted(set(self.data["patterns"])))

    def test_every_path_the_site_fetches_exists(self):
        missing = [p for p in self.data["paths"] if not (REPO / p).is_file()]
        self.assertEqual(missing, [], "the website fetches these from this tree at runtime "
                                      "and they are gone — a rename here 404s on securacv.com; "
                                      "restore the file or change the site first")

    def test_every_pattern_the_site_fetches_matches(self):
        empty = [g for g in self.data["patterns"] if not any(REPO.glob(g))]
        self.assertEqual(empty, [], "no file in this tree matches these patterns the website "
                                    "reads at runtime")

    def test_paths_are_repository_relative(self):
        for x in [*self.data["paths"], *self.data["patterns"]]:
            self.assertFalse(x.startswith("/"), x)
            self.assertNotIn("..", x.split("/"), x)
