#!/usr/bin/env python3
"""A firmware constant is read only when it is a plain literal, by one rule.

read_const() is how the generators take their numbers out of the firmware
tree: gen_flash.py's Vision detection defaults and bounds and the Sense's
reflexes and their bounds, and gen_vision.py's Vision constants (the detection
seeds, the voxel grid and frame, the cadences, the tuning bounds). It used to
take the first run of digits after the name, so an expression read as its
first number: `30u * 1000u` came out as 30, `0x10` as 0 and the octal `070`
as 70 (it compiles as 56), and the first textual match won, so a
commented-out or `#if`-ed-out definition above the live one was the one read:
each a wrong default in flash.json with `--check` green. It now reads past
comments (not past the strings that can hold `//`, and a definition spelled
inside a string is not one), takes a name defined exactly once, and reads a
plain decimal literal (U/L suffixes allowed, no leading zero, at most
2^53 - 1) that ends the #define's line or the constexpr's statement.
Anything else is refused by name, file and the text it found.

The rule lived in gen_flash.py (sweep F313). gen_vision.py read the same
constants with its own `NAME\\s*=\\s*(\\d+)` (no word boundary, no comment
strip, no single-definition check), and vision.test.js's num() with the same
pattern, so the page's data and its test agreed with each other on the wrong
number while only the flash-catalog step went red. It is now
_tooling.read_const (sweep F369), which both generators import, and
canary-local/tests/c_const.js is its JavaScript twin. c_const_cases.json, next
to this file, is the table this file and vision.test.js both run: the Python
rule and the JavaScript one give every row the same value or the same refusal,
word for word. Its `planted` rows edit the real Vision config.h, and
VisionGenerator runs gen_vision.py on each, on a scratch copy of its sources,
as gen_wap.py's test does.

The Vision's four detection defaults are read from the project config.h
(firmware/projects/canary-vision/include/canary/config.h), the first-boot
seeds detect_config.cpp loads, and nothing under firmware/configs/
canary-vision/ is read: the flavor file's copies of them are gone (sweep
F313, F182's follow-up). VisionDefaults holds which files vision_detect_block()
reads, and that its defaults follow an edited project config.h.

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import _tooling as tooling  # noqa: E402
import gen_flash as gf  # noqa: E402  (definitions only; main() runs under __main__)


# A header that is never on disk: gen_flash's read() is patched to serve it,
# and read_const names a file by its path under the repo.
SYNTHETIC = gf.REPO / "firmware/synthetic/consts.h"
CASES = json.loads((TOOLS / "tests/c_const_cases.json").read_text(encoding="utf-8"))
# die()'s prefix: the module that called read_const, never _tooling.py.
THIS = Path(__file__).name


REAL_READ = gf.read


def served(text: str, path: Path = SYNTHETIC):
    """gen_flash reads `text` for `path` and the real tree for anything else."""
    return mock.patch.object(gf, "read", lambda p: text if p == path else REAL_READ(p))


def respelled(text: str, name: str, value: str) -> str:
    """`text` with the value of its one `constexpr T name = ...;` spelled
    `value` (vision.test.js's respelled() makes the same edit)."""
    pattern = (rf"(?<![A-Za-z0-9_])constexpr[ \t]+[A-Za-z0-9_]+[ \t]+{name}[ \t]*=[ \t]*"
               r"(?P<value>[^;]*);")
    found = list(re.finditer(pattern, text))
    if len(found) != 1:
        raise AssertionError(f"{name} is not one constexpr in the text ({len(found)} found)")
    m = found[0]
    return text[:m.start("value")] + value + text[m.end("value"):]


def planted(text: str, row: dict) -> str:
    """The real header with a `planted` row of c_const_cases.json applied."""
    if "prepend" in row:
        return row["prepend"] + text
    return respelled(text, row["name"], row["respell"])


def refusal(row: dict, where: str) -> str:
    return row["refused"].replace("{where}", where)


class ReadConst(unittest.TestCase):
    def read(self, text: str, name: str) -> int:
        with served(text):
            return tooling.read_const(SYNTHETIC, name, gf.read)

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
            "static constexpr uint32_t SPLIT_MS =\n    /* seeded */ 2500;\n"
            "static constexpr uint32_t HOLD_MS = 2500 /* first\n   boot */;\n"
            "#define CS_RANGE_NEAR_CM 120 /* cm,\n   a block comment that runs on */\n")
        want = {"CS_PRESENT_DEBOUNCE_MS": 300, "CS_BREATH_MAX_BPM": 30, "WATCHDOG_MS": 1500,
                "SCORE_MIN": 70, "LOST_TIMEOUT_MS": 1500, "DETECT_SCORE_MIN_LO": 0,
                "SENSE_DEBOUNCE_MS_HI": 3000, "SPLIT_MS": 2500, "HOLD_MS": 2500,
                "CS_RANGE_NEAR_CM": 120}
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
            # A leading zero is octal in C and C++: 070 compiles as 56.
            ("static constexpr int SCORE_MIN = 070;\n", "SCORE_MIN"),
            ("#define CS_RANGE_NEAR_CM 0100\n", "CS_RANGE_NEAR_CM"),
        ]
        for text, name in cases:
            with self.subTest(text=text):
                said = self.refused(text, name)
                self.assertIn(name, said)
                self.assertIn("not a plain integer literal", said)

    def test_a_leading_zero_is_named_as_octal(self):
        said = self.refused("static constexpr int SCORE_MIN = 070;\n", "SCORE_MIN")
        self.assertIn("octal", said)

    def test_a_commented_out_definition_is_not_read(self):
        # Before, the first textual match won: 60 and 80 here.
        text = ("// was: static constexpr int SCORE_MIN = 60;\n"
                "static constexpr int SCORE_MIN = 70;\n"
                "/* the old room:\n#define CS_RANGE_NEAR_CM 80\n*/\n"
                "#define CS_RANGE_NEAR_CM 120\n")
        self.assertEqual(self.read(text, "SCORE_MIN"), 70)
        self.assertEqual(self.read(text, "CS_RANGE_NEAR_CM"), 120)

    def test_a_constant_defined_twice_is_refused(self):
        # Which definition compiles is the preprocessor's call, not a regex's:
        # before, the first one was read (50, 1500), with --check green.
        cases = [
            ("/* two rooms,\n   two scores */\n#if VARIANT\nstatic constexpr int SCORE_MIN = 50;\n"
             "#else\nstatic constexpr int SCORE_MIN = 70;\n#endif\n", "SCORE_MIN", "lines 4, 6"),
            ("#define CS_CLEAR_TIMEOUT_MS 1500\n#undef CS_CLEAR_TIMEOUT_MS\n"
             "#define CS_CLEAR_TIMEOUT_MS 2000\n", "CS_CLEAR_TIMEOUT_MS", "lines 1, 3"),
        ]
        for text, name, lines in cases:
            with self.subTest(name=name):
                said = self.refused(text, name)
                self.assertIn(f"{name} is defined 2 times", said)
                self.assertIn(lines, said)

    def test_comment_markers_inside_a_string_are_not_comments(self):
        # The project config.h carries an OTA URL; a string's `//` or `/*`
        # must not swallow the definitions after it.
        text = ('#define OTA_URL "https://example.invalid/manifest.json"\n'
                '#define LOG_GLOB "logs/*"\n'
                "static constexpr char QUOTE = '\"';\n"
                "static constexpr int SCORE_MIN = 70;  // ends */ here\n")
        self.assertEqual(self.read(text, "SCORE_MIN"), 70)

    def test_a_missing_or_valueless_constant_is_refused(self):
        self.assertIn("not found", self.refused("#define OTHER 5\n", "WANTED"))
        self.assertIn("not found", self.refused("#define WANTED_MS 5\n", "WANTED"))
        self.assertIn("not a plain integer literal", self.refused("#define WANTED   \n", "WANTED"))

    def test_a_definition_inside_a_string_is_not_one(self):
        # Before F369 the string's `#define` counted as a second definition,
        # and the live constant was refused as defined twice.
        text = ('static constexpr const char* HINT = "#define SCORE_MIN 99";\n'
                "static constexpr int SCORE_MIN = 70;\n")
        self.assertEqual(self.read(text, "SCORE_MIN"), 70)

    def test_both_generators_read_through_the_one_rule(self):
        # gen_flash.py keeps no copy of its own, and a refusal reached through
        # the shared reader still names the module that asked, not _tooling.py.
        self.assertIs(gf.read_const, tooling.read_const)
        for copy in ("strip_c_comments", "C_COMMENT_OR_LITERAL"):
            self.assertNotIn(copy, vars(gf), f"gen_flash.py defines its own {copy} again")
        gen_vision = (TOOLS / "gen_vision.py").read_text(encoding="utf-8")
        self.assertIn("from _tooling import die, read_const, repo_root", gen_vision)
        for own in ("igrab(", r"\s*=\s*(\d+)"):
            self.assertNotIn(own, gen_vision,
                             "gen_vision.py reads a number by its own pattern again")
        said = self.refused("#define OTHER 5\n", "WANTED")
        self.assertEqual(said, f"{THIS}: ERROR: constant WANTED not found in "
                               "firmware/synthetic/consts.h\n")


