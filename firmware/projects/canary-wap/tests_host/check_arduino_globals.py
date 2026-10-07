#!/usr/bin/env python3
"""Every host suite compiles the sketch's code after Arduino.h's names.

arduino-cli and PlatformIO both open canary_wap.ino with `#include
<Arduino.h>`, so every header the sketch includes, and every function the
suites cut out of the .ino, is compiled by both ESP32 builds after
arduino-esp32's global names (`typedef bool boolean;`, `byte`, `word`, the
pin and bit macros, `min`/`max`, `B0` ... `B11111111`). A host suite compiles
the same code in a plain hosted translation unit. Sweep F212's first
identity_json.h passed every host test and could not compile in the sketch:
`using namespace wap_json;` and then `boolean(w, ...)`, ambiguous next to
Arduino.h's typedef. tests_host/arduino_globals.h carries those names
(arduino-esp32 3.3.8's), and the Makefile puts it in front of every
translation unit it compiles with `-include` (sweep F229): the sketch builds
put Arduino.h in front of the .ino, and the suites put its names in front of
the sketch's .cpp files too, which is stricter than those files' own
includes.

This check holds every suite to that, by what the compiler reads rather than
by what the Makefile says. It asks make for the plan (`make -n -B` of `run`
and of every `tsan-*` target, with CXX replaced by a marker so that every
compile is found whatever compiler is configured; a recipe that names g++,
clang++ or c++ itself is found too), twice: as the Makefile stands, and with
the Makefile's own CXXFLAGS default given on make's command line, which
replaces every plain CXXFLAGS assignment (so the forced include must be
`override`). Each printed recipe line is split as the shell splits it, and
each of its commands (between `&&`, `||`, `;`, `|`, `&` and parentheses,
redirections dropped, `env`, `exec`, `command`, `time` and `VAR=value`
skipped) that starts with the marker or a compiler name is a compile; a
command that names the marker and is not one fails, so a compile is never
passed unread. It runs each compile's preprocessor (`-E`, the compile's own
flags and defines) once per source file, and reads the line markers: in each translation unit, arduino_globals.h must be entered before
the first line of any file in the sketch folder (arduino/canary_wap: its
headers, its .cpp files a suite compiles or #includes, and the code a suite
cuts out of canary_wap.ino or a sketch header, whose `#line` names the
sketch file). An include of arduino_globals.h written first in a suite
counts the same as the forced one. Which files that is, is derived, never
listed: whatever any suite's preprocessor reads from the sketch folder.

It fails closed: a make that fails, a preprocessor that fails, no compile in
the plan, or a tests_host/test_*.cpp no compile in the full plan preprocesses
(a suite the check cannot read) is a failure.

The self-test (on by default; --no-self-test skips it) guards the guard:
synthetic preprocessor output for each rule, recipe lines the shell splitter
must read (or refuse), and mutated copies of the Makefile (the forced include
dropped, its `override` dropped, a recipe that does not pass CXXFLAGS, a tsan
build without the include, one behind `cd . &&`, a command that names the
compiler and is not a compile) that the check must fail on, next to the
unmutated Makefile on the same targets, which it must pass, and an explicit
first include and a compile behind `cd . && env`, which it must accept.

Run it through make, which first cuts the .inc files the suites read out of
the .ino (CI: `make -C firmware/projects/canary-wap/tests_host`, whose `run`
target runs it; on its own:
`make -C firmware/projects/canary-wap/tests_host run-arduino-globals`). Run
directly on a fresh checkout it fails closed on the missing cuts.
"""

from __future__ import annotations

import functools
import os
import re
import shlex
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
MAKEFILE = os.path.join(HERE, "Makefile")
SKETCH = os.path.realpath(os.path.join(HERE, "..", "arduino", "canary_wap"))
GLOBALS = os.path.realpath(os.path.join(HERE, "arduino_globals.h"))

# CXX for the plan: every $(CXX) compile starts with it.
SENTINEL = "__wap_host_cxx__"
# A recipe that names a compiler itself.
COMPILER = re.compile(r"^(?:\S*/)?(?:[\w.]+-)?(?:g\+\+|clang\+\+|c\+\+)(?:-[\d.]+)?$")
SOURCE = re.compile(r"\.(?:cpp|cc|cxx|c)$")
# GCC's and clang's line markers: # <line> "<file>" [flags]
MARKER = re.compile(r'^# (\d+) "((?:[^"\\]|\\.)*)"((?: \d+)*)\s*$')
MARKERS = re.compile(r'^# \d+ "((?:[^"\\]|\\.)*)"', re.M)


