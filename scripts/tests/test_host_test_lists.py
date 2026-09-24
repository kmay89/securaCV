#!/usr/bin/env python3
"""One host-test list: every tests_host suite is built and run by its Makefile.

The host tests under firmware/tests_host and firmware/projects/*/tests_host
used to live on two lists. Each directory's Makefile had one, and
firmware.yml's "Mesh + Scout Host Tests" job had another: 34 inline
`g++ ... tests_host/test_*.cpp` steps. Nineteen of those suites were on the
workflow's list only, so `make` in their directory never built them; fifteen
were on both, compiled twice with different flags; and the Tin Can and
Companion Makefiles were on neither, because no workflow ran them (sweep CI3).
A suite added to one list and not the other ran in one place only, and nothing
said so.

The Makefile is now the one list, and this test keeps it that way:

  * every tests_host/test_*.cpp is compiled AND run by `make` in its own
    directory (the default goal, which is what CI's `make -C` runs), or sits
    in ALLOW with a reason;
  * no workflow compiles a tests_host source inline with g++ or clang++;
  * every tests_host Makefile runs in some workflow through `make -C <dir>`
    (no target, or `all` / `run`: a single-target call such as
    `make -C ... test_fleet_beacon` builds one suite, not the list).

The Makefiles register a suite in two styles: on a shared `run:` list, and by
a prerequisite hook (`run: run-x` with `run-x: $(X_BIN)` running it), which
lets parallel branches add a suite without touching the shared lines. This
test reads neither by hand. It asks make itself for the plan (`make -n -B`,
which prints every command the default goal would run, with every variable,
`$(call ...)` and hook expanded) and checks that plan: a command that compiles
the suite (`-o <bin> ... test_x.cpp`) and a later command that runs <bin>. So
both styles, and any third one make accepts, count the same way.

Same shape as test_docs_claims.py: MUST_PASS / MUST_FAIL Makefiles and
workflow snippets guard the guard, so a reader that stops seeing its own
failure mode is a red test, not a silent hole.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_host_test_lists.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
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

# test_*.cpp files in a tests_host directory that its Makefile deliberately
# does not build, as {repo-relative path: reason}. Empty: every suite builds.
# A row needs a reason a reviewer can check, and a row whose file is gone or
# is built after all fails the test, so the list cannot go stale.
ALLOW: dict[str, str] = {}

# Where CI steps live. Composite actions are scanned too, so an inline compile
# cannot hide one level down.
WORKFLOW_GLOBS = (
    ".github/workflows/*.yml",
    ".github/workflows/*.yaml",
    ".github/actions/*/action.yml",
    ".github/actions/*/action.yaml",
)

SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx")
# What a compiler reads: C++ and C sources, objects and archives.
OBJECT_SUFFIXES = SOURCE_SUFFIXES + (".c", ".o", ".a")
# Shell words after which the next word is in command position.
COMMAND_KEYWORDS = {"if", "then", "else", "elif", "do", "while", "until", "!", "{", "}", "time"}
ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")
# A C++ compiler in command position: g++, clang++, c++, $CXX / ${CXX}.
COMPILER = re.compile(r"""(?:^|[\s;&|(`"'])(?:g\+\+|clang\+\+|c\+\+|\$\{?CXX\}?)(?=[\s"']|$)""", re.M)
TESTS_HOST_SOURCE = re.compile(r"[\w./${}-]*tests_host/[\w./${}-]*\.(?:cpp|cc|cxx)\b")
# NAME=<path containing tests_host>, as a shell assignment in a run: block.
TESTS_HOST_VAR = re.compile(r"""(?:^|[\s;&|(])([A-Za-z_]\w*)=["']?([\w./-]*tests_host[\w./-]*)""", re.M)
MAKE_VALUE_OPTIONS = {"-C", "-f", "-I", "-o", "-W", "--directory", "--file", "--makefile"}
LIST_TARGETS = {"all", "run"}


# ── shell reading (shared by the make plan and the workflow scan) ──────────

def logical_lines(text: str) -> list[str]:
    """Join backslash-newline continuations, then split into lines."""
    return re.sub(r"\\\n", " ", text).splitlines()


def simple_commands(line: str) -> list[list[str]]:
    """Split one shell line into simple commands, each a list of words.

    A separator is any run of ; & | ( ) < > (so `out=$(./t)` puts `./t` in
    command position and `x > log` ends the command before the log name), and
    the words in COMMAND_KEYWORDS. This is a reader for CI recipes, not a
    shell: it only has to find command words and compiler arguments.
    """
    lex = shlex.shlex(line, posix=True, punctuation_chars=True)
    lex.whitespace_split = True
    lex.commenters = ""
    try:
        tokens = list(lex)
    except ValueError:  # an unbalanced quote: fall back to plain words
        tokens = line.split()
    commands: list[list[str]] = []
    current: list[str] = []
    for tok in tokens:
        if tok in COMMAND_KEYWORDS or (tok and set(tok) <= set(";&|()<>")):
            if current:
                commands.append(current)
            current = []
            continue
        current.append(tok)
    if current:
        commands.append(current)
    return commands


def command_word(words: list[str]) -> tuple[str | None, list[str]]:
    """(command, arguments) with leading VAR=value assignments skipped."""
    i = 0
    while i < len(words) and ASSIGNMENT.match(words[i]):
        i += 1
    if i == len(words):
        return None, []
    return words[i], words[i + 1:]


# ── the make plan ──────────────────────────────────────────────────────────

def make_plan(makedir: Path) -> str:
    """The commands `make` would run for the default goal, fully expanded."""
    env = {k: v for k, v in os.environ.items()
           if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEFILES", "GNUMAKEFLAGS")}
    env["LC_ALL"] = "C"
    proc = subprocess.run(
        ["make", "--no-print-directory", "-n", "-B", "-C", str(makedir)],
        capture_output=True, text=True, env=env, timeout=120, check=False,
    )
    if proc.returncode != 0:
        raise AssertionError(
            f"`make -n -B -C {makedir}` failed ({proc.returncode}); a Makefile "
            f"make cannot plan is a Makefile CI cannot run:\n{proc.stderr}")
    return proc.stdout


def _resolve(makedir: Path, word: str) -> Path:
    return Path(os.path.normpath(makedir / word))


def unrun_suites(makedir: Path, sources: list[Path], plan: str) -> dict[Path, str]:
    """{source: why} for every source the plan does not compile AND run."""
    wanted = {Path(os.path.normpath(s)) for s in sources}
    built: dict[Path, set[Path]] = {s: set() for s in wanted}
    made_from: dict[Path, set[Path]] = {}  # an output -> the suites in it
    ran: set[Path] = set()
    for line in logical_lines(plan):
        for words in simple_commands(line):
            cmd, args = command_word(words)
            if cmd is None:
                continue
            outputs = [args[i + 1] for i, w in enumerate(args[:-1]) if w == "-o"]
            outputs += [w[2:] for w in args if w.startswith("-o") and len(w) > 2]
            inputs = [_resolve(makedir, w) for w in args if not w.startswith("-")]
            if outputs and any(i in made_from or str(i).endswith(OBJECT_SUFFIXES)
                               for i in inputs):
                # A compile (or a link of earlier objects): whatever suite
                # went in comes out in each output.
                suites = set()
                for i in inputs:
                    suites |= {i} & wanted
                    suites |= made_from.get(i, set())
                for out in (_resolve(makedir, o) for o in outputs):
                    made_from.setdefault(out, set()).update(suites)
                    for suite in suites:
                        built[suite].add(out)
                continue
            if cmd == "for" and "in" in args:  # for b in A B C; do ./$b; done
                ran.update(_resolve(makedir, w) for w in args[args.index("in") + 1:])
            ran.add(_resolve(makedir, cmd))
    why: dict[Path, str] = {}
    for src, bins in sorted(built.items()):
        if not bins:
            why[src] = "no command in the Makefile's default goal compiles it"
        elif not bins & ran:
            names = ", ".join(sorted(os.path.relpath(b, makedir) for b in bins))
            why[src] = f"compiled to {names}, which the default goal never runs"
    return why


def tests_host_dirs(root: Path = REPO) -> list[Path]:
    found = []
    for dirpath, dirnames, _files in os.walk(root / "firmware"):
        dirnames[:] = sorted(d for d in dirnames
                             if not d.startswith(".") and d not in ("node_modules", "build"))
        if Path(dirpath).name == "tests_host":
            found.append(Path(dirpath))
    return found


# ── the workflows ──────────────────────────────────────────────────────────

def run_blocks(text: str) -> list[tuple[int, str]]:
    """[(line number, script)] for every `run:` value in a workflow file.

    Block scalars (`run: |`) take every following line indented past the
    `run:` key (and blank lines); a plain scalar is the rest of its line.
    """
    lines = text.splitlines()
    blocks: list[tuple[int, str]] = []
    i = 0
    while i < len(lines):
        m = re.match(r"^\s*(?:-\s+)?run:(?:\s+(.*))?$", lines[i])
        if not m:
            i += 1
            continue
        key_col = lines[i].index("run:")
        rest = (m.group(1) or "").strip()
        if rest[:1] in ("|", ">"):
            body = []
            j = i + 1
            while j < len(lines) and (not lines[j].strip()
                                      or len(lines[j]) - len(lines[j].lstrip()) > key_col):
                body.append(lines[j])
                j += 1
            blocks.append((i + 1, "\n".join(body)))
            i = j
            continue
        blocks.append((i + 1, rest))
        i += 1
    return blocks


def _without_comment_lines(script: str) -> str:
    return "\n".join(ln for ln in script.splitlines() if not ln.lstrip().startswith("#"))


def _expand_tests_host_vars(code: str) -> str:
    """Substitute shell variables that hold a tests_host path (D=.../tests_host
    then $D/test_x.cpp), so a path split across a variable is still seen."""
    for name, value in TESTS_HOST_VAR.findall(code):
        code = re.sub(r"\$\{?" + name + r"\b\}?", lambda _m: value, code)
    return code


def inline_compiles(text: str) -> list[tuple[int, str]]:
    """[(run: line, tests_host source)] a workflow compiles outside make."""
    hits = []
    for lineno, script in run_blocks(text):
        code = _expand_tests_host_vars(_without_comment_lines(script))
        if COMPILER.search(code):
            hits.extend((lineno, src) for src in sorted(set(TESTS_HOST_SOURCE.findall(code))))
    return hits


def make_list_dirs(text: str) -> set[str]:
    """Directories a workflow runs `make -C <dir>` in for the whole list."""
    dirs: set[str] = set()
    for _lineno, script in run_blocks(text):
        for line in logical_lines(_without_comment_lines(script)):
            for words in simple_commands(line):
                cmd, args = command_word(words)
                if cmd != "make":
                    continue
                directory, targets = None, []
                i = 0
                while i < len(args):
                    w = args[i]
                    if w in MAKE_VALUE_OPTIONS:
                        if w in ("-C", "--directory") and i + 1 < len(args):
                            directory = args[i + 1]
                        i += 2
                        continue
                    if w.startswith("--directory="):
                        directory = w.split("=", 1)[1]
                    elif w.startswith("-C") and len(w) > 2:
                        directory = w[2:]
                    elif not w.startswith("-") and "=" not in w:
                        targets.append(w)
                    i += 1
                if directory and set(targets) <= LIST_TARGETS:
                    dirs.add(os.path.normpath(directory))
    return dirs


def workflow_files(root: Path = REPO) -> list[Path]:
    return sorted(p for pattern in WORKFLOW_GLOBS for p in root.glob(pattern))


# ── the guard itself ───────────────────────────────────────────────────────

# Synthetic Makefiles: {name: (Makefile text, sources, sources that must be
# reported unrun)}. Recipe lines start with a TAB.
MAKEFILES = {
    # MUST_PASS: the shared `run:` list (the firmware/tests_host style).
    "shared list": (
        "BIN = build/test_a\n"
        "all: run\n"
        "build:\n\tmkdir -p build\n"
        "$(BIN): test_a.cpp | build\n\t$(CXX) -o $@ test_a.cpp\n"
        "run: $(BIN)\n\t./$(BIN)\n",
        ["test_a.cpp"], []),
    # MUST_PASS: a prerequisite hook (the canary-wap style), and a suite run
    # through a marker-checking $(call ...) whose binary sits inside $( ).
    "prerequisite hook": (
        "run_marked = out=$$(./$(1)); s=$$?; printf '%s\\n' \"$$out\"; [ $$s -eq 0 ]\n"
        "all: run\n"
        "test_a: test_a.cpp\n\t$(CXX) -o $@ $<\n"
        "test_b: test_b.cpp\n\t$(CXX) -Werror -o $@ test_b.cpp -lcrypto\n"
        "run: test_a\n\t./test_a\n"
        ".PHONY: run-b\nrun-b: test_b\n\t$(call run_marked,test_b)\n"
        "run: run-b\n",
        ["test_a.cpp", "test_b.cpp"], []),
    # MUST_PASS: one source built several ways and run from a for loop (the
    # canary-wap CSI config-shim style).
    "for loop": (
        "BINS = t_one t_two\n"
        "all: run\n"
        "t_one: test_s.cpp\n\t$(CXX) -DONE -o $@ test_s.cpp\n"
        "t_two: test_s.cpp\n\t$(CXX) -DTWO -o $@ test_s.cpp\n"
        "run: $(BINS)\n\tfor b in $(BINS); do ./$$b || exit 1; done\n",
        ["test_s.cpp"], []),
    # MUST_PASS: compiled to an object, linked, then run.
    "compile then link": (
        "all: run\n"
        "test_o.o: test_o.cpp\n\t$(CXX) -c -o $@ test_o.cpp\n"
        "test_o: test_o.o\n\t$(CXX) -o $@ test_o.o -lcrypto\n"
        "run: test_o\n\t./test_o\n",
        ["test_o.cpp"], []),
    # MUST_FAIL: a rule exists, but nothing on the default goal reaches it.
    "rule not on the list": (
        "all: run\n"
        "test_a: test_a.cpp\n\t$(CXX) -o $@ test_a.cpp\n"
        "test_c: test_c.cpp\n\t$(CXX) -o $@ test_c.cpp\n"
        "run: test_a\n\t./test_a\n",
        ["test_a.cpp", "test_c.cpp"], ["test_c.cpp"]),
    # MUST_FAIL: built as a prerequisite of `run`, never executed.
    "built never run": (
        "all: run\n"
        "test_a: test_a.cpp\n\t$(CXX) -o $@ test_a.cpp\n"
        "test_d: test_d.cpp\n\t$(CXX) -o $@ test_d.cpp\n"
        "run: test_a test_d\n\t./test_a\n",
        ["test_a.cpp", "test_d.cpp"], ["test_d.cpp"]),
    # MUST_FAIL: a source the Makefile never mentions (the 19 inline-only
    # suites on the base tree looked like this).
    "no rule at all": (
        "all: run\n"
        "test_a: test_a.cpp\n\t$(CXX) -o $@ test_a.cpp\n"
        "run: test_a\n\t./test_a\n",
        ["test_a.cpp", "test_e.cpp"], ["test_e.cpp"]),
}

# Workflow snippets: (text, inline tests_host compiles expected, make -C list dirs).
WORKFLOWS = {
    # MUST_FAIL: the base tree's shape — one step per suite.
    "inline step": (
        "    steps:\n"
        "      - name: Build + run canary-display Modbus RTU host test\n"
        "        # g++ in a comment is not a compile\n"
        "        run: |\n"
        "          set -euo pipefail\n"
        "          g++ -std=c++17 -Wall -Wextra -Werror \\\n"
        "              -I firmware/projects/canary-display/include \\\n"
        "              firmware/projects/canary-display/tests_host/test_modbus_rtu.cpp \\\n"
        "              -o /tmp/modbus_rtu\n"
        "          /tmp/modbus_rtu | tee /tmp/modbus_rtu.log\n"
        "          grep -q \"ALL .* PASSED\" /tmp/modbus_rtu.log\n",
        ["firmware/projects/canary-display/tests_host/test_modbus_rtu.cpp"], set()),
    # MUST_FAIL: the compiler behind a helper function and $CXX.
    "helper and $CXX": (
        "      - run: |\n"
        "          run_test() { \"$CXX\" \"$@\" -o /tmp/t && /tmp/t; }\n"
        "          run_test firmware/tests_host/test_x.cpp\n",
        ["firmware/tests_host/test_x.cpp"], set()),
    # MUST_FAIL: the directory held in a shell variable.
    "path in a variable": (
        "      - run: |\n"
        "          D=firmware/projects/canary-wap/tests_host\n"
        "          clang++ -std=c++17 -I x \"${D}/test_y.cpp\" -o /tmp/y\n",
        ["firmware/projects/canary-wap/tests_host/test_y.cpp"], set()),
    # MUST_PASS: a non-tests_host compile, a node suite, the make -C forms.
    "make steps": (
        "      - name: pull-OTA engine\n"
        "        run: |\n"
        "          g++ -std=c++17 firmware/common/ota/test_ota_logic.cpp -o /tmp/o\n"
        "          node --test firmware/tests_host/test_canary_mesh_alerts.test.js\n"
        "          # g++ firmware/tests_host/test_commented_out.cpp\n"
        "      - run: make -C firmware/projects/canary-wap/tests_host\n"
        "      - run: make -j4 -C firmware/tests_host run\n"
        "      - name: one suite is not the list\n"
        "        run: |\n"
        "          set -euo pipefail\n"
        "          make -C firmware/projects/canary-display/tests_host test_fleet_beacon\n",
        [], {"firmware/projects/canary-wap/tests_host", "firmware/tests_host"}),
}


class TheGuardItself(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(shutil.which("make"), "make is required: the plan is make's own")

    def test_makefiles_both_registration_styles_pass_and_holes_fail(self):
        for name, (text, sources, expect) in MAKEFILES.items():
            with self.subTest(makefile=name), tempfile.TemporaryDirectory() as tmp:
                d = Path(tmp)
                (d / "Makefile").write_text(text, encoding="utf-8")
                for s in sources:
                    (d / s).write_text("int main() { return 0; }\n", encoding="utf-8")
                got = unrun_suites(d, [d / s for s in sources], make_plan(d))
                self.assertEqual(sorted(p.name for p in got), sorted(expect), got)

    def test_workflow_snippets(self):
        for name, (text, compiles, dirs) in WORKFLOWS.items():
            with self.subTest(workflow=name):
                self.assertEqual([src for _ln, src in inline_compiles(text)], compiles)
                self.assertEqual(make_list_dirs(text), dirs)

    def test_run_block_reader_stops_at_the_next_key(self):
        text = ("      - run: |\n          echo one\n\n          echo two\n"
                "      - name: next\n        run: echo three\n")
        self.assertEqual(run_blocks(text),
                         [(1, "          echo one\n\n          echo two"), (6, "echo three")])


# ── the tree ───────────────────────────────────────────────────────────────

class TheTree(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(shutil.which("make"), "make is required: the plan is make's own")

    def test_every_tests_host_dir_has_a_makefile(self):
        dirs = tests_host_dirs()
        self.assertTrue(dirs, "no tests_host directory found — has firmware/ moved?")
        for d in dirs:
            with self.subTest(dir=str(d.relative_to(REPO))):
                self.assertTrue((d / "Makefile").is_file(), f"{d.relative_to(REPO)} has no Makefile")

    def test_every_suite_is_built_and_run_by_its_makefile(self):
        problems = []
        seen_allow = set()
        for d in tests_host_dirs():
            if not (d / "Makefile").is_file():
                continue
            sources = sorted(d.glob("test_*.cpp"))
            for src, why in unrun_suites(d, sources, make_plan(d)).items():
                rel = str(src.relative_to(REPO))
                if rel in ALLOW:
                    seen_allow.add(rel)
                    continue
                problems.append(f"  {rel}: {why}")
        for rel in sorted(set(ALLOW) - seen_allow):
            problems.append(f"  ALLOW[{rel!r}] is stale: the file is gone or its Makefile runs it")
        self.assertEqual(
            problems, [],
            "a tests_host suite is not on its Makefile's list. Add a named rule "
            "(the CI step's flags, -Werror) and hook it into `run` — on the shared "
            "list or by a `run: run-x` prerequisite, as the file already does:\n"
            + "\n".join(problems))

    def test_no_workflow_compiles_a_tests_host_source_inline(self):
        found = []
        for wf in workflow_files():
            for lineno, src in inline_compiles(wf.read_text(encoding="utf-8")):
                found.append(f"  {wf.relative_to(REPO)}:{lineno}: {src}")
        self.assertEqual(
            found, [],
            f"{len(found)} inline compile(s) of a tests_host source. The suite's "
            "Makefile is the one list: move the step's flags into a named rule "
            "there and let the workflow's `make -C` run it:\n" + "\n".join(found))

    def test_every_tests_host_makefile_runs_in_a_workflow(self):
        ran: set[str] = set()
        for wf in workflow_files():
            ran |= make_list_dirs(wf.read_text(encoding="utf-8"))
        missing = [str(d.relative_to(REPO)) for d in tests_host_dirs()
                   if (d / "Makefile").is_file() and str(d.relative_to(REPO)) not in ran]
        self.assertEqual(
            missing, [],
            "a tests_host Makefile no workflow runs (add a `make -C <dir>` step; "
            "a single-target call builds one suite, not the list): "
            + ", ".join(missing))


if __name__ == "__main__":
    unittest.main()
