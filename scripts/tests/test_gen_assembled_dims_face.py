"""The face-aperture half of docs/hardware/enclosure/gen_assembled_dims.py --check.

The Watch's bezel aperture and the Dash's view window are measured with the
envelope (`face_fig_mm`) because the massings draw the glass in them — so the
committed value is consumed data, and its checker must never answer "equal"
for something it cannot read as the measured number:

  • equal within TOL (and both absent) passes;
  • a moved aperture, a missing or extra key, a row that lost or gained its
    face, a string, a bool, or NaN is "moved" — never silently equal;
  • the committed ledger names a face exactly for the rows that declare one.

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


class TheCommittedLedgerNamesEachDeclaredFace(unittest.TestCase):

    def test_faces_are_exactly_the_rows_that_declare_one(self):
        led = json.loads((ENC / "assembled_dims.json").read_text(encoding="utf-8"))["devices"]
        want = sorted(fid for fid, s in gad.DEVICES.items() if "face" in s)
        have = sorted(fid for fid, r in led.items() if "face_fig_mm" in r)
        self.assertEqual(have, want)
        self.assertIn("device.canary-display-dash", want)
        self.assertIn("device.canary-display-watch", want)


if __name__ == "__main__":
    unittest.main()
