#!/usr/bin/env python3
"""Pins firmware/scripts/check_slot_budget.py — the release workflow's
byte-accurate OTA slot check, run on the exact bytes it is about to sign.

What is pinned and why:
  • an env with no budget is a `::warning::` annotation and still exit 0. It
    used to print a plain line, and 9 of the 17 OTA products fw-v2.4.16
    signed went out unmeasured with nothing on the run's summary page to say
    so;
  • an image exactly at its budget passes, one byte over fails, and a
    missing image fails: the release loops skip a variant on exit 1, so a
    check that passed a missing file would sign nothing and say it measured;
  • the headroom floor (F367): an image that fits with less than
    HEADROOM_FLOOR_PERCENT of its slot free is a one-line `::warning::`
    naming the env, its free bytes and the floor, and the exit status is the
    same as before the floor existed (fits passes, over fails) at every
    boundary, for a slot whose 5% is a whole byte and one whose 5% is not.
    fw-v2.4.16's measured images are the anchor: the C6 nightstand's 2,832
    free bytes warn, the Sense images' 7% free stay quiet;
  • firmware.yml's PR gate runs THIS script (its size_guard wrapper is run
    here, extracted from the workflow, against a stub `pio`): every guard
    of every build leg reaches the script with its own bin and slot, an
    absent or empty bin and an over-slot image fail the step with an
    `::error::`, an env the script has no budget for fails it too
    (--require-budget), and the floor's warning shows on the PR as well;
  • so does firmware.yml's Arduino CLI WAP size check, on the same sketch
    image the release signs and under the same env key the release
    measures it by (canary-wap-default): its old inline stat copy held the
    slot as a literal and had no floor;
  • every OTA product the release signs (the variant index and display loop
    check_ota_channels.py reads out of firmware-release.yml) has a budget in
    the committed firmware/flavors.json, except the ones declared in
    UNBUDGETED with a reason. A new published product without a budget
    fails here, not on the boards.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "firmware" / "scripts"


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


csb = _load("check_slot_budget")
sys.path.insert(0, str(SCRIPTS))  # check_ota_channels imports flavor_envs
coc = _load("check_ota_channels")
import flavor_envs  # noqa: E402  (on sys.path from the line above)

FIRMWARE_YML = REPO / ".github" / "workflows" / "firmware.yml"
BUILD_STEP = "Build this leg's environments (+ OTA-slot size guard)"

# The `--env` firmware-release.yml passes for the products whose manifest
# name is not their env name (its assemble step: the flagship's release_ha,
# the WAP's PR-CI env, and the default board/flavor of the vision and sense
# loops). Every other product's env IS its name.
ENV_FOR_PRODUCT = {
    "canary": "release_ha",
    "canary-wap": "canary-wap-default",
    "canary-vision": "canary-vision-default",
    "canary-sense": "canary-sense-default",
}

# Published, deliberately without a budget. Each needs a reason; the release
# prints a ::warning:: for each one every time it signs.
UNBUDGETED = {
    "canary-display-dash":
        "released from the arduino-cli build with PartitionScheme=huge_app "
        "(one 0x300000 app slot, no second slot), and its factory image lays "
        "that table down, while the PlatformIO env says default_16MB.csv "
        "(0x640000). No single budget is true until the two agree.",
    "canary-display-dash-modes":
        "same build and factory table as canary-display-dash.",
}


def guard(env: str, slot: int) -> dict:
    return {"bin": f".pio/build/{env}/firmware.bin", "slot_bytes": slot,
            "slot_name": "test slot"}


class CheckSlotBudget(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        flavors = self.dir / "flavors.json"
        flavors.write_text(json.dumps([{"name": "p", "size_guards": [
            guard("guarded", 100)]}]), encoding="utf-8")
        self._real = csb.FLAVORS
        csb.FLAVORS = flavors

    def tearDown(self) -> None:
        csb.FLAVORS = self._real
        self.tmp.cleanup()

    def image(self, size: int) -> str:
        path = self.dir / "fw.bin"
        path.write_bytes(b"\0" * size)
        return str(path)

    def run_main(self, *argv: str) -> str:
        out = io.StringIO()
        with redirect_stdout(out):
            csb.main(list(argv))
        return out.getvalue()

    def test_unbudgeted_env_is_a_warning_annotation_not_a_failure(self):
        out = self.run_main("--env", "unguarded", self.image(10_000))
        self.assertTrue(out.startswith("::warning::"), out)
        self.assertIn("'unguarded'", out)
        self.assertIn("NOT measured", out)

    def test_image_at_its_budget_passes(self):
        # Passes (no SystemExit) and is measured (not the unbudgeted
        # warning). An image that fills its slot leaves 0 bytes free, under
        # the headroom floor, so since F367 it also carries the floor's
        # warning; it used to print no annotation at all.
        out = self.run_main("--env", "guarded", self.image(100))
        self.assertIn("Firmware size: 100 / 100 bytes", out)
        self.assertNotIn("NOT measured", out)
        self.assertIn("::warning::check_slot_budget: env 'guarded' fits its "
                      "test slot with only 0 bytes free", out)

    def test_one_byte_over_fails(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_main("--env", "guarded", self.image(101))
        self.assertIn("is 101 bytes but", str(cm.exception.code))

    def test_missing_image_fails(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_main("--env", "guarded", str(self.dir / "absent.bin"))
        self.assertIn("not found", str(cm.exception.code))


# F367's floor, computed here rather than read from the script so that the
# boundary tests below fail on a script without one by what it prints (no
# warning), and held to the script's own constant and helper in
# test_the_floor_is_five_percent_rounded_up_to_a_whole_byte.
FLOOR_PERCENT = 5


def expected_floor(slot: int) -> int:
    """The fewest free bytes with free * 100 >= slot * FLOOR_PERCENT."""
    return (slot * FLOOR_PERCENT + 99) // 100


def sparse_image(directory: Path, size: int, name: str = "fw.bin") -> str:
    """A file whose st_size is `size` without writing `size` bytes."""
    path = directory / name
    with path.open("wb") as fh:
        fh.truncate(size)
    return str(path)


def run_check(argv: list[str]) -> tuple[int, str]:
    """main()'s exit status and stdout, the way a shell caller sees them."""
    out = io.StringIO()
    code = 0
    try:
        with redirect_stdout(out):
            csb.main(argv)
    except SystemExit as stop:
        code = 0 if stop.code in (None, 0) else 1
    return code, out.getvalue()


