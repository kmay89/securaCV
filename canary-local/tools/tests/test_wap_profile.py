#!/usr/bin/env python3
"""The WAP's Workshop flags are build_config.h's FULL profile, as a compiler reads it.

canary-local/tools/_wap_profile.py resolves the BUILD_PROFILE_FULL block of
the WAP sketch's build_config.h for the default XIAO ESP32-S3, and
gen_enclosures.py publishes the result as the Workshop's one "full" firmware
flavor (sweep F258 — the flags used to come from firmware/configs/canary-wap/,
which no build compiled). Pinned here, against the C preprocessor itself:
every flag the reader returns is the value `cpp` gives it with
-DHARDWARE_XIAO_ESP32S3 -DBUILD_PROFILE_FULL, and every FEATURE_* the
preprocessor defines that the reader leaves out is set outside the FULL
block. And the reader refuses, by line, every header shape it cannot read —
a guess there would publish a wrong flag on the page.

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import _wap_profile as wp  # noqa: E402  (no side effects, unlike gen_enclosures)

REPO = TOOLS.parents[1]
HEADER = REPO / "firmware/projects/canary-wap/arduino/canary_wap/build_config.h"
TEXT = HEADER.read_text(encoding="utf-8")


def preprocessor() -> list[str] | None:
    for cc in ("cpp", "gcc", "clang"):
        if shutil.which(cc):
            return [cc, "-E", "-P"] if cc == "cpp" else [cc, "-E", "-P", "-x", "c"]
    return None


def full_block_lines(text: str) -> tuple[int, int]:
    """1-based [start, end) of the `#elif defined(BUILD_PROFILE_FULL)` block that sets FEATURE_*."""
    lines = text.split("\n")
    starts = [i for i, line in enumerate(lines) if re.match(r"\s*#\s*elif\s+defined\(BUILD_PROFILE_FULL\)\s*$", line)]
    for s in starts:
        depth = 0
        for j in range(s + 1, len(lines)):
            d = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b", lines[j])
            if not d:
                continue
            if d.group(1) in ("if", "ifdef", "ifndef"):
                depth += 1
            elif d.group(1) == "endif" and depth:
                depth -= 1
            elif depth == 0:
                if any(re.match(r"\s*#\s*define\s+FEATURE_", x) for x in lines[s:j]):
                    return s + 1, j + 1
                break
    raise AssertionError("no FULL block that sets FEATURE_* in build_config.h")


def preprocessor_values(case: unittest.TestCase, include_dir: Path, flags) -> dict[str, str]:
    """What `cpp -DHARDWARE_XIAO_ESP32S3 -DBUILD_PROFILE_FULL` gives each flag
    for the build_config.h in include_dir: name -> "1" or "0"."""
    cmd = preprocessor()
    if cmd is None:
        if os.environ.get("CI"):
            case.fail("no C preprocessor on this CI runner: the oracle cannot run")
        case.skipTest("no cpp/gcc/clang on this host")
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "probe.c"
        # String literals name each flag so the preprocessor leaves the
        # label alone; #if gives the value it would compile.
        probe = ['#include "build_config.h"']
        for name in sorted(flags):
            probe += [f"#if {name}", f'"{name}" 1', "#else", f'"{name}" 0', "#endif"]
        src.write_text("\n".join(probe) + "\n")
        run = subprocess.run(
            cmd + ["-DHARDWARE_XIAO_ESP32S3", "-DBUILD_PROFILE_FULL", "-I", str(include_dir), str(src)],
            capture_output=True, text=True)
        case.assertEqual(run.returncode, 0, run.stderr)
        got = dict(re.findall(r'"(FEATURE_\w+)" ([01])', run.stdout))
        case.assertEqual(set(got), set(flags))
        return got


class RealHeader(unittest.TestCase):
    def setUp(self):
        self.flags = wp.read_full_profile(TEXT)

    def test_the_options_the_workshop_links_resolve_as_the_sketch_says(self):
        f = self.flags
        # opt_buzzer's flag: the piezo on CHIRP_GPIO is audible_chirp.h, gated on this.
        self.assertTrue(f["FEATURE_AUDIBLE_CHIRP"]["on"])
        # opt_camera's: HW_HAS_CAMERA on the S3, so on — and it says where it came from.
        self.assertEqual(f["FEATURE_CAMERA_PEEK"], {
            "on": True, "note": "ESP32-C3 has no camera interface", "expr": "HW_HAS_CAMERA"})
        # opt_tamper's: the #ifndef default, off, with the comment saying who sets it.
        self.assertFalse(f["FEATURE_TAMPER_GPIO"]["on"])
        self.assertIn("-D overridable", f["FEATURE_TAMPER_GPIO"]["note"])
        # a flag derived from two others resolves through them
        self.assertEqual(f["FEATURE_IMPROV"]["expr"], "(FEATURE_BLUETOOTH && FEATURE_BLE)")
        self.assertTrue(f["FEATURE_IMPROV"]["on"])

    def test_gnss_has_no_flag_to_read(self):
        # GPS is unconditional in the sketch (canary_wap.ino opens Serial1 for
        # the receiver at #if-depth 0), so the profile has no GNSS bit and the
        # Workshop's GPS option links none.
        self.assertFalse([n for n in self.flags if "GNSS" in n or "GPS" in n])
        self.assertNotRegex(TEXT, r"#\s*define\s+FEATURE_(GNSS|GPS)\b")

    def test_every_flag_is_the_preprocessors_value(self):
        got = preprocessor_values(self, HEADER.parent, self.flags)
        for name, entry in self.flags.items():
            self.assertEqual(entry["on"], got[name] == "1", f"{name}: reader vs preprocessor")
        # Every FEATURE_* the preprocessor defines for this build that the
        # reader leaves out is set OUTSIDE the FULL block.
        cmd = preprocessor()
        macros = subprocess.run(
            [cmd[0], "-dM", "-E"] + (["-x", "c"] if cmd[0] != "cpp" else [])
            + ["-DHARDWARE_XIAO_ESP32S3", "-DBUILD_PROFILE_FULL", str(HEADER)],
            capture_output=True, text=True)
        self.assertEqual(macros.returncode, 0, macros.stderr)
        defined = set(re.findall(r"^#define (FEATURE_\w+)\b", macros.stdout, re.M))
        start, end = full_block_lines(TEXT)
        block = "\n".join(TEXT.split("\n")[start - 1:end - 1])
        for extra in sorted(defined - set(self.flags)):
            self.assertNotRegex(block, r"#\s*define\s+%s\b" % extra,
                                f"{extra} is set in the FULL block but the reader dropped it")
        self.assertTrue(defined >= set(self.flags))

    def test_keeps_the_blocks_order(self):
        start, end = full_block_lines(TEXT)
        block = "\n".join(TEXT.split("\n")[start - 1:end - 1])
        in_text = []
        for name in re.findall(r"#\s*define\s+(FEATURE_\w+)", block):
            if name not in in_text:
                in_text.append(name)
        self.assertEqual(list(self.flags), in_text)


# A minimal header in the real one's shape, for the refusals.
GOOD = """\
#if !defined(HARDWARE_XIAO_ESP32S3) && !defined(HARDWARE_XIAO_ESP32C3)
#define HARDWARE_XIAO_ESP32S3   // XIAO
// #define HARDWARE_XIAO_ESP32C3
#endif
#if defined(HARDWARE_XIAO_ESP32C3)
  #define HW_HAS_CAMERA 0
  #define HW_HAS_PSRAM  0
