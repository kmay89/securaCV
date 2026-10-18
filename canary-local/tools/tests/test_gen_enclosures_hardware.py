#!/usr/bin/env python3
"""gen_enclosures.py carries hardware.json into build.json whole, or refuses.

build.json's `devices[<slug>].hardware` is each printable set's hardware as
the case's own HARDWARE echo counts it (gen_hardware.py -> hardware.json),
and the Lab's Assemble tab is held to that copy (tests/assembly.test.js). A
carry that dropped a set, an item or a count would show a builder fewer
screws than the CAD cuts holes for, with every gate green, so the carry
refuses instead. Pinned here: each refusal, by its message, and that a
refused run writes no build.json.

The generator is RUN, from a scratch copy of what it reads before build.json
is written — never imported (an import runs it against the real tree and
rewrites the real catalogs; see test_gen_enclosures_scratch.py), and a
mutated hardware.json never touches the real tree. The scratch run stops
later, at the workshop stage, whose firmware inputs are not copied: build.json
is written before that, and the control below proves the copy is complete
for it (an incomplete copy would make every refusal here vacuous).

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
REL_HW = Path("docs/hardware/enclosure/hardware.json")
REL_BUILD = Path("canary-local/devices/build.json")
# The meshes and images are never read by the build stage; leaving them out
# keeps the copy a few MB.
HEAVY = shutil.ignore_patterns("*.stl", "*.png", "*.glb", "*.3mf", "*.step", "*.stp",
                               "__pycache__")


class HardwareCarry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._td = tempfile.TemporaryDirectory()
        root = cls.root = Path(cls._td.name)
        (root / "canary-local/tools").mkdir(parents=True)
        (root / "canary-local/devices").mkdir()
        (root / "scripts").mkdir()
        for p in (REPO / "canary-local/tools").glob("*.py"):
            shutil.copy2(p, root / "canary-local/tools" / p.name)
        shutil.copy2(REPO / "scripts/bom_pricing.py", root / "scripts/bom_pricing.py")
        shutil.copytree(REPO / "devices", root / "devices", ignore=HEAVY)
        shutil.copytree(REPO / "docs/hardware", root / "docs/hardware", ignore=HEAVY)
        cls.hw = json.loads((REPO / REL_HW).read_text(encoding="utf-8"))

    @classmethod
    def tearDownClass(cls):
        cls._td.cleanup()

    def run_with(self, hw: dict | None) -> tuple[subprocess.CompletedProcess, dict | None]:
        """Run the scratch generator over `hw` as hardware.json (None: no
        file). Returns the process and the build.json it wrote, if any."""
        hw_path, build = self.root / REL_HW, self.root / REL_BUILD
        build.unlink(missing_ok=True)
        if hw is None:
            hw_path.unlink(missing_ok=True)
        else:
            hw_path.write_text(json.dumps(hw, indent=1, ensure_ascii=False) + "\n",
                               encoding="utf-8")
        proc = subprocess.run(
            [sys.executable, str(self.root / "canary-local/tools/gen_enclosures.py")],
            cwd=self.root, capture_output=True, text=True, timeout=120)
        out = json.loads(build.read_text(encoding="utf-8")) if build.exists() else None
        return proc, out

    def mutated(self) -> dict:
        return json.loads(json.dumps(self.hw))

    def assert_refused(self, hw: dict | None, needle: str):
        proc, out = self.run_with(hw)
        self.assertNotEqual(proc.returncode, 0, proc.stdout)
        self.assertIn(needle, proc.stderr)
        self.assertIsNone(out, "a refused carry must not write build.json")

    def test_control_the_scratch_carry_is_the_committed_one(self):
        # the committed hardware.json itself, not its re-serialization
        shutil.copy2(REPO / REL_HW, self.root / REL_HW)
        (self.root / REL_BUILD).unlink(missing_ok=True)
        proc = subprocess.run(
            [sys.executable, str(self.root / "canary-local/tools/gen_enclosures.py")],
            cwd=self.root, capture_output=True, text=True, timeout=120)
        built = self.root / REL_BUILD
        self.assertTrue(built.exists(), proc.stderr)
        self.assertEqual(built.read_bytes(), (REPO / REL_BUILD).read_bytes(),
                         "the scratch copy is missing an input the build stage reads")
        out = json.loads(built.read_text(encoding="utf-8"))
        carried = {s for d in out["devices"].values() for s in d.get("hardware", {}).get("sets", {})}
        self.assertEqual(carried, set(self.hw["sets"]), "every hardware.json set is carried")

    def test_every_set_lands_on_its_device_with_its_items_and_rib(self):
        _, out = self.run_with(self.hw)
        for set_id, s in self.hw["sets"].items():
            got = out["devices"][s["device"]]["hardware"]["sets"][set_id]
            self.assertEqual(got["items"], s["items"], set_id)
            self.assertEqual(got.get("lid_rib"), s.get("lid_rib"), set_id)
            self.assertEqual((got["scad"], got["overrides"]), (s["scad"], s["overrides"]), set_id)
            self.assertNotIn("bom", got, "the BOM join stays in hardware.json")

    def test_a_set_for_a_device_with_no_card_is_refused(self):
        hw = self.mutated()
        hw["sets"]["sense"]["device"] = "canary-nowhere"
        self.assert_refused(hw, "names device 'canary-nowhere'")

    def test_a_count_that_is_not_a_whole_positive_number_is_refused(self):
        for bad in (0, -4, 4.0, "4", True, None):
            with self.subTest(qty=bad):
                hw = self.mutated()
                hw["sets"]["wap.battery_weather"]["items"][0]["qty"] = bad
                self.assert_refused(hw, "is not a counted item")

    def test_an_item_missing_a_field_is_refused(self):
        hw = self.mutated()
        del hw["sets"]["vision.xiao_indoor"]["items"][1]["spec"]
        self.assert_refused(hw, "is not a counted item (['spec'])")

    def test_a_set_without_items_is_refused(self):
        for items in (None, []):
            with self.subTest(items=items):
                hw = self.mutated()
                if items is None:
                    del hw["sets"]["doorbell"]["items"]
                else:
                    hw["sets"]["doorbell"]["items"] = items
                self.assert_refused(hw, "set 'doorbell' lacks")

    def test_no_sets_and_no_file_are_refused(self):
        self.assert_refused({"sets": {}}, "has no sets")
        self.assert_refused(None, "is missing — run docs/hardware/enclosure/gen_hardware.py first")


if __name__ == "__main__":
    sys.exit(unittest.main())