class SharedCases(unittest.TestCase):
    """Every row of c_const_cases.json, which vision.test.js runs through
    c_const.js too: the same value, or the same refusal word for word."""

    def test_every_row(self):
        where = CASES["where"]
        self.assertEqual(SYNTHETIC, gf.REPO / where)
        self.assertGreaterEqual(len(CASES["cases"]), 40, "the shared table parsed thin")
        for row in CASES["cases"]:
            with self.subTest(why=row["why"]):
                err = io.StringIO()
                try:
                    with contextlib.redirect_stderr(err):
                        got = tooling.read_const(SYNTHETIC, row["name"], lambda _p: row["text"])
                except SystemExit:
                    self.assertIn("refused", row, f"refused a row that reads {row.get('value')}:"
                                                  f" {err.getvalue()}")
                    self.assertEqual(err.getvalue(), f"{THIS}: ERROR: {refusal(row, where)}\n")
                else:
                    self.assertIn("value", row, f"read {got} from a row the table refuses")
                    self.assertEqual(got, row["value"])
                    self.assertIs(type(got), int)

    def test_the_rows_cover_each_kind_of_verdict(self):
        said = " ".join(r.get("refused", "") for r in CASES["cases"])
        for reason in ("not found", "is defined 2 times", "not a plain integer literal;",
                       "(a leading 0 is octal", "past 2^53 - 1", "is not a C identifier"):
            self.assertIn(reason, said)
        self.assertTrue(any("value" in r for r in CASES["cases"]))


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

    def test_the_planted_rows_give_the_catalog_what_they_give_the_vision_page(self):
        # The rows VisionGenerator runs gen_vision.py on, through gen_flash.py's
        # four seeds: the same refusals, and otherwise the defaults unmoved.
        self.assertEqual(VISION_PROJECT, gf.REPO / CASES["planted_in"])
        text = REAL_READ(VISION_PROJECT)
        base = gf.vision_detect_block()["defaults"]
        for row in CASES["planted"]:
            with self.subTest(why=row["why"]):
                err = io.StringIO()
                with served(planted(text, row), VISION_PROJECT), \
                        contextlib.redirect_stderr(err):
                    try:
                        got = gf.vision_detect_block()["defaults"]
                    except SystemExit:
                        got = None
                if "refused" in row:
                    self.assertIsNone(got, f"read {got}")
                    said = err.getvalue()
                    self.assertTrue(said.startswith(
                        f"gen_flash.py: ERROR: {refusal(row, CASES['planted_in'])}"), said)
                else:
                    self.assertEqual(got, base, err.getvalue())


