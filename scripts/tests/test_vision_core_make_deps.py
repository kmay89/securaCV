#!/usr/bin/env python3
"""An edit to any header a host suite compiles rebuilds that suite.

Both host-test Makefiles, firmware/tests_host and
firmware/projects/canary-wap/tests_host, once named each suite's headers by
hand. firmware/tests_host built test_vision_core_bindings from the five
sources build.sh's Vision flavor hands em++, and its list missed the box
pipeline the bindings include, so after a pipeline edit `make` rebuilt
test_vision_detection_pipeline and reran a stale test_vision_core_bindings
that still passed (sweep F249). Fourteen more suites there missed headers
they compile, the canary-wap Bluetooth suite missed a dozen, 27 of the 87
canary-wap suites named no header at all, and an edit to arduino_globals.h,
which every canary-wap compile force-includes (F229), rebuilt only the five
suites that listed it (sweep F286). CI builds fresh and never saw any of it.

Now every compile writes its own dependency file (-MMD -MP, and -MM after a
command that compiles several sources: the Makefiles' DEPFLAGS and
with_deps), and each Makefile reads them back and gives every source they
name a rule, as -MP gives every header one. This test holds that to the two
tools that decide, not to a list kept by hand, for every compile of both
Makefiles:

  * make, for the plan: every command the default goal runs (`make -n -B
    --trace`), and the target whose recipe runs it; then the same for any
    other target with a recipe of its own that the default goal does not
    reach, so a suite not hooked into `run` is held too;
  * the plan alone: every compile of a non-phony target writes a dependency
    file, CXXFLAGS given on make's command line or not (`override`); one
    that compiles several sources is followed by a -M/-MM listing for its
    target; and no compile writes a dependency file it has no -o to name
    after (it would land in the tests_host directory);
  * the build: every one of those targets is built, as `make` builds it, in
    a scratch copy of the tests_host directory (the directory copied, every
    file around it linked, so the Makefile's relative paths still reach the
    sketch and common/), so the dependency files are the Makefile's own;
  * the compiler, for every file the compile reads: the same command, its
    output and dependency-file flags dropped, with -MM;
  * make again, for what it knows after the build: every target is up to
    date (`make -q`); the default goal is the one it was before any
    dependency file existed; the prerequisites make's database holds for
    each target (`make -p`), the dependency files included, name every file
    the compile read (compared by real path, as one file can be spelled two
    ways); and each of those files is itself the target of a rule, so one
    moved or deleted since its dependency file was written is a rebuild and
    not "No rule to make target".

A target make always remakes (a .PHONY one, such as the tsan builds and the
syntax checks) cannot run stale and is passed over, except that it too may
not write a dependency file it has no -o for.

The self-test reads the same build through each Makefile changed one line
at a time (MUTATIONS), and requires each check to refuse its change: with
the `-include` line taken out, which leaves only the hand lists, the Vision
pipeline headers for the bindings suite (F249) and the headers BT_CMD_DEPS
once missed for the Bluetooth suite (F286); with the source rules taken out,
the sources; with the `-include` above `all`, the default goal; without
`override`, the compiles that then write no dependency file; with with_deps
running the compile alone, the bindings suite; and with the syntax checks
keeping DEPFLAGS, their litter. A check that stopped seeing what it guards is
a red test, not a silent hole.

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
    depfile: bool = False       # writes a dependency file (-MD, -MMD)
    named: bool = False         # an -o or -MF names that file


def split_compile(target: str, argv: list[str]) -> Compile | None:
    """The compile's sources and preprocessor flags; None for a command that
    only lists dependencies (with_deps' -MM), which compiles nothing."""
    if any(a in DEP_LISTING for a in argv[1:]):
        return None
    c = Compile(target, argv)
    c.depfile = any(a in ("-MD", "-MMD") for a in argv[1:])
    c.named = any(a == "-o" or a.startswith("-MF") for a in argv[1:])
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


@dataclass
class Plan:
    compiles: list[Compile] = field(default_factory=list)
    traced: set[str] = field(default_factory=set)   # every target whose recipe runs
    listed: set[str] = field(default_factory=set)   # targets whose recipe runs a -M/-MM listing


def plan(cwd: Path, makefile: str, *args: str, require: bool = True) -> Plan:
    """Every compile `make` runs for its default goal (or for the targets and
    variables in args), and its target."""
    proc = make(cwd, "-f", makefile, "-n", "-B", "--trace",
                f"CXX={SENTINEL_CXX}", f"CC={SENTINEL_CC}", *args)
    target = ""
    out = Plan()
    for line in proc.stdout.replace("\\\n", " ").splitlines():
        m = TRACE.match(line)
        if m:
            target = m["target"]
            out.traced.add(target)
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
                if not target:
                    raise CheckError(f"a compile before any target: {line}")
                if c is None:
                    out.listed.add(target)
                else:
                    out.compiles.append(c)
            elif any(SENTINEL_CXX in w or SENTINEL_CC in w for w in argv):
                raise CheckError(f"a command that names the compiler and is not a compile: {line}")
    if require and not out.compiles:
        raise CheckError(f"make -n -B in {cwd} printed no compile")
    return out


@dataclass
class Db:
    """make's rule database (`make -p`)."""
    # each target's normal prerequisites (order-only ones never make a target
    # out of date)
    prereqs: dict[str, set[str]] = field(default_factory=dict)
    phony: set[str] = field(default_factory=set)
    targets: set[str] = field(default_factory=set)   # every file a rule names as its target
    recipes: set[str] = field(default_factory=set)   # the targets with a recipe of their own
    goal: str = ""                                   # .DEFAULT_GOAL


def database(cwd: Path, makefile: str) -> Db:
    """Each target's normal prerequisites, the .PHONY targets, every file a
    rule names as its target (not "# Not a target:", a file only named as a
    prerequisite, which make cannot remake and stops on when it is gone),
    the targets with a recipe of their own, and the default goal."""
    proc = make(cwd, "-f", makefile, "-p", "-q", check=False)
    lines = proc.stdout.splitlines()
    try:
        start = lines.index("# Files")
    except ValueError:
        raise CheckError(f"make -p in {cwd} printed no database:\n{proc.stderr[-2000:]}")
    db = Db()
    for line in lines[:start]:
        if line.startswith(".DEFAULT_GOAL := "):
            db.goal = line[len(".DEFAULT_GOAL := "):].strip()
    current = ""
    for prev, line in zip(lines[start - 1:], lines[start:]):
        if not line:
            current = ""
            continue
        if line[0] == "\t":
            if current:
                db.recipes.add(current)
            continue
        if line[0] in "# ":
            continue
        m = re.match(r"^(?P<t>[^:=]+?)::?(?![=:])(?P<rest>.*)$", line)
        if not m or "=" in m["rest"]:
            continue    # not a rule, or a target-specific variable (`t: VAR = x`)
        current = m["t"].strip()
        normal = m["rest"].split("|", 1)[0].split()
        if current == ".PHONY":
            db.phony.update(normal)
        db.prereqs.setdefault(current, set()).update(normal)
        if prev != "# Not a target:":
            db.targets.add(current)
    return db


def full_plan(cwd: Path, makefile: str, db: Db) -> Plan:
    """The default goal's plan, and then any other target with a recipe of
    its own that the default goal does not reach: a suite not hooked into
    `run` is held to the same rules."""
    p = plan(cwd, makefile)
    extra = sorted(t for t in db.recipes - p.traced - db.phony if not t.startswith("."))
    if extra:
        more = plan(cwd, makefile, *extra, require=False)
        have = {c.target for c in p.compiles}
        p.compiles += [c for c in more.compiles if c.target not in have]
        p.traced |= more.traced
        p.listed |= more.listed
    return p


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

# What a plan with CXXFLAGS given on make's command line passes, so that only
# an `override` keeps the Makefile's own additions to it.
CMDLINE_CXXFLAGS = "CXXFLAGS=-std=c++17"


@dataclass
class Result:
    compiles: int = 0
    targets: int = 0
    files: int = 0
    # target -> files its compile reads that no prerequisite of it names
    missing: dict[str, list[str]] = field(default_factory=dict)
    # target -> files its compile reads that no rule names as its target, so
    # make stops ("No rule to make target") once one is moved or deleted
    unruled: dict[str, list[str]] = field(default_factory=dict)
    stale: list[str] = field(default_factory=list)   # out of date right after the build
    nodeps: list[str] = field(default_factory=list)  # a compile that writes no dependency file
    unlisted: list[str] = field(default_factory=list)  # several sources and no -M/-MM listing
    litter: list[str] = field(default_factory=list)  # a dependency file with no -o to name it
    goal_before: str = ""                            # the default goal with no dependency file
    goal: str = ""                                   # ... and after the build
    closures: dict[str, dict[str, str]] = field(default_factory=dict)


def rel(path: str) -> str:
    try:
        return os.path.relpath(path, os.path.realpath(REPO))
    except ValueError:
        return path


def build(cwd: Path, makefile: str, targets: list[str]) -> None:
    make(cwd, "-f", makefile, f"-j{jobs()}", *targets)


def plan_faults(cwd: Path, makefile: str, db: Db, result: Result) -> None:
    """What the plan alone shows: each compile of a non-phony target writes
    a dependency file, CXXFLAGS on make's command line or not; one that
    compiles several sources is followed by a listing of its target's
    headers; and no compile writes a dependency file it has no -o for."""
    for args, how in (((), ""), ((CMDLINE_CXXFLAGS,), f" (with {CMDLINE_CXXFLAGS} given to make)")):
        p = full_plan(cwd, makefile, db) if not args else plan(cwd, makefile, *args)
        for c in p.compiles:
            real = [s for s in c.sources if s != "/dev/null"]
            if c.target not in db.phony and not c.depfile:
                result.nodeps.append(c.target + how)
            if c.target not in db.phony and len(real) > 1 and c.target not in p.listed and not how:
                result.unlisted.append(c.target)
            if c.depfile and not c.named:
                result.litter.append(f"{c.target}{how}: {' '.join(c.argv)}")
    for name in ("nodeps", "unlisted", "litter"):
        setattr(result, name, sorted(set(getattr(result, name))))


def read(cwd: Path, makefile: str, compiles: list[Compile],
         closures: dict[int, dict[str, str]], paths: Paths, goal_before: str) -> Result:
    """What make knows through `makefile`, against what each compile read."""
    db = database(cwd, makefile)
    result = Result(goal_before=goal_before, goal=db.goal)
    plan_faults(cwd, makefile, db, result)
    ruled = {paths.canon(cwd / t) for t in db.targets}
    for i, c in enumerate(compiles):
        if c.target in db.phony:
            continue
        result.compiles += 1
        known = {paths.canon(cwd / p) for p in db.prereqs.get(c.target, ())}
        got = closures[i]
        result.closures.setdefault(c.target, {}).update(got)
        for name, lost in (("missing", [p for p in got if p not in known]),
                           ("unruled", [p for p in got if p not in ruled])):
            if lost:
                table = getattr(result, name)
                table[c.target] = sorted(set(table.get(c.target, [])) | {rel(p) for p in lost})
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
    before = database(cwd, "Makefile")
    compiles = full_plan(cwd, "Makefile", before).compiles
    targets = sorted({c.target for c in compiles if c.target not in before.phony})
    build(cwd, "Makefile", targets)
    with ThreadPoolExecutor(max_workers=jobs()) as pool:
        got = list(pool.map(lambda c: closure(cwd, c, paths), compiles))
    closures = dict(enumerate(got))
    results = {"Makefile": read(cwd, "Makefile", compiles, closures, paths, before.goal)}
    q = make(cwd, "-f", "Makefile", "-q", *targets, check=False)
    if q.returncode != 0:
        results["Makefile"].stale = [t for t in targets
                                     if make(cwd, "-f", "Makefile", "-q", t, check=False).returncode != 0]
    for name, text in (also or {}).items():
        (cwd / name).write_text(text, encoding="utf-8")
        results[name] = read(cwd, name, compiles, closures, paths, before.goal)
    return results


# ── the self-test: Makefiles each check must refuse ─────────────────────
#
# Each is the tree's Makefile with one line changed, read against the same
# build (the dependency files are the real ones). `None` from a mutation
# means its line is not in this Makefile, and the self-test then says which
# line is gone rather than the whole class failing to set up.

INCLUDE_LINE = re.compile(r"^-include \$\(wildcard [^\n]*\.d\)\n", re.M)
SOURCE_RULE_LINE = re.compile(r"^\$\(filter %\.c %\.cpp,\$\(shell cat [^\n]*\.d\)[^\n]*\)\):\n", re.M)


def one(pattern: re.Pattern, text: str) -> re.Match | None:
    found = list(pattern.finditer(text))
    if len(found) > 1:
        raise CheckError(f"expected one line matching {pattern.pattern!r}, found {len(found)}")
    return found[0] if found else None


def without_include(text: str) -> str:
    """The Makefile with the line that reads the dependency files back taken
    out: what make knows from the hand lists alone. A Makefile that reads no
    dependency files (one from before sweep F286) is its own hand lists, so
    the main test names what it misses instead of the setup stopping here."""
    out, n = INCLUDE_LINE.subn("", text)
    if n > 1:
        raise CheckError(f"expected at most one `-include $(wildcard ....d)` line, found {n}")
    return out


def without_source_rules(text: str) -> str | None:
    """No rule for the sources the dependency files name."""
    m = one(SOURCE_RULE_LINE, text)
    return None if m is None else text[:m.start()] + text[m.end():]


def include_first(text: str) -> str | None:
    """The dependency files read in front of `all`, the first rule."""
    m = one(INCLUDE_LINE, text)
    if m is None or ".PHONY: all " not in text:
        return None
    rest = text[:m.start()] + text[m.end():]
    anchor = rest.index(".PHONY: all ")
    return rest[:anchor] + m.group(0) + rest[anchor:]


def no_override(text: str) -> str | None:
    """DEPFLAGS added to CXXFLAGS without `override`."""
    out, n = re.subn(r"^override CXXFLAGS \+= \$\(DEPFLAGS\)$", "CXXFLAGS += $(DEPFLAGS)", text, flags=re.M)
    return out if n == 1 else None


def no_listing(text: str) -> str | None:
    """with_deps runs the compile alone."""
    out, n = re.subn(r"^with_deps = .*$", "with_deps = $(1)", text, flags=re.M)
    return out if n == 1 else None


def syntax_checks_write_deps(text: str) -> str | None:
    """The syntax checks, which have no -o, keep DEPFLAGS."""
    out, n = re.subn(r"^run-syntax-checks: DEPFLAGS =\n", "", text, flags=re.M)
    return out if n == 1 else None


MUTATIONS = {
    "Makefile.hand_lists": without_include,
    "Makefile.no_source_rules": without_source_rules,
    "Makefile.include_first": include_first,
    "Makefile.no_override": no_override,
    "Makefile.no_listing": no_listing,
    "Makefile.syntax_deps": syntax_checks_write_deps,
}


def mutants(text: str) -> dict[str, str]:
    out = {}
    for name, mutate in MUTATIONS.items():
        mutated = mutate(text)
        if mutated is not None:
            out[name] = mutated
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
            cls.results[tree] = check_tree(tree, Path(cls._tmp.name) / str(i), also=mutants(text))

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

    def test_a_moved_header_or_source_is_a_rebuild(self) -> None:
        for tree in TREES:
            with self.subTest(tree=tree):
                r = self.results[tree]["Makefile"]
                self.assertEqual(
                    r.unruled, {},
                    f"{tree}: make stops with \"No rule to make target\" once one of these files "
                    "a suite's dependency file names is moved or deleted (does DEPFLAGS keep -MP? "
                    "does the line after the -include still give each source a rule?):\n" + "\n".join(
                        f"  {t}: {', '.join(fs)}" for t, fs in sorted(r.unruled.items())))

    def test_the_rules_every_compile_keeps(self) -> None:
        for tree in TREES:
            with self.subTest(tree=tree):
                r = self.results[tree]["Makefile"]
                self.assertEqual(r.goal, r.goal_before,
                                 f"{tree}: a rule from a dependency file became the default goal "
                                 "(is the -include still after `all`?)")
                self.assertEqual(r.nodeps, [], f"{tree}: these compiles write no dependency file "
                                 "(do they pass $(CXXFLAGS) or $(DEPFLAGS)? is it still `override`?)")
                self.assertEqual(r.unlisted, [], f"{tree}: these compile several sources and list "
                                 "no dependencies after (run them through $(call with_deps,...))")
                self.assertEqual(r.litter, [], f"{tree}: these write a dependency file into the "
                                 "directory, having no -o to name it after (set DEPFLAGS empty for them)")

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
            self.assertIn(f, hand.unruled.get(VISCORE, []),
                          "with no dependency files read, no rule names the pipeline headers")
        hand = self.results[WAP_HOST]["Makefile.hand_lists"]
        for f in BT_UNLISTED:
            self.assertIn(f, hand.missing.get(BT_SUITE, []),
                          "with the hand lists alone the check must name the Bluetooth headers")

    def test_the_check_refuses_each_mutation(self) -> None:
        for tree in TREES:
            with self.subTest(tree=tree):
                rs = self.results[tree]
                main = rs["Makefile"]
                lost = [name for name in MUTATIONS if name not in rs and name != "Makefile.syntax_deps"]
                self.assertEqual(lost, [], f"{tree}: the line each of these changes is not in the Makefile")
                # No rule for the sources: exactly the files each compile names
                # on its command line, as -MP gives every header one.
                cut = rs["Makefile.no_source_rules"]
                self.assertTrue(cut.unruled, "no source rule, and the check saw none missing")
                self.assertEqual(cut.missing, main.missing)
                for t, fs in cut.unruled.items():
                    for f in fs:
                        self.assertRegex(f, r"\.(?:c|cpp)$", f"{t}: a header without a rule: {f}")
                first = rs["Makefile.include_first"]
                self.assertNotEqual(first.goal, main.goal_before,
                                    "the dependency files read first, and the default goal held")
                self.assertTrue(rs["Makefile.no_override"].nodeps,
                                "DEPFLAGS without override, and every compile still wrote its file")
                self.assertTrue(rs["Makefile.no_listing"].unlisted,
                                "with_deps without its listing, and no compile was named")
        fw = self.results[FIRMWARE_HOST]
        # firmware/tests_host has no earlier `override` of CXXFLAGS, so without
        # this one only a CXXFLAGS given to make loses DEPFLAGS.
        self.assertTrue(all(t.endswith(f"(with {CMDLINE_CXXFLAGS} given to make)")
                            for t in fw["Makefile.no_override"].nodeps), fw["Makefile.no_override"].nodeps)
        self.assertIn(VISCORE, fw["Makefile.no_listing"].unlisted)
        self.assertIn(VISCORE, fw["Makefile.no_source_rules"].unruled)
        self.assertTrue(any(x.startswith("run-syntax-checks") for x in fw["Makefile.syntax_deps"].litter),
                        "the syntax checks kept DEPFLAGS, and no litter was named")


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
                  f"{len(r.missing)} suites miss some, {len(r.unruled)} name a file with no rule, "
                  f"{len(r.stale)} out of date after the build; default goal {r.goal_before!r} "
                  f"before the build, {r.goal!r} after")
            for t, fs in sorted(r.missing.items()):
                print(f"  {t}: {', '.join(fs)}")
            for t, fs in sorted(r.unruled.items()):
                print(f"  no rule, {t}: {', '.join(fs)}")
            for t in r.stale:
                print(f"  stale: {t}")
            for name in ("nodeps", "unlisted", "litter"):
                for x in getattr(r, name):
                    print(f"  {name}: {x}")
            bad += (len(r.missing) + len(r.unruled) + len(r.stale) + len(r.nodeps)
                    + len(r.unlisted) + len(r.litter) + (r.goal != r.goal_before))
    return 1 if bad else 0


if __name__ == "__main__":
    if "--report" in sys.argv:
        sys.exit(report(sys.argv[1:]))
    unittest.main()
