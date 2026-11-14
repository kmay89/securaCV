"""scripts/lint_site_links.py — every securacv.com link in this tree opens
something the website says it answers. These tests pin what the lint reads
(three ways a link is written), what it refuses (the four defects it was
written for), and that the committed tree is clean against the carried
contract."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import lint_site_links as lint  # noqa: E402

CONTRACT = {
    "routes": ["/", "/about", "/compare", "/help", "/home-assistant", "/lab", "/tv/"],
    "files": ["/share-card.jpg", "/store.json"],
    "anchors": {"/": ["install"], "/help": ["s-safe-mode", "probe-sd_card"]},
}


def problems(files: dict[str, str]) -> list[str]:
    links, _ = lint.scan(list(files), read=files.__getitem__)
    return lint.check(links, CONTRACT)


class TheFourDefectsItWasWrittenFor(unittest.TestCase):
    def test_a_page_that_was_never_a_page(self):
        # the Lab's footer: securacv.com/site-map.html
        out = problems({"a.js": 'x = "https://securacv.com/site-map.html";'})
        self.assertEqual(len(out), 1)
        self.assertIn("is not an address securacv.com answers", out[0])

    def test_a_short_link_nobody_made(self):
        # the welcome mail: /faq, before the site redirected it
        out = problems({"mail.mjs": "see https://securacv.com/faq for answers"})
        self.assertIn("is not an address", out[0])

    def test_an_anchor_the_page_lacks_written_as_a_joined_constant(self):
        # the desktop atlas: SITE + "/#about" — no URL regex sees this one
        out = problems({"app.js": 'const SITE = "x"; go(SITE + "/#about");'})
        self.assertEqual(len(out), 1)
        self.assertIn("#about is not an anchor on /", out[0])
        self.assertIn("joined", out[0])

    def test_a_file_name_instead_of_the_route(self):
        # the Lab's room: /compare.html answers, and is refused: one address per page
        out = problems({"room.js": "'https://securacv.com/compare.html'"})
        self.assertIn("the page's address is /compare", out[0])


class WhatCountsAsGood(unittest.TestCase):
    def test_routes_files_anchors_queries_and_trailing_slashes(self):
        good = {
            "a.md": "\n".join([
                "The site: https://securacv.com and https://securacv.com/.",
                "[help](https://securacv.com/help), then (https://securacv.com/help#s-safe-mode).",
                "A deep link: https://securacv.com/#install",
                "A query: https://securacv.com/lab?product=wap&tour=1",
                "A directory route: https://securacv.com/tv/ and https://securacv.com/about/",
                "A file: https://securacv.com/store.json",
                "`https://securacv.com/home-assistant`",
            ]),
            "b.js": 'open(SITE_ORIGIN + "/help"); open(SITE + \'/compare\');',
        }
        self.assertEqual(problems(good), [])

    def test_sentence_punctuation_is_not_part_of_the_link(self):
        self.assertEqual(problems({"a.md": "Read https://securacv.com/help. Then https://securacv.com/about, too."}), [])

    def test_another_host_is_not_this_site(self):
        text = "https://securacv.com.evil.example/x https://ops.securacv.com/forms https://securacv.community/y"
        links, _ = lint.scan(["a.md"], read={"a.md": text}.__getitem__)
        self.assertEqual(links, [])

    def test_a_templated_path_is_not_a_link_yet(self):
        text = 'u = "https://securacv.com/${page}"; v = f"https://securacv.com/{slug}"; w = "https://securacv.com/…"'
        self.assertEqual(problems({"a.js": text}), [])

    def test_an_anchor_on_a_file_or_on_a_page_without_anchors_is_refused(self):
        out = problems({"a.md": "https://securacv.com/store.json#x https://securacv.com/about#team"})
        self.assertEqual(len(out), 2)
        self.assertIn("is a file", out[0])
        self.assertIn("has no anchors at all", out[1])


class TheLabManifest(unittest.TestCase):
    MANIFEST = {
        "onramp": {"site": "/lab"},
        "stages": [{"site": [{"noun": "Compare", "href": "/compare"},
                             {"noun": "Old", "href": "/site-map.html"}]}],
        "siteNav": [{"label": "Help", "href": "/help"}],
        "footerNav": [{"label": "GitHub", "href": "https://github.com/kmay89/securaCV"},
                      {"label": "Gone", "href": "/gone"}],
    }

    def test_site_relative_hrefs_are_links_to_the_site(self):
        text = json.dumps(self.MANIFEST, indent=2)
        out = problems({lint.MANIFEST: text})
        self.assertEqual(len(out), 2, out)
        self.assertTrue(any('"/site-map.html"' in p and "manifest" in p for p in out))
        self.assertTrue(any('"/gone"' in p for p in out))

    def test_the_same_json_under_another_name_is_only_read_for_urls(self):
        self.assertEqual(problems({"other.json": json.dumps(self.MANIFEST)}), [])


class Housekeeping(unittest.TestCase):
    def test_binaries_bundles_and_the_lint_itself_are_not_read(self):
        for rel in ["a/dist/app.js", "x.png", "vendor/lib.js", "scripts/lint_site_links.py",
                    "canary-local/emulator/web/dist/display.wasm", "a.min.js", "Cargo.lock"]:
            self.assertFalse(lint.scannable(rel), rel)
        for rel in ["README.md", "canary-local/assets/room.js", "ios/Shared/EcosystemMap.swift",
                    "firmware/common/ui/help_qr.h", lint.MANIFEST]:
            self.assertTrue(lint.scannable(rel), rel)

    def test_an_example_file_is_skipped_and_reported_as_used(self):
        rel = next(iter(lint.EXAMPLES))
        links, used = lint.scan([rel], read={rel: "https://securacv.com/made-up"}.__getitem__)
        self.assertEqual((links, used), ([], [rel]))

    def test_every_example_still_holds_a_link(self):
        _, used = lint.scan(lint.tracked())
        self.assertEqual(sorted(set(lint.EXAMPLES) - set(used)), [],
                         "an EXAMPLES entry names a file with no securacv.com link — remove it")

    def test_a_contract_without_the_address_lists_stops_the_run(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            old = Path(tmp) / "site_contract.json"
            old.write_text(json.dumps({"paths": [], "patterns": []}), encoding="utf-8")
            with self.assertRaises(SystemExit) as ctx:
                lint.load_contract(old)
            self.assertIn("predates the site's address list", str(ctx.exception))


class TheCommittedTree(unittest.TestCase):
    def test_every_link_in_this_repository_is_answered(self):
        contract = lint.load_contract()
        links, _ = lint.scan(lint.tracked())
        self.assertGreater(len(links), 100, "the scan found almost nothing — it has stopped reading the tree")
        self.assertEqual(lint.check(links, contract), [])

    def test_the_scan_sees_all_three_ways_a_link_is_written(self):
        links, _ = lint.scan(lint.tracked())
        kinds = {how for _, _, _, how in links}
        self.assertEqual(kinds, {"url", "joined", "manifest"})

    def test_the_anchors_a_canary_prints_are_checked_not_skipped(self):
        # firmware draws these as QR codes; they must be among the links read
        links, _ = lint.scan(lint.tracked())
        raws = {raw for _, _, raw, _ in links}
        self.assertTrue(any(r.startswith("/help#s-") for r in raws), "no /help#s-… link found in the tree")


if __name__ == "__main__":
    unittest.main()
