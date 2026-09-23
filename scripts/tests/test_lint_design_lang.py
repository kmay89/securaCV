#!/usr/bin/env python3
"""Tests for scripts/lint_design_lang.py's knob-help rules.

The canon rules (a house value, or `deviates:` with a reason) are proven by
the tree itself every run. The help rules below judge what
gen_builder_manifest.parse_scad KEEPS — the parser whose output the web
builder and the Lab catalog carry — so each test writes a tiny case file and
asks the lint what that parser would have dropped.

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


if __name__ == "__main__":
    unittest.main()