#elif defined(HARDWARE_XIAO_ESP32S3)
  #define HW_HAS_CAMERA 1
  #define HW_HAS_BLE    1
  #define HW_HAS_PSRAM  0
#endif
#if defined(BUILD_PROFILE_MINIMAL)
  #define FEATURE_A 0
#elif defined(BUILD_PROFILE_FULL)
  #define FEATURE_A 1   // first
  #define FEATURE_B HW_HAS_CAMERA
  #ifndef FEATURE_C   // -D overridable
  #define FEATURE_C 0
  #endif
  #define FEATURE_D (FEATURE_A && !FEATURE_C)
  #define FEATURE_E (FEATURE_A && FEATURE_C)
  #define FEATURE_F (FEATURE_C || FEATURE_A)
  #define FEATURE_G (HW_HAS_CAMERA && HW_HAS_PSRAM)
  #define FEATURE_H FEATURE_A || FEATURE_C && FEATURE_C
  #define FEATURE_I !(FEATURE_C || HW_HAS_PSRAM)
  #define DEBUG_X 0
#endif
#if defined(BUILD_PROFILE_FULL)
  #define BUILD_PROFILE_NAME "FULL"
#endif
"""


class Refusals(unittest.TestCase):
    def test_the_good_shape_reads(self):
        self.assertEqual(wp.read_full_profile(GOOD), {
            "FEATURE_A": {"on": True, "note": "first"},
            "FEATURE_B": {"on": True, "expr": "HW_HAS_CAMERA"},
            "FEATURE_C": {"on": False, "note": "-D overridable"},
            "FEATURE_D": {"on": True, "expr": "(FEATURE_A && !FEATURE_C)"},
            # The operators come apart here (review of F258): the real header's
            # only && flags have every operand on, so neither it nor FEATURE_D
            # tells && from ||. E is off and F on only if each is itself; G
            # reads an S3 capability that is 0; H is on only if && binds
            # tighter than ||, as in C; I negates a parenthesized ||.
            "FEATURE_E": {"on": False, "expr": "(FEATURE_A && FEATURE_C)"},
            "FEATURE_F": {"on": True, "expr": "(FEATURE_C || FEATURE_A)"},
            "FEATURE_G": {"on": False, "expr": "(HW_HAS_CAMERA && HW_HAS_PSRAM)"},
            "FEATURE_H": {"on": True, "expr": "FEATURE_A || FEATURE_C && FEATURE_C"},
            "FEATURE_I": {"on": True, "expr": "!(FEATURE_C || HW_HAS_PSRAM)"},
        })

    def test_the_good_shape_is_the_preprocessors_value(self):
        # The fixture's answers are the C preprocessor's, not this file's
        # opinion of them: the same oracle the real header is held to.
        flags = wp.read_full_profile(GOOD)
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "build_config.h").write_text(GOOD)
            got = preprocessor_values(self, Path(tmp), flags)
        for name, entry in flags.items():
            self.assertEqual(entry["on"], got[name] == "1", f"{name}: reader vs preprocessor")

    def refuses(self, text, needle):
        with self.assertRaises(wp.WapProfileError) as ctx:
            wp.read_full_profile(text)
        self.assertIn(needle, str(ctx.exception))

    def test_no_full_block(self):
        self.refuses(GOOD.replace("#elif defined(BUILD_PROFILE_FULL)", "#elif defined(BUILD_PROFILE_DEV)"),
                     "BUILD_PROFILE_FULL")

    def test_two_full_blocks_that_set_flags(self):
        self.refuses(GOOD + "#if defined(BUILD_PROFILE_FULL)\n#define FEATURE_Z 1\n#endif\n",
                     "blocks set FEATURE_*")

    def test_no_s3_capability_block(self):
        self.refuses(GOOD.replace("#elif defined(HARDWARE_XIAO_ESP32S3)\n", "#else\n"), "HARDWARE_XIAO_ESP32S3")

    def test_a_different_default_target(self):
        self.refuses(GOOD.replace("#define HARDWARE_XIAO_ESP32S3   // XIAO\n// #define HARDWARE_XIAO_ESP32C3",
                                  "// #define HARDWARE_XIAO_ESP32S3\n#define HARDWARE_XIAO_ESP32C3"),
                     "in-file default hardware target")

    def test_an_unknown_name(self):
        self.refuses(GOOD.replace("HW_HAS_CAMERA\n  #ifndef", "HW_HAS_RADAR\n  #ifndef"), "HW_HAS_RADAR")

    def test_an_operator_it_does_not_read(self):
        self.refuses(GOOD.replace("(FEATURE_A && !FEATURE_C)", "(FEATURE_A > FEATURE_C)"), "cannot read")

    def test_defined_in_a_value(self):
        self.refuses(GOOD.replace("(FEATURE_A && !FEATURE_C)", "defined(FEATURE_A)"), "defined")

    def test_an_unclosed_paren(self):
        self.refuses(GOOD.replace("(FEATURE_A && !FEATURE_C)", "(FEATURE_A && !FEATURE_C"), "unclosed")

    def test_a_nested_conditional(self):
        self.refuses(GOOD.replace("  #define DEBUG_X 0\n", "  #if FEATURE_A\n  #define FEATURE_E 1\n  #endif\n"),
                     "#if")

    def test_an_ifndef_that_is_not_a_default(self):
        self.refuses(GOOD.replace("  #ifndef FEATURE_C   // -D overridable\n  #define FEATURE_C 0\n",
                                  "  #ifndef FEATURE_C   // -D overridable\n  #define FEATURE_Q 0\n"),
                     "#ifndef FEATURE_C")

    def test_a_directive_it_does_not_know(self):
        self.refuses(GOOD.replace("  #define DEBUG_X 0\n", '  #warning "x"\n'), "#warning")

    def test_a_flag_set_twice(self):
        self.refuses(GOOD.replace("  #define DEBUG_X 0\n", "  #define FEATURE_A 0\n"), "set twice")

    def test_a_flag_that_is_not_a_bit(self):
        self.refuses(GOOD.replace("#define FEATURE_A 1   // first", "#define FEATURE_A 2"), "comes out 2")

    def test_a_flag_that_names_itself(self):
        self.refuses(GOOD.replace("#define FEATURE_A 1   // first", "#define FEATURE_A FEATURE_D"),
                     "resolves to itself")

    def test_a_block_that_never_closes(self):
        self.refuses(GOOD.replace('#if defined(BUILD_PROFILE_FULL)\n  #define BUILD_PROFILE_NAME "FULL"\n#endif\n', "")
                     .rsplit("#endif", 1)[0], "never closes")


class WorkshopNotes(unittest.TestCase):
    """apply_workshop_notes(): the Workshop's curated notes, each held to the header."""

    FLAGS = {"FEATURE_A": {"on": True, "note": "first"},
             "FEATURE_B": {"on": True, "note": "ESP32-C3 has none", "expr": "HW_HAS_CAMERA"},
             "FEATURE_C": {"on": False, "note": "-D overridable"}}

    def test_a_curated_note_replaces_the_line_and_none_drops_it(self):
        got = wp.apply_workshop_notes(self.FLAGS, "/* C stays off until bench-validated */", {
            "FEATURE_B": (None, []),
            "FEATURE_C": ("off until bench-validated", [r"off until bench-validated"])})
        self.assertEqual(got, {"FEATURE_A": {"on": True, "note": "first"},
                               "FEATURE_B": {"on": True, "expr": "HW_HAS_CAMERA"},
                               "FEATURE_C": {"on": False, "note": "off until bench-validated"}})
        self.assertEqual(list(got["FEATURE_B"]), ["on", "expr"])
        self.assertEqual(self.FLAGS["FEATURE_C"]["note"], "-D overridable", "the input is not edited")

    def test_a_caveat_the_header_dropped_is_refused(self):
        with self.assertRaises(wp.WapProfileError) as ctx:
            wp.apply_workshop_notes(self.FLAGS, "/* C is validated now */", {
                "FEATURE_B": (None, []),
                "FEATURE_C": ("off until bench-validated", [r"off until bench-validated"])})
        self.assertIn("FEATURE_C", str(ctx.exception))
        self.assertIn("no longer says", str(ctx.exception))

    def test_a_note_for_a_flag_the_profile_dropped_is_refused(self):
        with self.assertRaises(wp.WapProfileError) as ctx:
            wp.apply_workshop_notes(self.FLAGS, "", {"FEATURE_B": (None, []), "FEATURE_Z": ("z", [])})
        self.assertIn("FEATURE_Z", str(ctx.exception))

    def test_a_note_naming_the_c3_is_refused_until_curated(self):
        with self.assertRaises(wp.WapProfileError) as ctx:
            wp.apply_workshop_notes(self.FLAGS, "", {})
        self.assertIn("FEATURE_B", str(ctx.exception))
        self.assertIn("another target", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
