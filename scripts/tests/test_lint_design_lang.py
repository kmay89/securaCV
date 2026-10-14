#!/usr/bin/env python3
"""Tests for scripts/lint_design_lang.py's knob-help and knob-name rules.

The canon rules (a house value, or `deviates:` with a reason) are proven by
the tree itself every run. The rules below judge what
gen_builder_manifest.parse_scad KEEPS — the parser whose output the web
builder and the Lab catalog carry — so each test writes a tiny case file and
asks the lint what that parser would have dropped (the third rule), which
meaning it would have carried (the fourth), which group it would have filed
a knob under (the fifth) or where a ranged knob's help stops (the sixth).

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

    def test_the_tree_has_no_shared_help_line(self):
        # C10 split the 7" case's four, the last the ledger held: no case
        # file may drop a help to a shared line now, the ledger included
        self.assertEqual(L.HELP_LINE_DEBT, set())
        for path in sorted(L.ENC.glob("canary_*.scad")):
            if path.name not in L.SKIP:
                self.assertEqual(L.lint_help_lines(path)[0], [], path.name)


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

    def test_a_pigtail_pass_through_is_pigtail_not_usb(self):
        # C10: the J-box's -X wall slot passes a pigtail cable, never a plug or
        # its boot, so its help under usb_w is the transfer the rule exists for
        bad = self.scad('/* [Shell] */\n'
                        'usb_w = 10.5;  // pigtail pass-through slot width, low on the -X wall\n',
                        "canary_bad.scad")
        problems = L.lint_meanings(bad)
        self.assertEqual(len(problems), 1)
        self.assertIn("pigtail_w", problems[0])
        good = self.scad('/* [Shell] */\n'
                         'pigtail_w = 10.5;  // pigtail pass-through slot width\n'
                         'pigtail_h = 6.5;   // pigtail pass-through slot height\n',
                         "canary_good.scad")
        self.assertEqual(L.lint_meanings(good), [])
        # and the reverse: boot clearance under the pigtail name sends it back to usb_*
        rev = self.scad('/* [Shell] */\n'
                        'pigtail_h = 6.5;  // pigtail opening height: boot clearance\n',
                        "canary_rev.scad")
        problems = L.lint_meanings(rev)
        self.assertEqual(len(problems), 1)
        self.assertIn("usb_h", problems[0])

    def test_the_jbox_names_its_pass_through(self):
        groups = L.gen_builder_manifest.parse_scad(L.ENC / "canary_jbox.scad")
        knobs = {p["name"]: p for g in groups for p in g["params"]}
        self.assertNotIn("usb_w", knobs)
        self.assertNotIn("usb_h", knobs)
        for name in ("pigtail_w", "pigtail_h"):
            self.assertIn("pigtail", knobs[name].get("desc", ""), name)

    def test_no_help_passes_and_range_only_help_is_no_help(self):
        # nothing is said, so nothing can transfer (the builder strips a bare range)
        p = self.scad('/* [B] */\nusb_h  = 6.5;        // [4:0.5:8]\nskirt_t = 1.6;\n')
        self.assertEqual(L.lint_meanings(p), [])

    def test_the_tree_is_clean(self):
        for path in sorted(L.ENC.glob("canary_*.scad")):
            if path.name not in L.SKIP:
                self.assertEqual(L.lint_meanings(path), [], path.name)


def knobs_without_help(path: Path) -> list[str]:
    """The knobs gen_builder_manifest.parse_scad keeps with no help, in file
    order. An option-list selector (`part`, ...) is not counted: OpenSCAD
    2021.01's desktop Customizer draws a dropdown only for a bare bracket
    comment, so the house leaves those bare. A bare range is no help either
    (the builder strips it, leaving nothing to show)."""
    return [p["name"] for g in L.gen_builder_manifest.parse_scad(path)
            for p in g["params"] if not p.get("desc") and "options" not in p]


# C10c: the development cases' knobs with no help, per file, counted as above.
# Shrink-only: write help and lower the number in the same change; a knob that
# loses its help, or a new knob without one, fails. The three monorepo-only dev
# cases (C10c-1) carry help on every knob alone on its line. What is left is
# handed up rather than done, because writing it moves catalog.json: each
# remaining knob shares its line with another (`lip_w = 1.5;  lip_h = 2.0;`),
# and a one-per-line split makes the Lab parser read knobs it skips today
# (gen_enclosures.py reads one assignment per line), which moves the catalog's
# engineering_param_count; field_case's bez_on is a catalog option whose help
# would become its label.
HELP_DEBT = {
    "canary_bench_fixture.scad": 7,    # clip_w..clip_clear, tol_slide, tol_press (two shared lines)
    "canary_field_case.scad": 23,      # 22 on eight shared lines, and bez_on
    "canary_jbox.scad": 12,            # lip_h/lip_t, screw_*, tol_slide/tol_hole, clip_*
}


class HelpDebt(_Case):
    """C10c: the dev cases' help debt only shrinks."""

    def test_the_count_skips_selectors_and_counts_bare_ranges(self):
        p = self.scad('/* [What to render] */\npart = "all";  // ["a","b","all"]\n'
                      '/* [G] */\nhelped = 1.0;  // has help\nbare = 2.0;\n'
                      'ranged = 3.0;  // [1:0.5:5]\nshared_a = 1.0;  shared_b = 2.0;\n'
                      '/* [Hidden] */\nsecret = 4.0;\n')
        self.assertEqual(knobs_without_help(p), ["bare", "ranged", "shared_a", "shared_b"])

    def test_the_ledger_matches_the_tree(self):
        for name, owed in sorted(HELP_DEBT.items()):
            missing = knobs_without_help(L.ENC / name)
            self.assertLessEqual(len(missing), owed,
                                 f"{name}: {len(missing)} knobs without help, the ledger allows "
                                 f"{owed} — write help for {missing}")
            self.assertEqual(len(missing), owed,
                             f"{name}: only {len(missing)} knobs without help now — lower "
                             f"HELP_DEBT['{name}'] to {len(missing)} (it only shrinks)")


