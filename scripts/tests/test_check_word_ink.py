#!/usr/bin/env python3
"""Tests for docs/hardware/enclosure/check_word_ink.py — the 7-inch frame's
brand words held to the keep-outs by their measured ink.

No OpenSCAD and no fontconfig here: the placement, the five transcribed
asserts, the known-failure verdict, the DXF reader, the probe parser, the
font answer and the source pins are pure Python, fed synthetic extents, and
the two subprocess calls (fc-match, the OpenSCAD word render) are patched.
The measuring half runs in enclosure.yml (`check_word_ink.py --check`).

  • place_word() is frame()'s transform: the triage's DXF extents land where
    the case cuts them (SECURACV 13.74..72.46, CANARY -64.68..-20.75), and
    place_label() starts the port label where frame() does, at port_lbl_x;
  • evaluate() holds each assert with the ink in place of the estimate,
    strictly, behind the case's own guards, and a keep-out finds the word on
    its own side of the port; the port label's ink must meet no word's;
  • the verdict fails a new failure, a KNOWN one that stopped, and a KNOWN
    one whose overlap moved from its record either way, and the shipped
    numbers give exactly the one KNOWN failure (C33) at its record;
  • the vent_gap and band facts never reach the exit status: evaluate()
    runs on a case that does not hold them, main() answers the same
    whatever they say, and an off-midpoint band from the probe is left
    unreported, not refused;
  • the pins find the live case's two cuts and five asserts once each,
    standing as statements of their own, ignore comments, and refuse a
    moved, wrapped or guarded one;
  • a fallback font refuses before any word is rendered; a word render that
    warns, fails or writes nothing refuses; a malformed DXF and a probe that
    cannot be read refuse.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
ENC = REPO / "docs" / "hardware" / "enclosure"
sys.path.insert(0, str(ENC))
spec = importlib.util.spec_from_file_location("check_word_ink", ENC / "check_word_ink.py")
assert spec is not None and spec.loader is not None
cw = importlib.util.module_from_spec(spec)
sys.modules["check_word_ink"] = cw  # dataclasses resolve their module by name
spec.loader.exec_module(cw)

# The case's values at the shipped defaults, as the probe echoes them
# (OpenSCAD 2021.01, canary_s3_lcd7.scad at 5d658112).
SHIPPED = {
    "vword_x": 42.9, "vword_half": 21.952, "edge_text_size": 7.0,
    "usb_dx": 0.0, "usb_open_w": 13.6, "grom_lip": 2.0, "vent_gap": 8.0,
    "port_teth_dx": -12.6, "sd_teth_hole": 3.2,
    "port_scoop_dx": 11.0, "port_scoop_ftp": 3.44093,
    "dock_key_bx": 84.0, "gill_w": 2.4, "flat": 88.28, "port_lbl_x": 70.852,
    "band_lo": 16.8, "band_hi": 69.0,
    "usb_port": True, "port_tether": True, "port_scoop": True,
    "port_labels": True, "dock_keys": True,
    "edge_text_l": "SECURACV", "edge_text_r": "CANARY",
    "label_font": "Liberation Sans:style=Bold", "port_lbl_a": "",
}
# The words' DXF ink at size 7 (Liberation Sans Bold, spacing 1.12, centered):
# the triage's t_SECURACV.dxf / t_CANARY.dxf, which a live render reproduces.
DXF_INK = {"SECURACV": (-29.5571, 29.1623), "CANARY": (-22.1485, 21.7822)}
# The port label's DXF ink at size 3.6 (Liberation Sans Bold, halign left), a
# live render each: frame() starts it at port_lbl_x, from the estimate.
LABEL_INK = {"USB": (0.300262, 10.1666), "USB-C": (0.300262, 15.4091)}


def inks(case=None, scale=1.0):
    """The two words placed as frame() places them; `scale` stands in for a
    smaller edge_text_size (ink scales with size)."""
    case = case or SHIPPED
    out = []
    for knob, s in cw.WORDS:
        lo, hi = DXF_INK[case[knob]]
        out.append(cw.place_word(knob, case[knob], lo * scale, hi * scale, s, case["vword_x"]))
    return out


def label(case):
    """The port label placed as frame() places it, or None when it cuts none."""
    if not cw.label_is_cut(case):
        return None
    lo, hi = LABEL_INK[case["port_lbl_a"]]
    return cw.place_label(case["port_lbl_a"], lo, hi, case["port_lbl_x"])


def keys(case, scale=1.0):
    return sorted(f.key for f in cw.evaluate(case, inks(case, scale), label(case)))


def with_(**kw):
    return {**SHIPPED, **kw}


class Placement(unittest.TestCase):
    def test_words_land_where_frame_cuts_them(self):
        sec, can = inks()
        self.assertEqual((sec.knob, sec.text), ("edge_text_l", "SECURACV"))
        self.assertAlmostEqual(sec.lo, 13.74, places=2)
        self.assertAlmostEqual(sec.hi, 72.46, places=2)
        self.assertAlmostEqual(sec.width, 58.72, places=2)
        self.assertEqual((can.knob, can.text), ("edge_text_r", "CANARY"))
        self.assertAlmostEqual(can.lo, -64.68, places=2)
        self.assertAlmostEqual(can.hi, -20.75, places=2)
        self.assertAlmostEqual(can.width, 43.93, places=2)

    def test_the_turn_mirrors_the_ink_about_the_word_center(self):
        # an ink sitting right of its own center lands left of vword_x after
        # the 180-degree turn, on either side
        w = cw.place_word("edge_text_l", "X", -1.0, 3.0, 1, 40.0)
        self.assertEqual((w.lo, w.hi), (37.0, 41.0))
        w = cw.place_word("edge_text_r", "X", -1.0, 3.0, -1, 40.0)
        self.assertEqual((w.lo, w.hi), (-43.0, -39.0))

    def test_inner_and_outer(self):
        w = cw.Ink("k", "W", -64.68, -20.75)
        self.assertAlmostEqual(w.inner, 20.75)
        self.assertAlmostEqual(w.outer, 64.68)
        self.assertEqual(cw.Ink("k", "W", -2.0, 5.0).inner, 0.0)   # straddles the port

    def test_the_label_starts_where_frame_starts_it(self):
        # port_lbl_x = vword_x + vword_half + 6.0, from the estimate: 1.30 mm
        # inside SECURACV's real ink, and not turned
        lab = label(with_(port_lbl_a="USB"))
        self.assertEqual((lab.knob, lab.text), ("port_lbl_a", "USB"))
        self.assertAlmostEqual(lab.lo, 71.15, places=2)
        self.assertAlmostEqual(lab.hi, 81.02, places=2)
        self.assertIsNone(label(SHIPPED))                      # ships empty
        self.assertIsNone(label(with_(port_lbl_a="USB", port_labels=False)))
        self.assertIsNone(label(with_(port_lbl_a="USB", usb_port=False)))


class Asserts(unittest.TestCase):
    def test_shipped_has_exactly_the_scoop_failure(self):
        fails = cw.evaluate(SHIPPED, inks())
        self.assertEqual([f.key for f in fails], ["scoop|edge_text_l"])
        self.assertIn("by 1.70 mm", fails[0].message)
        self.assertIn("6.56..15.44", fails[0].message)
        self.assertEqual(fails[0].pin, "scoop")
        self.assertAlmostEqual(fails[0].by, 11.0 + 3.44093 + 1 - (42.9 - 29.1623), places=6)

    def test_option_a_and_c_clear_it(self):
        self.assertEqual(keys(with_(port_scoop_dx=8.9)), [])          # (c)
        self.assertEqual(keys(SHIPPED, scale=6.2 / 7.0), [])          # (a)

    def test_strict_like_the_case(self):
        # a keep-out that ends exactly where the ink starts fails, as the
        # case's `<` does: reach = 13.7377 - 11 puts its edge on the V's
        # inboard ink, SECURACV's last letter after frame()'s 180-degree turn
        sec_lo = inks()[0].lo
        self.assertIn("scoop|edge_text_l",
                      keys(with_(port_scoop_ftp=sec_lo - 11.0 - 1.0)))
        self.assertNotIn("scoop|edge_text_l",
                         keys(with_(port_scoop_ftp=sec_lo - 11.0 - 1.0 - 1e-6)))

    def test_side_follows_the_sign_of_dx(self):
        self.assertEqual(keys(with_(port_teth_dx=-11.0)), ["scoop|edge_text_l"])
        # the anchor keep-out reaches 18.3 + 1.6 + 1 = 20.9 > 20.75: CANARY's
        # side, and only CANARY's
        self.assertEqual(keys(with_(port_teth_dx=-18.3)),
                         ["anchor|edge_text_r", "scoop|edge_text_l"])
        # swapped to +x it meets SECURACV instead, and the scoop on -x clears
        self.assertEqual(keys(with_(port_teth_dx=12.6, port_scoop_dx=-11.0)),
                         ["anchor|edge_text_l"])

    def test_guards_are_the_case_guards(self):
        self.assertEqual(keys(with_(port_scoop=False)), [])
        self.assertEqual(keys(with_(usb_port=False)), [])
        self.assertEqual(keys(with_(port_tether=False, port_teth_dx=-18.3)),
                         ["scoop|edge_text_l"])

    def test_flange_is_held_at_the_port(self):
        # the case reads the flange at x = 0; the port is cut at usb_dx
        self.assertIn("flange|edge_text_l", keys(with_(usb_dx=4.0, port_scoop_dx=7.0,
                                                       port_teth_dx=-16.6)))
        self.assertEqual(keys(with_(usb_open_w=50.0, port_scoop=False)),
                         ["flange|edge_text_l", "flange|edge_text_r"])

    def test_flat_span_dock_keys_and_label(self):
        self.assertEqual(keys(with_(flat=74.0, port_scoop=False, port_labels=False)),
                         ["flat_span|edge_text_l"])
        self.assertEqual(keys(with_(dock_key_bx=75.0, port_scoop=False)),
                         ["dock_key|edge_text_l"])

    def test_the_label_is_held_to_its_ink_where_frame_cuts_it(self):
        # frame() starts the label at 70.852, inside SECURACV's ink (to 72.46):
        # a cut the estimate-based case and the old transcription both passed
        fails = cw.evaluate(with_(port_lbl_a="USB", port_scoop=False), inks(),
                            label(with_(port_lbl_a="USB")))
        self.assertEqual([f.key for f in fails], ["port_label|edge_text_l"])
        self.assertEqual(fails[0].pin, "label_placement")
        self.assertIn("by 1.30 mm", fails[0].message)
        # its end against the flat span, by its own ink: 70.852 + 15.41
        # = 86.26, which 2.2 mm a letter (81.85) would have put inside 86
        self.assertEqual(keys(with_(port_lbl_a="USB-C", port_scoop=False, flat=86.0)),
                         ["flat_span|port_lbl_a", "port_label|edge_text_l"])
        self.assertEqual(keys(with_(port_lbl_a="USB-C", port_scoop=False)),
                         ["port_label|edge_text_l"])
        # started just past the word's real ink, it clears; on it, it fails
        clear = 72.4571 - 0.300262 + 1e-6
        self.assertEqual(keys(with_(port_lbl_a="USB", port_scoop=False, port_lbl_x=clear)), [])
        self.assertEqual(keys(with_(port_lbl_a="USB", port_scoop=False,
                                    port_lbl_x=clear - 2e-6)), ["port_label|edge_text_l"])

    def test_an_empty_or_guarded_label_cuts_nothing(self):
        for case in (with_(port_lbl_x=200.0),
                     with_(port_lbl_a="USB", port_labels=False, port_lbl_x=200.0),
                     with_(port_lbl_a="USB", usb_port=False, port_lbl_x=200.0)):
            self.assertNotIn("flat_span|port_lbl_a", keys(case))

    def test_a_cut_label_that_was_not_measured_is_an_error(self):
        with self.assertRaises(ValueError):
            cw.evaluate(with_(port_lbl_a="USB"), inks())

    def test_empty_word_keeps_nothing_out(self):
        placed = [w for w in inks() if w.knob != "edge_text_l"]
        self.assertEqual(cw.evaluate(SHIPPED, placed), [])


class Known(unittest.TestCase):
    def test_known_is_c33_alone_at_its_measured_overlap(self):
        self.assertEqual(set(cw.KNOWN), {"scoop|edge_text_l"})
        rec, text = cw.KNOWN["scoop|edge_text_l"]
        self.assertIn("C33", text)
        shipped = cw.evaluate(SHIPPED, inks())[0].by
        self.assertLessEqual(abs(shipped - rec), cw.MOVE_TOL)

    def test_verdict(self):
        fails = cw.evaluate(SHIPPED, inks())
        self.assertEqual(cw.verdict(fails, cw.KNOWN), ([], [], []))
        self.assertEqual(cw.verdict([], cw.KNOWN), ([], ["scoop|edge_text_l"], []))
        fails = cw.evaluate(with_(port_teth_dx=-18.3), inks())
        new, gone, moved = cw.verdict(fails, cw.KNOWN)
        self.assertEqual([f.key for f in new], ["anchor|edge_text_r"])
        self.assertEqual((gone, moved), ([], []))

    def test_the_known_failure_cannot_grow(self):
        # the scoop moved 0.5 outboard reaches 2.20 into the V, and the case's
        # own estimate-based assert still passes there; so does 16.5, with
        # the dish itself in the letters
        for dx, by in ((11.5, 2.20), (16.5, 7.20)):
            fails = cw.evaluate(with_(port_scoop_dx=dx), inks())
            new, gone, moved = cw.verdict(fails, cw.KNOWN)
            self.assertEqual((new, gone), ([], []))
            self.assertEqual([(k, round(now, 2), rec) for k, now, rec in moved],
                             [("scoop|edge_text_l", by, 1.70)])
        # a larger word grows into the scoop the same way
        _, _, moved = cw.verdict(cw.evaluate(SHIPPED, inks(scale=7.5 / 7.0)), cw.KNOWN)
        self.assertEqual([k for k, _, _ in moved], ["scoop|edge_text_l"])

    def test_a_partial_fix_must_be_recorded(self):
        _, _, moved = cw.verdict(cw.evaluate(with_(port_scoop_dx=10.5), inks()), cw.KNOWN)
        self.assertEqual([(k, round(now, 2)) for k, now, _ in moved],
                         [("scoop|edge_text_l", 1.20)])

    def test_rounding_in_the_record_is_absorbed(self):
        for dx in (11.0 - cw.MOVE_TOL + 0.01, 11.0 + cw.MOVE_TOL - 0.01):
            fails = cw.evaluate(with_(port_scoop_dx=dx), inks())
            self.assertEqual(cw.verdict(fails, cw.KNOWN), ([], [], []), dx)
        for dx in (11.0 - cw.MOVE_TOL - 0.01, 11.0 + cw.MOVE_TOL + 0.01):
            fails = cw.evaluate(with_(port_scoop_dx=dx), inks())
            self.assertEqual(len(cw.verdict(fails, cw.KNOWN)[2]), 1, dx)

    def test_the_worst_of_one_key_is_the_one_held(self):
        f = cw.evaluate(SHIPPED, inks())[0]
        worse = cw.Failure(f.key, f.pin, f.message, f.by + 1.0)
        self.assertEqual(len(cw.verdict([f, worse], cw.KNOWN)[2]), 1)
        self.assertEqual(len(cw.verdict([worse, f], cw.KNOWN)[2]), 1)


PIN_LINES = {"placement": 4103, "label_placement": 4081, "flat_span": 2303,
             "anchor": 2585, "scoop": 2602, "port_label": 2615, "dock_key": 2814}


@contextlib.contextmanager
def measured(case, placed, lab=None):
    with mock.patch.object(cw, "measure",
                           return_value=(case, placed, lab, "/font.ttf", PIN_LINES)):
        yield


def run_main(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = cw.main(argv)
    return rc, out.getvalue()


class ReportedNotGated(unittest.TestCase):
    def test_evaluate_never_reads_the_facts(self):
        bare = {k: v for k, v in SHIPPED.items()
                if k not in ("vent_gap", "band_lo", "band_hi", "vword_half", "edge_text_size")}
        self.assertEqual([f.key for f in cw.evaluate(bare, inks())], ["scoop|edge_text_l"])

    def test_facts_say_what_the_triage_found(self):
        sec, can = cw.facts(SHIPPED, inks())
        self.assertIn("leaves 4.94 of vent_gap's 8 mm", sec)
        self.assertIn("runs 3.06 inside its inner edge and 3.46 past its outer edge", sec)
        self.assertIn("43.90 estimated", sec)
        self.assertIn("the ink fits it", can)
        # (a)'s 6.2: 52.01 mm of ink, 0.08 past the 52.2 band's outer edge
        sec62 = cw.facts(with_(edge_text_size=6.2), inks(scale=6.2 / 7.0))[0]
        self.assertIn("0.08 past its outer edge", sec62)

    def test_exit_status_ignores_the_facts(self):
        for extreme in ({"vent_gap": 100.0, "band_lo": 40.0, "band_hi": 45.8},
                        {"vent_gap": 0.0, "band_lo": 0.0, "band_hi": 85.8},
                        {"band_lo": 0.0, "band_hi": 200.0}):       # off its midpoint
            with measured(with_(**extreme), inks()):
                rc, text = run_main(["--check"])
            self.assertEqual(rc, 0, text)
            with measured(with_(port_scoop_dx=8.9, **extreme), inks()):
                rc, text = run_main(["--check"])
            self.assertEqual(rc, 1, text)

    def test_an_off_midpoint_band_is_unreported_not_refused(self):
        # what -D vword_x=44 gives: the probe reads it, the gate measures it,
        # and only the band's fit goes unreported
        case = cw.parse_probe(probe_echoes(vword_x="44.0"))
        self.assertEqual(case["vword_x"], 44.0)
        line = cw.facts(case, inks(case))[0]
        self.assertIn("band not reported: vword_x 44 is not the midpoint", line)
        self.assertNotIn("the ink runs", line)
        self.assertIn("the ink runs", cw.facts(SHIPPED, inks())[0])


class MainExits(unittest.TestCase):
    def test_shipped_passes_listing_the_known_failure(self):
        with measured(SHIPPED, inks()):
            rc, text = run_main(["--check"])
        self.assertEqual(rc, 0, text)
        self.assertIn("known failure (scoop|edge_text_l, the case's assert at :2602, "
                      "recorded at 1.70 mm)", text)
        self.assertIn("word ink OK", text)

    def test_known_got_worse_fails(self):
        for case, scale in ((with_(port_scoop_dx=11.5), 1.0), (SHIPPED, 7.5 / 7.0)):
            with measured(case, inks(case, scale)):
                rc, text = run_main(["--check"])
            self.assertEqual(rc, 1, text)
            self.assertIn("known failure got worse (scoop|edge_text_l)", text)

    def test_known_got_better_asks_for_the_record(self):
        with measured(with_(port_scoop_dx=10.5), inks()):
            rc, text = run_main(["--check"])
        self.assertEqual(rc, 1, text)
        self.assertIn("known failure got better (scoop|edge_text_l): 1.20 mm, recorded 1.70", text)

    def test_a_label_cut_into_the_word_fails(self):
        case = with_(port_lbl_a="USB")
        with measured(case, inks(), label(case)):
            rc, text = run_main(["--check"])
        self.assertEqual(rc, 1, text)
        self.assertIn("new failure (port_label|edge_text_l, frame()'s cut at :4081)", text)

    def test_known_gone_fails(self):
        with measured(SHIPPED, inks(scale=6.2 / 7.0)):
            rc, text = run_main(["--check"])
        self.assertEqual(rc, 1)
        self.assertIn("known failure no longer happens (scoop|edge_text_l)", text)

    def test_new_failure_fails_and_names_it(self):
        with measured(with_(port_teth_dx=-18.3), inks()):
            rc, text = run_main(["--check"])
        self.assertEqual(rc, 1)
        self.assertIn("new failure (anchor|edge_text_r, the case's assert at :2585)", text)

    def test_report_mode_does_not_gate(self):
        with measured(with_(port_teth_dx=-18.3), inks()):
            rc, text = run_main([])
        self.assertEqual(rc, 0)
        self.assertIn("new failure (anchor|edge_text_r", text)

    def test_refusal_is_exit_2(self):
        with mock.patch.object(cw, "measure", side_effect=cw.Refusal("dirty")):
            rc, text = run_main(["--check"])
        self.assertEqual(rc, 2)
        self.assertIn("refused, nothing measured — dirty", text)

    def test_defines(self):
        self.assertEqual(cw.parse_defines(["edge_text_size=6.2", 'edge_text_l="A=B"']),
                         {"edge_text_size": "6.2", "edge_text_l": '"A=B"'})
        for bad in ("edge_text_size", "1x=2", "a=", "a=1\nb=2"):
            with self.assertRaises(cw.Refusal, msg=bad):
                cw.parse_defines([bad])


class Dxf(unittest.TestCase):
    HEAD = "  0\nSECTION\n  2\nENTITIES\n"
    TAIL = "  0\nENDSEC\n  0\nEOF\n"

    def line(self, x0, y0, x1, y1):
        return f"  0\nLINE\n  8\n0\n 10\n{x0}\n 20\n{y0}\n 11\n{x1}\n 21\n{y1}\n"

    def test_reads_every_point(self):
        text = self.HEAD + self.line(-3, 0, 1, 2) + self.line(1, 2, 4.5, -1) + self.TAIL
        self.assertEqual(cw.dxf_x_extent(text), (-3.0, 4.5))

    def test_points_outside_entities_do_not_count(self):
        text = ("  0\nSECTION\n  2\nHEADER\n 10\n-999\n  0\nENDSEC\n"
                + self.HEAD + self.line(0, 0, 1, 1) + self.TAIL)
        self.assertEqual(cw.dxf_x_extent(text), (0.0, 1.0))

    def test_refusals(self):
        arc = "  0\nARC\n 10\n0\n 20\n0\n 40\n5\n"
        for text in (self.HEAD + arc + self.TAIL,          # an arc bulges past its points
                     self.HEAD + self.TAIL,                # no ink
                     self.HEAD + "  0\nLINE\n 10\n",       # not pairs
                     self.HEAD + "  0\nLINE\n 10\nx\n" + self.TAIL):
            with self.assertRaises(cw.Refusal):
                cw.dxf_x_extent(text)

    def test_scad_str_needs_no_quoting(self):
        self.assertEqual(cw.scad_str('a"\\'), "chr([97, 34, 92])")


def probe_echoes(**over):
    """The probe's echo payloads at SHIPPED, with `over` replacing raw values."""
    vals = {**{k: str(SHIPPED[k]) for k in cw.NUMBERS},
            **{k: "true" if SHIPPED[k] else "false" for k in cw.FLAGS},
            **{k: "[" + ", ".join(str(ord(c)) for c in SHIPPED[k]) + "]"
               for k in cw.STRINGS}}
    vals.update(over)
    return [f'"{cw.TAG}{k}", {v}' for k, v in vals.items()] + ['"other", 1']


