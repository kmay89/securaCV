#!/usr/bin/env python3
"""Tests for scripts/lint_design_lang.py's knob-help and knob-name rules.

The canon rules (a house value, or `deviates:` with a reason) are proven by
the tree itself every run. The rules below judge what
gen_builder_manifest.parse_scad KEEPS — the parser whose output the web
builder and the Lab catalog carry — so each test writes a tiny case file and
asks the lint what that parser would have dropped (the third rule), which
meaning it would have carried (the fourth), which group it would have filed
a knob under (the fifth), where a ranged knob's help stops (the sixth) or
which group header heads nothing (the seventh).

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

    def test_computed_neighbor_fails(self):
        # `vm_w = brd_w(...)` is not a literal, so the builder's parser reads
        # one knob on the line and hands it the whole help — which may be the
        # computed neighbor's fact (C32: this test used to keep that a pass)
        p = self.scad('/* [B] */\nvm_l = 40.0;  vm_w = brd_w("grove_v2");   // Grove Vision AI V2\n')
        problems, _ = L.lint_help_lines(p)
        self.assertEqual(len(problems), 1)
        self.assertIn("canary_probe.scad:2: the knob vm_l shares the line and its trailing "
                      "help with the computed vm_w", problems[0])
        self.assertIn("Move vm_w to a line of its own", problems[0])

    def test_the_planted_half_fails_by_name(self):
        # the shape the review planted: a knob, a value computed from it, one help
        p = self.scad('/* [Shell] */\nknob = 1;  half = knob / 2;  // help\n')
        problems, _ = L.lint_help_lines(p)
        self.assertEqual(len(problems), 1)
        self.assertIn("the computed half", problems[0])
        self.assertIn("hands that help to knob although the help may belong to the computed half",
                      problems[0])
        # and the parser it judges did hand it over: this is what the rule saves
        groups = L.gen_builder_manifest.parse_scad(p)
        self.assertEqual([(q["name"], q.get("desc")) for g in groups for q in g["params"]],
                         [("knob", "help")])

    def test_two_knobs_and_a_computed_neighbor_name_all_three(self):
        # two literals: the parser keeps the help for neither, so nothing is
        # handed to `a` — the message says what does happen
        p = self.scad('/* [B] */\na = 1;  b = 2;  c = a + b;  // help\n')
        problems, _ = L.lint_help_lines(p)
        self.assertEqual(len(problems), 1)
        self.assertIn("the knobs a, b share the line and its trailing help with the computed c",
                      problems[0])
        self.assertIn("none of the knobs gets it", problems[0])
        self.assertNotIn("hands that help", problems[0])
        self.assertEqual([q.get("desc") for g in L.gen_builder_manifest.parse_scad(p) for q in g["params"]],
                         [None, None])

    def test_a_ternary_or_a_let_neighbor_counts(self):
        # a ternary's value and a value computed through let() are assignments
        # the help may describe; the let()'s own binding is part of y, not a third
        p = self.scad('/* [B] */\nopt_x = true;  w = opt_x ? 1 : 0;  // the x opening\n'
                      'k = 2.0;  y = let(m = 3) k * m;  // the y scale\n')
        problems, _ = L.lint_help_lines(p)
        self.assertEqual(len(problems), 2)
        self.assertIn("canary_probe.scad:2: the knob opt_x", problems[0])
        self.assertIn("the computed w —", problems[0])
        self.assertIn("canary_probe.scad:3: the knob k", problems[1])
        self.assertIn("the computed y —", problems[1])

    def test_a_neighbor_that_is_not_a_value_is_named_for_what_it_is(self):
        # a function, a call with a named argument and a special variable each
        # take the help from the knob just the same, and none of them is a
        # "computed" value or a nameless `?` (review of C32)
        p = self.scad('/* [B] */\n'
                      'k1 = 1;  function half() = k1 / 2;  // help\n'
                      'k2 = 1;  echo(w = k2);  // help\n'
                      'k3 = 1;  $fn = 48;  // help\n')
        problems, _ = L.lint_help_lines(p)
        self.assertEqual(len(problems), 3)
        self.assertIn("canary_probe.scad:2: the knob k1 shares the line and its trailing help "
                      "with the function half() —", problems[0])
        self.assertIn("Move the function half() to a line of its own", problems[0])
        self.assertIn("canary_probe.scad:3: the knob k2 shares the line and its trailing help "
                      "with `echo(w = k2)` —", problems[1])
        self.assertIn("may belong to `echo(w = k2)`", problems[1])
        self.assertIn("canary_probe.scad:4: the knob k3 shares the line and its trailing help "
                      "with the special variable $fn —", problems[2])
        for problem in problems:
            self.assertNotIn("?", problem)
            self.assertNotIn("the computed", problem)
        self.assertEqual(L.assignments('k = 1;  function f(a = 2) = a;  for (i = [0:1]) echo(i);'),
                         [("k", "the computed k"), (None, "the function f()"),
                          (None, "`for (i = [0:1]) echo(i)`")])

    def test_a_computed_neighbor_without_help_passes(self):
        # nothing is handed over when there is no help to hand
        p = self.scad('/* [B] */\nknob = 1;  half = knob / 2;\n')
        self.assertEqual(L.lint_help_lines(p)[0], [])

    def test_a_strings_text_and_a_comparison_are_not_assignments(self):
        # the font name's `style=Bold` is text; `>=`, `<=`, `!=` and `==` compare;
        # a block comment is not code: each line holds its one knob
        p = self.scad('/* [B] */\n'
                      'label_font = "Liberation Sans:style=Bold";  // the label font\n'
                      'gap = 1.0;  assert(gap >= 0 && gap <= 2 && gap != 3 && gap == gap);  // gap\n'
                      'lip = 2.0;  /* lip_h = 3; */  // the lip\n')
        self.assertEqual(L.lint_help_lines(p)[0], [])
        self.assertEqual(L.assigned_names('label_font = "a=b";  // x = y'), ["label_font"])
        self.assertEqual(L.assigned_names('g = 1;  assert(g >= 0 && g <= 2 && g != 3 && g == g);'),
                         ["g"])
        self.assertEqual(L.assigned_names('a = 1;  /* b = 2; */  c = 3;  // d = 4'), ["a", "c"])
        self.assertEqual(L.assigned_names('   done */  e = 5;'), ["e"])
        self.assertEqual(L.assigned_names('f = 6;  /* g = 7; opens a block'), ["f"])

    def test_the_169_skirt_wall_keeps_its_own_help(self):
        # `skirt_wall = 1.6; skirt_dep = back_stack;   // must not exceed
        # back_stack` passed the rule above (skirt_dep is computed, so not a
        # knob), and gen_builder_manifest.parse_scad gave skirt_dep's help to
        # skirt_wall. No surface showed it: the Lab's parser skipped the
        # two-assignment line whole, and the web builder does not offer this
        # case. But the lint and HELP_DEBT read that parser, so they counted
        # skirt_wall as helped. Split (C10c-2), each line says its own fact;
        # test_enclosure_parsers.py holds what the Lab now lists.
        groups = L.gen_builder_manifest.parse_scad(L.ENC / "canary_s3_touch169.scad")
        knobs = {p["name"]: p for g in groups for p in g["params"]}
        desc = knobs["skirt_wall"].get("desc", "")
        self.assertIn("skirt", desc)
        self.assertNotIn("back_stack", desc)

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
# cases (C10c-1) carry help on every knob alone on its line except the field
# case's bez_on (below) and the bare `part` selectors (not counted, see
# knobs_without_help). What is left is handed up rather than done, because
# writing it moves catalog.json: each remaining knob but bez_on shares its
# line with another (`lip_w = 1.5;  lip_h = 2.0;`),
# and a one-per-line split makes the Lab parser read knobs it skips today
# (gen_enclosures.py reads one assignment per line), which moves the catalog's
# engineering_param_count; field_case's bez_on is a catalog option whose help
# would become its label.
# C10c-2 took the next monorepo-only cases (not the four carried ones, combo,
# fit_coupon, dash_display and watch_station, nor the released four): every
# numeric or text knob has help read off the geometry that uses it,
# and their shared lines are split one knob per line, each with its own help
# (that split moved catalog.json's engineering_param_count for seven products,
# +36 in all), and their boolean options have help too, which is also their
# catalog label (21 labels moved from the bare knob name). They hold at zero.
# C31: the released four (the WAP, the Sense, the Vision and its doorbell) have
# help on every knob, read off the geometry that uses it, and they stay at zero:
# a released case's new knob cannot land without help.
# C10, the carried four (combo, fit_coupon, dash_display, watch_station, whose
# bytes the website carries): every knob alone on its line has help except
# two of watch_station's, which is not edited here: label_text, so a one-string
# help does not force a re-render of its print STLs on the website
# (gen_print_files.py pins the file's sha256), and opt_batt, a catalog option
# whose help would become its label (C27's class). The rest share a line, as
# C10c-1's do, and wait on the same split (C27), which moves catalog.json.
HELP_DEBT = {
    "canary_bench_fixture.scad": 7,    # clip_w..clip_clear, tol_slide, tol_press (two shared lines)
    "canary_c3_lcd147.scad": 0,
    "canary_c6_display.scad": 0,
    "canary_combo.scad": 30,           # carried: 30 on ten shared lines (cam_post_d/cam_screw_d .. hood_len/hood_t)
    "canary_dash_display.scad": 6,     # carried: pilot_d/screw_c/cb_d, vent_n/vent_w/vent_l (two shared lines)
    "canary_dock.scad": 0,
    "canary_field_case.scad": 23,      # 22 on eight shared lines, and bez_on
    "canary_fit_coupon.scad": 6,       # carried: screw_d/screw_head_d, mag_d/lp_d, lip_t/lip_h (three shared lines)
    "canary_hammond_chassis.scad": 0,
    "canary_hub_din.scad": 0,
    "canary_inserts.scad": 0,
    "canary_jbox.scad": 12,            # lip_h/lip_t, screw_*, tol_slide/tol_hole, clip_*
    "canary_mount_adapters.scad": 0,
    "canary_outlet_cradle.scad": 0,
    "canary_relay_solar.scad": 0,
    "canary_s3_lcd147.scad": 0,
    "canary_s3_lcd7.scad": 0,
    "canary_s3_touch169.scad": 0,
    "canary_sense_enclosure.scad": 0,  # released: stays at zero
    "canary_sense_gang.scad": 0,
    "canary_sense_stand.scad": 0,
    "canary_shop_tools.scad": 0,
    "canary_sign.scad": 0,
    "canary_vehicle_mount.scad": 0,
    "canary_vision_doorbell.scad": 0,  # released: stays at zero
    "canary_vision_enclosure.scad": 0, # released: stays at zero
    "canary_vision_pro_mount.scad": 0,
    "canary_wap_enclosure.scad": 0,    # released: stays at zero
    "canary_watch_station.scad": 5,    # carried: opt_batt (C27's label class), label_text (rides its next
                                       # print-files re-render), label_size/label_depth/label_font (one shared line)
    "canary_wear_clip.scad": 0,
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


class EmptyGroups(_Case):
    """The seventh rule: a group header heads at least one assignment."""

    def test_the_169s_orphaned_header_fails(self):
        # origin/main's shape (C22): [Side cluster] sat directly over the
        # next header, so the side knobs below were filed as port datums
        p = self.scad('/* [Side cluster] */\n'
                      '/* [Port / button datums] */\n'
                      'btn_dz = 1.0;   // the button datum\n'
                      'opt_side = true;   // the side opening\n')
        problems, _ = L.lint_empty_groups(p)
        self.assertEqual(len(problems), 1)
        self.assertIn("canary_probe.scad:1: the group header [Side cluster] heads no assignment "
                      "before the next header [Port / button datums] (line 2)", problems[0])
        # what the rule saves: the parser did file opt_side as a port datum
        self.assertEqual([(g["name"], [q["name"] for q in g["params"]])
                          for g in L.gen_builder_manifest.parse_scad(p)],
                         [("Port / button datums", ["btn_dz", "opt_side"])])

    def test_the_moved_header_passes(self):
        p = self.scad('/* [Port / button datums] */\n'
                      'btn_dz = 1.0;   // the button datum\n'
                      '\n'
                      '/* [Side cluster] */\n'
                      'opt_side = true;   // the side opening\n')
        self.assertEqual(L.lint_empty_groups(p)[0], [])

    def test_a_last_header_with_nothing_before_the_first_module_fails(self):
        p = self.scad('/* [A] */\na = 1;\n/* [Vents] */\n// none, on purpose\n'
                      'module m() { b = 2; }\n')
        problems, _ = L.lint_empty_groups(p)
        self.assertEqual(len(problems), 1)
        self.assertIn(":3: the group header [Vents] heads no assignment before the first module",
                      problems[0])

    def test_any_assignment_counts_and_comments_do_not(self):
        # a [Quality] group holds only $fn, which the parser skips; a computed
        # value is not a knob either; both are something under the header. A
        # commented-out knob, in either comment form, is not
        p = self.scad('/* [Quality] */\n$fn = 48;\n'
                      '/* [Derived] */\nhalf = 2 / 2;\n'
                      '/* [Gone] */\n// a = 1;\n/* b = 2;\n   c = 3; */\n'
                      '/* [Last] */\nd = 4;\n')
        problems, _ = L.lint_empty_groups(p)
        self.assertEqual(len(problems), 1)
        self.assertIn(":5: the group header [Gone] heads no assignment before the next "
                      "header [Last] (line 9)", problems[0])

    def test_hidden_is_not_judged_but_a_header_over_it_is(self):
        p = self.scad('/* [A] */\n/* [Hidden] */\nx = 1;\n/* [Hidden] */\n')
        problems, _ = L.lint_empty_groups(p)
        self.assertEqual(len(problems), 1)
        self.assertIn("[A] heads no assignment before the next header [Hidden]", problems[0])

    def test_an_allowed_header_is_reported_seen_not_failed(self):
        p = self.scad('/* [Cooling] — deliberately no vents */\n// none\n/* [B] */\nb = 1;\n')
        problems, seen = L.lint_empty_groups(p, {("canary_probe.scad", "Cooling")})
        self.assertEqual((problems, seen), ([], {("canary_probe.scad", "Cooling")}))

    def test_the_headers_are_the_parsers_headers(self):
        # the rule reads headers the way parse_scad does: every group the
        # parser returns is a header the rule saw, in the same order
        for path in sorted(L.ENC.glob("canary_*.scad")):
            if path.name in L.SKIP:
                continue
            heads = [name for _, name, _ in L.customizer_lines(path)
                     if name and name.lower() != "hidden"]
            groups = [g["name"] for g in L.gen_builder_manifest.parse_scad(path)
                      if g["name"] != "Parameters" or "Parameters" in heads]
            it = iter(heads)
            self.assertTrue(all(g in it for g in groups), path.name)

    def test_the_ledger_matches_the_tree(self):
        problems, seen = [], set()
        for path in sorted(L.ENC.glob("canary_*.scad")):
            if path.name not in L.SKIP:
                found, s = L.lint_empty_groups(path, L.EMPTY_GROUP_OK.keys())
                problems += found
                seen |= s
        self.assertEqual(problems, [])
        self.assertEqual(seen, set(L.EMPTY_GROUP_OK), "an EMPTY_GROUP_OK entry heads "
                         "something now or is gone: delete it")
        for reason in L.EMPTY_GROUP_OK.values():
            self.assertTrue(reason.strip())


if __name__ == "__main__":
    unittest.main()
