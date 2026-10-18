#!/usr/bin/env python3
"""gen_flash.py reads a firmware constant only when it is a plain literal.

read_const() is how the flasher catalog takes its numbers out of the firmware
tree: the Vision's detection defaults and bounds, the Sense's reflexes and
their bounds. It used to take the first run of digits after the name, so an
expression read as its first number: `30u * 1000u` came out as 30 and
`0x10` as 0, a wrong default in flash.json with `--check` green. It now reads
a plain decimal literal (U/L suffixes and a trailing comment allowed) that
ends the #define's line or the constexpr's statement, and refuses anything
else by name, file and the text it found.

The Vision's four detection defaults are read from the project config.h
(firmware/projects/canary-vision/include/canary/config.h), the first-boot
seeds detect_config.cpp loads, and nothing under firmware/configs/
canary-vision/ is read: the flavor file's copies of them are gone (sweep
F182's follow-up). VisionDefaults holds which files vision_detect_block()
reads, and that its defaults follow an edited project config.h.

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import contextlib
import io
import re
import sys
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import gen_flash as gf  # noqa: E402  (definitions only; main() runs under __main__)


# A header that is never on disk: gen_flash's read() is patched to serve it,
# and read_const names a file by its path under the repo.
SYNTHETIC = gf.REPO / "firmware/synthetic/consts.h"


REAL_READ = gf.read


def served(text: str, path: Path = SYNTHETIC):
    """gen_flash reads `text` for `path` and the real tree for anything else."""
    return mock.patch.object(gf, "read", lambda p: text if p == path else REAL_READ(p))


class ReadConst(unittest.TestCase):
    def read(self, text: str, name: str) -> int:
        with served(text):
            return gf.read_const(SYNTHETIC, name)

    def refused(self, text: str, name: str) -> str:
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit, msg=text):
            self.read(text, name)
        return err.getvalue()

    def test_the_shapes_the_firmware_spells(self):
        text = (
            "#define CS_PRESENT_DEBOUNCE_MS  300     // sustained target before \"present\"\n"
            "#define CS_BREATH_MAX_BPM       30\n"
            "#define WATCHDOG_MS\t1500u /* ms */\n"
            "static constexpr int SCORE_MIN     = 70;       // 0-100\n"
            "static constexpr uint32_t LOST_TIMEOUT_MS = 1500;\n"
            "constexpr uint8_t  DETECT_SCORE_MIN_LO   = 0;\n"
            "inline constexpr uint32_t SENSE_DEBOUNCE_MS_HI = 3000UL;\n"
            "static constexpr uint32_t SPLIT_MS =\n    /* seeded */ 2500;\n")
        want = {"CS_PRESENT_DEBOUNCE_MS": 300, "CS_BREATH_MAX_BPM": 30, "WATCHDOG_MS": 1500,
                "SCORE_MIN": 70, "LOST_TIMEOUT_MS": 1500, "DETECT_SCORE_MIN_LO": 0,
                "SENSE_DEBOUNCE_MS_HI": 3000, "SPLIT_MS": 2500}
        self.assertEqual({n: self.read(text, n) for n in want}, want)

    def test_an_expression_is_refused_not_read_as_its_first_number(self):
        # Each of these read as its first number before the fix.
        cases = [
            ("static constexpr uint32_t WINDOW_MS = 30u * 1000u;\n", "WINDOW_MS"),
            ("static constexpr uint32_t LOST_TIMEOUT_MS = 3 * 500;\n", "LOST_TIMEOUT_MS"),
            ("#define CS_CLEAR_TIMEOUT_MS 1500 + 1\n", "CS_CLEAR_TIMEOUT_MS"),
            ("#define DETECT_MASK 0x10\n", "DETECT_MASK"),
            ("static constexpr int SCORE_MIN = 70.5;\n", "SCORE_MIN"),
            ("#define HELD_MS 1500 \\\n  + 10\n", "HELD_MS"),
        ]
        for text, name in cases:
            with self.subTest(text=text):
                said = self.refused(text, name)
                self.assertIn(name, said)
                self.assertIn("not a plain integer literal", said)

    def test_a_missing_or_valueless_constant_is_refused(self):
        self.assertIn("not found", self.refused("#define OTHER 5\n", "WANTED"))
        self.assertIn("not found", self.refused("#define WANTED_MS 5\n", "WANTED"))
        self.assertIn("not a plain integer literal", self.refused("#define WANTED   \n", "WANTED"))


VISION_PROJECT = gf.REPO / "firmware/projects/canary-vision/include/canary/config.h"
VISION_FLAVORS = gf.REPO / "firmware/configs/canary-vision"


def retuned(text: str, name: str, by: int = 7) -> tuple[str, int]:
    """`text` with the `constexpr ... name = N;` literal moved by `by`, and the new value."""
    m = re.search(rf"constexpr\s+\w+\s+{name}\s*=\s*(\d+)\s*;", text)
    if not m:
        raise AssertionError(f"{name} is not a plain constexpr literal in the project config.h")
    value = int(m.group(1)) + by
    return text[:m.start(1)] + str(value) + text[m.end(1):], value


class VisionDefaults(unittest.TestCase):
    def test_it_reads_the_project_config_and_nothing_in_the_flavor_directory(self):
        seen = []

        def spy(path):
            seen.append(path)
            return REAL_READ(path)

        with mock.patch.object(gf, "read", spy):
            gf.vision_detect_block()
        self.assertIn(VISION_PROJECT, seen)
        self.assertEqual([p for p in seen if VISION_FLAVORS in p.parents], [])

    def test_the_defaults_follow_an_edited_project_config(self):
        text = REAL_READ(VISION_PROJECT)
        want = {}
        for key, name in (("target", "PERSON_TARGET"), ("score", "SCORE_MIN"),
                          ("lost_ms", "LOST_TIMEOUT_MS"), ("dwell_ms", "DWELL_START_MS")):
            text, want[key] = retuned(text, name)
        with served(text, VISION_PROJECT):
            block = gf.vision_detect_block()
        self.assertEqual(block["defaults"], want)
        ships = next(p for p in block["presets"] if p["id"] == "ships")
        self.assertEqual(ships["values"], want)

    def test_an_expression_in_the_project_config_is_refused(self):
        text = REAL_READ(VISION_PROJECT)
        edited, n = re.subn(r"(constexpr\s+\w+\s+LOST_TIMEOUT_MS\s*=\s*)(\d+)\s*;",
                            lambda m: f"{m.group(1)}3 * {m.group(2)};", text)
        self.assertEqual(n, 1)
        err = io.StringIO()
        with served(edited, VISION_PROJECT), contextlib.redirect_stderr(err), \
                self.assertRaises(SystemExit):
            gf.vision_detect_block()
        self.assertIn("LOST_TIMEOUT_MS", err.getvalue())


if __name__ == "__main__":
    unittest.main()