class Probe(unittest.TestCase):
    def echoes(self, **over):
        return probe_echoes(**over)

    def test_round_trip(self):
        case = cw.parse_probe(self.echoes())
        self.assertEqual(case, {**SHIPPED})

    def test_body_echoes_strings_as_codes(self):
        body = cw.probe_body()
        self.assertIn('echo("word_ink:label_font", [for (c = label_font) ord(c)]);', body)
        self.assertIn('echo("word_ink:flat", fr_xi/2 - fr_ri);', body)
        self.assertIn('echo("word_ink:port_lbl_x", vword_x + vword_half + 6.0);', body)

    def test_refusals(self):
        for over in ({"vword_x": "undef"}, {"vword_x": "inf"}, {"port_scoop": "1"},
                     {"label_font": '"Liberation Sans"'}, {"edge_text_l": "[83, x]"},
                     {"port_lbl_x": "undef"}):
            with self.assertRaises(cw.Refusal, msg=over):
                cw.parse_probe(self.echoes(**over))
        # a band whose midpoint is not vword_x is a fact, never a refusal
        self.assertEqual(cw.parse_probe(self.echoes(band_hi="70"))["band_hi"], 70.0)
        with self.assertRaises(cw.Refusal):
            cw.parse_probe([e for e in self.echoes() if "usb_dx" not in e])