class CheckError(Exception):
    pass


def clean_env() -> dict[str, str]:
    """The environment for a make of our own: none of the calling make's flags
    (its jobserver, its -n or -B) leak into it."""
    env = dict(os.environ)
    for k in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES", "GNUMAKEFLAGS"):
        env.pop(k, None)
    return env


def real_cxx() -> list[str]:
    """The configured compiler, as make would run it (CXX may carry a
    wrapper or flags: `ccache g++`)."""
    return shlex.split(os.environ.get("CXX") or "g++")


def default_targets(makefile: str) -> list[str]:
    with open(makefile, encoding="utf-8") as f:
        text = f.read()
    tsan = sorted(set(re.findall(r"^(tsan-[\w-]+)\s*:", text, re.M)))
    return ["run"] + tsan


# A shell redirection (`>`, `2>&1`, `&>`, `<`): an operator and its target.
REDIRECT = re.compile(r"^(?:&>>?|[<>][<>&|]*)$")
# Commands that run the rest of their segment: `env X=1 g++ ...`, `exec g++ ...`.
RUNNERS = {"env", "exec", "command", "time"}
ASSIGNMENT = re.compile(r"^[A-Za-z_]\w*=")


def shell_tokens(line: str) -> list[str]:
    """One recipe line as the shell splits it, its operators (`&&`, `;`, `|`,
    `(`, `>`) as tokens of their own."""
    lx = shlex.shlex(line, posix=True, punctuation_chars=True)
    lx.whitespace_split = True
    lx.commenters = ""
    return list(lx)


def compiles_in(line: str) -> list[list[str]]:
    """The compile commands one printed recipe line runs: each command of the
    line (between `&&`, `||`, `;`, `|`, `&` and parentheses), its redirections
    dropped and `env`, `VAR=value` and the like skipped, that starts with the
    CXX sentinel or a compiler name. A command that names the sentinel and is
    not one of them is a compile this check cannot read: it fails, never
    passes unread."""
    try:
        toks = shell_tokens(line)
    except ValueError:
        if SENTINEL in line:
            raise CheckError(f"a recipe line with the compiler the check cannot split: {line.strip()}")
        return []
    segments: list[list[str]] = [[]]
    for t in toks:
        if t and not REDIRECT.match(t) and all(ch in "();<>|&" for ch in t):
            segments.append([])
        else:
            segments[-1].append(t)
    cmds = []
    for seg in segments:
        words: list[str] = []
        i = 0
        while i < len(seg):
            if REDIRECT.match(seg[i]):
                if words and words[-1].isdigit():
                    words.pop()            # the fd of `2>&1`
                i += 2
                continue
            words.append(seg[i])
            i += 1
        while words and (words[0] in RUNNERS or ASSIGNMENT.match(words[0])):
            if words.pop(0) == "env":
                while words and words[0].startswith("-"):
                    words.pop(0)        # env's own options (-i, -u NAME is not followed)
        if words and (words[0] == SENTINEL or COMPILER.match(words[0])):
            cmds.append(words)
        elif any(SENTINEL in w for w in words):
            raise CheckError("a recipe command that names the compiler and is not a compile the check "
                             f"can read: {' '.join(words)}")
    return cmds


def default_cxxflags(makefile: str) -> str:
    """The Makefile's own CXXFLAGS default (`CXXFLAGS ?= ...`)."""
    with open(makefile, encoding="utf-8") as f:
        m = re.search(r"^CXXFLAGS\s*\?=[ \t]*(.*?)\s*$", f.read(), re.M)
    if m is None:
        raise CheckError(f"{os.path.basename(makefile)} has no `CXXFLAGS ?=` default: the check plans with "
                         "it given on make's command line too")
    return m.group(1)


