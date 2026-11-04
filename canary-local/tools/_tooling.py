#!/usr/bin/env python3
"""Shared plumbing for the canary-local/tools generators — ONE behavior each.

Every generator in this directory used to carry its own copy of `die()` (ten
of them, three different behaviors: with and without an `ERROR:` token, with
and without a second "drift-gated" hint line, `sys.exit(str)` vs
`SystemExit(1)`) and two of `_warn()`. This module is the single definition.

  die(msg, code=1)
      `<prog>: ERROR: <msg>` on stderr, then — only when GITHUB_ACTIONS is
      set — a `::error::<prog>: <msg>` workflow command on stdout, so the
      failure surfaces as a red annotation on the PR (the same stream the
      existing `::warning::` annotations in this directory use). Exits with
      `code`, default 1. A caller that needs a distinct exit status passes it
      explicitly, so the status is always visible at the call site; none does
      today. `<prog>` is the basename of the module that called die() —
      `gen_vision.py`, not whatever wrapper imported it — so the line a person
      reads in the log names the generator that actually failed. A die()
      reached through a reader in this file (read_const) names the module
      that called the reader: frames of this file are stepped past.

  warn(msg)
      `<prog>: WARNING: <msg>` on stderr, plus a `::warning::` annotation
      under GITHUB_ACTIONS. Never exits.

  repo_root(levels=2, start=None)
      The repository root as a Path. Anchored on THIS file by default: it
      lives at canary-local/tools/_tooling.py, two levels below the root, so
      `repo_root()` is correct for every caller wherever that caller lives.
      `start` / `levels` let a script anchor on itself instead (a test in
      tools/tests/ is three deep: `repo_root(3, __file__)`). The result is
      checked either way — it must contain this very file — so a miscount
      fails here with a clear message, not three functions later with a
      "missing source" error that names the wrong path.

  read_const(path, name, read)
      One integer constant out of a firmware header, by one strict rule:
      comments are not code, and a definition spelled inside a string or
      character literal (raw strings included) is not a definition; NAME is
      a whole word defined exactly once, as `#define NAME value` or
      `constexpr T NAME = value;`; the value is a plain decimal literal (U/L
      suffixes allowed, no leading zero, which C reads as octal, at most
      2^53 - 1, the largest integer a browser reads exactly out of the JSON
      the generators write) that ends the #define's line or the constexpr's
      statement (a block comment that runs onto a later line does not end
      the #define's line: code after its close is refused, F429). Anything
      else is refused by die(), naming the constant, the file and the
      reason, rather than read as its first number. `read` is
      the caller's own reader (a path in, its text out), so a generator
      keeps its cache and its missing-source message and a test can serve a
      header that is not on disk. gen_flash.py (the flasher catalog's Vision
      and Sense dials) and gen_vision.py (the Vision page's detection
      constants) read through it. Its JavaScript twin is
      canary-local/tests/c_const.js, which vision.test.js reads the same
      constants with, and canary-local/tests/c_const_cases.json is the table
      both are held to, verdict for verdict and message for message (sweep
      F369).

Importable as `from _tooling import die, warn, repo_root, read_const`
because the scripts run as `python3 canary-local/tools/<tool>.py`, which puts
this directory at sys.path[0]; the unit tests in tools/tests/ insert the same
directory themselves. NOT used by hub_seed_apply.py or hub_host_provision.sh:
those are embedded verbatim (include_str! in desktop/hub-io, sha256-pinned by
gen_hub_provision_bundle.py) and must stay self-contained.
"""
from __future__ import annotations

import os
import re
import sys
from collections.abc import Callable
from pathlib import Path
from typing import NoReturn

_HERE = Path(__file__).resolve()


def _prog(depth: int) -> str:
    """Basename of the module `depth` frames up the stack (the caller of the
    caller, for die/warn), stepping past frames of this file, so a refusal
    raised in read_const names the generator that asked for the constant.
    Falls back to argv[0], then to a constant, so a frozen or interactive
    caller still gets a readable prefix."""
    try:
        frame = sys._getframe(depth)
        while frame.f_globals is globals() and frame.f_back is not None:
            frame = frame.f_back
        path = frame.f_globals.get("__file__")
    except (ValueError, AttributeError):
        path = None
    if not path:
        path = sys.argv[0] if sys.argv and sys.argv[0] else "tool"
    return Path(path).name


