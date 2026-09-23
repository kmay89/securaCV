#!/usr/bin/env python3
"""Tests for scripts/lint_design_lang.py's knob-help and knob-name rules.

The canon rules (a house value, or `deviates:` with a reason) are proven by
the tree itself every run. The two rules below judge what
gen_builder_manifest.parse_scad KEEPS — the parser whose output the web
builder and the Lab catalog carry — so each test writes a tiny case file and
asks the lint what that parser would have dropped (the third rule) or which
meaning it would have carried (the fourth).

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("lint_design_lang", ROOT / "scripts" / "lint_design_lang.py")
L = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(L)


class _Case(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def scad(self, body: str, name: str = "canary_probe.scad") -> Path:
        p = self.dir / name
        p.write_text(body, encoding="utf-8")
        return p


class HelpLines(_Case):
    """Third rule: a knob's help stays on the knob's own line."""

    def test_shared_trailing_help_fails(self):
        p = self.scad('/* [Board] */\nboard_l = 21.0;  board_w = 17.5;   // the xiao spec\n')
        problems, _ = L.lint_help_lines(p)
        self.assertEqual(len(problems), 1)
        self.assertIn("canary_probe.scad:2: 2 knobs (board_l, board_w)", problems[0])

    def test_split_lines_pass(self):
        p = self.scad('/* [Board] */\nboard_l = 21.0;   // length — brd_l("xiao")\n'
                      'board_w = 17.5;   // width — brd_w("xiao")\n')
        self.assertEqual(L.lint_help_lines(p)[0], [])

    def test_shared_line_without_help_passes(self):
        # nothing is being dropped: the rule is about help that goes missing
        p = self.scad('/* [Board] */\ncam_w = 25.0;  cam_h = 24.0;\n')
        self.assertEqual(L.lint_help_lines(p)[0], [])

    def test_hidden_group_and_module_locals_are_not_knobs(self):
        # the parser's own boundaries: [Hidden] and everything after `module`
        p = self.scad('/* [Hidden] */\na = 1;  b = 2;   // not a knob\n'
                      'module m() { }\nc = 1;  d = 2;   // after the first module\n')
        self.assertEqual(L.lint_help_lines(p)[0], [])

    def test_computed_neighbor_does_not_count(self):
        # `vm_w = brd_w(...)` is not a literal knob, so one knob keeps its help
        p = self.scad('/* [B] */\nvm_l = 40.0;  vm_w = brd_w("grove_v2");   // Grove Vision AI V2\n')
        self.assertEqual(L.lint_help_lines(p)[0], [])

    def test_debt_entry_is_reported_seen_not_failed(self):
        p = self.scad('/* [B] */\nlob_d = 9.0;  lob_o = 3.0;   // lobe Ø / offset\n')
        problems, seen = L.lint_help_lines(p, {("canary_probe.scad", "lob_d")})
        self.assertEqual(problems, [])
        self.assertEqual(seen, {("canary_probe.scad", "lob_d")})

    def test_debt_ledger_matches_the_tree(self):
        # every HELP_LINE_DEBT entry is still a real shared-help line — the
        # ledger only shrinks, and main() fails an entry nobody saw
        seen = set()
        for name in {n for n, _ in L.HELP_LINE_DEBT}:
            seen |= L.lint_help_lines(L.ENC / name, L.HELP_LINE_DEBT)[1]
        self.assertEqual(seen, set(L.HELP_LINE_DEBT))


class Meanings(_Case):
    """Fourth rule: a shared knob name keeps one meaning, stated in its help."""

    def test_the_four_transfers_the_audit_found_fail(self):
        # each is the minority meaning's REAL help, under the majority's name
        p = self.scad('/* [B] */\n'
                      'usb_w  = 9.05;     // stadium opening width — shell + 0.11 total\n'
                      'vm_l   = 44.0;     // carrier length (Y; XIAO/USB edge down) — brd_l("mr60")\n'
                      'skirt_t = 1.0;     // finger thickness — the skirt is relieved to this\n'
                      'clip_w = 45.0;     // width (extrusion length)\n')
        problems = L.lint_meanings(p)
        self.assertEqual([x.split(":", 2)[1] for x in problems], ["2", "3", "4", "5"])
        self.assertIn("usb_shell_w", problems[0])
        self.assertIn("radar_l", problems[1])
        self.assertIn("finger_t", problems[2])
        self.assertIn("leaf_w", problems[3])

    def test_the_majority_meanings_pass(self):
        p = self.scad('/* [B] */\n'
                      'usb_w = 12.0;   // opening width: clears rugged USB-C cable boots\n'
                      'usb_h = 6.5;    // opening height: boot clearance\n'
                      'vm_w = 20.0;    // module short side (X) — measured, brd_w("grove_v2")\n'
                      'skirt_t = 1.6;  // skirt wall thickness\n'
                      'clip_w = 6.0;   // tab width (along the board edge)\n')
        self.assertEqual(L.lint_meanings(p), [])

    def test_the_renamed_minorities_hold_their_own_meaning(self):
        good = self.scad('/* [B] */\nradar_l = 44.0;  // carrier length — brd_l("mr60")\n'
                         'finger_t = 1.0;  // finger thickness\nleaf_w = 45.0;  // belt-clip width\n'
                         'usb_slot_w = 11.0;  // side-slot width\nusb_shell_h = 4.5;  // shell height\n',
                         "canary_good.scad")
        self.assertEqual(L.lint_meanings(good), [])
        # and a Grove module's help under the radar name is the same transfer, reversed
        bad = self.scad('/* [B] */\nradar_l = 40.0;  // Grove Vision AI V2 long side\n', "canary_bad.scad")
        self.assertEqual(len(L.lint_meanings(bad)), 1)

    def test_no_help_passes_and_range_only_help_is_no_help(self):
        # nothing is said, so nothing can transfer (the builder strips a bare range)
        p = self.scad('/* [B] */\nusb_h  = 6.5;        // [4:0.5:8]\nskirt_t = 1.6;\n')
        self.assertEqual(L.lint_meanings(p), [])

    def test_the_tree_is_clean(self):
        for path in sorted(L.ENC.glob("canary_*.scad")):
            if path.name not in L.SKIP:
                self.assertEqual(L.lint_meanings(path), [], path.name)


if __name__ == "__main__":
    unittest.main()