def plan(makefile: str, targets: list[str]) -> list[list[str]]:
    """Every compile command `make -n -B` prints for the targets, tokenized,
    planned twice: as the Makefile stands, and with its own CXXFLAGS default
    given on make's command line (`make CXXFLAGS=...`), which replaces every
    plain assignment of CXXFLAGS in the Makefile; the forced include is
    `override` so that it survives that, and a plan that loses it fails. A
    compile both plans print is read once."""
    seen: set[tuple[str, ...]] = set()
    cmds: list[list[str]] = []
    for extra in ([], [f"CXXFLAGS={default_cxxflags(makefile)}"]):
        out = subprocess.run(
            ["make", "-n", "-B", "-f", makefile, f"CXX={SENTINEL}"] + extra + targets,
            cwd=HERE, env=clean_env(), capture_output=True, text=True)
        if out.returncode != 0:
            raise CheckError(f"make -n -B {' '.join(extra + targets)} failed:\n{out.stderr.strip()}")
        lines: list[str] = []
        buf = ""
        for raw in out.stdout.splitlines():
            if raw.endswith("\\"):
                buf += raw[:-1] + " "
                continue
            lines.append(buf + raw)
            buf = ""
        if buf:
            lines.append(buf)
        for line in lines:
            for toks in compiles_in(line):
                if tuple(toks) not in seen:
                    seen.add(tuple(toks))
                    cmds.append(toks)
    return cmds


def split_command(toks: list[str]) -> tuple[list[str], list[str], list[str], str]:
    """(compiler, preprocessor flags, sources, output) of one compile command."""
    cxx = real_cxx() if toks[0] == SENTINEL else [toks[0]]
    flags: list[str] = []
    sources: list[str] = []
    output = ""
    i = 1
    while i < len(toks):
        t = toks[i]
        if t == "-o" and i + 1 < len(toks):
            output = toks[i + 1]
            i += 2
            continue
        if SOURCE.search(t) and not t.startswith("-"):
            sources.append(t)
        elif t.startswith(("-l", "-L", "-Wl,")):
            pass  # link-only
        else:
            flags.append(t)
        i += 1
    return cxx, flags, sources, output


def unescape(name: str) -> str:
    return re.sub(r"\\(.)", r"\1", name)


@functools.lru_cache(maxsize=None)
def resolve(name: str) -> str | None:
    if name.startswith("<") and name.endswith(">"):
        return None
    return os.path.realpath(name if os.path.isabs(name) else os.path.join(HERE, name))


def in_sketch(path: str | None) -> bool:
    return path is not None and (path + os.sep).startswith(SKETCH + os.sep)


def analyze(text: str) -> tuple[list[str], set[str]]:
    """One translation unit's preprocessed text: (what was compiled before
    arduino_globals.h, every sketch file it read)."""
    seen = False
    current: str | None = None
    current_line = 0
    problems: list[str] = []
    sketch_files: set[str] = set()
    reported: set[str] = set()   # one problem per sketch file
    pos = 0
    n = len(text)
    while pos < n and not seen:
        end = text.find("\n", pos)
        if end < 0:
            end = n
        line = text[pos:end]
        pos = end + 1
        m = MARKER.match(line)
        if m:
            current = resolve(unescape(m.group(2)))
            current_line = int(m.group(1))
            flags = m.group(3).split()
            if current == GLOBALS and "1" in flags:
                seen = True
            elif in_sketch(current):
                sketch_files.add(current)
                if "1" in flags and current not in reported:
                    reported.add(current)
                    problems.append(f"{os.path.relpath(current, HERE)} is included before "
                                    "arduino_globals.h")
            continue
        if in_sketch(current) and line.strip() and current not in reported:
            reported.add(current)
            problems.append(f"{os.path.relpath(current, HERE)}:{current_line} is compiled "
                            "before arduino_globals.h")
        current_line += 1
    # Past the include nothing can come before it: only which sketch files
    # the unit reads is left to collect.
    for name in set(MARKERS.findall(text, pos)):
        path = resolve(unescape(name))
        if in_sketch(path):
            sketch_files.add(path)
    return problems, sketch_files


def preprocess(cxx: list[str], flags: list[str], src: str) -> str:
    out = subprocess.run(cxx + flags + ["-E", src], cwd=HERE, capture_output=True, text=True)
    if out.returncode != 0:
        err = out.stderr.strip().splitlines()
        hint = (" (the .inc files are cut out of the .ino by make: run "
                "`make run-arduino-globals`)" if ".inc" in out.stderr else "")
        raise CheckError(f"{src}: the preprocessor failed: " + " | ".join(err[:6]) + hint)
    return out.stdout