class Font(unittest.TestCase):
    PAT = "Liberation Sans:style=Bold"

    def test_the_real_font_passes(self):
        self.assertIsNone(cw.font_resolves(
            self.PAT, "Liberation Sans|Bold|/usr/share/fonts/truetype/liberation/"
                      "LiberationSans-Bold.ttf"))

    def test_a_fallback_refuses(self):
        why = cw.font_resolves(self.PAT, "DejaVu Sans|Bold|/x/DejaVuSans-Bold.ttf")
        self.assertIn("a fallback", why)
        self.assertIsNotNone(cw.font_resolves(self.PAT, "Liberation Sans|Regular|/x.ttf"))
        self.assertIsNotNone(cw.font_resolves(self.PAT, "garbage"))

    def test_pattern(self):
        self.assertEqual(cw.parse_font_pattern(self.PAT), ("Liberation Sans", "Bold"))
        self.assertEqual(cw.parse_font_pattern("Liberation Sans"), ("Liberation Sans", None))
        for bad in ("", "A,B:style=Bold", "A:style=Bold,Italic", "A\\:B", "A:bold"):
            with self.assertRaises(cw.Refusal, msg=bad):
                cw.parse_font_pattern(bad)

    def test_openscad_font_path_refuses(self):
        with mock.patch.dict("os.environ", {"OPENSCAD_FONT_PATH": "/fonts"}):
            with self.assertRaises(cw.Refusal):
                cw.check_font(self.PAT)

    def fc_match(self, stdout="", rc=0, raises=None):
        """subprocess.run as fc-match: one answer, and the argv it was asked with."""
        calls = []

        def run(argv, **kw):
            calls.append(argv)
            if raises:
                raise raises
            return subprocess.CompletedProcess(argv, rc, stdout, "no such pattern")
        return mock.patch.object(cw.subprocess, "run", side_effect=run), calls

    def test_check_font_asks_fc_match_and_returns_the_file(self):
        patch, calls = self.fc_match("Liberation Sans|Bold|/f/LiberationSans-Bold.ttf")
        with mock.patch.dict("os.environ", {}, clear=True), patch:
            self.assertEqual(cw.check_font(self.PAT), "/f/LiberationSans-Bold.ttf")
        self.assertEqual(calls[0][0], "fc-match")
        self.assertEqual(calls[0][-1], self.PAT)

    def test_check_font_refuses_what_fc_match_falls_back_to(self):
        for answer in ("DejaVu Sans|Bold|/x/DejaVuSans-Bold.ttf",
                       "Liberation Sans|Regular|/x/LiberationSans-Regular.ttf", ""):
            patch, _ = self.fc_match(answer)
            with mock.patch.dict("os.environ", {}, clear=True), patch:
                with self.assertRaises(cw.Refusal, msg=answer):
                    cw.check_font(self.PAT)

    def test_check_font_refuses_without_fc_match(self):
        # a non-zero exit refuses even beside an answer that would pass
        for patch, _ in (self.fc_match("Liberation Sans|Bold|/f/LiberationSans-Bold.ttf", rc=1),
                         self.fc_match(raises=FileNotFoundError(2, "No such file"))):
            with mock.patch.dict("os.environ", {}, clear=True), patch:
                with self.assertRaises(cw.Refusal):
                    cw.check_font(self.PAT)


