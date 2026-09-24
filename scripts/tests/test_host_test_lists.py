#!/usr/bin/env python3
"""One host-test list: every tests_host suite is built and run by its Makefile.

The host tests under firmware/tests_host and firmware/projects/*/tests_host
used to live on two lists. Each directory's Makefile had one, and
firmware.yml's "Mesh + Scout Host Tests" job had another: 34 inline compiles
of tests_host sources, in 33 steps. Nineteen of those suites were on the
workflow's list only, so `make` in their directory never built them. Fifteen
were on both: twelve were compiled twice in CI, with different flags, and the
Tin Can and Companion three only inline, because no workflow ran their
Makefiles (sweep CI3). A suite added to one list and not the other ran in one
place only, and nothing said so.

The Makefile is now the one list, and this test keeps it that way:

  * every tests_host/test_*.cpp is compiled AND run by `make` in its own
    directory (the default goal, which is what CI's `make -C` runs), in a way
    whose failure fails make, or sits in ALLOW with a reason;
  * no workflow or composite action compiles a tests_host source inline;
  * every tests_host Makefile runs, whole, in a step whose failure fails a
    workflow that pull requests run.

How a Makefile is read. The Makefiles register a suite in two styles: on a
shared `run:` list, and by a prerequisite hook (`run: run-x` with
`run-x: $(X_BIN)` running it), which lets parallel branches add a suite
without touching the shared lines. This test reads neither by hand. It asks
make for the plan (`make -n -B --trace -p`: every command the default goal
would run, with every variable, `$(call ...)` and hook expanded, the recipe
each command came from, and make's rule database) and checks the plan: a
command that compiles the suite (`-o <bin> ... test_x.cpp`) and a later
command that runs <bin>, so both styles, and any third one make accepts, count
the same way. A run counts only when a failure there reaches make:

  * its recipe line has no `-` prefix, and the Makefile sets no `.IGNORE`
    for that target and no `-i` in MAKEFLAGS;
  * in the recipe's shell line, the suite's exit status is what the line
    returns: it is the line's last command (last in a `{ }` group or an
    `if` branch counts, since the compound returns it); or `|| exit N`
    (N not 0), `|| false` or a `{ ...; exit 1; }` group follows it; or the
    next command reads `$?`; or `set -e` (or `-e` in .SHELLFLAGS) is on. A
    suite on the left of a pipe counts only under `set -o pipefail`.
    `|| true`, `|| :`, `|| echo ...` and `|| exit 0` swallow a failure, and
    so does a `;` that nothing reads (in a for loop, that keeps only the
    last iteration's status).

A `-` counts whether it is written in the recipe or comes out of a variable
the line starts with (`$(IGN)./t`): make strips the prefix after expanding
and `-n` prints the command without it, so the database's recipe text is
read, and the references a line starts with are expanded by a second read of
the Makefile. One case is read strictly, and is not in the tree: a suite used
as a condition (`if ./t; then ...`, `! ./t`) does not count.

How a workflow is read. A small block-YAML reader finds every step (a mapping
with a `run:` script), its working directory (the step's `working-directory`,
else a job or workflow `defaults.run.working-directory`), its shell, its
`if:` and `continue-on-error:` and its job's, and the workflow's triggers.

  * An inline compile is a step whose script names a C or C++ compiler (g++,
    clang++, c++, gcc, clang or cc, with or without a directory, a target
    prefix such as x86_64-linux-gnu- or a version suffix such as -18, or $CXX
    or $CC in any ${...} form) and a C++ source in a tests_host directory.
    The source can be named by its path (a glob counts), through a shell
    variable, or as a relative name under a working-directory, `cd` or
    `pushd` into tests_host (a `cd` inside `( )` or `$( )` stays there). It
    is not read when the name is computed at run time (`find` or `ls`
    output), when the compile is in a script file the step calls, or when
    the working directory is an expression.
  * A Makefile runs when a step calls `make` in its directory (`-C <dir>`,
    or no -C under a working-directory or `cd` into it) for the default goal
    or `all` / `run`, without -n, -q, -t or -i, with nothing like `|| true`
    after it. The step and its job must not be `if: false` or
    `continue-on-error: true`, and the file must be a workflow triggered by
    `pull_request` or `merge_group`, or a reusable workflow or composite
    action that one uses (`uses: ./...`). Path filters and `if:` expressions
    other than a literal false are not evaluated.

Same shape as test_docs_claims.py: MUST_PASS / MUST_FAIL Makefiles and
workflow snippets guard the guard, so a reader that stops seeing its own
failure mode is a red test, not a silent hole.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_host_test_lists.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import os
import posixpath
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from dataclasses import dataclass, field
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
# Triggers that run a workflow before a change merges.
PR_TRIGGERS = {"pull_request", "merge_group"}

SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx")
# What a compiler reads: C++ and C sources, objects and archives.
OBJECT_SUFFIXES = SOURCE_SUFFIXES + (".c", ".o", ".a")
# Shell reserved words, recognized in command position only.
KEYWORDS = {"if", "then", "else", "elif", "fi", "do", "done", "while", "until",
            "!", "{", "}", "time", "case", "esac"}
CONDITION_KEYWORDS = {"if", "elif", "while", "until", "!"}
REDIRECT = re.compile(r"^(?:[<>]{1,3}|[<>]&|&>{1,2}|>\||<>)$")
ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")
# A C or C++ compiler driver: an optional directory and target prefix, the
# driver, an optional version suffix; or $CXX / $CC, bare or in ${...}.
COMPILER = re.compile(
    r"""(?:^|[\s;&|(`"'/])(?:[\w.]+-)*(?:g\+\+|clang\+\+|c\+\+|gcc|clang|cc)(?:-[\d.]+)?"""
    r"""(?=[\s;&|)`"']|$)|\$\{?(?:CXX|CC)\b""", re.M)
# A C++ source path with a tests_host directory in it (globs allowed).
TESTS_HOST_SOURCE = re.compile(r"[\w./${}*?\[\]-]*tests_host/[\w./${}*?\[\]-]*\.(?:cpp|cc|cxx)\b")
SOURCE_WORD = re.compile(r"[^\s=]*\.(?:cpp|cc|cxx)")
# NAME=<path containing tests_host>, as a shell assignment in a run: block.
TESTS_HOST_VAR = re.compile(r"""(?:^|[\s;&|(])([A-Za-z_]\w*)=["']?([\w./-]*tests_host[\w./-]*)""", re.M)
WORKSPACE = re.compile(r"^(?:\$\{?GITHUB_WORKSPACE\}?|\$\{\{\s*github\.workspace\s*\}\})/?")
EXPRESSION = re.compile(r"\$\{\{.*?\}\}")
LIST_TARGETS = {"all", "run"}
MAKE_COMMANDS = {"make", "gmake"}
# make options that take a separate value, and options that stop make running
# the recipes (-n, -q, -t) or stop a failing recipe failing make (-i).
MAKE_VALUE_OPTIONS = {"-f", "-I", "-o", "-W", "--file", "--makefile", "--include-dir",
                      "--old-file", "--assume-old", "--what-if", "--new-file", "--assume-new"}
MAKE_NO_RUN_SHORT = {"n": "-n", "q": "-q", "t": "-t", "i": "-i"}
MAKE_NO_RUN_LONG = {"--just-print", "--dry-run", "--recon", "--question", "--touch",
                    "--ignore-errors"}
COMMAND_WRAPPERS = {"env", "sudo", "nice", "command", "exec"}


# ── shell reading (shared by the make plan and the workflows) ──────────────

def logical_lines(text: str) -> list[str]:
    """Join backslash-newline continuations, then split into lines."""
    return re.sub(r"\\\n", " ", text).splitlines()


def shell_items(line: str) -> list[tuple[str, object]]:
    """One shell line as ("cmd", words), ("op", separator) and ("kw", word).

    A separator is any run of ; & | ( ) (so `out=$(./t)` puts `./t` in
    command position); a redirection and its target are dropped, so
    `./t > log` is the command `./t`. This is a reader for CI recipes, not a
    shell: it only has to find commands, their arguments, and what follows
    each one.
    """
    lex = shlex.shlex(line, posix=True, punctuation_chars=True)
    lex.whitespace_split = True
    lex.commenters = ""
    try:
        tokens = list(lex)
    except ValueError:  # an unbalanced quote: fall back to plain words
        tokens = line.split()
    items: list[tuple[str, object]] = []
    words: list[str] = []
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if REDIRECT.match(tok):
            if words and words[-1].isdigit():  # the 2 of 2>&1
                words.pop()
            i += 2
            continue
        if not words and tok in KEYWORDS:
            items.append(("kw", tok))
        elif tok and set(tok) <= set(";&|()<>"):
            if words:
                items.append(("cmd", words))
                words = []
            items.append(("op", tok))
        else:
            words.append(tok)
        i += 1
    if words:
        items.append(("cmd", words))
    return items


def script_items(lines: list[str]) -> list[tuple[str, object]]:
    """Several shell lines as one item list, each line ending in a `;`."""
    items: list[tuple[str, object]] = []
    for line in lines:
        part = shell_items(line)
        if part:
            items.extend(part)
            items.append(("op", ";"))
    return items


def command_word(words: list[str]) -> tuple[str | None, list[str]]:
    """(command, arguments) with leading VAR=value assignments skipped."""
    i = 0
    while i < len(words) and ASSIGNMENT.match(words[i]):
        i += 1
    if i == len(words):
        return None, []
    return words[i], words[i + 1:]


def _skip_command(items: list, j: int) -> int:
    """The index after the command (or { } / ( ) group) that starts at j."""
    while j < len(items) and items[j][0] == "kw" and items[j][1] in CONDITION_KEYWORDS:
        j += 1
    if j >= len(items):
        return j
    kind, val = items[j]
    if (kind, val) in (("kw", "{"), ("op", "(")):
        depth = 0
        while j < len(items):
            kind, val = items[j]
            if (kind, val) == ("kw", "{") or (kind == "op" and val.endswith("(")):
                depth += 1
            elif (kind, val) == ("kw", "}") or (kind == "op" and val.startswith(")")):
                depth -= 1
                if depth == 0:
                    return j + 1
            j += 1
        return j
    return j + 1 if kind == "cmd" else j


def _fails(words: list[str]) -> bool:
    cmd, args = command_word(words)
    if cmd == "false":
        return True
    if cmd in ("exit", "return"):
        return not args or args[0] != "0"
    return False


def _alternative_fails(items: list, j: int) -> bool:
    """Whether the command after `||` (at j) fails the line in its turn."""
    end = _skip_command(items, j)
    return any(kind == "cmd" and _fails(val) for kind, val in items[j:end])


def status_reaches(items: list, k: int, *, errexit: bool, pipefail: bool) -> tuple[bool, str]:
    """Whether a failure of the command at items[k] fails the shell line.

    Returns (True, "") or (False, why not).
    """
    if k and items[k - 1][0] == "kw" and items[k - 1][1] in CONDITION_KEYWORDS:
        return False, f"it is a condition (`{items[k - 1][1]}`), not a command whose failure stops the line"
    j = k + 1
    while j < len(items):
        kind, val = items[j]
        if kind == "kw":
            if val in ("}", "fi", "esac"):  # a compound returns its last command's status
                j += 1
                continue
            if val in ("else", "elif"):  # the other branches do not run: on to the `fi`
                depth = 0
                while j < len(items):
                    if items[j] == ("kw", "if"):
                        depth += 1
                    elif items[j] == ("kw", "fi"):
                        if depth == 0:
                            break
                        depth -= 1
                    j += 1
                continue
            if val == "done":
                return False, ("it runs in a loop that keeps only the last iteration's status "
                               "(add `|| exit 1`)")
            return False, f"`{val}` follows it and nothing reads its status"
        if kind == "cmd":
            j += 1
            continue
        op = val.lstrip(")")  # `$(./t)` and `( ./t )` pass the status out
        if not op:
            j += 1
            continue
        if op in ("|", "|&"):
            if not pipefail:
                return False, "it is piped into another command without `set -o pipefail`"
            j += 1
            while True:  # to the end of the pipeline
                j = _skip_command(items, j)
                if j < len(items) and items[j] in (("op", "|"), ("op", "|&")):
                    j += 1
                    continue
                break
            continue
        if op == "&&":
            # The next command runs only on success; a failure carries past
            # it, and `set -e` does not act on the left of an && list.
            j = _skip_command(items, j + 1)
            errexit = False
            continue
        if op == "||":
            if _alternative_fails(items, j + 1):
                return True, ""
            alt = next((" ".join(v) for kd, v in items[j + 1:] if kd == "cmd"), "")
            return False, f"`|| {alt}` follows it, and that succeeds"
        if op.startswith(";"):
            if errexit:
                return True, ""
            nxt = items[j + 1] if j + 1 < len(items) else None
            if nxt is None or (nxt[0] == "cmd" and any("$?" in w for w in nxt[1])):
                return True, ""
            if nxt[0] == "kw":
                j += 1
                continue
            return False, "`;` follows it and nothing reads its status (no `$?`, no `set -e`)"
        if op == "&":
            return False, "it runs in the background"
        j += 1
    return True, ""


@dataclass
class Command:
    cmd: str | None
    args: list[str]
    words: list[str]
    cwd: str | None  # the directory it runs in, relative to the start; None: unknown
    gated: bool      # a failure here fails the shell line
    why: str         # why not, when it does not


def _subst(word: str, variables: dict[str, str]) -> str:
    for name, value in variables.items():
        word = re.sub(r"\$(?:\{" + name + r"\}|" + name + r"(?![A-Za-z0-9_]))",
                      value.replace("\\", "\\\\"), word)
    return word


def _chdir(cwd: str | None, target: str | None) -> str | None:
    if target is None:
        return None
    target = WORKSPACE.sub("", target) or "."
    if target in ("-", "~") or "$" in target or "`" in target:
        return None
    if target.startswith("/"):
        return posixpath.normpath(target)
    if cwd is None:
        return None
    return posixpath.normpath(posixpath.join(cwd, target))


def walk(items: list, *, cwd: str | None = ".", errexit: bool = False,
         pipefail: bool = False) -> list[Command]:
    """Every command in an item list, with its directory and whether a
    failure there fails the line. A command in a for loop that names the loop
    variable comes out once per loop value, with the value substituted."""
    variables: dict[str, str] = {}
    loops: list[tuple[str, list[str]]] = []
    dirstack: list[str | None] = []
    subshells: list[str | None] = []  # a `cd` inside ( ) or $( ) stays there
    out: list[Command] = []
    for k, (kind, val) in enumerate(items):
        if kind == "kw":
            if val == "done" and loops:
                loops.pop()
            continue
        if kind == "op":
            for ch in val:
                if ch == "(":
                    subshells.append(cwd)
                elif ch == ")" and subshells:
                    cwd = subshells.pop()
            continue
        if kind != "cmd":
            continue
        words = [_subst(w, variables) for w in val]
        cmd, args = command_word(words)
        if cmd is None:  # NAME=value: remember a plain value, forget any other
            for w in words:
                name, _eq, value = w.partition("=")
                if value and "$" not in value and "`" not in value:
                    variables[name] = value
                else:
                    variables.pop(name, None)
            continue
        while cmd in COMMAND_WRAPPERS and args:
            rest = [a for a in args if not (a.startswith("-") or ASSIGNMENT.match(a))]
            if not rest:
                break
            cmd, args = rest[0], args[args.index(rest[0]) + 1:]
        gated, why = status_reaches(items, k, errexit=errexit, pipefail=pipefail)
        if cmd in ("set",):
            for flag in args:
                if flag.startswith("-") and not flag.startswith("--") and "e" in flag[1:]:
                    errexit = True
                if flag.startswith("+") and "e" in flag[1:]:
                    errexit = False
            if "pipefail" in args:
                pipefail = args[args.index("pipefail") - 1].startswith("-")
            if "errexit" in args:
                errexit = args[args.index("errexit") - 1].startswith("-")
        if cmd == "for" and "in" in args:
            loops.append((args[0], args[args.index("in") + 1:]))
        variants = [words]
        for var, values in loops:
            ref = re.compile(r"\$(?:\{" + re.escape(var) + r"\}|" + re.escape(var) + r"(?![A-Za-z0-9_]))")
            if cmd != "for" and any(ref.search(w) for w in words):
                variants = [[ref.sub(v.replace("\\", "\\\\"), w) for w in ws]
                            for ws in variants for v in values]
        for ws in variants:
            v_cmd, v_args = command_word(ws)
            out.append(Command(v_cmd, v_args, ws, cwd, gated, why))
        if cmd == "cd":
            cwd = _chdir(cwd, args[0] if args else None)
        elif cmd == "pushd":
            dirstack.append(cwd)
            cwd = _chdir(cwd, args[0] if args else None)
        elif cmd == "popd":
            cwd = dirstack.pop() if dirstack else None
    return out


# ── the make plan ──────────────────────────────────────────────────────────

# --trace: "Makefile:12: update target 'x' due to: ..." or "...: target 'x' does not exist"
TRACE = re.compile(r"^(?P<loc>.+?): (?:update )?target '(?P<target>[^']*)'")
DB_START = re.compile(r"^# Make data base, printed on ")
RECIPE_FROM = re.compile(r"^#  recipe to execute \(from '(?P<file>[^']+)', line (?P<line>\d+)\):$")


IGNORES_LINE = "__CI3_IGNORES__\t"  # make_plan's own line: "<tag><loc>\t<recipe line>"


def _make_env() -> dict[str, str]:
    env = {k: v for k, v in os.environ.items()
           if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEFILES", "GNUMAKEFLAGS")}
    env["LC_ALL"] = "C"
    return env


def make_plan(makedir: Path) -> str:
    """What `make` prints for the default goal: the fully expanded commands,
    each target's recipe location (--trace), then its rule database (-p).

    A recipe line whose prefix comes out of a variable (`$(IGN)./t` with
    IGN = -) shows no `-` in either: make strips the prefix after expanding,
    and the database holds the text unexpanded. So the references a recipe
    line starts with are expanded by a second read of the Makefile (an extra
    `-f` holding `$(info ...)` lines), and each line whose expanded prefix
    has a `-` is appended as an IGNORES_LINE.
    """
    proc = subprocess.run(
        ["make", "--no-print-directory", "-n", "-B", "--trace", "-p", "-C", str(makedir)],
        capture_output=True, text=True, env=_make_env(), timeout=120, check=False,
    )
    if proc.returncode != 0:
        raise AssertionError(
            f"`make -n -B -C {makedir}` failed ({proc.returncode}); a Makefile "
            f"make cannot plan is a Makefile CI cannot run:\n{proc.stderr}")
    lines = proc.stdout.splitlines()
    start = next((n for n, ln in enumerate(lines) if DB_START.match(ln)), len(lines))
    leading = []  # (loc, recipe line, the references it starts with)
    for loc, line in db_recipes(lines[start:]):
        refs = _leading_references(line)
        if refs and "#" not in refs and "\n" not in refs:
            leading.append((loc, line, refs))
    extra = []
    makefile = next((n for n in ("GNUmakefile", "makefile", "Makefile") if (makedir / n).is_file()), None)
    if leading and makefile:
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / "prefix_probe.mk"
            probe.write_text("".join(f"$(info __CI3_PREFIX__ {n} [{refs}])\n"
                                     for n, (_loc, _line, refs) in enumerate(leading))
                             + "__ci3_prefix_probe__: ;\n", encoding="utf-8")
            out = subprocess.run(
                ["make", "--no-print-directory", "-n", "-C", str(makedir), "-f", makefile,
                 "-f", str(probe), "__ci3_prefix_probe__"],
                capture_output=True, text=True, env=_make_env(), timeout=120, check=False).stdout
        for m in re.finditer(r"^__CI3_PREFIX__ (\d+) \[(.*)$", out, re.M):
            loc, line, _refs = leading[int(m.group(1))]
            literal = line[:len(line) - len(line.lstrip("@+- \t"))]
            expanded = m.group(2)
            if "-" in literal + expanded[:len(expanded) - len(expanded.lstrip("@+- \t"))]:
                extra.append(f"{IGNORES_LINE}{loc}\t{line}")
    return proc.stdout + "".join(f"\n{e}" for e in extra) + ("\n" if extra else "")


def _paren_end(text: str, i: int) -> int:
    """The index of the bracket that closes the one at text[i]."""
    opener = text[i]
    closer = ")" if opener == "(" else "}"
    depth = 0
    for j in range(i, len(text)):
        depth += (text[j] == opener) - (text[j] == closer)
        if depth == 0:
            return j
    return len(text) - 1


def _leading_references(line: str) -> str:
    """The variable references a recipe line starts with, among its prefix
    characters: `$(Q)$(IGN)` of `@$(Q)$(IGN)./t`."""
    refs = []
    i = 0
    while i < len(line):
        if line[i] in "@+- \t":
            i += 1
        elif line[i] == "$" and line[i + 1:i + 2] in ("(", "{"):
            j = _paren_end(line, i + 1)
            refs.append(line[i:j + 1])
            i = j + 1
        elif line[i] == "$" and line[i + 1:i + 2] not in ("", "$"):
            refs.append(line[i:i + 2])
            i += 2
        else:
            break
    return "".join(refs)


def db_recipes(db: list[str]) -> list[tuple[str, str]]:
    """[(recipe location, logical recipe line)] from make's -p database."""
    found: list[tuple[str, str]] = []
    n = 0
    while n < len(db):
        m = RECIPE_FROM.match(db[n])
        n += 1
        if not m:
            continue
        recipe: list[str] = []
        while n < len(db) and (db[n].startswith("\t") or (recipe and recipe[-1].endswith("\\"))):
            if recipe and recipe[-1].endswith("\\"):
                recipe[-1] = recipe[-1][:-1] + " " + db[n].lstrip("\t")
            else:
                recipe.append(db[n][1:])
            n += 1
        found.extend((f"{m['file']}:{m['line']}", r) for r in recipe)
    return found


@dataclass
class MakePlan:
    # (recipe location "file:line", target, command line), in plan order
    commands: list[tuple[str, str, str]] = field(default_factory=list)
    # recipe location -> [(recipe line make runs with errors ignored (`-`),
    # a pattern for what that line can expand to)]
    ignoring: dict[str, list[tuple[str, re.Pattern]]] = field(default_factory=dict)
    ignore_targets: set[str] = field(default_factory=set)  # .IGNORE prerequisites
    ignore_all: str = ""  # why make ignores every recipe's errors, or ""
    errexit: bool = False  # -e in .SHELLFLAGS
    pipefail: bool = False  # pipefail in .SHELLFLAGS
    oneshell: bool = False

    def ignored(self, loc: str, target: str, line: str) -> str:
        """Why make ignores a failure of this command line, or ""."""
        if self.ignore_all:
            return self.ignore_all
        if target in self.ignore_targets:
            return f"`.IGNORE: {target}` ignores its recipe's errors"
        flat = " ".join(line.split())
        for text, pattern in self.ignoring.get(loc, []):
            if pattern.fullmatch(flat):
                return f"its recipe line ({loc}) has a `-` prefix: `{text}`"
        return ""

    def scripts(self) -> list[tuple[str, list[tuple[str, object]]]]:
        """(why make ignores its failure or "", items) per shell make starts:
        one per command line, or one per recipe under .ONESHELL."""
        if not self.oneshell:
            return [(self.ignored(loc, t, line), shell_items(line)) for loc, t, line in self.commands]
        grouped: list[tuple[str, str, list[str]]] = []
        for loc, t, line in self.commands:
            if grouped and grouped[-1][:2] == (loc, t):
                grouped[-1][2].append(line)
            else:
                grouped.append((loc, t, [line]))
        return [(next(filter(None, (self.ignored(loc, t, ln) for ln in lines)), ""), script_items(lines))
                for loc, t, lines in grouped]


def _expansion_pattern(text: str) -> re.Pattern:
    """What a recipe line can expand to: its literal text, with any string
    for each $(...), ${...} or $x reference and any spacing around them."""
    out = []
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "$" and text[i + 1:i + 2] == "$":
            out.append(re.escape("$"))
            i += 2
        elif ch == "$" and text[i + 1:i + 2] in ("(", "{"):
            opener = text[i + 1]
            closer = ")" if opener == "(" else "}"
            depth, j = 0, i + 1
            while j < len(text):
                depth += (text[j] == opener) - (text[j] == closer)
                if depth == 0:
                    break
                j += 1
            out.append(".*?")
            i = j + 1
        elif ch == "$":
            out.append(".*?")
            i += 2
        elif ch.isspace():
            out.append(r"\s*")
            i += 1
        else:
            out.append(re.escape(ch))
            i += 1
    return re.compile("".join(out), re.S)


def read_plan(text: str) -> MakePlan:
    lines = text.splitlines()
    start = next((n for n, ln in enumerate(lines) if DB_START.match(ln)), len(lines))
    head = max((n for n in range(start) if lines[n].startswith("# GNU Make ")), default=start)
    plan = MakePlan()
    loc, target = "", ""
    for line in logical_lines("\n".join(lines[:head])):
        m = TRACE.match(line)
        if m:
            loc, target = m["loc"], m["target"]
        elif line.strip():
            plan.commands.append((loc, target, line))
    db = lines[start:]
    for where, r in db_recipes(db):
        prefix = r[:len(r) - len(r.lstrip("@+- \t"))]
        if "-" in prefix:
            plan.ignoring.setdefault(where, []).append((r.strip(), _expansion_pattern(r[len(prefix):])))
    for line in db:
        if line.startswith(IGNORES_LINE):
            where, r = line[len(IGNORES_LINE):].split("\t", 1)
            prefix = r[:len(r) - len(r.lstrip("@+- \t"))]
            plan.ignoring.setdefault(where, []).append((r.strip(), _expansion_pattern(r[len(prefix):])))
    n = 0
    while n < len(db):
        line = db[n]
        if line.startswith("MAKEFLAGS = ") or line.startswith("MAKEFLAGS := "):
            first = (line.split("=", 1)[1].split() or [""])[0]
            if not first.startswith("--") and "i" in first.lstrip("-"):
                plan.ignore_all = "MAKEFLAGS has -i, so make ignores every recipe's errors"
        elif line.startswith(".IGNORE:"):
            prereqs = line.split(":", 1)[1].split()
            if prereqs:
                plan.ignore_targets.update(prereqs)
            else:
                plan.ignore_all = "`.IGNORE:` with no targets ignores every recipe's errors"
        elif line.startswith(".ONESHELL:"):
            plan.oneshell = True
        elif re.match(r"^\.SHELLFLAGS :?= ", line):
            flags = line.split("=", 1)[1].split()
            plan.errexit = any(f.startswith("-") and not f.startswith("--") and "e" in f[1:]
                               for f in flags) or "errexit" in flags
            plan.pipefail = "pipefail" in flags
        n += 1
    return plan


def _resolve(makedir: Path, cwd: str | None, word: str) -> Path:
    base = makedir if cwd is None or cwd == "." else makedir / cwd
    return Path(os.path.normpath(base / word))


def unrun_suites(makedir: Path, sources: list[Path], plan_text: str) -> dict[Path, str]:
    """{source: why} for every source the plan does not compile AND run in a
    way whose failure fails make."""
    plan = read_plan(plan_text)
    wanted = {Path(os.path.normpath(s)) for s in sources}
    built: dict[Path, set[Path]] = {s: set() for s in wanted}
    made_from: dict[Path, set[Path]] = {}  # an output -> the suites in it
    ran: set[Path] = set()
    swallowed: dict[Path, str] = {}  # a run whose failure cannot fail make -> why
    for ignored, items in plan.scripts():
        for c in walk(items, errexit=plan.errexit, pipefail=plan.pipefail):
            if c.cmd is None:
                continue
            args = c.args
            outputs = [args[i + 1] for i, w in enumerate(args[:-1]) if w == "-o"]
            outputs += [w[2:] for w in args if w.startswith("-o") and len(w) > 2]
            inputs = [_resolve(makedir, c.cwd, w) for w in args if not w.startswith("-")]
            if outputs and any(i in made_from or str(i).endswith(OBJECT_SUFFIXES)
                               for i in inputs):
                # A compile (or a link of earlier objects): whatever suite
                # went in comes out in each output.
                suites = set()
                for i in inputs:
                    suites |= {i} & wanted
                    suites |= made_from.get(i, set())
                for out in (_resolve(makedir, c.cwd, o) for o in outputs):
                    made_from.setdefault(out, set()).update(suites)
                    for suite in suites:
                        built[suite].add(out)
                continue
            path = _resolve(makedir, c.cwd, c.cmd)
            if c.gated and not ignored:
                ran.add(path)
            else:
                swallowed.setdefault(path, ignored or c.why)
    why: dict[Path, str] = {}
    for src, bins in sorted(built.items()):
        names = ", ".join(sorted(os.path.relpath(b, makedir) for b in bins))
        lost = sorted({swallowed[b] for b in bins if b in swallowed})
        if not bins:
            why[src] = "no command in the Makefile's default goal compiles it"
        elif bins & ran:
            continue
        elif lost:
            why[src] = f"compiled to {names} and run, but a failure there cannot fail make: " + "; ".join(lost)
        else:
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

@dataclass
class YNode:
    """One key or sequence item of a block-style YAML file."""
    key: str | None
    value: str
    line: int
    indent: float
    parent: YNode | None = None
    children: list[YNode] = field(default_factory=list)
    item: bool = False

    def child(self, key: str) -> YNode | None:
        return next((c for c in self.children if c.key == key), None)

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()


YAML_KEY = re.compile(r"""^(?P<key>[\w.-]+|"[^"]*"|'[^']*')\s*:(?:\s+(?P<val>.*?))?\s*$""")
BLOCK_SCALAR = re.compile(r"^[|>][-+0-9]*\s*(?:#.*)?$")


def yaml_outline(text: str) -> YNode:
    """The key/item tree of a block-style YAML file, with line numbers.

    Enough for workflow files: keys, sequence items (indented or not) and
    block scalars (`run: |`). Flow collections stay as their text.
    """
    lines = text.splitlines()
    root = YNode(None, "", 0, -1.0)
    stack = [root]

    def attach(node: YNode) -> None:
        while stack[-1].indent >= node.indent:
            stack.pop()
        node.parent = stack[-1]
        stack[-1].children.append(node)
        stack.append(node)

    i = 0
    while i < len(lines):
        raw = lines[i]
        if not raw.strip() or raw.strip().startswith("#"):
            i += 1
            continue
        col = len(raw) - len(raw.lstrip(" "))
        content = raw[col:]
        if content == "-" or content.startswith("- "):
            attach(YNode(None, "", i + 1, col + 0.5, item=True))
            rest = content[1:].lstrip(" ")
            if not rest:
                i += 1
                continue
            col += len(content) - len(rest)
            content = rest
        m = YAML_KEY.match(content)
        if not m:  # an item's scalar, or a plain scalar's next line
            if stack[-1].indent < col:
                stack[-1].value = (stack[-1].value + " " + content).strip()
            i += 1
            continue
        node = YNode(m["key"].strip("\"'"), "", i + 1, float(col))
        attach(node)
        val = m["val"] or ""
        if BLOCK_SCALAR.match(val):
            j = i + 1
            while j < len(lines) and (not lines[j].strip()
                                      or len(lines[j]) - len(lines[j].lstrip(" ")) > col):
                j += 1
            node.value = "\n".join(lines[i + 1:j])
            i = j
            continue
        node.value = val
        i += 1
    return root


def _scalar(node: YNode | None) -> str:
    """A node's value as plain text: no quotes, no trailing comment, no ${{ }}."""
    if node is None:
        return ""
    v = node.value.strip()
    if v[:1] in ("'", '"') and v[:1] in v[1:]:
        v = v[1:v.index(v[0], 1)]
    else:
        v = re.sub(r"\s+#.*$", "", v)
    m = re.fullmatch(r"\$\{\{\s*(.*?)\s*\}\}", v)
    return m.group(1) if m else v


def _ancestors(node: YNode | None):
    while node is not None:
        yield node
        node = node.parent


@dataclass
class Step:
    line: int       # the run: line
    script: str
    workdir: str | None  # repo-relative ("." = the root); None: an expression
    shell: str
    skip: str       # why a failure here cannot fail the run ("" when it can)


def steps(text: str) -> list[Step]:
    """Every `run:` step of a workflow or composite action."""
    found = []
    for node in yaml_outline(text).walk():
        run = node.child("run")
        if run is None or run.children or not run.value.strip():
            continue
        workdir: str | None = "."
        have_workdir = False
        shell = ""
        skip = ""
        for anc in _ancestors(node):
            if anc is node:
                wd, sh = anc.child("working-directory"), anc.child("shell")
            else:
                defaults = anc.child("defaults")
                drun = defaults.child("run") if defaults else None
                wd = drun.child("working-directory") if drun else None
                sh = drun.child("shell") if drun else None
            if wd is not None and not have_workdir:
                have_workdir = True
                value = WORKSPACE.sub("", _scalar(wd))
                workdir = None if "${{" in value else posixpath.normpath(value or ".")
            if sh is not None and not shell:
                shell = _scalar(sh)
            if not skip and _scalar(anc.child("if")).lower() == "false":
                skip = f"`if: false` at line {anc.child('if').line}"
            if not skip and _scalar(anc.child("continue-on-error")).lower() == "true":
                skip = f"`continue-on-error: true` at line {anc.child('continue-on-error').line}"
        found.append(Step(run.line, run.value, workdir, shell, skip))
    return found


def _shell_modes(shell: str) -> tuple[bool, bool] | None:
    """(errexit, pipefail) for a step's shell; None when it is not a POSIX shell."""
    if shell in ("", "bash"):
        return True, True  # bash --noprofile --norc -eo pipefail {0}
    if shell == "sh":
        return True, False  # sh -e {0}
    if "{0}" in shell and re.match(r"^\S*(?:ba)?sh\b", shell):
        flags = shell.split()
        errexit = any(f.startswith("-") and not f.startswith("--") and "e" in f[1:] for f in flags)
        return errexit or "errexit" in flags, "pipefail" in flags
    return None


def _script_lines(script: str) -> list[str]:
    code = "\n".join(ln for ln in script.splitlines() if not ln.lstrip().startswith("#"))
    code = re.sub(r"\$\{\{\s*github\.workspace\s*\}\}", "$GITHUB_WORKSPACE", code)
    code = EXPRESSION.sub("__EXPR__", code)
    return logical_lines(code)


def _expand_tests_host_vars(code: str) -> str:
    """Substitute shell variables that hold a tests_host path (D=.../tests_host
    then $D/test_x.cpp), so a path split across a variable is still seen."""
    for name, value in TESTS_HOST_VAR.findall(code):
        code = re.sub(r"\$\{?" + name + r"\b\}?", lambda _m: value, code)
    return code


def _in_tests_host(path: str) -> bool:
    return "tests_host" in path.split("/")[:-1]


def step_compiles(step: Step) -> list[str]:
    """The tests_host sources a step compiles outside make."""
    lines = _script_lines(step.script)
    code = "\n".join(lines)
    if not COMPILER.search(code):
        return []
    hits: set[str] = set()
    modes = _shell_modes(step.shell) or (True, True)
    for c in walk(script_items(lines), cwd=step.workdir, errexit=modes[0], pipefail=modes[1]):
        for w in c.words:
            if w.startswith("-") or not SOURCE_WORD.fullmatch(w):
                continue
            w = WORKSPACE.sub("", w)
            if c.cwd is None or w.startswith("/"):
                p = posixpath.normpath(w)
            else:
                p = posixpath.normpath(posixpath.join(c.cwd, w))
            if _in_tests_host(p):
                hits.add(p)
    for src in TESTS_HOST_SOURCE.findall(_expand_tests_host_vars(code)):
        if not any(h == src or h.endswith("/" + src) for h in hits):
            hits.add(src)
    return sorted(hits)


def inline_compiles(text: str) -> list[tuple[int, str]]:
    """[(run: line, tests_host source)] a workflow compiles outside make."""
    return [(s.line, src) for s in steps(text) for src in step_compiles(s)]


def _make_call(args: list[str], cwd: str | None) -> tuple[str | None, list[str], str]:
    """(directory, targets, why it does not run the list) for make's arguments."""
    directory, targets, problem = cwd, [], ""
    i = 0
    while i < len(args):
        w = args[i]
        if w in ("-C", "--directory"):
            directory = _chdir(directory, args[i + 1] if i + 1 < len(args) else None)
            i += 2
            continue
        if w in MAKE_VALUE_OPTIONS:
            i += 2
            continue
        if w.startswith("--directory="):
            directory = _chdir(directory, w.split("=", 1)[1])
        elif w.startswith("--"):
            if w.split("=", 1)[0] in MAKE_NO_RUN_LONG:
                problem = problem or f"`{w}` stops a failing suite failing make"
        elif w.startswith("-") and len(w) > 1:
            for n, ch in enumerate(w[1:], 1):
                attached = w[n + 1:]
                if ch == "C":
                    if not attached:
                        i += 1
                    directory = _chdir(directory, attached or (args[i] if i < len(args) else None))
                    break
                if ch in "fIoW":
                    if not attached:
                        i += 1
                    break
                if ch in "jl":
                    if not attached and i + 1 < len(args) and re.fullmatch(r"[\d.]+", args[i + 1]):
                        i += 1
                    break
                if ch in "OE":
                    break
                if ch in MAKE_NO_RUN_SHORT:
                    problem = problem or f"`{MAKE_NO_RUN_SHORT[ch]}` stops a failing suite failing make"
        elif "=" not in w:
            targets.append(w)
        i += 1
    if not set(targets) <= LIST_TARGETS:
        problem = problem or f"it builds `{' '.join(targets)}`, not the list"
    return directory, targets, problem


def make_calls(text: str) -> list[tuple[int, str, str]]:
    """[(run: line, directory, why it does not count)] for every `make` in
    a workflow's steps, the reason empty when it runs a Makefile's list."""
    calls = []
    for step in steps(text):
        modes = _shell_modes(step.shell)
        if modes is None:
            continue
        for c in walk(script_items(_script_lines(step.script)), cwd=step.workdir,
                      errexit=modes[0], pipefail=modes[1]):
            if c.cmd is None or posixpath.basename(c.cmd) not in MAKE_COMMANDS:
                continue
            directory, _targets, problem = _make_call(c.args, c.cwd)
            if directory is None:
                continue
            problem = problem or step.skip or ("" if c.gated else c.why)
            calls.append((step.line, posixpath.normpath(directory), problem))
    return calls


def make_list_dirs(text: str) -> set[str]:
    """Directories a workflow runs a Makefile's whole list in (triggers aside)."""
    return {d for _ln, d, problem in make_calls(text) if not problem}


def triggers(text: str) -> set[str]:
    on = yaml_outline(text).child("on")
    if on is None:
        return set()
    names = set(re.findall(r"[\w-]+", on.value))
    names |= {c.key or _scalar(c) for c in on.children}
    return names


def local_uses(text: str) -> set[str]:
    """Repo paths a workflow or action uses locally (`uses: ./...`)."""
    found = set()
    for node in yaml_outline(text).walk():
        if node.key == "uses" and _scalar(node).startswith("./"):
            found.add(posixpath.normpath(_scalar(node).split("@", 1)[0]))
    return found


def pr_reaching(files: dict[str, str]) -> set[str]:
    """The files among {repo path: text} that pull requests run: workflows
    triggered by one, and the reusable workflows and actions they use."""
    reach = {f for f, text in files.items()
             if f.startswith(".github/workflows/") and triggers(text) & PR_TRIGGERS}
    todo = list(reach)
    while todo:
        for used in local_uses(files[todo.pop()]):
            for f in (used, used + "/action.yml", used + "/action.yaml"):
                if f in files and f not in reach:
                    reach.add(f)
                    todo.append(f)
    return reach


def ci_make_dirs(files: dict[str, str]) -> tuple[set[str], dict[str, list[str]]]:
    """(directories whose list a pull request runs, {directory: why each
    other `make` there does not count})."""
    reach = pr_reaching(files)
    counted: set[str] = set()
    near: dict[str, list[str]] = {}
    for f, text in sorted(files.items()):
        for line, directory, problem in make_calls(text):
            if not problem and f not in reach:
                problem = "the file is not a workflow pull requests run, nor used by one"
            if problem:
                near.setdefault(directory, []).append(f"{f}:{line}: {problem}")
            else:
                counted.add(directory)
    return counted, near


def workflow_files(root: Path = REPO) -> list[Path]:
    return sorted(p for pattern in WORKFLOW_GLOBS for p in root.glob(pattern))


# ── the guard itself ───────────────────────────────────────────────────────

# The streaming marker check the four project Makefiles use (see run_marked
# in firmware/projects/canary-wap/tests_host/Makefile).
RUN_MARKED = (
    "run_marked = @echo \"./$(1)\"; t=$$(mktemp) || exit 1; \\\n"
    "  { ./$(1); echo $$? > \"$$t.rc\"; } | tee \"$$t\"; s=$$(cat \"$$t.rc\"); \\\n"
    "  grep -q \"ALL .* PASSED\" \"$$t\"; m=$$?; rm -f \"$$t\" \"$$t.rc\"; \\\n"
    "  [ \"$$s\" = 0 ] || { echo \"$(1): exit $$s\" >&2; exit 1; }; \\\n"
    "  [ $$m -eq 0 ] || { echo \"$(1): no marker\" >&2; exit 1; }\n"
)

# Synthetic Makefiles: {name: (Makefile text, sources, sources that must be
# reported)}. Recipe lines start with a TAB.
MAKEFILES = {
    # MUST_PASS: the shared `run:` list (the firmware/tests_host style).
    "shared list": (
        "BIN = build/test_a\n"
        "all: run\n"
        "build:\n\tmkdir -p build\n"
        "$(BIN): test_a.cpp | build\n\t$(CXX) -o $@ test_a.cpp\n"
        "run: $(BIN)\n\t./$(BIN)\n",
        ["test_a.cpp"], []),
    # MUST_PASS: a prerequisite hook (the canary-wap style), a suite run
    # through the streaming marker check, and one whose status `$?` reads.
    "prerequisite hook": (
        RUN_MARKED +
        "all: run\n"
        "test_a: test_a.cpp\n\t$(CXX) -o $@ $<\n"
        "test_b: test_b.cpp\n\t$(CXX) -Werror -o $@ test_b.cpp -lcrypto\n"
        "test_c: test_c.cpp\n\t$(CXX) -o $@ test_c.cpp\n"
        "run: test_a\n\t./test_a\n"
        ".PHONY: run-b\nrun-b: test_b test_c\n\t$(call run_marked,test_b)\n"
        "\t@out=$$(./test_c); s=$$?; printf '%s\\n' \"$$out\"; [ $$s -eq 0 ]\n"
        "run: run-b\n",
        ["test_a.cpp", "test_b.cpp", "test_c.cpp"], []),
    # MUST_PASS: one source built several ways and run from a for loop (the
    # canary-wap CSI config-shim style), `|| exit 1` and a { } group, a suite
    # last in a { } group and in an `if` branch (each returns its status),
    # and an `@` prefix that comes out of $(Q).
    "for loop": (
        "BINS = t_one t_two\n"
        "all: run\n"
        "t_one: test_s.cpp\n\t$(CXX) -DONE -o $@ test_s.cpp\n"
        "t_two: test_s.cpp\n\t$(CXX) -DTWO -o $@ test_s.cpp\n"
        "test_g: test_g.cpp\n\t$(CXX) -o $@ test_g.cpp\n"
        "test_k: test_k.cpp\n\t$(CXX) -o $@ test_k.cpp\n"
        "test_m: test_m.cpp\n\t$(CXX) -o $@ test_m.cpp\n"
        "Q = @\ntest_j: test_j.cpp\n\t$(CXX) -o $@ test_j.cpp\n"
        "run: $(BINS) test_g test_k test_m test_j\n\tfor b in $(BINS); do ./$$b || exit 1; done\n"
        "\t$(Q)./test_j\n"
        "\t./test_g || { echo test_g failed; exit 1; }\n"
        "\t{ echo k; ./test_k; }\n"
        "\tif [ -x ./test_m ]; then ./test_m; else exit 1; fi\n",
        ["test_s.cpp", "test_g.cpp", "test_k.cpp", "test_m.cpp", "test_j.cpp"], []),
    # MUST_PASS: compiled to an object, linked, then run; a pipe under
    # pipefail; a `;` under -e in .SHELLFLAGS.
    "compile then link": (
        ".SHELLFLAGS = -ec\n"
        "all: run\n"
        "test_o.o: test_o.cpp\n\t$(CXX) -c -o $@ test_o.cpp\n"
        "test_o: test_o.o\n\t$(CXX) -o $@ test_o.o -lcrypto\n"
        "test_p: test_p.cpp\n\t$(CXX) -o $@ test_p.cpp\n"
        "run: test_o test_p\n\t./test_o; echo done\n"
        "\tset -o pipefail; ./test_p | tee test_p.log\n",
        ["test_o.cpp", "test_p.cpp"], []),
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
    # MUST_FAIL: run, but a failure cannot fail make. make -n prints none of
    # the `-` prefixes; the recipe text in make's database has the written
    # ones, and the prefix probe expands the one $(IGN) makes.
    "failure swallowed": (
        "BINS = t_one t_two\nIGN = -\n"
        "all: run\n"
        "test_%: test_%.cpp\n\t$(CXX) -o $@ $<\n"
        "t_one t_two: test_s.cpp\n\t$(CXX) -o $@ test_s.cpp\n"
        "run: test_a test_b test_c test_d test_e test_f test_g test_h test_i test_v test_w test_y "
        "test_z $(BINS)\n"
        "\t-./test_a\n\t@-./test_b\n\t./test_c || true\n\t./test_d || :\n"
        "\t./test_e || echo test_e failed\n\t./test_f | tee test_f.log\n"
        "\t./test_g; echo done\n\t./test_h || exit 0\n"
        "\tfor b in $(BINS); do ./$$b; done\n\t./test_z\n"
        "\t{ ./test_w; }; echo w\n\tif true; then ./test_v; fi; echo v\n"
        "\t$(IGN)./test_i\n"
        # A target with no prerequisites: --trace says "target 'x' does not
        # exist" instead of "update target", and its `-` must still be read.
        "run: run-y\nrun-y:\n\t-./test_y\n",
        [f"test_{x}.cpp" for x in "abcdefghisvwyz"],
        [f"test_{x}.cpp" for x in "abcdefghisvwy"]),
    # MUST_FAIL: MAKEFLAGS += -i ignores every recipe's errors.
    "MAKEFLAGS -i": (
        "MAKEFLAGS += -i\n"
        "all: run\n"
        "test_a: test_a.cpp\n\t$(CXX) -o $@ test_a.cpp\n"
        "run: test_a\n\t./test_a\n",
        ["test_a.cpp"], ["test_a.cpp"]),
    # MUST_FAIL for the target .IGNORE names; the other target still counts.
    ".IGNORE": (
        ".IGNORE: run\n"
        "all: run\n"
        "test_a: test_a.cpp\n\t$(CXX) -o $@ test_a.cpp\n"
        "test_b: test_b.cpp\n\t$(CXX) -o $@ test_b.cpp\n"
        "run: test_a run-b\n\t./test_a\n"
        "run-b: test_b\n\t./test_b\n",
        ["test_a.cpp", "test_b.cpp"], ["test_a.cpp"]),
}

# Workflow snippets: (text, inline tests_host compiles expected, make -C list
# dirs expected).
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
    # MUST_FAIL: the driver with a version, a directory, a target prefix, a
    # default in ${CXX:-...}, and gcc (which compiles .cpp as C++).
    "compiler spellings": (
        "      - run: clang++-18 -std=c++17 firmware/tests_host/test_a.cpp -o /tmp/a\n"
        "      - run: g++-13 firmware/tests_host/test_b.cpp -o /tmp/b\n"
        "      - run: /usr/bin/g++ firmware/tests_host/test_c.cpp -o /tmp/c\n"
        "      - run: x86_64-linux-gnu-g++-12 firmware/tests_host/test_d.cpp -o /tmp/d\n"
        "      - run: ${CXX:-g++} firmware/tests_host/test_e.cpp -o /tmp/e\n"
        "      - run: gcc firmware/tests_host/test_f.cpp -lstdc++ -o /tmp/f\n",
        [f"firmware/tests_host/test_{x}.cpp" for x in "abcdef"], set()),
    # MUST_FAIL: a glob over the directory, in a for loop.
    "for loop over a glob": (
        "      - run: |\n"
        "          for f in firmware/projects/canary-wap/tests_host/test_*.cpp; do\n"
        "            g++ -std=c++17 \"$f\" -o /tmp/t && /tmp/t\n"
        "          done\n",
        ["firmware/projects/canary-wap/tests_host/test_*.cpp"], set()),
    # MUST_FAIL: a relative name under a working-directory, a job default,
    # a `cd` and a `pushd`. MUST_PASS: a `cd` inside $( ) or ( ) stays
    # there (the `make` in that subshell runs the list; test_top.cpp is at
    # the root).
    "relative to the directory": (
        "jobs:\n"
        "  a:\n"
        "    steps:\n"
        "      - working-directory: firmware/projects/canary-wap/tests_host\n"
        "        run: g++ -std=c++17 test_auth_logic.cpp -o /tmp/auth\n"
        "      - run: |\n"
        "          cd firmware/projects/canary-display/tests_host\n"
        "          g++ -std=c++17 -I ../include test_modbus_rtu.cpp -o /tmp/m\n"
        "      - run: |\n"
        "          pushd \"$GITHUB_WORKSPACE/firmware/tests_host\"\n"
        "          c++ test_q.cpp -o /tmp/q\n"
        "          popd\n"
        "          g++ tools/test_other.cpp -o /tmp/o\n"
        "      - run: |\n"
        "          sha=$(cd firmware/tests_host && git rev-parse HEAD)\n"
        "          (cd firmware/tests_host && make)\n"
        "          g++ test_top.cpp -o /tmp/top\n"
        "  b:\n"
        "    defaults:\n"
        "      run:\n"
        "        working-directory: ${{ github.workspace }}/firmware/projects/canary-tincan/tests_host\n"
        "    steps:\n"
        "      - run: g++ test_link_core.cpp -o /tmp/l\n"
        "      - working-directory: firmware/tools\n"
        "        run: g++ test_tool.cpp -o /tmp/t\n",
        ["firmware/projects/canary-wap/tests_host/test_auth_logic.cpp",
         "firmware/projects/canary-display/tests_host/test_modbus_rtu.cpp",
         "firmware/tests_host/test_q.cpp",
         "firmware/projects/canary-tincan/tests_host/test_link_core.cpp"],
        {"firmware/tests_host"}),
    # MUST_PASS: a non-tests_host compile, a node suite, tools that only look
    # like compilers, and the make forms that run the list.
    "make steps": (
        "      - name: pull-OTA engine\n"
        "        run: |\n"
        "          g++ -std=c++17 firmware/common/ota/test_ota_logic.cpp -o /tmp/o\n"
        "          node --test firmware/tests_host/test_canary_mesh_alerts.test.js\n"
        "          # g++ firmware/tests_host/test_commented_out.cpp\n"
        "      - run: clang-format --dry-run firmware/tests_host/test_x.cpp\n"
        "      - run: sudo apt-get install -y g++-13 libstdc++-13-dev\n"
        "      - run: make -C firmware/projects/canary-wap/tests_host\n"
        "      - run: make -j4 -C firmware/tests_host run\n"
        "      - run: make -j 4 --directory=firmware/projects/canary-a/tests_host all\n"
        "      - run: cd firmware/projects/canary-b/tests_host && make\n"
        "      - working-directory: firmware/projects/canary-c/tests_host\n"
        "        run: make 2>&1 | tee /tmp/make.log\n"
        "      - run: make -C firmware/projects/canary-d/tests_host || exit 1\n",
        [], {"firmware/projects/canary-wap/tests_host", "firmware/tests_host",
             "firmware/projects/canary-a/tests_host", "firmware/projects/canary-b/tests_host",
             "firmware/projects/canary-c/tests_host", "firmware/projects/canary-d/tests_host"}),
    # MUST_FAIL (for the make list): each of these reaches the directory,
    # and none can fail the run on a failing suite, or runs the whole list.
    "make that does not count": (
        "jobs:\n"
        "  a:\n"
        "    steps:\n"
        "      - name: one suite is not the list\n"
        "        run: |\n"
        "          set -euo pipefail\n"
        "          make -C firmware/projects/canary-display/tests_host test_fleet_beacon\n"
        "      - run: make -C firmware/tests_host || true\n"
        "      - run: make -ik -C firmware/tests_host\n"
        "      - run: make -n -C firmware/tests_host\n"
        "      - if: false\n"
        "        run: make -C firmware/tests_host\n"
        "      - continue-on-error: true\n"
        "        run: make -C firmware/tests_host\n"
        "      - shell: bash {0}\n"
        "        run: |\n"
        "          make -C firmware/tests_host\n"
        "          echo done\n"
        "  b:\n"
        "    if: ${{ false }}\n"
        "    steps:\n"
        "      - run: make -C firmware/tests_host\n",
        [], set()),
}

# Whole files for the trigger and `uses:` reading: {path: text}, and the
# directories a pull request runs the list in.
REPO_FILES = {
    ".github/workflows/pr.yml": (
        "on:\n  pull_request:\n    paths: ['firmware/**']\n"
        "jobs:\n  a:\n    runs-on: ubuntu-latest\n    steps:\n"
        "      - uses: ./.github/actions/host-tests\n"
        "      - run: make -C firmware/projects/canary-pr/tests_host\n"
        "  b:\n    uses: ./.github/workflows/reusable.yml\n"),
    ".github/workflows/reusable.yml": (
        "on: workflow_call\n"
        "jobs:\n  a:\n    steps:\n      - run: make -C firmware/projects/canary-reused/tests_host\n"),
    ".github/workflows/nightly.yml": (
        "on:\n  schedule:\n    - cron: '0 3 * * *'\n  workflow_dispatch:\n"
        "jobs:\n  a:\n    steps:\n      - run: make -C firmware/projects/canary-nightly/tests_host\n"),
    ".github/workflows/merge.yml": (
        "on: [merge_group, push]\n"
        "jobs:\n  a:\n    steps:\n      - run: make -C firmware/projects/canary-merge/tests_host\n"),
    ".github/actions/host-tests/action.yml": (
        "runs:\n  using: composite\n  steps:\n"
        "    - shell: bash\n      run: make -C firmware/projects/canary-action/tests_host\n"),
    ".github/actions/unused/action.yml": (
        "runs:\n  using: composite\n  steps:\n"
        "    - shell: bash\n      run: make -C firmware/projects/canary-unused/tests_host\n"),
}
REPO_FILES_RUN = {"firmware/projects/canary-pr/tests_host",
                  "firmware/projects/canary-reused/tests_host",
                  "firmware/projects/canary-merge/tests_host",
                  "firmware/projects/canary-action/tests_host"}


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
                self.assertEqual(sorted(src for _ln, src in inline_compiles(text)), sorted(compiles))
                self.assertEqual(make_list_dirs(text), dirs, make_calls(text))

    def test_every_refused_make_says_why(self):
        calls = make_calls(WORKFLOWS["make that does not count"][0])
        self.assertEqual(len(calls), 8, calls)
        self.assertTrue(all(problem for _ln, _d, problem in calls), calls)

    def test_triggers_and_local_uses(self):
        counted, near = ci_make_dirs(REPO_FILES)
        self.assertEqual(counted, REPO_FILES_RUN)
        self.assertEqual(set(near), {"firmware/projects/canary-nightly/tests_host",
                                     "firmware/projects/canary-unused/tests_host"})

    def test_composite_actions_are_scanned(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            action = root / ".github/actions/build/action.yml"
            action.parent.mkdir(parents=True)
            action.write_text(
                "runs:\n  using: composite\n  steps:\n    - shell: bash\n"
                "      run: g++ firmware/tests_host/test_hidden.cpp -o /tmp/h\n", encoding="utf-8")
            found = [src for wf in workflow_files(root)
                     for _ln, src in inline_compiles(wf.read_text(encoding="utf-8"))]
            self.assertEqual(found, ["firmware/tests_host/test_hidden.cpp"])

    def test_step_reader_stops_at_the_next_key(self):
        text = ("      - run: |\n          echo one\n\n          echo two\n"
                "      - name: next\n        run: echo three\n")
        self.assertEqual([(s.line, s.script.split()) for s in steps(text)],
                         [(1, ["echo", "one", "echo", "two"]), (6, ["echo", "three"])])


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
            "list or by a `run: run-x` prerequisite, as the file already does — "
            "so that its failure fails make:\n" + "\n".join(problems))

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
        files = {str(wf.relative_to(REPO)): wf.read_text(encoding="utf-8")
                 for wf in workflow_files()}
        ran, near = ci_make_dirs(files)
        missing = [str(d.relative_to(REPO)) for d in tests_host_dirs()
                   if (d / "Makefile").is_file() and str(d.relative_to(REPO)) not in ran]
        self.assertEqual(
            missing, [],
            "a tests_host Makefile no pull request runs. Add a `make -C <dir>` step "
            "to a workflow pull requests trigger (a single-target call builds one "
            "suite, not the list):\n" + "\n".join(
                f"  {d}" + "".join(f"\n    not counted: {why}" for why in near.get(d, []))
                for d in missing))


if __name__ == "__main__":
    unittest.main()