# What gen_vision.py reads besides the canary-vision project, which is copied
# whole. A source it gains fails setUpClass, naming the file it found missing.
VISION_FILES = (
    "canary-local/tools/gen_vision.py",
    "canary-local/tools/_tooling.py",
    "canary-local/tools/_pseudonym.py",
    "canary-local/devices/registry.json",
    "canary-local/devices/boards.json",
    "canary-local/devices/flash.json",
    "canary-local/emulator/vision/vision_core_bindings.cpp",
    "canary-local/emulator/web/vision-core.js",
    "docs/hardware/canary_vision_getting_started.md",
    "docs/hardware/grove_vision_ai_v2_guide.md",
    "firmware/common/boot/boot_banner.cpp",
    "firmware/common/identity/device_pseudonym.h",
    "firmware/common/network/wifi_supervisor.cpp",
    "firmware/projects/canary-wap/tests_host/test_device_pseudonym_common.cpp",
)
VISION_PROJECT_DIR = "firmware/projects/canary-vision"
VISION_CONFIG = CASES["planted_in"]
VISION_BOUNDS = f"{VISION_PROJECT_DIR}/include/canary/detect_config.h"
VISION_OUT = "canary-local/devices/vision.json"


def seeds(page: dict) -> dict:
    """vision.json's four detection seeds, the short form of a failure."""
    return {k: page["detect"][k]
            for k in ("person_target", "score_min", "lost_timeout_ms", "dwell_start_ms")}


