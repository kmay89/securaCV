#!/usr/bin/env python3
"""An edit to any header a host suite compiles rebuilds that suite.

Both host-test Makefiles, firmware/tests_host and
firmware/projects/canary-wap/tests_host, once named each suite's headers by
hand. firmware/tests_host built test_vision_core_bindings from the five
sources build.sh's Vision flavor hands em++, and its list missed the box
pipeline the bindings include, so after a pipeline edit `make` rebuilt
test_vision_detection_pipeline and reran a stale test_vision_core_bindings
that still passed (sweep F249). Fourteen more suites there missed headers
they compile, the canary-wap Bluetooth suite missed a dozen, most canary-wap
suites named no header at all, and an edit to arduino_globals.h, which every
canary-wap compile force-includes (F229), rebuilt four suites (sweep F286).
CI builds fresh and never saw any of it.

Now every compile writes its own dependency file (-MMD -MP, and -MM after a
command that compiles several sources: the Makefiles' DEPFLAGS and
with_deps), and each Makefile reads them back. This test holds that to the
two tools that decide, not to a list kept by hand, for every compile of
both Makefiles' default goal:

  * make, for the plan: every command the default goal runs (`make -n -B
    --trace`), and the target whose recipe runs it;
  * the build: every one of those targets is built, as `make` builds it, in
    a scratch copy of the tests_host directory (the directory copied, every
    file around it linked, so the Makefile's relative paths still reach the
    sketch and common/), so the dependency files are the Makefile's own;
  * the compiler, for every file the compile reads: the same command, its
    output and dependency-file flags dropped, with -MM;
  * make again, for what it knows after the build: every target is up to
    date (`make -q`), and the prerequisites make's database holds for it
    (`make -p`), the dependency files included, name every file the
    compile read (compared by real path, as one file can be spelled two
    ways).

A target make always remakes (a .PHONY one, such as the tsan builds and the
syntax checks) cannot run stale and is passed over.

The self-test reads the same build through each Makefile with its
`-include` line taken out, which leaves only the hand lists, and requires the
check to name the Vision pipeline headers for the bindings suite (F249) and
the headers BT_CMD_DEPS once missed for the Bluetooth suite (F286): a check
that stopped seeing a missing dependency is a red test, not a silent hole.

Run on its own, it takes a Makefile to read instead of each tree's own
(`--makefile <tree>=<path>`), and prints what it finds:
`python3 scripts/tests/test_vision_core_make_deps.py --report`.
MAKE_DEPS_JOBS sets the build's -j (default: the CPU count, at most 4).
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FIRMWARE_HOST = "firmware/tests_host"
WAP_HOST = "firmware/projects/canary-wap/tests_host"
TREES = (FIRMWARE_HOST, WAP_HOST)

VISCORE = "build/test_vision_core_bindings"
PIPELINE = (
    "firmware/projects/canary-vision/include/canary/vision/detection_pipeline.h",
    "firmware/projects/canary-vision/include/canary/vision/optical_features.h",
)
BT_SUITE = "test_bluetooth_commands_wap"
# Headers the Bluetooth suite compiles that BT_CMD_DEPS never listed (F286).
BT_UNLISTED = tuple(
    "firmware/projects/canary-wap/arduino/canary_wap/" + h
    for h in ("ble_console.h", "ble_log_export.h", "ble_ota.h", "ble_presence.h",
              "ble_provision.h", "ble_witness_export.h", "build_config.h", "health_log.h",
              "log_level.h", "loop_snapshot.h", "nvs_store.h", "ota_release_key.h")
) + ("firmware/projects/canary-wap/tests_host/arduino_globals.h",)

# The compiler names make's plan is read with: every $(CXX) and $(CC)
# compile starts with one, whatever compiler is configured.
SENTINEL_CXX = "__make_deps_cxx__"
SENTINEL_CC = "__make_deps_cc__"
# A recipe that names a compiler itself.
COMPILER = re.compile(r"^(?:\S*/)?(?:[\w.]+-)?(?:g\+\+|clang\+\+|c\+\+|gcc|clang|cc)(?:-[\d.]+)?$")
SOURCE = re.compile(r"\.(?:cpp|cc|cxx|c)$")
TRACE = re.compile(r"^(?P<loc>[^\s:][^:]*:\d+): (?:update )?target '(?P<target>[^']*)'")
DEP_FLAGS = {"-MD", "-MMD", "-MP", "-MG"}
DEP_FLAGS_WITH_ARG = {"-MF", "-MT", "-MQ"}
DEP_LISTING = {"-M", "-MM"}
REDIRECT = re.compile(r"^(?:&>>?|[<>][<>&|]*)$")
RUNNERS = {"env", "exec", "command", "time"}
ASSIGNMENT = re.compile(r"^[A-Za-z_]\w*=")


class CheckError(Exception):
    pass


def make_env() -> dict[str, str]:
    env = {k: v for k, v in os.environ.items()
           if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES", "GNUMAKEFLAGS")}
    env["LC_ALL"] = "C"
    return env


def compilers() -> dict[str, list[str]]:
    """What each sentinel stands for: the compiler make itself would run."""
    return {SENTINEL_CXX: shlex.split(os.environ.get("CXX") or "g++"),
            SENTINEL_CC: shlex.split(os.environ.get("CC") or "cc")}


def jobs() -> int:
    try:
        return max(1, int(os.environ["MAKE_DEPS_JOBS"]))
    except (KeyError, ValueError):
        return max(1, min(4, os.cpu_count() or 1))


# ── the scratch tree ─────────────────────────────────────────────────────

def tree_files(tree: Path) -> list[str]:
    """The tree's files as git sees them: tracked, and untracked but not
    ignored (a new suite not yet added still counts); never a build output."""
    proc = subprocess.run(["git", "ls-files", "-co", "--exclude-standard", "-z", "--", "."],
                          cwd=tree, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise CheckError(f"git ls-files in {tree} failed:\n{proc.stderr}")
    return [f for f in proc.stdout.split("\0") if f and (tree / f).is_file()]


def shadow(rel: str, dest: Path) -> Path:
    """dest/<rel>: the tree's files copied (mtimes kept), and every entry
    beside the tree and beside each directory above it, up to the repo root,
    a symlink to the real one."""
    parts = Path(rel).parts
    src, dst = REPO, dest
    for part in parts:
        dst.mkdir(parents=True, exist_ok=True)
        for entry in src.iterdir():
            if entry.name != part:
                (dst / entry.name).symlink_to(entry)
        src, dst = src / part, dst / part
    dst.mkdir()
    for f in tree_files(src):
        target = dst / f
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src / f, target)
    return dst


# ── what make plans and knows ────────────────────────────────────────────

def make(cwd: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    proc = subprocess.run(["make", "--no-print-directory", *args], cwd=cwd,
                          capture_output=True, text=True, env=make_env(), check=False)
    if check and proc.returncode != 0:
        raise CheckError(f"make {' '.join(args)} failed ({proc.returncode}) in {cwd}:\n"
                         f"{proc.stdout[-3000:]}{proc.stderr[-3000:]}")
    return proc


def shell_commands(line: str) -> list[list[str]]:
    """Each command of one printed recipe line, split as the shell splits it,
    its redirections dropped and `env`, `VAR=value` and the like skipped."""
    lx = shlex.shlex(line, posix=True, punctuation_chars=True)
    lx.whitespace_split = True
    lx.commenters = ""
    toks = list(lx)
    segments: list[list[str]] = [[]]
    for t in toks:
        if t and not REDIRECT.match(t) and all(ch in "();<>|&" for ch in t):
            segments.append([])
        else:
            segments[-1].append(t)
    out = []
    for seg in segments:
        words: list[str] = []
        i = 0
        while i < len(seg):
            if REDIRECT.match(seg[i]):
                if words and words[-1].isdigit():
                    words.pop()
                i += 2
                continue
            words.append(seg[i])
            i += 1
        while words and (words[0] in RUNNERS or ASSIGNMENT.match(words[0])):
            if words.pop(0) == "env":
                while words and words[0].startswith("-"):
                    words.pop(0)
        if words:
            out.append(words)
    return out


@dataclass
class Compile:
    target: str                 # the make target whose recipe runs it
    argv: list[str]             # as make prints it, the sentinel first
    sources: list[str] = field(default_factory=list)
    flags: list[str] = field(default_factory=list)


def split_compile(target: str, argv: list[str]) -> Compile | None:
    """The compile's sources and preprocessor flags; None for a command that
    only lists dependencies (with_deps' -MM), which compiles nothing."""
    if any(a in DEP_LISTING for a in argv[1:]):
        return None
    c = Compile(target, argv)
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ("-o", *DEP_FLAGS_WITH_ARG) and i + 1 < len(argv):
            i += 2
            continue
        if a in DEP_FLAGS or a[:3] in DEP_FLAGS_WITH_ARG or a.startswith(("-l", "-L", "-Wl,")):
            i += 1
            continue
        if (SOURCE.search(a) or a == "/dev/null") and not a.startswith("-"):
            c.sources.append(a)
        else:
            c.flags.append(a)
        i += 1
    return c


def plan(cwd: Path, makefile: str) -> list[Compile]:
    """Every compile `make` runs for its default goal, and its target."""
    proc = make(cwd, "-f", makefile, "-n", "-B", "--trace",
                f"CXX={SENTINEL_CXX}", f"CC={SENTINEL_CC}")
    target = ""
    compiles: list[Compile] = []
    for line in proc.stdout.replace("\\\n", " ").splitlines():
        m = TRACE.match(line)
        if m:
            target = m["target"]
            continue
        if not line.strip():
            continue
        try:
            commands = shell_commands(line)
        except ValueError:
            if SENTINEL_CXX in line or SENTINEL_CC in line:
                raise CheckError(f"a recipe line with a compiler that cannot be split: {line}")
            continue
        for argv in commands:
            if argv[0] in (SENTINEL_CXX, SENTINEL_CC) or COMPILER.match(argv[0]):
                c = split_compile(target, argv)
                if c is not None:
                    if not target:
                        raise CheckError(f"a compile before any target: {line}")
                    compiles.append(c)
            elif any(SENTINEL_CXX in w or SENTINEL_CC in w for w in argv):
                raise CheckError(f"a command that names the compiler and is not a compile: {line}")
    if not compiles:
        raise CheckError(f"make -n -B in {cwd} printed no compile")
    return compiles


def database(cwd: Path, makefile: str) -> tuple[dict[str, set[str]], set[str]]:
    """make's rule database: each target's normal prerequisites (order-only
    ones never make a target out of date), and the .PHONY targets."""
    proc = make(cwd, "-f", makefile, "-p", "-q", check=False)
    lines = proc.stdout.splitlines()
    try:
        start = lines.index("# Files")
    except ValueError:
        raise CheckError(f"make -p in {cwd} printed no database:\n{proc.stderr[-2000:]}")
    prereqs: dict[str, set[str]] = {}
    phony: set[str] = set()
    for line in lines[start:]:
        if not line or line[0] in "#\t ":
            continue
        m = re.match(r"^(?P<t>[^:=]+?)::?(?![=:])(?P<rest>.*)$", line)
        if not m or "=" in m["rest"]:
            continue    # not a rule, or a target-specific variable (`t: VAR = x`)
        target = m["t"].strip()
        normal = m["rest"].split("|", 1)[0].split()
        if target == ".PHONY":
            phony.update(normal)
        prereqs.setdefault(target, set()).update(normal)
    return prereqs, phony


class Paths:
    """One file's name, wherever it is spelled: its real path, with the
    scratch tree's copies mapped back onto the repo's files."""

    def __init__(self, scratch: Path) -> None:
        self.scratch = os.path.realpath(scratch) + os.sep
        self.repo = os.path.realpath(REPO) + os.sep

    def canon(self, path: str | Path) -> str:
        real = os.path.realpath(path)
        if real.startswith(self.scratch):
            return self.repo + real[len(self.scratch):]
        return real


def closure(cwd: Path, c: Compile, paths: Paths) -> dict[str, str]:
    """{canonical path: spelling} of every file the compile reads, by -MM."""
    cc = compilers().get(c.argv[0], [c.argv[0]])
    sources = [s for s in c.sources if s != "/dev/null"]
    if not sources:
        return {}
    proc = subprocess.run(cc + c.flags + ["-MM"] + sources, cwd=cwd,
                          capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise CheckError(f"-MM failed for {c.target}:\n{' '.join(cc + c.flags + ['-MM'] + sources)}\n"
                         + proc.stderr[-2000:])
    found: dict[str, str] = {}
    for word in proc.stdout.replace("\\\n", " ").split():
        if word.endswith(":"):
            continue
        found.setdefault(paths.canon(cwd / word), word)
    return found


# ── the check ────────────────────────────────────────────────────────────

@dataclass
class Result:
    compiles: int = 0
    targets: int = 0
    files: int = 0
    # target -> files its compile reads that no prerequisite of it names
    missing: dict[str, list[str]] = field(default_factory=dict)
    stale: list[str] = field(default_factory=list)   # out of date right after the build
    closures: dict[str, dict[str, str]] = field(default_factory=dict)


def rel(path: str) -> str:
    try:
        return os.path.relpath(path, os.path.realpath(REPO))
    except ValueError:
        return path


def build(cwd: Path, makefile: str, targets: list[str]) -> None:
    make(cwd, "-f", makefile, f"-j{jobs()}", *targets)


def read(cwd: Path, makefile: str, compiles: list[Compile],
         closures: dict[int, dict[str, str]], paths: Paths) -> Result:
    """What make knows through `makefile`, against what each compile read."""
    prereqs, phony = database(cwd, makefile)
    result = Result()
    for i, c in enumerate(compiles):
        if c.target in phony:
            continue
        result.compiles += 1
        known = {paths.canon(cwd / p) for p in prereqs.get(c.target, ())}
        got = closures[i]
        result.closures.setdefault(c.target, {}).update(got)
        lost = sorted(rel(p) for p in got if p not in known)
        if lost:
            result.missing.setdefault(c.target, [])
            result.missing[c.target] = sorted(set(result.missing[c.target]) | set(lost))
    result.targets = len(result.closures)
    result.files = len({p for got in result.closures.values() for p in got})
    return result


def check_tree(rel_tree: str, scratch: Path, makefile_text: str | None = None,
               also: dict[str, str] | None = None) -> dict[str, Result]:
    """Build every compile target of the tree in a scratch copy, then read it
    through its Makefile (or makefile_text in its place) and through each
    extra Makefile text in `also` (no rebuild): {name: Result}."""
    cwd = shadow(rel_tree, scratch)
    paths = Paths(scratch)
    if makefile_text is not None:
        (cwd / "Makefile").write_text(makefile_text, encoding="utf-8")
    compiles = plan(cwd, "Makefile")
    _prereqs, phony = database(cwd, "Makefile")
    targets = sorted({c.target for c in compiles if c.target not in phony})
    build(cwd, "Makefile", targets)
    with ThreadPoolExecutor(max_workers=jobs()) as pool:
        got = list(pool.map(lambda c: closure(cwd, c, paths), compiles))
    closures = dict(enumerate(got))
    results = {"Makefile": read(cwd, "Makefile", compiles, closures, paths)}
    q = make(cwd, "-f", "Makefile", "-q", *targets, check=False)
    if q.returncode != 0:
        results["Makefile"].stale = [t for t in targets
                                     if make(cwd, "-f", "Makefile", "-q", t, check=False).returncode != 0]
    for name, text in (also or {}).items():
        (cwd / name).write_text(text, encoding="utf-8")
        results[name] = read(cwd, name, compiles, closures, paths)
    return results


def without_include(text: str) -> str:
    """The Makefile with the line that reads the dependency files back taken
    out: what make knows from the hand lists alone."""
    out, n = re.subn(r"^-include \$\(wildcard [^\n]*\.d\)\n", "", text, flags=re.M)
    if n != 1:
        raise CheckError(f"expected one `-include $(wildcard ....d)` line, found {n}")
    return out


@unittest.skipUnless(shutil.which("make") and shutil.which("g++") and shutil.which("cc")
                     and shutil.which("git"), "needs make, g++, cc and git")
class TestsHostMakeDeps(unittest.TestCase):
    results: dict[str, dict[str, Result]] = {}

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory(prefix="make-deps-")
        cls.results = {}
        for i, tree in enumerate(TREES):
            text = (REPO / tree / "Makefile").read_text(encoding="utf-8")
            cls.results[tree] = check_tree(tree, Path(cls._tmp.name) / str(i),
                                           also={"Makefile.hand_lists": without_include(text)})

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def test_every_header_a_suite_compiles_rebuilds_it(self) -> None:
        for tree in TREES:
            with self.subTest(tree=tree):
                r = self.results[tree]["Makefile"]
                self.assertGreaterEqual(r.targets, 60, f"{tree}: {r.targets} suites read")
                self.assertEqual(r.stale, [], f"{tree}: out of date right after the build")
                self.assertEqual(
                    r.missing, {},
                    f"{tree}: a suite is not rebuilt after an edit to these files its compile "
                    "reads (does its compile pass $(DEPFLAGS)? does a command that compiles "
                    "several sources run through $(call with_deps,...)?):\n" + "\n".join(
                        f"  {t}: {', '.join(fs)}" for t, fs in sorted(r.missing.items())))

    def test_vision_core_bindings_read_the_pipeline(self) -> None:
        r = self.results[FIRMWARE_HOST]["Makefile"]
        read_files = {rel(p) for p in r.closures[VISCORE]}
        for f in PIPELINE:
            self.assertIn(f, read_files, "the bindings include the box pipeline; -MM must see it")
        self.assertGreaterEqual(len(read_files), 15, sorted(read_files))
        self.assertNotIn(VISCORE, r.missing)

    def test_the_check_names_what_the_hand_lists_miss(self) -> None:
        hand = self.results[FIRMWARE_HOST]["Makefile.hand_lists"]
        for f in PIPELINE:
            self.assertIn(f, hand.missing.get(VISCORE, []),
                          "with the hand lists alone the check must name the pipeline headers")
        hand = self.results[WAP_HOST]["Makefile.hand_lists"]
        for f in BT_UNLISTED:
            self.assertIn(f, hand.missing.get(BT_SUITE, []),
                          "with the hand lists alone the check must name the Bluetooth headers")


def report(argv: list[str]) -> int:
    overrides = {}
    for a in argv:
        if a.startswith("--makefile="):
            tree, _, path = a[len("--makefile="):].partition("=")
            overrides[tree] = Path(path).read_text(encoding="utf-8")
    bad = 0
    with tempfile.TemporaryDirectory(prefix="make-deps-") as tmp:
        for i, tree in enumerate(TREES):
            r = check_tree(tree, Path(tmp) / str(i), overrides.get(tree))["Makefile"]
            print(f"{tree}: {r.compiles} compiles of {r.targets} suites read {r.files} files; "
                  f"{len(r.missing)} suites miss some, {len(r.stale)} out of date after the build")
            for t, fs in sorted(r.missing.items()):
                print(f"  {t}: {', '.join(fs)}")
            for t in r.stale:
                print(f"  stale: {t}")
            bad += len(r.missing) + len(r.stale)
    return 1 if bad else 0


if __name__ == "__main__":
    if "--report" in sys.argv:
        sys.exit(report(sys.argv[1:]))
    unittest.main()