class InterfaceGroup(_Case):
    """Fifth rule: the stud/keyhole interface keeps one group name."""

    def test_the_old_names_fail(self):
        # three of the 14 names the audit found, each holding interface knobs
        p = self.scad('/* [Keyholes] — blind, seal-safe */\nkh_head_d = 7.0;  // pass hole\n'
                      '/* [Mounting] */\nkh_face = 1.0;  // face web\n'
                      '/* [Stud interface] — match the case */\nstud_gap = 36.0;  // spacing\n')
        problems = L.lint_interface_group(p)
        self.assertEqual([x.split(":", 2)[1] for x in problems], ["2", "4", "6"])
        self.assertIn("Stud/keyhole interface", problems[0])

    def test_the_one_name_passes_with_a_note_and_neighbors(self):
        # a per-file note after the bracket is fine; so are the toggles and
        # placements that ride along (opt_keyhole, kh_extra, kh_inset)
        p = self.scad('/* [Stud/keyhole interface] — blind pockets in the back */\n'
                      'opt_keyhole = true;  // one pocket\nkh_extra = 3.0;  // back thickening\n'
                      'kh_head_d = 7.0;  // pass hole\nkh_inset = 12.0;  // centers\n'
                      '/* [Engineering] */\nkh_lock = true;  // knockouts, not the interface\n')
        self.assertEqual(L.lint_interface_group(p), [])

    def test_the_exempt_file_is_named_with_its_reason(self):
        self.assertIn("canary_c3_lcd147.scad", L.INTERFACE_EXEMPT)
        p = self.scad('/* [Back face] */\nkh_head_d = 9.5;  // egg base\n', "canary_c3_lcd147.scad")
        self.assertEqual(L.lint_interface_group(p), [])

    def test_the_tree_is_clean(self):
        for path in sorted(L.ENC.glob("canary_*.scad")):
            if path.name not in L.SKIP:
                self.assertEqual(L.lint_interface_group(path), [], path.name)


class RangedHelp(_Case):
    """Sixth rule: a ranged knob's help ends on its own line."""

    def test_the_three_wraps_the_review_found_fail(self):
        # the shapes C10 left: a next line going on in lowercase, a dangling
        # "The" before a capitalized proper noun, a trailing preposition
        p = self.scad(
            '/* [Shell] */\n'
            'floor_cove = 0.8;  // 45° cove, inside (cavity_cut): the sharp  // [0:0.2:1.2]\n'
            '                   // notch there was the crack-starter\n'
            'clip_dx = 5.25;  // clip centers (the validated spot). The  // [3:0.25:8]\n'
            "                 // XIAO's castellated pads run to ±8.5\n"
            'hood_seat = 0.6;  // groove the spigot presses into; bond with  // [0.4:0.1:1.0]\n'
            '                  // neutral-cure silicone.\n')
        problems = L.lint_ranged_help(p)
        self.assertEqual([x.split(":", 2)[1] for x in problems], ["2", "4", "6"])
        self.assertIn("mid-sentence", problems[0])
        self.assertIn("dangling 'The'", problems[1])
        self.assertIn("dangling 'with'", problems[2])

    def test_an_open_parenthesis_fails(self):
        p = self.scad('/* [Shell] */\ngasket = 0.3;  // stand-proud (~20 % squeeze;  // [0:0.1:1]\n'
                      '               // TPU is incompressible)\n')
        self.assertEqual(len(L.lint_ranged_help(p)), 1)

    def test_a_whole_sentence_then_a_new_one_passes(self):
        p = self.scad('/* [Shell] */\n'
                      'floor_cove = 0.8;  // 45° cove, inside (cavity_cut); 0 = a square corner  // [0:0.2:1.2]\n'
                      '                   // The sharp notch there was the crack-starter.\n'
                      'hood_len = 9.0;  // rain-hood protrusion  // [5:0.5:15]\n'
                      'hood_t = 1.8;  // hood wall\n')
        self.assertEqual(L.lint_ranged_help(p), [])

    def test_unranged_and_unwrapped_knobs_are_not_judged(self):
        # no range: not this rule's (yet); a range with no continuation: whole by definition
        p = self.scad('/* [Board] */\nboard_w = 17.5;  // a real board mics\n'
                      '                 // 17.8 (canary_dock lesson)\n'
                      'usb_h = 6.5;  // opening height for the\n'
                      'lid_t = 2.0;  // lid  // [1:0.5:3]\n'
                      '// an above-line comment for the next knob\n'
                      'lid_h = 3.0;  // lid height  // [2:0.5:5]\n')
        self.assertEqual(L.lint_ranged_help(p), [])

    def test_the_tree_is_clean(self):
        for path in sorted(L.ENC.glob("canary_*.scad")):
            if path.name not in L.SKIP:
                self.assertEqual(L.lint_ranged_help(path), [], path.name)


if __name__ == "__main__":
    unittest.main()