def _annotate(kind: str, prog: str, msg: str) -> None:
    if os.environ.get("GITHUB_ACTIONS"):
        # Workflow commands must be a single line; fold a multi-line message so
        # the runner does not truncate the annotation at the first newline.
        one_line = " ".join(msg.split())
        print(f"::{kind}::{prog}: {one_line}", flush=True)


def die(msg: str, code: int = 1) -> NoReturn:
    """Fail closed and loud: better a red build than a catalog that quietly lies."""
    prog = _prog(2)
    print(f"{prog}: ERROR: {msg}", file=sys.stderr)
    _annotate("error", prog, msg)
    raise SystemExit(code)


def warn(msg: str) -> None:
    prog = _prog(2)
    print(f"{prog}: WARNING: {msg}", file=sys.stderr)
    _annotate("warning", prog, msg)


def repo_root(levels: int = 2, start: str | os.PathLike[str] | None = None) -> Path:
    base = _HERE if start is None else Path(start).resolve()
    try:
        root = base.parents[levels]
    except IndexError:
        raise RuntimeError(
            f"repo_root: {base} has no ancestor {levels} levels up"
        ) from None
    marker = root / "canary-local" / "tools" / "_tooling.py"
    if not marker.is_file():
        raise RuntimeError(
            f"repo_root: {root} is not the securaCV checkout "
            f"(expected {marker.relative_to(root)} under it) — wrong `levels` for {base}?"
        )
    return root


# ── one integer constant out of a firmware header ────────────────────────────
# The rule is stated once, in read_const's docstring and the module's above;
# canary-local/tests/c_const.js spells the same patterns in JavaScript, so a
# change here is a change there, and c_const_cases.json holds the two equal.

# What the scan steps over whole, left to right, so whichever opens first
# wins: the places a definition is not code, ended where the compiler's lexer
# ends them, and numbers:
#   - a raw string literal, `R"d(...)d"` with an optional u8/u/U/L prefix,
#     whose body runs verbatim (quotes, backslashes and newlines) to its own
#     `)d"`;
#   - a string or character literal, which can hold `//` or `/*` without
#     opening a comment (the Vision's project config.h carries an OTA URL);
#   - a block comment;
#   - a line comment;
#   - a number (a pp-number, so the digit separator in `1'000` does not open
#     a character literal that runs into the comment after it).
# A backslash that ends a line (blanks after it allowed, as GCC allows them)
# splices the next line into the literal or the line comment it sits in, as
# it does for the compiler.
_SPLICE = r"\\[ \t]*\r?\n"
_C_TOKEN = re.compile("|".join((
    r"(?<![A-Za-z0-9_])(?:u8|[uUL])?R\"([!-'*-\[\]-~]{0,16})\(.*?\)\1\"",
    rf'"(?:{_SPLICE}|\\.|[^"\\\n])*"',
    rf"'(?:{_SPLICE}|\\.|[^'\\\n])*'",
    r"/\*.*?\*/",
    rf"//(?:{_SPLICE}|[^\n])*",
    r"(?<![A-Za-z0-9_.])\.?[0-9](?:[eEpP][+-]|'[0-9A-Za-z_]|[0-9A-Za-z_.])*",
)), re.S)
# C's own whitespace and identifier characters, spelled out: Python's \s and
# \w also take Unicode spaces and letters, which neither the compiler nor the
# JavaScript twin does.
_C_SPACE = "[ \t\n\r\f\v]"
_C_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_C_DECIMAL = re.compile(r"(0|[1-9][0-9]*)[uUlL]*")
_C_OCTAL = re.compile(r"0[0-9]+[uUlL]*")
# The largest integer a browser holds exactly (Number.MAX_SAFE_INTEGER): every
# generator here writes JSON a page reads.
_JS_SAFE_MAX = 2**53 - 1