def check(makefile: str, targets: list[str], full: bool) -> tuple[list[str], dict]:
    """Problems found, and what was read."""
    cmds = plan(makefile, targets)
    if not cmds:
        raise CheckError(f"make -n -B {' '.join(targets)} printed no compile command")
    jobs = []
    for toks in cmds:
        cxx, flags, sources, output = split_command(toks)
        if not sources:
            raise CheckError(f"a compile with no source it can read: {' '.join(toks)}")
        for src in sources:
            jobs.append((output or "?", cxx, flags, src))

    def run(job):
        output, cxx, flags, src = job
        try:
            problems, files = analyze(preprocess(cxx, flags, src))
        except CheckError as e:
            return job, [str(e)], set()
        return job, problems, files

    try:
        workers = max(2, len(os.sched_getaffinity(0)))
    except AttributeError:
        workers = max(2, os.cpu_count() or 2)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(run, jobs))

    problems: list[str] = []
    sketch_files: set[str] = set()
    units_with_sketch = 0
    read_sources: set[str] = set()
    for (output, _cxx, _flags, src), unit_problems, files in results:
        read_sources.add(os.path.realpath(os.path.join(HERE, src)))
        if files:
            units_with_sketch += 1
        sketch_files |= files
        if unit_problems:
            more = (f" (and {len(unit_problems) - 1} more sketch files)"
                    if len(unit_problems) > 1 else "")
            problems.append(f"{output}: {src}: {unit_problems[0]}{more}")
    if full:
        for name in sorted(os.listdir(HERE)):
            if name.startswith("test_") and name.endswith(".cpp"):
                if os.path.realpath(os.path.join(HERE, name)) not in read_sources:
                    problems.append(f"{name}: no compile in `make -n -B {' '.join(targets)}` "
                                    "preprocesses it, so this check cannot read it")
    stats = {"commands": len(cmds), "units": len(jobs), "units_with_sketch": units_with_sketch,
             "sketch_files": sketch_files}
    return problems, stats


# ── self-test ────────────────────────────────────────────────────────────

def _m(line: int, path: str, *flags: int) -> str:
    tail = "".join(f" {f}" for f in flags)
    return f'# {line} "{path}"{tail}'


def parser_cases() -> list[tuple[str, str, bool]]:
    """(name, preprocessed text, must it pass)."""
    s_h = os.path.join(SKETCH, "wap_diagnostics.h")
    s_cpp = os.path.join(SKETCH, "csi_mqtt.cpp")
    s_ino = os.path.join(SKETCH, "canary_wap.ino")
    main = os.path.join(HERE, "test_x.cpp")
    inc = os.path.join(HERE, "test_x_cut.inc")
    head = [_m(0, main), _m(0, "<built-in>"), _m(0, "<command-line>")]
    glob = [_m(1, GLOBALS, 1), "typedef bool boolean;", _m(0, "<command-line>", 2)]
    return [
        ("a sketch header after the forced include",
         "\n".join(head + glob + [_m(1, main), _m(1, s_h, 1), "int f();", _m(2, main, 2), "int x;"]),
         True),
        ("a sketch header before any include of arduino_globals.h",
         "\n".join(head + [_m(1, main), _m(1, s_h, 1), "int f();", _m(2, main, 2),
                           _m(1, GLOBALS, 1), "typedef bool boolean;", _m(3, main, 2)]),
         False),
        ("a sketch header of macros only, before the include",
         "\n".join(head + [_m(1, main), _m(1, s_h, 1), "", _m(2, main, 2),
                           _m(1, GLOBALS, 1), "typedef bool boolean;"]),
         False),
        ("an explicit first include of arduino_globals.h",
         "\n".join(head + [_m(1, main), _m(1, GLOBALS, 1), "typedef bool boolean;",
                           _m(2, main, 2), _m(1, s_h, 1), "int f();"]),
         True),
        ("a sketch source compiled with no include",
         "\n".join([_m(0, s_cpp), _m(0, "<built-in>"), _m(0, "<command-line>"),
                    _m(1, s_cpp), "int g() { return 1; }"]),
         False),
        ("a sketch source compiled after the forced include",
         "\n".join([_m(0, s_cpp), _m(0, "<built-in>"), _m(0, "<command-line>")] + glob +
                   [_m(1, s_cpp), "int g() { return 1; }"]),
         True),
        ("a function cut out of the .ino (#line), before the include",
         "\n".join(head + [_m(1, main), _m(1, inc, 1), _m(40, s_ino), "static int cut() { return 0; }",
                           _m(3, main, 2), _m(1, GLOBALS, 1), "typedef bool boolean;"]),
         False),
        ("a function cut out of the .ino (#line), after the include",
         "\n".join(head + glob + [_m(1, main), _m(1, inc, 1), _m(40, s_ino),
                                  "static int cut() { return 0; }", _m(3, main, 2)]),
         True),
    ]


