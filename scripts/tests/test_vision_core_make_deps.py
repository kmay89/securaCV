#!/usr/bin/env python3
"""An edit to any header the Vision core bindings suite reads rebuilds it.

firmware/tests_host/Makefile builds test_vision_core_bindings from the five
sources build.sh's Vision flavor hands em++. Its prerequisites once named the
presence FSM's headers, the config, the shim and the emscripten stub, but not
the box pipeline (detection_pipeline.h, optical_features.h) the bindings
include, nor version.h, detect_profiles.h or log.h. So after a pipeline edit
`make` rebuilt test_vision_detection_pipeline and reran a stale
test_vision_core_bindings that still passed (sweep F249). CI builds fresh and
never saw it.

This test asks the two tools that decide, not a list kept by hand:

  * the compiler, for every file the suite's own compile reads: make prints
    the recipe (`make -n -B`), and the same command with `-MM` in place of
    `-o <bin>` prints the closure;
  * make, for whether touching each of those files rebuilds the suite: with
    the binary newer than everything (`make -q` says it is up to date),
    `make -q -W <file>` (make's "what if this file had just changed") must
    say it is not. That is a touch without touching the tree.

The self-test runs the same check against the Makefile with VISCORE_HDR put
back to its old list and requires it to name the pipeline headers, so a check
that stopped seeing its own defect is a red test, not a silent hole.
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TESTS_HOST = REPO / "firmware" / "tests_host"
MAKEFILE = TESTS_HOST / "Makefile"
SUITE = "test_vision_core_bindings"

# VISCORE_HDR as it stood before F249: the FSM headers, the config, the shim
# and the stub, and none of the pipeline's.
OLD_VISCORE_HDR = (
    "VISCORE_HDR = $(VISFSM_HDR) $(VISION_CFG)/config.h $(wildcard $(EMU_DIR)/shim/*.h) \\\n"
    "              stubs/emscripten/emscripten.h\n"
)
PIPELINE = (
    "../projects/canary-vision/include/canary/vision/detection_pipeline.h",
    "../projects/canary-vision/include/canary/vision/optical_features.h",
)


def make(makefile: Path, build_dir: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["make", "--no-print-directory", "-C", str(TESTS_HOST), "-f", str(makefile),
         f"BUILD_DIR={build_dir}", *args],
        capture_output=True, text=True, check=False,
    )


def compile_command(makefile: Path, build_dir: Path) -> list[str]:
    """The command make runs to build the suite, as make prints it."""
    target = f"{build_dir}/{SUITE}"
    proc = make(makefile, build_dir, "-n", "-B", target)
    if proc.returncode != 0:
        raise AssertionError(f"make -n -B {target} failed:\n{proc.stderr}")
    lines = proc.stdout.replace("\\\n", " ").splitlines()
    hits = [ln for ln in lines if re.search(r"(^|\s)-o\s+" + re.escape(target) + r"(\s|$)", ln)]
    if len(hits) != 1:
        raise AssertionError(f"expected one command that writes {target}, found {len(hits)}:\n"
                             + proc.stdout)
    return shlex.split(hits[0])


def header_closure(cmd: list[str]) -> set[str]:
    """Every file the compile reads, by the compiler's own -MM, relative to tests_host."""
    argv = list(cmd)
    i = argv.index("-o")
    del argv[i:i + 2]
    argv.insert(1, "-MM")
    proc = subprocess.run(argv, cwd=TESTS_HOST, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise AssertionError(f"-MM failed:\n{' '.join(argv)}\n{proc.stderr}")
    words = proc.stdout.replace("\\\n", " ").split()
    return {os.path.normpath(w) for w in words if not w.endswith(":")}


def not_rebuilt_after_touch(makefile: Path, files: set[str]) -> list[str]:
    """Files whose touch leaves make content with a binary built before it."""
    with tempfile.TemporaryDirectory() as tmp:
        build_dir = Path(tmp)
        binary = build_dir / SUITE
        binary.write_bytes(b"")
        newest = max([makefile.stat().st_mtime, MAKEFILE.stat().st_mtime]
                     + [(TESTS_HOST / f).stat().st_mtime for f in files])
        os.utime(binary, (newest + 1, newest + 1))
        target = str(binary)
        settled = make(makefile, build_dir, "-q", target)
        if settled.returncode != 0:
            raise AssertionError(f"make -q {target} should be up to date before any touch "
                                 f"(exit {settled.returncode}):\n{settled.stdout}{settled.stderr}")
        missed = []
        for f in sorted(files):
            proc = make(makefile, build_dir, "-q", "-W", f, target)
            if proc.returncode == 0:
                missed.append(f)
            elif proc.returncode != 1:
                raise AssertionError(f"make -q -W {f} errored (exit {proc.returncode}):\n"
                                     + proc.stderr)
        return missed


def check(makefile: Path) -> tuple[set[str], list[str]]:
    with tempfile.TemporaryDirectory() as tmp:
        closure = header_closure(compile_command(makefile, Path(tmp)))
    return closure, not_rebuilt_after_touch(makefile, closure)


def old_makefile(dest: Path) -> Path:
    text = MAKEFILE.read_text(encoding="utf-8")
    pattern = re.compile(r"^VISCORE_HDR\s*=.*?(?<!\\)\n", re.M | re.S)
    found = pattern.findall(text)
    if len(found) != 1:
        raise AssertionError(f"expected one VISCORE_HDR assignment, found {len(found)}")
    path = dest / "Makefile.before_f249"
    path.write_text(pattern.sub(lambda _m: OLD_VISCORE_HDR, text), encoding="utf-8")
    return path


@unittest.skipUnless(shutil.which("make") and shutil.which("g++"), "needs make and g++")
class VisionCoreMakeDeps(unittest.TestCase):
    def test_every_header_read_rebuilds_the_suite(self) -> None:
        closure, missed = check(MAKEFILE)
        for f in PIPELINE:
            self.assertIn(f, closure, "the bindings include the box pipeline; -MM must see it")
        self.assertGreaterEqual(len(closure), 15, sorted(closure))
        self.assertEqual(
            missed, [],
            f"{SUITE} is not rebuilt after an edit to these files it compiles "
            "(add them to VISCORE_HDR in firmware/tests_host/Makefile):\n  " + "\n  ".join(missed))

    def test_check_names_the_pipeline_on_the_old_list(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            _closure, missed = check(old_makefile(Path(tmp)))
        for f in PIPELINE:
            self.assertIn(f, missed, "the check must name a pipeline header the old list missed")


if __name__ == "__main__":
    unittest.main()
