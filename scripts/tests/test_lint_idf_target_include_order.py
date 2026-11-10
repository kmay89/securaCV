#!/usr/bin/env python3
"""Pins scripts/lint_idf_target_include_order.py: green on the tree, red on
the thermal provider's old ordering, quiet on a macro that is only mentioned.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "lint_idf_target_include_order.py"
THERMAL = "firmware/canary/lib/securacv_thermal/src/securacv_thermal.cpp"

spec = importlib.util.spec_from_file_location("lint_idf_target_include_order", SCRIPT)
lint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lint)  # type: ignore[union-attr]

TARGET_TEST = "#if defined(CONFIG_IDF_TARGET_ESP32S3)\n#endif\n"


def run(files: dict[str, str]) -> tuple[int, str]:
    """The lint's exit code and output on a scratch tree holding `files`."""
    with tempfile.TemporaryDirectory() as tmp:
        for rel, text in files.items():
            path = Path(tmp) / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = lint.main(["--root", tmp])
        return code, out.getvalue()


class RealTree(unittest.TestCase):
    def test_tree_is_clean(self):
        self.assertEqual(lint.lint(REPO), [])

    def test_tree_has_sources_that_test_a_target(self):
        # Otherwise "clean" would only mean nothing was read.
        tested = [p for p in lint.sources(REPO)
                  if lint.MACRO.search(p.read_text(encoding="utf-8", errors="replace"))]
        self.assertGreater(len(tested), 10)


class OldThermalProvider(unittest.TestCase):
    """The provider as it shipped: no include reached sdkconfig.h above its
    target test, so the S3 compiled the sensorless stub."""

    def setUp(self):
        self.fixed = (REPO / THERMAL).read_text(encoding="utf-8")
        self.assertEqual(self.fixed.count("#include <sdkconfig.h>\n"), 1)
        self.old = self.fixed.replace("#include <sdkconfig.h>\n", "")

    def test_old_ordering_fails(self):
        code, out = run({THERMAL: self.old})
        self.assertEqual(code, 1, out)
        line = self.old.split("\n").index("#if defined(CONFIG_IDF_TARGET_ESP32S3) || "
                                          "defined(CONFIG_IDF_TARGET_ESP32S2) || \\") + 1
        self.assertIn(f"{THERMAL}:{line}: CONFIG_IDF_TARGET_ESP32S3", out)

    def test_fixed_ordering_passes(self):
        code, out = run({THERMAL: self.fixed})
        self.assertEqual(code, 0, out)


class Rule(unittest.TestCase):
    def early(self, text: str):
        return lint.first_early_test(text)

    def test_each_defining_include_covers_the_test(self):
        for inc in ("<sdkconfig.h>", '"sdkconfig.h"', "<Arduino.h>", "<esp_idf_version.h>",
                    '"esp_camera.h"', "<driver/gpio.h>", "<freertos/FreeRTOS.h>"):
            with self.subTest(inc=inc):
                self.assertIsNone(self.early(f"#include {inc}\n{TARGET_TEST}"))

    def test_other_includes_do_not(self):
        for inc in ('"securacv_thermal.h"', "<stdint.h>", '"my_driver/gpio.h"', "<WiFi.h>"):
            with self.subTest(inc=inc):
                self.assertEqual(self.early(f"#include {inc}\n{TARGET_TEST}"),
                                 (2, "CONFIG_IDF_TARGET_ESP32S3"))

    def test_include_after_the_test_is_too_late(self):
        self.assertEqual(self.early(TARGET_TEST + "#include <sdkconfig.h>\n"),
                         (1, "CONFIG_IDF_TARGET_ESP32S3"))

    def test_every_conditional_form_and_both_macros(self):
        for directive in ("#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)",
                          "#ifdef CONFIG_IDF_TARGET_ESP32C3",
                          "#ifndef CONFIG_IDF_TARGET_ESP32C6",
                          "#if FOO\n#elif defined(CONFIG_IDF_TARGET_ESP32S2)",
                          "  #  if \\\n  ESP_IDF_VERSION_MAJOR >= 5"):
            with self.subTest(directive=directive):
                self.assertIsNotNone(self.early(directive + "\n#endif\n"))

    def test_a_mention_is_not_a_test(self):
        for text in ("// #if CONFIG_IDF_TARGET_ESP32S3 would be wrong here\n",
                     "/* the old code:\n#if ESP_IDF_VERSION_MAJOR >= 5\n*/\n",
                     'const char* s = R"x(\n#ifdef CONFIG_IDF_TARGET_ESP32\n)x";\n',
                     "#define TSENS_OK defined(CONFIG_IDF_TARGET_ESP32S3)\n",
                     "#if FOO  // not CONFIG_IDF_TARGET_ESP32\n#endif\n"):
            with self.subTest(text=text):
                self.assertIsNone(self.early(text))

    def test_a_commented_out_include_covers_nothing(self):
        for inc in ("// #include <sdkconfig.h>", "/* #include <Arduino.h> */"):
            with self.subTest(inc=inc):
                self.assertIsNotNone(self.early(f"{inc}\n{TARGET_TEST}"))

    def test_comment_marker_inside_a_string_is_not_a_comment(self):
        self.assertIsNotNone(self.early(f'const char* u = "http://x/*";\n{TARGET_TEST}// */\n'))


class Scope(unittest.TestCase):
    def test_all_three_suffixes_and_only_firmware(self):
        code, out = run({"firmware/a/x.cpp": TARGET_TEST, "firmware/a/x.h": TARGET_TEST,
                         "firmware/a/x.ino": TARGET_TEST, "firmware/a/x.txt": TARGET_TEST,
                         "src/x.cpp": TARGET_TEST})
        self.assertEqual(code, 1, out)
        flagged = sorted(row.split(":")[0] for row in out.splitlines() if ": CONFIG_IDF" in row)
        self.assertEqual(flagged, ["firmware/a/x.cpp", "firmware/a/x.h", "firmware/a/x.ino"])

    def test_an_empty_tree_is_an_error_not_a_pass(self):
        code, _ = run({"README.md": "x\n"})
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