GOOD_DXF = ("  0\nSECTION\n  2\nENTITIES\n  0\nLINE\n  8\n0\n 10\n-2.5\n 20\n0\n"
            " 11\n3.25\n 21\n1\n  0\nENDSEC\n  0\nEOF\n")


def fake_openscad(rc=0, stdout="", stderr="", write=True, raises=None):
    """subprocess.run as `openscad -o <dxf> <scad>`: writes GOOD_DXF unless told
    not to, and records the source it was asked to render."""
    seen = []

    def run(argv, **kw):
        if raises:
            raise raises
        seen.append(Path(argv[3]).read_text(encoding="utf-8"))
        if write:
            Path(argv[2]).write_text(GOOD_DXF, encoding="utf-8")
        return subprocess.CompletedProcess(argv, rc, stdout, stderr)
    return mock.patch.object(cw.subprocess, "run", side_effect=run), seen


class Render(unittest.TestCase):
    FONT = "Liberation Sans:style=Bold"

    def test_a_clean_render_is_measured_as_frame_cuts_it(self):
        patch, seen = fake_openscad(stderr="Total rendering time: 0:00:00.013\n")
        with patch:
            self.assertEqual(cw.measure_word("SECURACV", 7.0, self.FONT), (-2.5, 3.25))
            self.assertEqual(cw.measure_label("USB", self.FONT), (-2.5, 3.25))
        word, lab = seen
        self.assertIn("size = 7.0,", word)
        self.assertIn("spacing = 1.12,", word)
        self.assertIn('halign = "center", valign = "center"', word)
        self.assertIn("size = 3.6,", lab)
        self.assertNotIn("spacing", lab)               # frame()'s label call has none
        self.assertIn('halign = "left", valign = "center"', lab)

    def test_a_render_that_is_not_clean_refuses(self):
        for why, kw in (("a warning", {"stderr": "WARNING: Can't get font Liberation Sans\n"}),
                        ("an error", {"stdout": "ERROR: Parser error\n"}),
                        ("a non-zero exit", {"rc": 1}),
                        ("no DXF written", {"write": False}),
                        ("no OpenSCAD", {"raises": FileNotFoundError(2, "No such file")})):
            patch, _ = fake_openscad(**kw)
            with patch:
                with self.assertRaises(cw.Refusal, msg=why):
                    cw.measure_word("SECURACV", 7.0, self.FONT)
                with self.assertRaises(cw.Refusal, msg=why):
                    cw.measure_label("USB", self.FONT)


