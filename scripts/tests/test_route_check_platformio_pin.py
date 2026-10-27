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
  - PLATFORMIO_PIN_RELEASES is the two workflows that build the flagship's
    shipped env (`pio run -e release_ha`), by value and by what each runs,
    so a name dropped from the tuple does not drop its check unseen (the
    cases below loop over the tuple);
  - it FAILS on each drift it claims to catch: either release workflow's pin
    bumped alone, a second pin in a release workflow that is not
    PLATFORMIO_PIN (every pin is held, not the first), a release workflow
    with no pin (setup-platformio then installs the newest), the
    route-check job's pin bumped, a second pin in that job in either order
    and the pin twice (exactly one), its install step removed or moved below
    the check, a pin in another job only, and the check run twice or not at
    all;
  - each invocation the rule reads is held, not only the pins a workflow
    spells: in each release workflow and in the route-check job (above the
    check or below it), a second setup-platformio step with no
    platformio-version (bare, or with only its extras), one with two, and an
    install of PlatformIO at no pin or another one FAIL, where the pins
    collected across the file used to cover them (Codex, #1852): pip's
    `--upgrade platformio`, `platformio==6.2.0`, a range, a name split onto
    a continuation line or folded onto the next line of a `run: >` block,
    `pip3.11`, an option before `install`, `-mpip`, PlatformIO's source by
    URL or VCS and a wheel of it by path, `uv tool install` and `pipx
    upgrade`; and so do `pio upgrade`, `platformio upgrade` (also as `python3
    -m platformio upgrade`) and the get-platformio.py installer, which name
    no package (the #1852 review). An install at the pin passes, as do a
    package whose name only starts with platformio, a `pio` command that
    installs nothing and a comment naming an upgrade; firmware.yml's other
    jobs, which take the newest PlatformIO on purpose, are not held;
  - a pin quoted in a comment is not read as one;
  - main() refuses another PlatformIO, or none, before any rule runs, naming
    the install command (in CI its job installs the pin, so this refusal is
    otherwise never seen to work).

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import re
import shutil
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

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
# A second setup-platformio step, as a workflow would add one.
STEP = ("      - uses: ./.github/actions/setup-platformio\n"
        "        with:\n"
        '          platformio-version: "{}"\n')
# Steps that run setup-platformio with no platformio-version: the action then
# installs the newest PlatformIO (`pip install --upgrade platformio`).
UNPINNED_STEPS = (
    "      - uses: ./.github/actions/setup-platformio\n",
    "      - name: Set up PlatformIO again\n"
    "        uses: ./.github/actions/setup-platformio\n"
    "        with:\n          extras: intelhex==2.3.0\n",
)
# A step that hands it the pin twice (the last one wins).
TWICE_PINNED_STEP = STEP.format(PIN) + f'          platformio-version: "{PIN}"\n'
# A pinned step written with its `with:` above its `uses:`.
WITH_FIRST_STEP = ("      - name: Set up PlatformIO, keys reordered\n        with:\n"
                   f'          platformio-version: "{PIN}"\n'
                   "        uses: ./.github/actions/setup-platformio\n")
# Steps that install PlatformIO with pip, other than at the pin.
UNPINNED_INSTALLS = (
    "      - run: python3 -m pip install --upgrade platformio\n",
    "      - run: pip install platformio==6.2.0\n",
    "      - run: pip3 install --quiet 'platformio>=6.1'\n",
    "      - name: Tools\n        run: |\n          python3 -m pip install --quiet \\\n"
    "            intelhex==2.3.0 \\\n            platformio\n",
    # The pip of one interpreter, an option before `install`, `-mpip`, a
    # folded block that puts the name on the next line, and PlatformIO's own
    # source (its documented way to install the development version) or a
    # wheel of it, by URL.
    "      - run: pip3.11 install -U platformio\n",
    "      - run: pip --quiet install platformio\n",
    "      - run: python3 -mpip install platformio\n",
    "      - run: >\n          python3 -m pip install --quiet\n          platformio\n",
    "      - run: pip install -U https://github.com/platformio/platformio-core/archive/"
    "develop.zip\n",
    "      - run: pip install git+https://github.com/platformio/platformio-core.git@develop\n",
    "      - run: python3 -m pip install ./dist/platformio-6.2.0-py3-none-any.whl\n",
    # Installers other than pip.
    "      - run: uv tool install platformio\n",
    "      - run: pipx upgrade platformio\n",
)
# Steps that replace PlatformIO Core with the newest release without naming
# the package (PlatformIO's own CLI suggests the first when one is out), and
# the words a problem names for each.
NEWEST_INSTALLS = (
    ("      - run: pio upgrade\n", "`pio upgrade` replaces PlatformIO Core with the newest"),
    ("      - name: Upgrade PlatformIO\n        run: |\n          platformio upgrade --dev\n",
     "`platformio upgrade` replaces PlatformIO Core with the newest"),
    ("      - run: python3 -m platformio upgrade\n",
     "`platformio upgrade` replaces PlatformIO Core with the newest"),
    ("      - run: curl -fsSLO https://raw.githubusercontent.com/platformio/"
     "platformio-core-installer/master/get-platformio.py && python3 get-platformio.py\n",
     "get-platformio.py installs the newest PlatformIO Core"),
)
# Steps that use PlatformIO, or install another package, and pass.
PASSING_STEPS = (
    f"      - run: python3 -m pip install --quiet platformio=={PIN} platformio-extras==1.0\n",
    f"      - run: uv tool install platformio=={PIN}\n",
    "      - run: pip install https://example.com/platformio-extras-1.0.tar.gz\n",
    "      - run: pio --version && pio pkg update -e release_ha  # not pio upgrade\n",
    "      - run: |\n          # pip install --upgrade platformio, once\n          pio run\n",
)


def step_bounds(text: str, at: int) -> tuple:
    """(start, end) of the workflow step (`      - ` at six spaces) that holds
    offset `at` of `text`; `end` is the next step's start."""
    start = text.rfind("\n      - ", 0, at) + 1
    end = text.find("\n      - ", at) + 1
    assert start > 0 and end > 0, (start, end)
    return start, end


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

    def test_release_workflows_are_the_flagship_release_builds(self) -> None:
        self.assertEqual(crs.PLATFORMIO_PIN_RELEASES,
                         ("firmware-release.yml", "flasher-release.yml"))
        builds = sorted(p.name for p in WORKFLOWS.glob("*.yml")
                        if re.search(r"\bpio run -e release_ha\b", p.read_text()))
        self.assertEqual(builds, sorted(crs.PLATFORMIO_PIN_RELEASES))

    def test_second_release_pin_fails(self) -> None:
        for name in crs.PLATFORMIO_PIN_RELEASES:
            with self.subTest(name=name):
                text = (WORKFLOWS / name).read_text()
                _start, end = step_bounds(text, text.index(f"          {PIN_LINE}\n"))
                got = self.doctored(name, text, text[:end] + STEP.format("6.2.0") + text[end:])
                self.assert_named(got, name, "platformio-version '6.2.0' is not "
                                             "check_route_security.py's PLATFORMIO_PIN")

    def test_every_setup_step_in_a_release_is_pinned(self) -> None:
        # A second setup-platformio step with no platformio-version installs
        # the newest PlatformIO over the pinned one. Each step is held, not
        # only the pins the workflow spells somewhere in it.
        for name in crs.PLATFORMIO_PIN_RELEASES:
            text = (WORKFLOWS / name).read_text()
            _start, end = step_bounds(text, text.index(f"          {PIN_LINE}\n"))
            for step in UNPINNED_STEPS:
                with self.subTest(name=name, step=step):
                    got = self.doctored(name, text, text[:end] + step + text[end:])
                    self.assert_named(got, name, "a setup-platformio step hands it no "
                                                 "platformio-version")
            with self.subTest(name=name, step=TWICE_PINNED_STEP):
                got = self.doctored(name, text, text[:end] + TWICE_PINNED_STEP + text[end:])
                self.assert_named(got, name, "a setup-platformio step hands it 2 "
                                             "platformio-version lines")
            # A pinned step whose `with:` comes before its `uses:` is pinned.
            with self.subTest(name=name, step=WITH_FIRST_STEP):
                self.assertEqual(self.doctored(name, text,
                                               text[:end] + WITH_FIRST_STEP + text[end:]), [])

    def test_every_pip_install_of_platformio_in_a_release_is_pinned(self) -> None:
        for name in crs.PLATFORMIO_PIN_RELEASES:
            text = (WORKFLOWS / name).read_text()
            _start, end = step_bounds(text, text.index(f"          {PIN_LINE}\n"))
            for step in UNPINNED_INSTALLS:
                with self.subTest(name=name, step=step):
                    got = self.doctored(name, text, text[:end] + step + text[end:])
                    self.assert_named(got, name, f"not platformio=={PIN}")
            # At the pin, it holds; a package whose name only starts with
            # platformio is another package, and a comment or a `pio` command
            # that installs nothing is not an install.
            for step in PASSING_STEPS:
                with self.subTest(name=name, step=step):
                    self.assertEqual(self.doctored(name, text, text[:end] + step + text[end:]),
                                     [])

    def test_every_upgrade_of_platformio_in_a_release_fails(self) -> None:
        # `pio upgrade` and PlatformIO's installer script name no package, and
        # take the newest PlatformIO over the pinned one.
        for name in crs.PLATFORMIO_PIN_RELEASES:
            text = (WORKFLOWS / name).read_text()
            _start, end = step_bounds(text, text.index(f"          {PIN_LINE}\n"))
            for step, words in NEWEST_INSTALLS:
                with self.subTest(name=name, step=step):
                    got = self.doctored(name, text, text[:end] + step + text[end:])
                    self.assert_named(got, name, words)

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

    def test_route_check_job_with_two_pins_fails(self) -> None:
        text = (WORKFLOWS / "firmware.yml").read_text()
        check, _end = step_bounds(text, text.index(f"          {CHECK_RUN}\n"))
        bumped = text.replace(PIN_LINE, 'platformio-version: "6.2.0"')
        self.assertEqual(text.count(PIN_LINE), 1)
        for doctored, first, second in (
                (text[:check] + STEP.format("6.2.0") + text[check:], PIN, "6.2.0"),
                (bumped[:check] + STEP.format(PIN) + bumped[check:], "6.2.0", PIN),
                (text[:check] + STEP.format(PIN) + text[check:], PIN, PIN)):
            with self.subTest(pins=(first, second)):
                got = self.doctored("firmware.yml", text, doctored)
                self.assertTrue(any(re.search(
                    rf"installs platformio-version '{re.escape(first)}' \(line \d+\), "
                    rf"'{re.escape(second)}' \(line \d+\) above it, not exactly "
                    rf"PLATFORMIO_PIN", p) for p in got), got)

    def test_route_check_job_second_unpinned_setup_or_install_fails(self) -> None:
        # Above the check, a setup-platformio step with no platformio-version
        # (or with two), an install of PlatformIO at no pin or an upgrade of
        # it replaces the pinned one before the check runs; below it, the job
        # still installs it unpinned.
        text = (WORKFLOWS / "firmware.yml").read_text()
        check, end = step_bounds(text, text.index(f"          {CHECK_RUN}\n"))
        for at in (check, end):
            for step, words in ([(s, "a setup-platformio step hands it no platformio-version")
                                 for s in UNPINNED_STEPS]
                                + [(s, f"not platformio=={PIN}") for s in UNPINNED_INSTALLS]
                                + list(NEWEST_INSTALLS)
                                + [(TWICE_PINNED_STEP, "a setup-platformio step hands it 2 "
                                                       "platformio-version lines")]
                                + [(STEP.format("6.2.0"), "platformio-version '6.2.0'")]
                                * (at == end)):
                with self.subTest(below=at == end, step=step):
                    got = self.doctored("firmware.yml", text, text[:at] + step + text[at:])
                    self.assert_named(got, "firmware.yml", words)

    def test_unpinned_platformio_in_another_firmware_job_passes(self) -> None:
        # firmware.yml's build jobs take the newest PlatformIO on purpose; only
        # the route-check job is held.
        text = (WORKFLOWS / "firmware.yml").read_text()
        check = text.index(f"          {CHECK_RUN}\n")
        job = text.rindex("\n  build-platformio:\n", 0, check)
        _start, end = step_bounds(text, text.index("\n      - ", job) + 1)
        for step in UNPINNED_STEPS + UNPINNED_INSTALLS + tuple(s for s, _w in NEWEST_INSTALLS):
            with self.subTest(step=step):
                self.assertEqual(self.doctored("firmware.yml", text,
                                               text[:end] + step + text[end:]), [])

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


class MainRefusesAnotherPlatformioTest(unittest.TestCase):
    """main()'s version guard: another PlatformIO, or none, stops it before
    any rule runs, naming the install command; PLATFORMIO_PIN gets past it."""

    def run_main(self, module) -> tuple:
        out = io.StringIO()
        ran = mock.Mock(side_effect=RuntimeError("main() ran past its version guard"))
        with mock.patch.object(crs, "platformio", module), \
                mock.patch.object(crs, "_assert_parser_robust", ran), \
                contextlib.redirect_stdout(out):
            code = crs.main()
        return code, out.getvalue()

    def test_another_version_or_none_is_refused(self) -> None:
        for module, found in ((types.SimpleNamespace(__version__="6.2.0"),
                               "platformio 6.2.0 is installed"),
                              (types.SimpleNamespace(__version__="6.1.18"),
                               "platformio 6.1.18 is installed"),
                              (None, "no platformio module imports")):
            with self.subTest(found=found):
                code, out = self.run_main(module)
                self.assertEqual(code, 1, out)
                self.assertIn(found, out)
                self.assertIn(f"run: {crs.PLATFORMIO_INSTALL}", out)
                self.assertIn(f"platformio=={PIN}", crs.PLATFORMIO_INSTALL)

    def test_the_pin_gets_past_the_guard(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "ran past its version guard"):
            self.run_main(types.SimpleNamespace(__version__=PIN))


if __name__ == "__main__":
    unittest.main()