def _blank_c(text: str, keep_literals: bool,
             runs_on: dict[int, int] | None = None) -> str:
    """`text` with every comment blanked (its newlines kept, so a line number
    still names its line), every number kept, and every literal kept or,
    without `keep_literals`, masked. Given `runs_on`, each block comment that
    holds a newline is entered in it: the offset its blank starts at in the
    result, mapped to the offset that blank ends at. Those newlines are the
    comment's, not the line's: a #define goes on past them, where the
    compiler takes the line the comment closes on as more of it."""
    shrunk = 0  # how much shorter the result is than `text`, so far

    def blank(m: re.Match) -> str:
        nonlocal shrunk
        token = m.group(0)
        if token[0] == "/":
            out = "\n" * token.count("\n") or " "
            if runs_on is not None and token[1] == "*" and out[0] == "\n":
                at = m.start() - shrunk
                runs_on[at] = at + len(out)
            shrunk += len(token) - len(out)
            return out
        if token[0] in "0123456789.":
            return token
        # Masked to quote marks (newlines kept), the literal keeps its
        # length, so a span found in the masked text is the same span of the
        # code, and it is not whitespace, so a #define's value still starts
        # where the literal does.
        return token if keep_literals else re.sub(r"[^\n]", '"', token)
    return _C_TOKEN.sub(blank, text)


def _past_comments(code: str, shape: str, end: int, runs_on: dict[int, int]) -> str:
    """What a #define's line goes on with after the block comments it runs
    into, from `end`, where its value stops in `shape` (the first newline
    there, or the end of the text). Each block comment `runs_on` holds that
    starts there is stepped over to the next newline, again and again, and
    what lies between is cut from `code` at the same span, its edge
    whitespace stripped; the pieces are joined by a space. Empty when the
    value's line ends for real, or only blanks follow each comment's close."""
    pieces = []
    while end in runs_on:
        start = runs_on[end]
        found = shape.find("\n", start)
        end = len(shape) if found < 0 else found
        piece = code[start:end].strip(" \t\n\r\f\v")
        if piece:
            pieces.append(piece)
    return " ".join(pieces)


def _line_before(code: str, shape: str, at: int,
                 runs_on: dict[int, int]) -> tuple[str, bool]:
    """What the line `at` sits on says before it, as the compiler reads the
    line: from the newline that ends a line for real (each newline a block
    comment left in `shape`, one of `runs_on`'s spans, is stepped back over)
    or the start of the text, cut from `code` at the same span with its
    whitespace runs collapsed to one space; and whether such a comment lies
    between that and `at`. Empty when only blanks and comments come before
    `at` on its line, which is where a directive may begin."""
    start = at
    while start > 0:
        newline = shape.rfind("\n", 0, start)
        if newline < 0:
            start = 0
            break
        span = next((s for s, e in runs_on.items() if s <= newline < e), None)
        if span is None:
            start = newline + 1
            break
        start = span
    last = start + len(shape[start:at].rstrip(" \t\n\r\f\v"))
    before = re.sub(r"[ \t\n\r\f\v]+", " ", code[start:last]).strip(" ")
    return before, any(last <= s < at for s in runs_on)


def _where(path: Path) -> str:
    """`path` as a refusal names it: relative to the repository when it is in it."""
    try:
        return path.relative_to(repo_root()).as_posix()
    except ValueError:
        return path.as_posix()


