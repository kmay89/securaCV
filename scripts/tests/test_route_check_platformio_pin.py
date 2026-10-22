#!/usr/bin/env python3
"""Pins the flagship route check's PlatformIO to the release builds' one
(firmware/canary/scripts/check_route_security.py: PLATFORMIO_PIN,
platformio_pin_problems).

The route check's library rule reads firmware/canary/platformio.ini with
PlatformIO's own reader at PLATFORMIO_PIN, which its docstring calls the
version the builds that ship the flagship pin: firmware-release.yml and
flasher-release.yml hand it to setup-platformio, and firmware.yml's
route-check job installs it before the check. Nothing made those move
together: a release pin bumped alone left every gate green while the check
kept reading with the old PlatformIO (the eleventh round's review). The
route check runs under firmware.yml's path filter, which does not name
flasher-release.yml, so the equality is held here instead, in lint.yml's
unfiltered `unittest discover -s scripts/tests`.

What is pinned:
  - the tree holds: every pin is PLATFORMIO_PIN;
  - it FAILS on each drift it claims to catch: either release workflow's pin
    bumped alone, a release workflow with no pin (setup-platformio then
    installs the newest), the route-check job's pin bumped, its install step
    removed or moved below the check, a pin in another job only, and the
    check run twice or not at all;
  - a pin quoted in a comment is not read as one.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github" / "workflows"
SCRIPT = REPO / "firmware" / "canary" / "scripts" / "check_route_security.py"

spec = importlib.util.spec_from_file_location("check_route_security", SCRIPT)
crs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(crs)  # type: ignore[union-attr]

PIN = crs.PLATFORMIO_PIN
NAMES = crs.PLATFORMIO_PIN_RELEASES + crs.PLATFORMIO_PIN_CHECK[:1]
PIN_LINE = f'platformio-version: "{PIN}"'
CHECK_RUN = crs.PLATFORMIO_PIN_CHECK[1]


class PlatformioPinTest(unittest.TestCase):
    def doctored(self, name: str, old: str, new: str, count: int = 1) -> list:
        """platformio_pin_problems() over a copy of the three workflows with
        `old` replaced by `new` in `name` (`old` must occur `count` times)."""
        with tempfile.TemporaryDirectory() as tmp:
            for n in NAMES:
                shutil.copy(WORKFLOWS / n, Path(tmp) / n)
            path = Path(tmp) / name
            text = path.read_text()
            self.assertEqual(text.count(old), count, (name, old))
            path.write_text(text.replace(old, new))
            return crs.platformio_pin_problems(Path(tmp))

    def assert_named(self, problems: list, name: str, words: str) -> None:
        self.assertTrue(any(name in p and words in p for p in problems), problems)

    def test_tree_holds(self) -> None:
        self.assertEqual(crs.platformio_pin_problems(WORKFLOWS), [])

    def test_release_pin_bumped_alone_fails(self) -> None:
        for name in crs.PLATFORMIO_PIN_RELEASES:
            with self.subTest(name=name):
                got = self.doctored(name, PIN_LINE, 'platformio-version: "6.2.0"')
                self.assert_named(got, name, "platformio-version '6.2.0' is not "
                                             "check_route_security.py's PLATFORMIO_PIN")

    def test_release_without_a_pin_fails(self) -> None:
        for name in crs.PLATFORMIO_PIN_RELEASES:
            with self.subTest(name=name):
                got = self.doctored(name, f"          {PIN_LINE}\n", "")
                self.assert_named(got, name, "hands setup-platformio no platformio-version")

    def test_route_check_job_pin_bumped_fails(self) -> None:
        got = self.doctored("firmware.yml", PIN_LINE, 'platformio-version: "6.2.0"')
        self.assert_named(got, "firmware.yml", "installs platformio-version '6.2.0'")

    def test_route_check_job_without_its_install_fails(self) -> None:
        got = self.doctored("firmware.yml", f"          {PIN_LINE}\n", "")
        self.assert_named(got, "firmware.yml", "installs platformio-version none above it")

    def test_install_below_the_check_fails(self) -> None:
        text = (WORKFLOWS / "firmware.yml").read_text()
        run = f"          {CHECK_RUN}\n"
        self.assertEqual(text.count(run), 1)
        moved = text.replace(f"          {PIN_LINE}\n", "").replace(
            run, run + f"      - uses: ./.github/actions/setup-platformio\n"
                       f"        with:\n          {PIN_LINE}\n")
        got = self.doctored("firmware.yml", text, moved)
        self.assert_named(got, "firmware.yml", "installs platformio-version none above it")

    def test_pin_in_another_job_only_fails(self) -> None:
        text = (WORKFLOWS / "firmware.yml").read_text()
        moved = text.replace(f"          {PIN_LINE}\n", "", 1)
        jobs = moved.index("\njobs:\n") + len("\njobs:\n")
        moved = (moved[:jobs] + "  pinned-elsewhere:\n    runs-on: ubuntu-24.04\n    steps:\n"
                 "      - uses: ./.github/actions/setup-platformio\n        with:\n"
                 f"          {PIN_LINE}\n" + moved[jobs:])
        got = self.doctored("firmware.yml", text, moved)
        self.assert_named(got, "firmware.yml", "installs platformio-version none above it")

    def test_check_run_twice_or_never_fails(self) -> None:
        run = f"          {CHECK_RUN}\n"
        got = self.doctored("firmware.yml", run, run + run)
        self.assert_named(got, "firmware.yml", "twice")
        got = self.doctored("firmware.yml", run, "")
        self.assert_named(got, "firmware.yml", "0 times")

    def test_pin_in_a_comment_is_not_read(self) -> None:
        name = crs.PLATFORMIO_PIN_RELEASES[0]
        got = self.doctored(name, f"          {PIN_LINE}\n",
                            f"          # was platformio-version: \"{PIN}\"\n")
        self.assert_named(got, name, "hands setup-platformio no platformio-version")


if __name__ == "__main__":
    unittest.main()