def run_gen_vision(root: Path, edits: dict[str, str] | None = None) -> dict | str:
    """gen_vision.py on the scratch tree at `root`, `edits` (path -> text)
    written first: the vision.json it wrote, or its stderr when it refuses."""
    for rel, text in (edits or {}).items():
        (root / rel).write_text(text, encoding="utf-8")
    out = root / VISION_OUT
    out.unlink(missing_ok=True)
    env = {k: v for k, v in os.environ.items() if k != "GITHUB_ACTIONS"}
    run = subprocess.run([sys.executable, str(root / "canary-local/tools/gen_vision.py")],
                         cwd=root, env=env, capture_output=True, text=True, check=False)
    if run.returncode != 0:
        return run.stderr or f"exit {run.returncode}"
    page: dict = json.loads(out.read_text(encoding="utf-8"))
    return page


class VisionGenerator(unittest.TestCase):
    """gen_vision.py on a scratch copy of its sources, a header edited."""

    _td: tempfile.TemporaryDirectory
    root: Path
    pristine: dict[str, str]
    base: dict

    @classmethod
    def setUpClass(cls) -> None:
        cls._td = tempfile.TemporaryDirectory()
        cls.root = Path(cls._td.name)
        for rel in VISION_FILES:
            (cls.root / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(gf.REPO / rel, cls.root / rel)
        shutil.copytree(gf.REPO / VISION_PROJECT_DIR, cls.root / VISION_PROJECT_DIR)
        cls.pristine = {rel: (cls.root / rel).read_text(encoding="utf-8")
                        for rel in (VISION_CONFIG, VISION_BOUNDS)}
        base = run_gen_vision(cls.root)
        if not isinstance(base, dict):
            cls._td.cleanup()
            raise AssertionError(f"gen_vision.py fails on an unmodified copy of its sources: "
                                 f"{base}")
        cls.base = base

    @classmethod
    def tearDownClass(cls) -> None:
        cls._td.cleanup()

    def tearDown(self) -> None:
        for rel, text in self.pristine.items():
            (self.root / rel).write_text(text, encoding="utf-8")

    def generate(self, edits: dict[str, str] | None = None) -> dict | str:
        return run_gen_vision(self.root, edits)

    def test_a_planted_definition_is_refused_or_moves_nothing(self):
        # The old `NAME\s*=\s*(\d+)` read 99 for each prepend row but the
        # SCORE_MIN_X one, 3 for the expression and 70 for the octal 070.
        for row in CASES["planted"]:
            with self.subTest(why=row["why"]):
                got = self.generate({VISION_CONFIG: planted(self.pristine[VISION_CONFIG], row)})
                if "refused" in row:
                    if not isinstance(got, str):
                        self.fail(f"gen_vision.py read what the table refuses: {seeds(got)}")
                    self.assertTrue(got.startswith(
                        f"gen_vision.py: ERROR: {refusal(row, VISION_CONFIG)}"), got)
                else:
                    self.assertIsInstance(got, dict, got)  # else gen_vision.py's refusal
                    self.assertEqual(seeds(got), seeds(self.base))
                    self.assertEqual(got, self.base)

    def test_a_bound_spelled_as_an_expression_is_refused(self):
        # detect_config.h's bounds are read by the same rule as config.h's seeds.
        got = self.generate({VISION_BOUNDS: respelled(self.pristine[VISION_BOUNDS],
                                                      "DETECT_LOST_MS_HI", "60 * 1000")})
        self.assertEqual(got, f"gen_vision.py: ERROR: constant DETECT_LOST_MS_HI in "
                              f"{VISION_BOUNDS} is `60 * 1000`, not a plain integer literal; "
                              "refusing to read a number out of it\n")

    def test_a_retuned_constant_moves_the_page(self):
        # The reads are live: each edited literal lands where the page shows it.
        text = self.pristine[VISION_CONFIG]
        want = {}
        for name in ("SCORE_MIN", "LOST_TIMEOUT_MS", "FRAME_W", "AIM_AUTO_OFF_MS"):
            text, want[name] = retuned(text, name)
        got = self.generate({VISION_CONFIG: text})
        self.assertIsInstance(got, dict, got)
        self.assertEqual({"SCORE_MIN": got["detect"]["score_min"],
                          "LOST_TIMEOUT_MS": got["detect"]["lost_timeout_ms"],
                          "FRAME_W": got["detect"]["frame"]["w"],
                          "AIM_AUTO_OFF_MS": got["aim"]["auto_off_ms"]}, want)


if __name__ == "__main__":
    unittest.main()