def line_cases() -> list[tuple[str, str, int | None]]:
    """(name, a printed recipe line, how many compiles it holds; None: it must fail)."""
    c = SENTINEL
    return [
        ("a compile", f"{c} -std=c++17 -I x -o t t.cpp", 1),
        ("a compile behind cd and &&", f"cd . && {c} -std=c++17 -o t t.cpp", 1),
        ("a compile behind env and an assignment", f"env -i X=1 Y=2 {c} -o t t.cpp", 1),
        ("a compile after a ; and before a pipe and a redirection",
         f"mkdir -p d; {c} -o d/t t.cpp 2>&1 | tee log", 1),
        ("two compiles in a subshell", f"( {c} -o a a.cpp && {c} -o b b.cpp )", 2),
        ("a run, no compile", "./test_x 2>&1 | grep -q 'ALL .* PASSED'", 0),
        ("the compiler handed to another command", f"echo {c} -o t t.cpp", None),
        ("the compiler inside a word", f"sh -c '{c} -o t t.cpp'", None),
    ]


# (name, anchor, replacement, targets, must it pass, what the failure names)
MUTATIONS = [
    ("the unmutated Makefile, the diagnostics suite",
     None, None, ["test_wap_diagnostics"], True, None),
    ("the forced include dropped",
     "override CXXFLAGS += $(ARDUINO_GLOBALS)\n", "\n",
     ["test_wap_diagnostics"], False, "wap_diagnostics.h is included before arduino_globals.h"),
    ("the forced include dropped, a suite that includes arduino_globals.h first",
     "override CXXFLAGS += $(ARDUINO_GLOBALS)\n", "\n",
     ["test_identity_json"], True, None),
    ("a recipe that does not pass CXXFLAGS",
     "$(DIAG_BIN): $(DIAG_SRC)",
     None,  # computed: the rule's recipe with $(CXXFLAGS) spelled out without the include
     ["test_wap_diagnostics"], False, "is included before arduino_globals.h"),
    ("the unmutated Makefile, a tsan build",
     None, None, ["tsan-loop-snapshot"], True, None),
    ("a tsan build without the include",
     "-fsanitize=thread -pthread $(ARDUINO_GLOBALS) -I $(SRC_DIR) -o $(LOOP_SNAP_BIN)_tsan",
     "-fsanitize=thread -pthread -I $(SRC_DIR) -o $(LOOP_SNAP_BIN)_tsan",
     ["tsan-loop-snapshot"], False, "loop_snapshot.h is included before arduino_globals.h"),
    ("a tsan build behind `cd . &&`, without the include",
     "\t$(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread $(ARDUINO_GLOBALS) -I $(SRC_DIR) -o $(LOOP_SNAP_BIN)_tsan",
     "\tcd . && $(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread -I $(SRC_DIR) -o $(LOOP_SNAP_BIN)_tsan",
     ["tsan-loop-snapshot"], False, "loop_snapshot.h is included before arduino_globals.h"),
    ("a tsan build behind `cd . && env X=1`, with the include",
     "\t$(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread $(ARDUINO_GLOBALS) -I $(SRC_DIR) -o $(LOOP_SNAP_BIN)_tsan",
     "\tcd . && env X=1 $(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread $(ARDUINO_GLOBALS) -I $(SRC_DIR) "
     "-o $(LOOP_SNAP_BIN)_tsan",
     ["tsan-loop-snapshot"], True, None),
    ("the compiler named by a command that is not a compile",
     "\t$(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread $(ARDUINO_GLOBALS) -I $(SRC_DIR) -o $(LOOP_SNAP_BIN)_tsan",
     "\ttrue $(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread $(ARDUINO_GLOBALS) -I $(SRC_DIR) "
     "-o $(LOOP_SNAP_BIN)_tsan",
     ["tsan-loop-snapshot"], False, "names the compiler and is not a compile"),
    ("`override` dropped: a CXXFLAGS given to make loses the include",
     "override CXXFLAGS += $(ARDUINO_GLOBALS)\n", "CXXFLAGS += $(ARDUINO_GLOBALS)\n",
     ["test_wap_diagnostics"], False, "wap_diagnostics.h is included before arduino_globals.h"),
    ("the unmutated Makefile, a suite that compiles a sketch source on its own",
     None, None, ["test_mqtt_reinit"], True, None),
    ("the forced include dropped, a sketch source compiled on its own",
     "override CXXFLAGS += $(ARDUINO_GLOBALS)\n", "\n",
     ["test_mqtt_reinit"], False, "../arduino/canary_wap/csi_mqtt.cpp: "),
]


