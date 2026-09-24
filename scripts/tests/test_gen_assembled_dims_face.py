"""The face half of docs/hardware/enclosure/gen_assembled_dims.py --check.

The Watch's bezel aperture and the Dash's view window are measured with the
envelope (`face_fig_mm`) because the massings draw the glass in them, and the
Combo's lens and radome window likewise (`features_fig_mm`, off-center, each
a center on the envelope and an extent) — so the committed values are
consumed data, and their checkers must never answer "equal" for something
they cannot read as the measured number:

  • equal within TOL (and both absent) passes;
  • a moved aperture or feature, a missing or extra key (or feature), a row
    that lost or gained its face or features, a string, a bool, or NaN is
    "moved" — never silently equal;
  • the committed ledger names a face and features exactly for the rows
    that declare them.

No OpenSCAD here: face_moved() and the committed file only.
Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import json
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ENC = REPO / "docs" / "hardware" / "enclosure"
sys.path.insert(0, str(ENC))
spec = importlib.util.spec_from_file_location("gen_assembled_dims", ENC / "gen_assembled_dims.py")
gad = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gad)  # type: ignore[union-attr]

FACE = {"w": 101.3, "h": 61.2}
FEATURES = {"lens": {"x": 21.6, "z": 59.1, "w": 10.0, "h": 10.0},
            "radome": {"x": 57.8, "z": 30.6, "w": 24.0, "h": 24.0}}


class FaceMovedNeverFallsThrough(unittest.TestCase):

    def test_equal_and_absent_pass(self):
        self.assertFalse(gad.face_moved(FACE, dict(FACE)))
        self.assertFalse(gad.face_moved(FACE, {"w": 101.3 + gad.TOL / 2, "h": 61.2}))
        self.assertFalse(gad.face_moved(None, None))

    def test_anything_it_cannot_read_as_the_measurement_is_moved(self):
        for got in (
            {"w": 100.0, "h": 61.2},           # the window moved
            None,                              # the committed row lost its face
            {"w": 101.3},                      # a key is missing
            {"w": 101.3, "h": 61.2, "d": 1},   # an extra key
            {"w": "101.3", "h": 61.2},         # a string that prints the same
            {"w": True, "h": 61.2},            # a bool is not a length
            {"w": float("nan"), "h": 61.2},    # NaN compares unequal to everything
            [101.3, 61.2],                     # the wrong shape
        ):
            with self.subTest(got=got):
                self.assertTrue(gad.face_moved(FACE, got))
        self.assertTrue(gad.face_moved(None, dict(FACE)), "a stray committed face is moved too")


class FeaturesMovedNeverFallsThrough(unittest.TestCase):

    def _with(self, name, **kv):
        got = json.loads(json.dumps(FEATURES))
        got[name].update(kv)
        return got

    def test_equal_and_absent_pass(self):
        self.assertFalse(gad.features_moved(FEATURES, json.loads(json.dumps(FEATURES))))
        self.assertFalse(gad.features_moved(FEATURES, self._with("lens", x=21.6 + gad.TOL / 2)))
        self.assertFalse(gad.features_moved(None, None))

    def test_anything_it_cannot_read_as_the_measurement_is_moved(self):
        lens_only = {"lens": dict(FEATURES["lens"])}
        extra = {**json.loads(json.dumps(FEATURES)), "led": {"x": 1, "z": 1, "w": 3, "h": 3}}
        for got in (
            self._with("lens", x=22.6),                  # the lens moved across the face
            self._with("radome", z=31.6),                # the window moved up the face
            self._with("radome", w=20.0),                # the window shrank
            None,                                        # the committed row lost its features
            lens_only,                                   # a feature is missing
            extra,                                       # a feature the CAD did not name
            self._with("lens", d=1.0),                   # an extra key
            {"lens": {"x": 21.6, "z": 59.1, "w": 10.0}, "radome": FEATURES["radome"]},  # a missing key
            self._with("lens", x="21.6"),                # a string that prints the same
            self._with("lens", w=True),                  # a bool is not a length
            self._with("radome", h=float("nan")),        # NaN compares unequal to everything
            [FEATURES["lens"], FEATURES["radome"]],      # the wrong shape
            {"lens": [21.6, 59.1, 10.0, 10.0], "radome": FEATURES["radome"]},
        ):
            with self.subTest(got=got):
                self.assertTrue(gad.features_moved(FEATURES, got))
        self.assertTrue(gad.features_moved(None, json.loads(json.dumps(FEATURES))),
                        "stray committed features are moved too")


class MeasureRefusesAFeatureItCannotPlace(unittest.TestCase):
    """measure() with the OpenSCAD probe stubbed: what the write path records
    for a feature, and what it refuses to write at all."""

    SPEC = {"scad": "x.scad", "overrides": {"part": '"back"'}, "body": "b()",
            "seams": "[1]", "features": {"lens": "[lens_x, lens_y, 10, 10]"},
            "placement": "p"}

    def _measure(self, lens, bbox=(80.0, 60.0, 20.0), lo=(-30.0, -20.0, 0.0)):
        echoes = ['"SEAMS", [1]', f'"FEATURE_lens", {lens}']
        fake = gad.scad_probe.Result(diag="", echoes=echoes, bbox=list(bbox), lo=list(lo))
        real = gad.scad_probe.probe
        gad.scad_probe.probe = lambda *a, **k: fake
        try:
            return gad.measure("device.test", self.SPEC)
        finally:
            gad.scad_probe.probe = real

    def test_a_center_is_read_from_the_min_corner_not_assumed_symmetric(self):
        # an outline NOT centered on the scad origin (x runs -30..50): the
        # lens at scad x = 0 sits 30 from the envelope's left edge, not 40
        rec = self._measure("[0, 5, 10, 10]")
        self.assertEqual(rec["features_fig_mm"], {"lens": {"x": 30.0, "z": 25.0, "w": 10.0, "h": 10.0}})

    def test_what_it_cannot_place_on_the_face_is_refused_not_written(self):
        for lens in ("[0, 5, 10]",           # not [cx, cy, w, h]
                     "[0, 5, 0, 10]",        # no extent
                     "[0, 5, nan, 10]",      # JSON cannot spell it; --check could never match it
                     "[0, 5, inf, 10]",
                     "[48, 5, 10, 10]",      # hangs off the right edge (x 50 is the edge)
                     "[0, 38, 10, 10]",      # hangs off the top (y 40 is the edge)
                     "[0, 5, undef, 10]"):   # a typo'd variable echoes undef
            with self.subTest(lens=lens), self.assertRaises(SystemExit):
                self._measure(lens)


class TheCommittedLedgerNamesEachDeclaredFace(unittest.TestCase):

    def test_faces_are_exactly_the_rows_that_declare_one(self):
        led = json.loads((ENC / "assembled_dims.json").read_text(encoding="utf-8"))["devices"]
        want = sorted(fid for fid, s in gad.DEVICES.items() if "face" in s)
        have = sorted(fid for fid, r in led.items() if "face_fig_mm" in r)
        self.assertEqual(have, want)
        self.assertIn("device.canary-display-dash", want)
        self.assertIn("device.canary-display-watch", want)

    def test_features_are_exactly_the_rows_that_declare_them(self):
        led = json.loads((ENC / "assembled_dims.json").read_text(encoding="utf-8"))["devices"]
        for fid, spec in gad.DEVICES.items():
            with self.subTest(fid=fid):
                self.assertEqual(sorted((led[fid].get("features_fig_mm") or {})),
                                 sorted(spec.get("features", {})))
        self.assertEqual(sorted(led["device.canary-combo"]["features_fig_mm"]), ["lens", "radome"])


if __name__ == "__main__":
    unittest.main()