def read_const(path: Path, name: str, read: Callable[[Path], str]) -> int:
    """Read `#define NAME value` or `constexpr T NAME = value;` as an int.

    Comments are not code, so a commented-out definition is not read, and a
    definition spelled inside a string, character or raw string literal is
    not one either. Where each of those ends is found by the compiler's own
    lexing rules for the cases a header meets: a backslash ending a line
    continues a line comment or a literal, a raw string runs to its own
    delimiter, and a digit separator (`1'000`) does not open a character
    literal. The preprocessor is not run, and a splice outside a comment or
    a literal is not undone (a #define continued onto the next line by a
    backslash is refused, as below). A #define's line ends at the first
    newline outside a comment, as the compiler ends it, so one whose line
    runs into a block comment closed on a later line goes on there: code
    after the comment's close is more of the value, and is refused by its
    own reason (`#define WAIT_MS 30 /* thirty` over ` */ * 1000` is
    `30 * 1000` to the compiler, never 30), while only blanks or a comment
    after the close leave the value as the first line spells it. A #define
    is a directive only where its `#` begins the line as the compiler reads
    it, so one with code before it on that line, on the same physical line
    or on the tail of a line that runs into such a comment (`#define Y 1 /*
    a` over ` */ #define X 30` defines no X), is refused by that reason. One
    with such a comment between `#define` and the name is not found at all
    (the definition pattern does not cross the comment's newline; the
    compiler defines it): a safe verdict whose reason is not the compiler's,
    a known limit. The name is a whole word and must be defined exactly
    once: two definitions (an `#if`/`#else` pair, an `#undef` and a
    redefinition) leave the preprocessor to pick one, which a regex cannot
    do. The value must be a plain decimal literal (a U/L suffix allowed, no
    leading zero, which C reads as octal, at most 2^53 - 1) that ends the
    #define's line or the constexpr's statement. Anything else is refused
    rather than read: the first number of an expression such as
    `30u * 1000u`, or `070` read as 70 when it compiles as 56, would put a
    wrong default in the catalog with --check green. Every refusal names the
    constant, the file and the reason. `read` is the caller's reader, path
    to text."""
    where = _where(path)
    if not _C_NAME.fullmatch(name):
        die(f"constant name `{name}` is not a C identifier; refusing to look for it in {where}")
    text = read(path)
    code = _blank_c(text, keep_literals=True)
    # Definitions are found where literals are masked and the value is cut
    # from the code at the same span, so a refusal quotes what the file says.
    runs_on: dict[int, int] = {}
    shape = _blank_c(text, keep_literals=False, runs_on=runs_on)
    sp = _C_SPACE
    found = list(re.finditer(
        rf"#define[ \t]+{name}[ \t]+(?P<define>[^\n]*)"
        rf"|(?<![A-Za-z0-9_])constexpr{sp}+[A-Za-z0-9_]+{sp}+{name}{sp}*={sp}*"
        rf"(?P<constexpr>[^;]*);", shape))
    if not found:
        die(f"constant {name} not found in {where}")
    if len(found) > 1:
        lines = ", ".join(str(shape.count("\n", 0, m.start()) + 1) for m in found)
        die(f"constant {name} is defined {len(found)} times in {where} (lines {lines}); which "
            "one compiles is the preprocessor's call, so refusing to pick one")
    m = found[0]
    group = "define" if m.group("define") is not None else "constexpr"
    value = code[m.start(group):m.end(group)].strip(" \t\n\r\f\v")
    if group == "define":
        # A directive's `#` begins its line as the compiler reads it: after
        # code on that line, across a block comment closed on a later line
        # or not, `#define` is tokens of something else and defines nothing.
        before, crossed = _line_before(code, shape, m.start(), runs_on)
        if before:
            across = ", across a block comment closed on a later line" if crossed else ""
            die(f"constant {name} in {where} has `{before}` before its #define on the line"
                f"{across}, so the compiler reads no directive there; refusing to read a number "
                "out of it")
    # The #define's value stops at a newline in `shape`; one a block comment
    # left there is not the end of its line (a constexpr's runs to its `;`).
    after = _past_comments(code, shape, m.end(group), runs_on) if group == "define" else ""
    if after:
        die(f"constant {name} in {where} runs into a block comment closed on a later line, "
            f"and the #define goes on past the comment (`{after}`); refusing to read a "
            "number out of it")
    literal = _C_DECIMAL.fullmatch(value)
    if not literal:
        octal = " (a leading 0 is octal in C and C++)" if _C_OCTAL.fullmatch(value) else ""
        die(f"constant {name} in {where} is `{value}`, not a plain integer literal{octal}; "
            "refusing to read a number out of it")
    digits = literal.group(1)
    # More than 16 digits is past 2^53 - 1 (16 of them) whatever they are.
    # It is checked first, so int() never meets a literal longer than the
    # 4300 digits CPython converts, which it would answer with a ValueError
    # instead of this refusal.
    if len(digits) > 16 or int(digits) > _JS_SAFE_MAX:
        die(f"constant {name} in {where} is `{value}`, past 2^53 - 1, which a browser cannot "
            "hold exactly; refusing to read it")
    return int(digits)