def mutate(text: str, name: str, anchor: str | None, replacement: str | None) -> str:
    if anchor is None:
        return text
    if anchor not in text:
        raise CheckError(f"self-test: mutation '{name}' no longer applies (anchor {anchor!r})")
    if replacement is None:
        # Spell the diagnostics rule's flags out by hand, without $(CXXFLAGS).
        start = text.index(anchor)
        rule_end = text.index("\n\n", start)
        rule = text[start:rule_end]
        if "$(CXX) $(CXXFLAGS)" not in rule:
            raise CheckError(f"self-test: mutation '{name}' no longer applies (no $(CXX) $(CXXFLAGS))")
        changed = rule.replace("$(CXX) $(CXXFLAGS)", "$(CXX) -std=c++17 -O2 -Wall -Wextra -Wpedantic", 1)
        return text[:start] + changed + text[rule_end:]
    return text.replace(anchor, replacement, 1)


def self_test() -> list[str]:
    problems: list[str] = []
    for name, line, want in line_cases():
        try:
            got: int | None = len(compiles_in(line))
            words = compiles_in(line)
        except CheckError:
            got, words = None, []
        if got != want:
            problems.append(f"self-test: line case '{name}': {got} compiles read, {want} expected")
        elif any(w in ("2", ">&", "1", "|", "tee", "log") for cmd in words for w in cmd[1:]):
            problems.append(f"self-test: line case '{name}' kept shell words in a compile: {words}")
    for name, text, must_pass in parser_cases():
        found, _ = analyze(text)
        if must_pass and found:
            problems.append(f"self-test: parser case '{name}' failed: {found[0]}")
        if not must_pass and not found:
            problems.append(f"self-test: the check did not bite on parser case '{name}'")
    with open(MAKEFILE, encoding="utf-8") as f:
        base = f.read()
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, anchor, replacement, targets, must_pass, names) in enumerate(MUTATIONS):
            try:
                text = mutate(base, name, anchor, replacement)
            except CheckError as e:
                problems.append(str(e))
                continue
            if anchor is not None and text == base:
                problems.append(f"self-test: mutation '{name}' changed nothing")
                continue
            path = os.path.join(tmp, f"Makefile.{i}")
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            try:
                found, _ = check(path, targets, full=False)
            except CheckError as e:
                found = [str(e)]
            if must_pass and found:
                problems.append(f"self-test: '{name}' must pass and failed: {found[0]}")
            elif not must_pass and not found:
                problems.append(f"self-test: the check did not bite on mutation '{name}'")
            elif not must_pass and names and not any(names in p for p in found):
                problems.append(f"self-test: mutation '{name}' failed for another reason: {found[0]}")
    return problems


def main(argv: list[str]) -> int:
    try:
        targets = default_targets(MAKEFILE)
        problems, stats = check(MAKEFILE, targets, full=True)
        if "--no-self-test" not in argv:
            problems += self_test()
    except CheckError as e:
        problems, stats = [str(e)], None
    if problems:
        print("check_arduino_globals: FAIL", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    files = sorted(os.path.relpath(p, SKETCH) for p in stats["sketch_files"])
    headers = [f for f in files if f.endswith(".h")]
    print(f"check_arduino_globals: OK — {stats['units']} translation units in {stats['commands']} "
          f"compiles ({' '.join(targets)}); {stats['units_with_sketch']} read {len(files)} files of "
          f"the sketch folder ({len(headers)} headers), each after arduino_globals.h"
          + ("" if "--no-self-test" in argv else
             f"; self-test: {len(parser_cases())} parser cases, {len(line_cases())} recipe-line cases, "
             f"{len(MUTATIONS)} Makefile cases"))
    if "--list" in argv:
        for f in files:
            print(f"  {f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
