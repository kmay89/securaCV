#!/usr/bin/env python3
"""A builder preset grays exactly the controls it overrides.

The released cases' presets are a `preset` selector and a `_pre()` helper in
the .scad: every option `_pre()` wraps is overridden while the preset is not
"custom". The web builder grays out the controls gen_builder_manifest.py's
CURATED entry lists in `preset_controls` — so the two must be one list. The
WAP's and the Vision's presets overrode opt_weep while the builder left its
checkbox live (C10 found it wiring the doorbell's and the Sense's presets):
a control that did nothing, with nothing saying so. build_manifest() now
refuses the mismatch; these tests pin that it does, and that the tree holds.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import copy
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENC = ROOT / "docs" / "hardware" / "enclosure"
if str(ENC) not in sys.path:
    sys.path.insert(0, str(ENC))
import gen_builder_manifest as gbm  # noqa: E402

PRE = re.compile(r"=\s*_pre\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,")


class Presets(unittest.TestCase):
    def test_every_released_preset_grays_what_it_overrides(self):
        with_preset = 0
        for spec in gbm.CURATED:
            overridden = set(PRE.findall((ENC / spec["file"]).read_text(encoding="utf-8")))
            if not spec["preset_param"]:
                self.assertEqual(overridden, set(), spec["file"])
                continue
            with_preset += 1
            self.assertEqual(set(spec["preset_controls"]), overridden, spec["file"])
            self.assertIn(spec["preset_param"], spec["simple"], spec["file"])
            self.assertIn("custom", spec["choices"][spec["preset_param"]], spec["file"])
        # the WAP, the Vision, the doorbell and the Sense (C10 added the last two)
        self.assertEqual(with_preset, 4)

    def test_a_control_the_preset_overrides_but_does_not_gray_is_refused(self):
        saved = gbm.CURATED
        try:
            gbm.CURATED = copy.deepcopy(saved)
            sense = next(s for s in gbm.CURATED if s["id"] == "sense")
            sense["preset_controls"].remove("opt_weep")
            with self.assertRaises(SystemExit) as cm:
                gbm.build_manifest()
            self.assertIn("preset overrides", str(cm.exception))
            self.assertIn("opt_weep", str(cm.exception))
        finally:
            gbm.CURATED = saved

    def test_a_source_preset_the_builder_does_not_know_is_refused(self):
        saved = gbm.CURATED
        try:
            gbm.CURATED = copy.deepcopy(saved)
            door = next(s for s in gbm.CURATED if s["id"] == "doorbell")
            door["preset_param"] = None
            door["preset_controls"] = []
            door["simple"].remove("preset")
            door["labels"].pop("preset")
            door["choices"].pop("preset")
            with self.assertRaises(SystemExit) as cm:
                gbm.build_manifest()
            self.assertIn("names no preset_param", str(cm.exception))
        finally:
            gbm.CURATED = saved


if __name__ == "__main__":
    unittest.main()