class Measure(unittest.TestCase):
    """measure() end to end, with the probe, fc-match and OpenSCAD patched."""

    def run_measure(self, case=None, font=None, overrides=None):
        case = case or SHIPPED
        order: list[str] = []
        probe = mock.Mock(echoes=probe_echoes(**(overrides or {})))

        def check_font(pattern):
            order.append("font")
            if font:
                raise cw.Refusal(font)
            return "/f/LiberationSans-Bold.ttf"

        def measure_text(text, size, f, halign, spacing):
            order.append(text)
            return DXF_INK[text] if text in DXF_INK else LABEL_INK[text]
        with mock.patch.object(cw.scad_probe, "probe", return_value=probe), \
                mock.patch.object(cw, "check_font", side_effect=check_font), \
                mock.patch.object(cw, "measure_text", side_effect=measure_text):
            return cw.measure({}), order

    def test_the_font_is_checked_before_any_ink(self):
        (case, placed, lab, font_file, pins), order = self.run_measure()
        self.assertEqual(order, ["font", "SECURACV", "CANARY"])
        self.assertEqual([(w.knob, round(w.lo, 2)) for w in placed],
                         [("edge_text_l", 13.74), ("edge_text_r", -64.68)])
        self.assertIsNone(lab)
        self.assertEqual(font_file, "/f/LiberationSans-Bold.ttf")
        self.assertEqual(set(pins), set(cw.PINS))

    def test_a_fallback_font_refuses_before_anything_is_rendered(self):
        with self.assertRaises(cw.Refusal):
            self.run_measure(font="DejaVu Sans Bold, a fallback")

    def test_a_fallback_font_is_exit_2_with_nothing_measured(self):
        out = io.StringIO()
        with mock.patch.object(cw, "check_font", side_effect=cw.Refusal("a fallback")), \
                mock.patch.object(cw, "measure_text") as mt, \
                mock.patch.object(cw.scad_probe, "probe",
                                  return_value=mock.Mock(echoes=probe_echoes())), \
                contextlib.redirect_stdout(out):
            rc = cw.main(["--check"])
        self.assertEqual(rc, 2, out.getvalue())
        mt.assert_not_called()

    def test_a_cut_label_is_measured_and_placed(self):
        codes = "[" + ", ".join(str(ord(c)) for c in "USB") + "]"
        (case, placed, lab, _, _), order = self.run_measure(overrides={"port_lbl_a": codes})
        self.assertEqual(order, ["font", "SECURACV", "CANARY", "USB"])
        self.assertAlmostEqual(lab.lo, 70.852 + 0.300262, places=6)

    def test_an_off_midpoint_band_goes_through_to_a_verdict(self):
        # the probe's band ends no longer centered on vword_x: measured, not refused
        (case, placed, _, _, _), _ = self.run_measure(overrides={"band_hi": "70"})
        self.assertEqual(case["band_hi"], 70.0)
        self.assertEqual(len(placed), 2)


