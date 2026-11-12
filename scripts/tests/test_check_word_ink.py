#!/usr/bin/env python3
"""Tests for docs/hardware/enclosure/check_word_ink.py — the 7-inch frame's
brand words held to the keep-outs by their measured ink.

No OpenSCAD and no fontconfig here: the placement, the five transcribed
asserts, the known-failure verdict, the DXF reader, the probe parser, the
font answer and the source pins are pure Python, fed synthetic extents. The
measuring half runs in enclosure.yml (`check_word_ink.py --check`).

  • place_word() is frame()'s transform: the triage's DXF extents land where
    the case cuts them (SECURACV 13.74..72.46, CANARY -64.68..-20.75);
  • evaluate() holds each assert with the ink in place of the estimate,
    strictly, behind the case's own guards, and a keep-out finds the word on
    its own side of the port;
  • the verdict fails a new failure and a KNOWN one that stopped, and the
    shipped numbers give exactly the one KNOWN failure (C33);
  • the vent_gap and band facts never reach the exit status: evaluate()
    runs on a case that does not hold them, and main() answers the same
    whatever they say;
  • the pins find the live case's placement loop and five asserts once
    each, ignore comments, and refuse a moved one; a fallback font, a
    malformed DXF and a probe that cannot be read all refuse.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
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
    "dock_key_bx": 84.0, "gill_w": 2.4, "flat": 88.28,
    "band_lo": 16.8, "band_hi": 69.0,
    "usb_port": True, "port_tether": True, "port_scoop": True,
    "port_labels": True, "dock_keys": True,
    "edge_text_l": "SECURACV", "edge_text_r": "CANARY",
    "label_font": "Liberation Sans:style=Bold", "port_lbl_a": "",
}
# The words' DXF ink at size 7 (Liberation Sans Bold, spacing 1.12, centered):
# the triage's t_SECURACV.dxf / t_CANARY.dxf, which a live render reproduces.
DXF_INK = {"SECURACV": (-29.5571, 29.1623), "CANARY": (-22.1485, 21.7822)}


def inks(case=None, scale=1.0):
    """The two words placed as frame() places them; `scale` stands in for a
    smaller edge_text_size (ink scales with size)."""
    case = case or SHIPPED
    out = []
    for knob, s in cw.WORDS:
        lo, hi = DXF_INK[case[knob]]
        out.append(cw.place_word(knob, case[knob], lo * scale, hi * scale, s, case["vword_x"]))
    return out


def keys(case, scale=1.0):
    return sorted(f.key for f in cw.evaluate(case, inks(case, scale)))


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


class Asserts(unittest.TestCase):
    def test_shipped_has_exactly_the_scoop_failure(self):
        fails = cw.evaluate(SHIPPED, inks())
        self.assertEqual([f.key for f in fails], ["scoop|edge_text_l"])
        self.assertIn("by 1.70 mm", fails[0].message)
        self.assertIn("6.56..15.44", fails[0].message)
        self.assertEqual(fails[0].assert_name, "scoop")

    def test_option_a_and_c_clear_it(self):
        self.assertEqual(keys(with_(port_scoop_dx=8.9)), [])          # (c)
        self.assertEqual(keys(SHIPPED, scale=6.2 / 7.0), [])          # (a)

    def test_strict_like_the_case(self):
        # a keep-out that ends exactly where the ink starts fails, as the
        # case's `<` does: reach = 13.7377 - 11 puts its edge on the S
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
        # 72.46 + 6 + 2.2 * 5 = 89.46 > 88.28
        self.assertEqual(keys(with_(port_lbl_a="USB-C", port_scoop=False)),
                         ["port_label"])
        self.assertEqual(keys(with_(port_lbl_a="USB", port_scoop=False)), [])

    def test_empty_word_keeps_nothing_out(self):
        placed = [w for w in inks() if w.knob != "edge_text_l"]
        self.assertEqual(cw.evaluate(SHIPPED, placed), [])


class Known(unittest.TestCase):
    def test_known_is_c33_alone(self):
        self.assertEqual(set(cw.KNOWN), {"scoop|edge_text_l"})
        self.assertIn("C33", cw.KNOWN["scoop|edge_text_l"])

    def test_verdict(self):
        fails = cw.evaluate(SHIPPED, inks())
        self.assertEqual(cw.verdict(fails, cw.KNOWN), ([], []))
        new, gone = cw.verdict([], cw.KNOWN)
        self.assertEqual((new, gone), ([], ["scoop|edge_text_l"]))
        fails = cw.evaluate(with_(port_teth_dx=-18.3), inks())
        new, gone = cw.verdict(fails, cw.KNOWN)
        self.assertEqual([f.key for f in new], ["anchor|edge_text_r"])
        self.assertEqual(gone, [])


@contextlib.contextmanager
def measured(case, placed):
    with mock.patch.object(cw, "measure",
                           return_value=(case, placed, "/font.ttf", {
                               "placement": 4103, "flat_span": 2303, "anchor": 2585,
                               "scoop": 2602, "port_label": 2615, "dock_key": 2814})):
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
                        {"vent_gap": 0.0, "band_lo": 0.0, "band_hi": 85.8}):
            with measured(with_(**extreme), inks()):
                rc, text = run_main(["--check"])
            self.assertEqual(rc, 0, text)
            with measured(with_(port_scoop_dx=8.9, **extreme), inks()):
                rc, text = run_main(["--check"])
            self.assertEqual(rc, 1, text)


class MainExits(unittest.TestCase):
    def test_shipped_passes_listing_the_known_failure(self):
        with measured(SHIPPED, inks()):
            rc, text = run_main(["--check"])
        self.assertEqual(rc, 0, text)
        self.assertIn("known failure (scoop|edge_text_l, the case's assert at :2602)", text)
        self.assertIn("word ink OK", text)

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


class Probe(unittest.TestCase):
    def echoes(self, **over):
        vals = {**{k: str(SHIPPED[k]) for k in cw.NUMBERS},
                **{k: "true" if SHIPPED[k] else "false" for k in cw.FLAGS},
                **{k: "[" + ", ".join(str(ord(c)) for c in SHIPPED[k]) + "]"
                   for k in cw.STRINGS}}
        vals.update(over)
        return [f'"{cw.TAG}{k}", {v}' for k, v in vals.items()] + ['"other", 1']

    def test_round_trip(self):
        case = cw.parse_probe(self.echoes())
        self.assertEqual(case, {**SHIPPED})

    def test_body_echoes_strings_as_codes(self):
        body = cw.probe_body()
        self.assertIn('echo("word_ink:label_font", [for (c = label_font) ord(c)]);', body)
        self.assertIn('echo("word_ink:flat", fr_xi/2 - fr_ri);', body)

    def test_refusals(self):
        for over in ({"vword_x": "undef"}, {"vword_x": "inf"}, {"port_scoop": "1"},
                     {"label_font": '"Liberation Sans"'}, {"edge_text_l": "[83, x]"},
                     {"band_hi": "70"}):        # vword_x no longer the band's midpoint
            with self.assertRaises(cw.Refusal, msg=over):
                cw.parse_probe(self.echoes(**over))
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


class Pins(unittest.TestCase):
    def setUp(self):
        self.src = (ENC / cw.CASE).read_text(encoding="utf-8")

    def test_live_case_holds_each_pin_once(self):
        found = cw.check_pins(self.src)
        self.assertEqual(set(found), {"placement", *cw.ASSERTS})
        lines = self.src.splitlines()
        self.assertIn("for (s = [1, -1])", lines[found["placement"] - 1])
        for name in cw.ASSERTS:
            self.assertTrue(lines[found[name] - 1].lstrip().startswith("assert("), name)

    def test_comments_and_whitespace_do_not_move_a_pin(self):
        src = self.src.replace("rotate([90, 0, 0]) rotate([0, 0, 180]) translate([-s*vword_x, 0])",
                               "rotate([90,0,0]) // turn\n rotate([0,0,180]) /* x */ "
                               "translate([-s*vword_x,0])")
        self.assertEqual(set(cw.check_pins(src)), {"placement", *cw.ASSERTS})

    def test_a_moved_placement_or_assert_refuses(self):
        for old, new in (("translate([-s*vword_x, 0])", "translate([-s*vword_x - 3, 0])"),
                         ("port_scoop_ftp + 1 < vword_x", "port_scoop_ftp + 1.5 < vword_x"),
                         ("dock_key_bx - gill_w/2 - 2 >", "dock_key_bx - gill_w/2 - 3 >")):
            self.assertIn(old, self.src)
            with self.assertRaises(cw.Refusal, msg=new):
                cw.check_pins(self.src.replace(old, new))

    def test_a_second_cut_refuses(self):
        with self.assertRaises(cw.Refusal):
            cw.check_pins(self.src + "\n" + cw.PLACEMENT)

    def test_strip_comments_keeps_strings(self):
        src = 'a = "x // y"; // gone\nb = 1; /* gone\n */ c = "/*";'
        self.assertEqual(cw.strip_comments(src), 'a = "x // y"; \nb = 1; \n c = "/*";')
        self.assertEqual(cw.find_pinned("x\n// f(1);\nf( 1 );", "f(1);"), [3])


if __name__ == "__main__":
    unittest.main()
