#!/usr/bin/env python3
"""Pins firmware/scripts/lint_platform_pins.py — the gate that keeps the
espressif32 / pioarduino platform pin in ONE file.

What is pinned and why:
  • the current tree is clean — the lint must be green on the tree it ships in;
  • it actually FAILS on each thing it claims to catch: a literal pin outside
    platforms.ini (version spec AND release URL), a reference to a section
    that does not exist, a section no env uses, two sections with one literal,
    an interpolation inside platforms.ini — a guard that reads as covered
    while catching nothing is worse than no guard (RELEASE_LESSONS.md, (x));
  • it does NOT fire on a pin quoted in a comment, on an inline comment after
    a value, or on a platform that is not ours (`native`), so a rationale
    comment can still name the number it explains.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "firmware" / "scripts" / "lint_platform_pins.py"

spec = importlib.util.spec_from_file_location("lint_platform_pins", SCRIPT)
lp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lp)  # type: ignore[union-attr]

URL = "https://github.com/pioarduino/platform-espressif32/releases/download/55.03.38-1/platform-espressif32.zip"
GOOD_PINS = f"""; comment naming espressif32@6.9.0 is fine here
[platform_s3c3]
platform = espressif32@6.9.0

[platform_core3]
platform = {URL}
"""
GOOD_ENV = """[common_esp32s3]
platform = ${platform_s3c3.platform}

[env:c6]
; platform = espressif32@6.9.0   <- quoted in a comment, not a pin
platform = ${platform_core3.platform}  ; inline comment after the value

[env:host]
platform = native
"""


def run(pins: str | None, files: dict[str, str]) -> tuple[int, str]:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        if pins is not None:
            p = root / lp.PLATFORMS_INI
            p.parent.mkdir(parents=True)
            p.write_text(pins, encoding="utf-8")
        for relpath, text in files.items():
            f = root / "firmware" / relpath
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text(text, encoding="utf-8")
        out = io.StringIO()
        with redirect_stdout(out):
            rc = lp.main(["--root", str(root)])
        return rc, out.getvalue()


class CurrentTree(unittest.TestCase):
    def test_repo_is_clean(self):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = lp.main(["--root", str(REPO)])
        self.assertEqual(rc, 0, out.getvalue())
        self.assertIn("lint_platform_pins OK", out.getvalue())


class Passes(unittest.TestCase):
    def test_interpolated_envs_pass(self):
        rc, out = run(GOOD_PINS, {"projects/x/platformio.ini": GOOD_ENV})
        self.assertEqual(rc, 0, out)
        self.assertIn("2 `platform =` lines interpolate 2 pins", out)

    def test_pio_build_dirs_are_ignored(self):
        rc, out = run(GOOD_PINS, {
            "projects/x/platformio.ini": GOOD_ENV,
            "projects/x/.pio/libdeps/env/Lib/library.ini": "[env]\nplatform = espressif32@1.0.0\n",
        })
        self.assertEqual(rc, 0, out)


class Fails(unittest.TestCase):
    def test_literal_version_spec_outside(self):
        rc, out = run(GOOD_PINS, {"projects/x/platformio.ini": GOOD_ENV + "[env:bad]\nplatform = espressif32 @ ^7.0.0\n"})
        self.assertEqual(rc, 1)
        self.assertIn("literal platform pin 'espressif32 @ ^7.0.0' in [env:bad]", out)
        self.assertIn("projects/x/platformio.ini:11", out)

    def test_literal_url_outside(self):
        rc, out = run(GOOD_PINS, {"envs/platformio/canary-x.ini": f"[env:bad]\nplatform = {URL}\n",
                                  "projects/x/platformio.ini": GOOD_ENV})
        self.assertEqual(rc, 1)
        self.assertIn("envs/platformio/canary-x.ini:2: literal platform pin", out)

    def test_dangling_reference(self):
        rc, out = run(GOOD_PINS, {"projects/x/platformio.ini": GOOD_ENV + "[env:bad]\nplatform = ${platform_nope.platform}\n"})
        self.assertEqual(rc, 1)
        self.assertIn("references [platform_nope], which platforms.ini does not define", out)

    def test_dead_section(self):
        rc, out = run(GOOD_PINS + "\n[platform_unused]\nplatform = espressif32@6.5.0\n",
                      {"projects/x/platformio.ini": GOOD_ENV})
        self.assertEqual(rc, 1)
        self.assertIn("[platform_unused] is referenced by no env", out)

    def test_duplicate_literal(self):
        rc, out = run(GOOD_PINS + "\n[platform_twin]\nplatform = espressif32@6.9.0\n",
                      {"projects/x/platformio.ini": GOOD_ENV + "[env:t]\nplatform = ${platform_twin.platform}\n"})
        self.assertEqual(rc, 1)
        self.assertIn("appears in 2 sections (platform_s3c3, platform_twin)", out)

    def test_interpolation_inside_platforms_ini(self):
        rc, out = run(GOOD_PINS.replace("platform = espressif32@6.9.0", "platform = ${platform_core3.platform}"),
                      {"projects/x/platformio.ini": GOOD_ENV})
        self.assertEqual(rc, 1)
        self.assertIn("platforms.ini holds LITERALS", out)

    def test_foreign_option_and_bad_name(self):
        rc, out = run(GOOD_PINS + "\n[platform_extra]\nplatform = espressif32@6.5.0\nboard = x\n\n[Common]\nplatform = espressif32@6.5.0\n",
                      {"projects/x/platformio.ini": GOOD_ENV + "[env:e]\nplatform = ${platform_extra.platform}\n"})
        self.assertEqual(rc, 1)
        self.assertIn("carries 'board'", out)
        self.assertIn("[Common] must be named platform_<name>", out)

    def test_missing_platforms_ini(self):
        rc, out = run(None, {"projects/x/platformio.ini": GOOD_ENV})
        self.assertEqual(rc, 1)
        self.assertIn("missing", out)


if __name__ == "__main__":
    unittest.main()