class Pins(unittest.TestCase):
    def setUp(self):
        self.src = (ENC / cw.CASE).read_text(encoding="utf-8")

    def test_live_case_holds_each_pin_once(self):
        found = cw.check_pins(self.src)
        self.assertEqual(set(found), {"placement", "label_placement", *cw.ASSERTS})
        self.assertEqual(set(found), set(cw.PINS))
        lines = self.src.splitlines()
        self.assertIn("for (s = [1, -1])", lines[found["placement"] - 1])
        self.assertIn("if (port_labels && usb_port)", lines[found["label_placement"] - 1])
        for name in cw.ASSERTS:
            self.assertTrue(lines[found[name] - 1].lstrip().startswith("assert("), name)

    def test_comments_and_whitespace_do_not_move_a_pin(self):
        old = "rotate([90, 0, 0]) rotate([0, 0, 180]) translate([-s*vword_x, 0])"
        self.assertIn(old, self.src)
        src = self.src.replace(old, "rotate([90,0,0]) // turn\n rotate([0,0,180]) /* x */ "
                                    "translate([-s*vword_x,0])")
        self.assertEqual(set(cw.check_pins(src)), set(cw.PINS))
        loop = "        for (s = [1, -1])\n"
        self.assertEqual(self.src.count(loop), 1)
        src = self.src.replace(loop, "        // the words\n        /* both */ " + loop.lstrip())
        self.assertEqual(set(cw.check_pins(src)), set(cw.PINS))

    def test_a_moved_placement_or_assert_refuses(self):
        for old, new in (("translate([-s*vword_x, 0])", "translate([-s*vword_x - 3, 0])"),
                         ("translate([vword_x + vword_half + 6.0,",
                          "translate([vword_x + vword_half + 9.0,"),
                         ("text(port_lbl_a, size = 3.6,", "text(port_lbl_a, size = 4.2,"),
                         ("port_scoop_ftp + 1 < vword_x", "port_scoop_ftp + 1.5 < vword_x"),
                         ("dock_key_bx - gill_w/2 - 2 >", "dock_key_bx - gill_w/2 - 3 >")):
            self.assertEqual(self.src.count(old), 1, old)
            with self.assertRaises(cw.Refusal, msg=new):
                cw.check_pins(self.src.replace(old, new))

    def test_a_wrapped_or_guarded_cut_refuses(self):
        # each still holds the pinned text exactly once, so only the boundary
        # in front of it says the cut moved or is gated
        loop = "        for (s = [1, -1])\n"
        lbl = "        if (port_labels && usb_port)\n            translate([vword_x + vword_half"
        scoop = "assert(!port_scoop || !usb_port"
        for old, new in ((loop, "        translate([3, 0, 0]) " + loop.lstrip()),
                         (loop, "        if (edge_text_size > 6) " + loop.lstrip()),
                         (loop, "        mirror([1, 0, 0]) { " + loop.lstrip()),
                         (lbl, "        translate([3, 0, 0])\n" + lbl),
                         (scoop, "if (port_scoop_dx > 0) " + scoop)):
            self.assertEqual(self.src.count(old), 1, old)
            src = self.src.replace(old, new)
            if "{ " in new:
                src = src.replace('valign = "center");\n        // ...and the intake',
                                  'valign = "center"); }\n        // ...and the intake')
            with self.assertRaises(cw.Refusal, msg=new) as cm:
                cw.check_pins(src)
            self.assertIn("wrapped or guarded", str(cm.exception), new)

    def test_a_second_cut_refuses(self):
        with self.assertRaises(cw.Refusal):
            cw.check_pins(self.src + "\n;" + cw.PLACEMENT)

    def test_strip_comments_keeps_strings(self):
        src = 'a = "x // y"; // gone\nb = 1; /* gone\n */ c = "/*";'
        self.assertEqual(cw.strip_comments(src), 'a = "x // y"; \nb = 1; \n c = "/*";')
        self.assertEqual(cw.find_pinned("x;\n// f(1);\nf( 1 );", "f(1);"), ([3], []))

    def test_find_pinned_says_standing_or_wrapped(self):
        self.assertEqual(cw.find_pinned("f(1);", "f(1);"), ([1], []))           # file start
        self.assertEqual(cw.find_pinned("g() {}\nf(1);", "f(1);"), ([2], []))
        self.assertEqual(cw.find_pinned("g()\nf(1);", "f(1);"), ([], [2]))
        self.assertEqual(cw.find_pinned("x;\nelse f(1);", "f(1);"), ([], [2]))


if __name__ == "__main__":
    unittest.main()