def warning_lines(out: str) -> list[str]:
    return [line for line in out.splitlines() if line.startswith("::warning::")]


class HeadroomFloor(unittest.TestCase):
    """The floor's boundaries, exact in integers, and an exit status that
    does not move: at every size the verdict is c2741d9f's `size <= slot`."""

    # 100: 5% is 5 bytes exactly. 101: 5% is 5.05, so 6 free bytes clear it
    # and 5 do not. 2,031,616 is the C6 nightstand's real slot (101,580.8).
    SLOTS = (100, 101, 2_031_616)

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        flavors = self.dir / "flavors.json"
        flavors.write_text(json.dumps([{"name": "p", "size_guards": [
            guard(f"slot{slot}", slot) for slot in self.SLOTS]}]),
            encoding="utf-8")
        self._real = csb.FLAVORS
        csb.FLAVORS = flavors

    def tearDown(self) -> None:
        csb.FLAVORS = self._real
        self.tmp.cleanup()

    def verdict(self, slot: int, size: int, *extra: str) -> tuple[int, str]:
        return run_check([*extra, "--env", f"slot{slot}",
                          sparse_image(self.dir, size)])

    def test_the_floor_is_five_percent_rounded_up_to_a_whole_byte(self):
        self.assertEqual(csb.HEADROOM_FLOOR_PERCENT, FLOOR_PERCENT)
        self.assertEqual(expected_floor(100), 5)
        self.assertEqual(expected_floor(101), 6)
        self.assertEqual(expected_floor(1_966_080), 98_304)    # exact
        self.assertEqual(expected_floor(2_031_616), 101_581)   # 101,580.8 up
        for slot in (*self.SLOTS, 1, 19, 20, 21, 1_966_080, 3_342_336,
                     6_553_600, 2**40 + 7, 2**64 + 1):
            floor = csb.floor_bytes(slot)
            self.assertEqual(floor, expected_floor(slot), slot)
            # the fewest free bytes with free * 100 >= slot * 5, in integers
            self.assertGreaterEqual(floor * 100, slot * 5, slot)
            self.assertLess((floor - 1) * 100, slot * 5, slot)

    def test_every_boundary(self):
        for slot in self.SLOTS:
            floor = expected_floor(slot)
            cases = {
                "slot+1 fails": (slot + 1, 1, False),
                "exactly the slot passes, warns": (slot, 0, True),
                "slot-1 passes, warns": (slot - 1, 0, True),
                "one byte under the floor warns": (slot - floor + 1, 0, True),
                "exactly at the floor is quiet": (slot - floor, 0, False),
                "well clear is quiet": (slot // 2, 0, False),
                "0 bytes passes, quiet (as on c2741d9f)": (0, 0, False),
            }
            for label, (size, code, warns) in cases.items():
                with self.subTest(slot=slot, case=label, size=size):
                    got_code, out = self.verdict(slot, size)
                    self.assertEqual(got_code, code, out)
                    # the exit status is the pre-floor rule, untouched
                    self.assertEqual(got_code == 0, size <= slot)
                    warned = warning_lines(out)
                    self.assertEqual(bool(warned), warns, out)
                    if warns:
                        self.assertEqual(len(warned), 1, out)
                        line = warned[0]
                        self.assertIn(f"env 'slot{slot}'", line)
                        self.assertIn(f"only {slot - size} bytes free", line)
                        self.assertIn(f"headroom floor of {floor} bytes", line)
                        self.assertIn(f"({size} of {slot})", line)

    def test_exactly_five_percent_free_is_quiet_and_one_byte_less_warns(self):
        # slot 100: 5 free bytes is exactly 5%
        self.assertEqual(warning_lines(self.verdict(100, 95)[1]), [])
        self.assertEqual(len(warning_lines(self.verdict(100, 96)[1])), 1)

    def test_require_budget_changes_nothing_for_a_budgeted_env(self):
        # the PR gate passes --require-budget, the release's display loop
        # does not: for an env with a budget the two calls are the same call
        for slot in self.SLOTS:
            floor = expected_floor(slot)
            for size in (0, slot // 2, slot - floor, slot - floor + 1,
                         slot - 1, slot, slot + 1):
                with self.subTest(slot=slot, size=size):
                    self.assertEqual(self.verdict(slot, size),
                                     self.verdict(slot, size,
                                                  "--require-budget"))

    def test_require_budget_fails_an_env_with_no_budget(self):
        with self.assertRaises(SystemExit) as cm, \
                redirect_stdout(io.StringIO()):
            csb.main(["--require-budget", "--env", "ghost",
                      sparse_image(self.dir, 10)])
        self.assertIn("'ghost'", str(cm.exception.code))
        self.assertIn("NOT measured", str(cm.exception.code))
        # without the flag it is the release path's warning and a pass
        code, out = run_check(["--env", "ghost", sparse_image(self.dir, 10)])
        self.assertEqual(code, 0)
        self.assertIn("NOT measured", out)


class Fw2416Images(unittest.TestCase):
    """The sizes F367 was filed on (the fw-v2.4.16 release assets), against
    the committed firmware/flavors.json budgets."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def check(self, env: str, size: int) -> tuple[int, list[str]]:
        code, out = run_check(["--env", env, sparse_image(self.dir, size)])
        return code, warning_lines(out)

    def test_the_c6_nightstand_warns_and_still_passes(self):
        code, warned = self.check("canary-display-nightstand-c6", 2_028_784)
        self.assertEqual(code, 0)
        self.assertEqual(len(warned), 1, warned)
        self.assertIn("'canary-display-nightstand-c6'", warned[0])
        self.assertIn("only 2832 bytes free", warned[0])
        self.assertIn("floor of 101581 bytes", warned[0])

    def test_the_sense_images_stay_quiet(self):
        for env, size in (("canary-sense-default", 1_821_632),
                          ("canary-sense-wellbeing", 1_825_472)):
            with self.subTest(env=env):
                self.assertEqual(self.check(env, size), (0, []))


def build_step_script() -> str:
    """firmware.yml's build-and-guard step, the bash GitHub runs."""
    workflow = yaml.safe_load(FIRMWARE_YML.read_text(encoding="utf-8"))
    for step in workflow["jobs"]["build-platformio"]["steps"]:
        if step.get("name") == BUILD_STEP:
            return step["run"]
    raise AssertionError(f"no step named {BUILD_STEP!r} in {FIRMWARE_YML}")


def commands(script: str) -> str:
    """A `run:` block's commands: comment lines dropped and backslash
    continuations joined, so a pin on the call cannot be met by a comment
    that merely names a flag."""
    kept = [line for line in script.splitlines()
            if not line.lstrip().startswith("#")]
    return re.sub(r"[ \t]*\\\n\s*", " ", "\n".join(kept))


STUB_PIO = """#!/usr/bin/env bash
# `pio run -e <env>`: lay down .pio/build/<env>/firmware.bin at the size
# $STUB_SIZES names for it (default 1000), or none for "missing".
set -euo pipefail
[ "$1" = run ] && [ "$2" = -e ] || { echo "stub pio: $*" >&2; exit 2; }
size=$(jq -r --arg e "$3" '.[$e] // 1000' <<< "$STUB_SIZES")
mkdir -p ".pio/build/$3"
[ "$size" = missing ] || truncate -s "$size" ".pio/build/$3/firmware.bin"
echo "stub pio: built $3 ($size)"
"""


@unittest.skipUnless(shutil.which("bash") and shutil.which("jq")
                     and shutil.which("truncate"),
                     "the PR gate's step is bash + jq, as on the runner")
class PrGateRunsTheScript(unittest.TestCase):
    """firmware.yml's size_guard, run as the workflow runs it, with a stub
    `pio` laying down images of chosen sizes. GITHUB_WORKSPACE is this
    checkout, so the step calls the committed script against the committed
    budgets."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.script = build_step_script()
        cls.legs = flavor_envs.build_matrix(flavor_envs.load_flavors())
        cls.budgets = csb.load_budgets()

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        bindir = self.root / "bin"
        bindir.mkdir()
        (bindir / "pio").write_text(STUB_PIO, encoding="utf-8")
        (bindir / "pio").chmod(0o755)
        self.path = f"{bindir}{os.pathsep}{os.environ.get('PATH', '')}"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_leg(self, leg: dict, sizes: dict) -> subprocess.CompletedProcess:
        work = self.root / "leg"
        if work.exists():
            shutil.rmtree(work)
        work.mkdir()
        env = {"PATH": self.path, "HOME": str(self.root),
               "GITHUB_WORKSPACE": str(REPO),
               "FLAVOR_JSON": json.dumps(leg),
               "STUB_SIZES": json.dumps(sizes)}
        # GitHub's default `run` shell for bash
        return subprocess.run(
            ["bash", "--noprofile", "--norc", "-eo", "pipefail", "-c",
             self.script], cwd=work, env=env, capture_output=True,
            text=True, timeout=300, check=False)

    def guarded(self, leg: dict) -> dict[str, dict]:
        return {flavor_envs.guard_env(g): g for g in leg.get("size_guards") or []}

    def test_the_step_is_the_scripts_not_an_inline_copy(self):
        code = commands(self.script)
        self.assertIn('python3 "$GITHUB_WORKSPACE/firmware/scripts/'
                      'check_slot_budget.py" --env "$GENV" --require-budget '
                      '"$BIN"', code)
        self.assertNotIn("stat -c", code)
        self.assertNotIn("slot_bytes", code)

    def test_every_guard_of_every_leg_reaches_the_script(self):
        self.assertTrue(any(self.guarded(leg) for leg in self.legs))
        for leg in self.legs:
            guards = self.guarded(leg)
            if not guards:
                continue
            # each guarded env one byte under its floor: every guard must
            # print its own size line against its own slot and warn
            sizes = {}
            for env, g in guards.items():
                slot = int(g["slot_bytes"])
                self.assertEqual(self.budgets[env], g, env)
                sizes[env] = slot - expected_floor(slot) + 1
            with self.subTest(leg=leg["leg"]):
                got = self.run_leg(leg, sizes)
                self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
                for env, g in guards.items():
                    slot = int(g["slot_bytes"])
                    self.assertIn(f"Firmware size: {sizes[env]} / {slot} bytes",
                                  got.stdout)
                    self.assertIn(f"::warning::check_slot_budget: env '{env}' "
                                  f"fits its", got.stdout)
                self.assertEqual(got.stdout.count("Firmware size: "),
                                 len(guards), got.stdout)

    def test_clear_images_are_quiet(self):
        leg = next(leg for leg in self.legs if self.guarded(leg))
        sizes = {env: int(g["slot_bytes"]) - expected_floor(int(g["slot_bytes"]))
                 for env, g in self.guarded(leg).items()}
        got = self.run_leg(leg, sizes)
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertNotIn("::warning::", got.stdout)
        self.assertNotIn("::error::", got.stdout)

    def refusal(self, leg: dict, sizes: dict) -> str:
        got = self.run_leg(leg, sizes)
        self.assertNotEqual(got.returncode, 0, got.stdout + got.stderr)
        errors = [line for line in got.stdout.splitlines()
                  if line.startswith("::error::")]
        self.assertEqual(len(errors), 1, got.stdout + got.stderr)
        return errors[0]

    def one_guarded_leg(self) -> tuple[dict, str, dict]:
        leg = next(leg for leg in self.legs if self.guarded(leg))
        env, g = next(iter(self.guarded(leg).items()))
        return leg, env, g

    def test_an_image_over_its_slot_fails_the_step(self):
        leg, env, g = self.one_guarded_leg()
        slot = int(g["slot_bytes"])
        line = self.refusal(leg, {env: slot + 1})
        self.assertIn(f"is {slot + 1} bytes but", line)
        self.assertIn(g["bin"], line)

    def test_an_image_exactly_at_its_slot_passes_the_step(self):
        leg, env, g = self.one_guarded_leg()
        got = self.run_leg(leg, {env: int(g["slot_bytes"])})
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)

    def test_an_absent_or_empty_image_fails_the_step(self):
        leg, env, g = self.one_guarded_leg()
        for size in ("missing", 0):
            with self.subTest(size=size):
                line = self.refusal(leg, {env: size})
                self.assertEqual(line, f"::error::{g['bin']} not found — "
                                       f"size guard cannot run.")

    def test_a_guard_the_script_has_no_budget_for_fails_the_step(self):
        leg = {"leg": "ghost", "build_envs": ["ghost-env"],
               "size_guards": [guard("ghost-env", 1000)]}
        line = self.refusal(leg, {"ghost-env": 10})
        self.assertIn("'ghost-env'", line)
        self.assertIn("NOT measured", line)


WAP_JOB = "build-arduino-cli"
WAP_COMPILE_STEP = "Compile firmware"
WAP_SIZE_STEP = "Check binary size against the OTA slot"
WAP_DIR = "/tmp/wap-build"
WAP_BIN = f"{WAP_DIR}/canary_wap.ino.bin"
FIRMWARE_RELEASE_YML = REPO / ".github" / "workflows" / "firmware-release.yml"


def wap_steps() -> dict[str, str]:
    workflow = yaml.safe_load(FIRMWARE_YML.read_text(encoding="utf-8"))
    return {step["name"]: step.get("run", "")
            for step in workflow["jobs"][WAP_JOB]["steps"] if "name" in step}


def release_wap_env() -> str:
    """The --env firmware-release.yml measures the WAP sketch image under:
    the staged copy of WAP_BIN, then the check on that copy."""
    text = commands(FIRMWARE_RELEASE_YML.read_text(encoding="utf-8"))
    staged = re.search(re.escape(f"cp {WAP_BIN} ") + r'"([^"]+)"', text)
    assert staged, f"firmware-release.yml no longer stages {WAP_BIN}"
    call = re.search(r"check_slot_budget\.py --env (\S+) "
                     + re.escape(f'"{staged.group(1)}"'), text)
    assert call, "firmware-release.yml no longer measures the WAP image"
    return call.group(1)


@unittest.skipUnless(shutil.which("bash") and shutil.which("truncate"),
                     "the WAP size check is bash, as on the runner")
class WapArduinoStepRunsTheScript(unittest.TestCase):
    """firmware.yml's Arduino CLI WAP job measures the sketch image the
    release signs. Its size check is run here as the workflow runs it, with
    the image's directory pointed at a scratch dir; GITHUB_WORKSPACE is this
    checkout, so it calls the committed script against the committed
    budget."""

    @classmethod
    def setUpClass(cls) -> None:
        steps = wap_steps()
        cls.compile = steps[WAP_COMPILE_STEP]
        cls.script = steps[WAP_SIZE_STEP]
        cls.env = release_wap_env()
        cls.guard = csb.load_budgets()[cls.env]
        cls.slot = int(cls.guard["slot_bytes"])

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.wap = self.root / "wap-build"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_step(self, size, workspace: Path = REPO) -> subprocess.CompletedProcess:
        if self.wap.exists():
            shutil.rmtree(self.wap)
        self.wap.mkdir()
        if size != "missing":
            sparse_image(self.wap, size, "canary_wap.ino.bin")
        # the step names the arduino-cli output dir literally; point it here
        self.assertIn(WAP_BIN, self.script)
        script = self.script.replace(WAP_DIR, str(self.wap))
        env = {"PATH": os.environ.get("PATH", ""), "HOME": str(self.root),
               "GITHUB_WORKSPACE": str(workspace)}
        return subprocess.run(
            ["bash", "--noprofile", "--norc", "-eo", "pipefail", "-c", script],
            cwd=self.root, env=env, capture_output=True, text=True,
            timeout=120, check=False)

    def lines(self, got: subprocess.CompletedProcess, kind: str) -> list[str]:
        return [line for line in got.stdout.splitlines()
                if line.startswith(f"::{kind}::")]

    def test_the_step_measures_the_compiled_image_under_the_releases_env(self):
        self.assertEqual(self.env, "canary-wap-default")
        self.assertIn(f"--output-dir {WAP_DIR}", commands(self.compile))
        code = commands(self.script)
        self.assertIn('python3 "$GITHUB_WORKSPACE/firmware/scripts/'
                      f'check_slot_budget.py" --env {self.env} '
                      '--require-budget "$BIN"', code)
        self.assertIn(f"BIN={WAP_BIN}", code)
        # the budget is flavors.json's, not a literal in the workflow
        self.assertNotIn("stat -c", code)
        self.assertNotIn(str(self.slot), code)

    def test_a_clear_image_is_quiet(self):
        size = self.slot - expected_floor(self.slot)
        got = self.run_step(size)
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertIn(f"Firmware size: {size} / {self.slot} bytes", got.stdout)
        self.assertEqual(self.lines(got, "warning"), [], got.stdout)
        self.assertEqual(self.lines(got, "error"), [], got.stdout)

    def test_an_image_under_the_floor_warns_and_passes(self):
        for size in (self.slot - expected_floor(self.slot) + 1, self.slot - 1,
                     self.slot):
            with self.subTest(size=size):
                got = self.run_step(size)
                self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
                warned = self.lines(got, "warning")
                self.assertEqual(len(warned), 1, got.stdout)
                self.assertIn(f"Firmware size: {size} / {self.slot} bytes",
                              got.stdout)
                self.assertIn(f"env '{self.env}'", warned[0])
                self.assertIn(f"only {self.slot - size} bytes free", warned[0])
                self.assertIn(f"floor of {expected_floor(self.slot)} bytes",
                              warned[0])

    def test_an_image_over_its_slot_fails_the_step(self):
        got = self.run_step(self.slot + 1)
        self.assertNotEqual(got.returncode, 0, got.stdout + got.stderr)
        errors = self.lines(got, "error")
        self.assertEqual(len(errors), 1, got.stdout + got.stderr)
        self.assertIn(f"is {self.slot + 1} bytes but", errors[0])
        self.assertNotIn("Firmware size:", got.stdout)

    def test_an_absent_or_empty_image_fails_the_step_as_before(self):
        for size in ("missing", 0):
            with self.subTest(size=size):
                got = self.run_step(size)
                self.assertNotEqual(got.returncode, 0, got.stdout + got.stderr)
                self.assertEqual(self.lines(got, "error"),
                                 [f"::error::canary_wap.ino.bin not found in "
                                  f"{self.wap}"], got.stdout)

    def test_the_step_fails_if_the_wap_budget_goes_missing(self):
        # a workspace whose flavors.json lost the WAP guard: the step must
        # refuse, not pass an image it never measured
        ws = self.root / "ws"
        (ws / "firmware" / "scripts").mkdir(parents=True)
        shutil.copy(SCRIPTS / "check_slot_budget.py",
                    ws / "firmware" / "scripts" / "check_slot_budget.py")
        flavors = json.loads(csb.FLAVORS.read_text(encoding="utf-8"))
        for flavor in flavors:
            flavor["size_guards"] = [
                g for g in flavor.get("size_guards", [])
                if flavor_envs.guard_env(g) != self.env]
        (ws / "firmware" / "flavors.json").write_text(json.dumps(flavors),
                                                      encoding="utf-8")
        got = self.run_step(1000, workspace=ws)
        self.assertNotEqual(got.returncode, 0, got.stdout + got.stderr)
        errors = self.lines(got, "error")
        self.assertEqual(len(errors), 1, got.stdout + got.stderr)
        self.assertIn(f"'{self.env}'", errors[0])
        self.assertIn("NOT measured", errors[0])


class EveryPublishedProductIsBudgeted(unittest.TestCase):
    def setUp(self) -> None:
        self.published = {}
        for name in coc.published_manifests():
            product = name[len("manifest-"):-len(".json")]
            self.published[product] = ENV_FOR_PRODUCT.get(product, product)
        self.budgets = csb.load_budgets()

    def test_the_published_set_is_read_not_empty(self):
        # a resolver that found nothing would make the next test vacuous
        self.assertGreaterEqual(len(self.published), 17, self.published)

    def test_every_published_env_has_a_budget_or_a_declared_reason(self):
        missing = sorted(env for product, env in self.published.items()
                         if env not in self.budgets and product not in UNBUDGETED)
        self.assertEqual(missing, [],
                         "these published OTA products have no size_guards "
                         "budget in firmware/flavors.json, so the release "
                         "signs them unmeasured — add one (the slot of the "
                         "table the fielded boards carry), or declare it in "
                         "UNBUDGETED with the reason")

    def test_declared_exceptions_are_still_published_and_still_unbudgeted(self):
        # keep the exception list honest: drop an entry once it has a budget
        # or stops shipping
        for product in UNBUDGETED:
            self.assertIn(product, self.published)
            self.assertNotIn(self.published[product], self.budgets,
                             f"{product} now has a budget — remove it from "
                             f"UNBUDGETED")


if __name__ == "__main__":
    unittest.main()
