#!/usr/bin/env python3
"""canary-local/tools/_wap_profile.py — the WAP's firmware flags, read from its sketch.

The Canary WAP builds one Arduino sketch
(firmware/projects/canary-wap/arduino/canary_wap/), and that sketch's build
profile is build_config.h: a hardware-target block derives the board's HW_HAS_*
capabilities, and three BUILD_PROFILE_* blocks (MINIMAL, DEV, FULL) set its
profile flags. A few module flags default elsewhere and are not the profile's
(FEATURE_BEACON_CHANNEL in beacon_channel.h, the BLE sub-modes
FEATURE_BLE_OPERA / _CHIRP / _NEARBY in ble_config.h, FEATURE_USB_EVIDENCE_DRIVE
after the blocks). No build puts a firmware/configs/ directory on its include
path. This module reads the one combination a stock
WAP compiles — the header's in-file defaults, the XIAO ESP32-S3 and
BUILD_PROFILE_FULL, which is also what [env:canary-wap-default] passes with
-D — and resolves each FEATURE_* the FULL block sets to on or off, the way the
preprocessor would for that board.

    read_full_profile(text)       -> {FEATURE_X: {"on": bool, ...}}
    read_full_profile_file(path)  -> the same, from the file

Each entry carries "on", the line's own trailing comment as "note" where it
has one (for a `#ifndef X / #define X v / #endif` default, the comment on the
`#ifndef` line, which says who overrides it), and "expr" when the value is not
a literal 0 or 1 (`HW_HAS_CAMERA`, `(FEATURE_BLUETOOTH && FEATURE_BLE)`), so a
page can say what the bit was derived from. Entries keep the block's order.
Flags set outside the FULL block (FEATURE_USB_EVIDENCE_DRIVE's opt-in default
after it) are not the profile's and are not returned.

It refuses rather than guesses. Every shape it cannot read raises
WapProfileError naming the line: no FULL block, or more than one that sets
FEATURE_*; no XIAO ESP32-S3 block of HW_HAS_* values; an in-file default
target other than the S3; a nested conditional other than the `#ifndef X`
default; a directive it does not know (`#warning`, `#undef`, `#include`); a
value that is not an integer, a known name, or a `!` / `&&` / `||` / `( )`
combination of them; a flag set twice; a name that resolves to itself; a flag
that comes out other than 0 or 1.
"""
from __future__ import annotations

import re
from pathlib import Path

PROFILE = "FULL"
TARGET = "HARDWARE_XIAO_ESP32S3"
OTHER_TARGETS = ("HARDWARE_XIAO_ESP32C3",)


class WapProfileError(ValueError):
    """build_config.h has a shape this reader does not understand."""


# ── lexing ────────────────────────────────────────────────────────────────

def _strip_block_comments(text: str) -> str:
    """Blank /* ... */ comments, keeping every newline so line numbers hold."""
    return re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)


def _logical_lines(text: str) -> list[tuple[int, str, str]]:
    """(line number, code, trailing // comment) per logical line.

    Backslash continuations are joined onto the line that starts them (a
    `#if ... && \\` guard spans three physical lines). Block comments are
    blanked first; a `//` inside a string literal does not occur in this
    header's directives, and a directive that held one would fail to parse
    below rather than be misread.
    """
    out: list[tuple[int, str, str]] = []
    physical = _strip_block_comments(text).split("\n")
    i = 0
    while i < len(physical):
        start = i + 1
        line = physical[i]
        while line.rstrip().endswith("\\") and i + 1 < len(physical):
            i += 1
            line = line.rstrip()[:-1] + " " + physical[i]
        code, _, comment = line.partition("//")
        out.append((start, code.strip(), comment.strip()))
        i += 1
    return out


_DIRECTIVE = re.compile(r"^#\s*(\w+)\s*(.*)$")


def _directive(code: str) -> tuple[str, str] | None:
    m = _DIRECTIVE.match(code)
    return (m.group(1), m.group(2).strip()) if m else None


# ── blocks ────────────────────────────────────────────────────────────────

def _blocks_opened_by(lines, opener: re.Pattern) -> list[tuple[int, list]]:
    """Every `#if`/`#elif` whose condition matches `opener`, with its body.

    The body runs to the matching `#elif`, `#else` or `#endif` at the same
    depth; nested conditionals stay inside it, for the caller to judge.
    """
    found = []
    for idx, (ln, code, _) in enumerate(lines):
        d = _directive(code)
        if not d or d[0] not in ("if", "elif") or not opener.fullmatch(d[1]):
            continue
        depth, body = 0, []
        for item in lines[idx + 1:]:
            dd = _directive(item[1])
            if dd and dd[0] in ("if", "ifdef", "ifndef"):
                depth += 1
            elif dd and dd[0] == "endif":
                if depth == 0:
                    break
                depth -= 1
            elif dd and dd[0] in ("elif", "else") and depth == 0:
                break
            body.append(item)
        else:
            raise WapProfileError(f"build_config.h:{ln}: the block opened here never closes")
        found.append((ln, body))
    return found


def _one_block_defining(lines, macro: str, prefix: str, what: str):
    """The single `#if/#elif defined(<macro>)` block that #defines <prefix>*."""
    opener = re.compile(r"defined\s*\(\s*%s\s*\)" % re.escape(macro))
    candidates = [
        (ln, body) for ln, body in _blocks_opened_by(lines, opener)
        if any((_directive(c) or ("", ""))[0] == "define"
               and (_directive(c) or ("", ""))[1].startswith(prefix) for _, c, _ in body)
    ]
    if not candidates:
        raise WapProfileError(
            f"build_config.h: no `#if/#elif defined({macro})` block sets {prefix}* — "
            f"the {what} this reader resolves is gone or renamed")
    if len(candidates) > 1:
        raise WapProfileError(
            f"build_config.h: {len(candidates)} `defined({macro})` blocks set {prefix}* "
            f"(lines {', '.join(str(ln) for ln, _ in candidates)}); which one a build "
            f"compiles is not something this reader can decide")
    return candidates[0]


# ── values ────────────────────────────────────────────────────────────────

_TOKEN = re.compile(r"\s*(?:(\d+)[uUlL]*|([A-Za-z_]\w*)|(&&|\|\||!|\(|\)))")


def _tokens(expr: str, ln: int) -> list[str]:
    out, pos = [], 0
    expr = expr.strip()
    while pos < len(expr):
        m = _TOKEN.match(expr, pos)
        if not m or m.end() == pos:
            raise WapProfileError(
                f"build_config.h:{ln}: cannot read `{expr}` — a flag's value must be an "
                f"integer, a name, or a !/&&/||/( ) combination of them")
        out.append(m.group(1) or m.group(2) or m.group(3))
        pos = m.end()
        while pos < len(expr) and expr[pos].isspace():
            pos += 1
    if not out:
        raise WapProfileError(f"build_config.h:{ln}: a flag is defined with no value")
    return out


def _evaluate(expr: str, ln: int, lookup) -> int:
    toks = _tokens(expr, ln)
    pos = 0

    def peek():
        return toks[pos] if pos < len(toks) else None

    def take():
        nonlocal pos
        pos += 1
        return toks[pos - 1]

    def primary():
        if peek() is None:
            raise WapProfileError(f"build_config.h:{ln}: `{expr}` ends early")
        t = take()
        if t == "(":
            v = disjunction()
            if peek() != ")":
                raise WapProfileError(f"build_config.h:{ln}: `{expr}` has an unclosed (")
            take()
            return v
        if t.isdigit():
            return int(t)
        if t == "defined":
            raise WapProfileError(f"build_config.h:{ln}: `defined` in a flag's value is not read here")
        if re.fullmatch(r"[A-Za-z_]\w*", t):
            return lookup(t, ln)
        raise WapProfileError(f"build_config.h:{ln}: unexpected `{t}` in `{expr}`")

    def unary():
        if peek() == "!":
            take()
            return int(not unary())
        return primary()

    def conjunction():
        v = unary()
        while peek() == "&&":
            take()
            rhs = unary()
            v = int(bool(v) and bool(rhs))
        return v

    def disjunction():
        v = conjunction()
        while peek() == "||":
            take()
            rhs = conjunction()
            v = int(bool(v) or bool(rhs))
        return v

    value = disjunction()
    if pos != len(toks):
        raise WapProfileError(f"build_config.h:{ln}: cannot read `{expr}` past `{toks[pos]}`")
    return value


# ── the reader ────────────────────────────────────────────────────────────

def _check_default_target(lines) -> None:
    """The in-file default target (Arduino IDE builds) must be the S3."""
    guard = re.compile(r"!\s*defined\s*\(\s*%s\s*\)(?:\s*&&\s*!\s*defined\s*\(\s*\w+\s*\))*"
                       % re.escape(TARGET))
    blocks = _blocks_opened_by(lines, guard)
    if len(blocks) != 1:
        raise WapProfileError(
            f"build_config.h: expected one `#if !defined({TARGET}) && ...` default-target "
            f"block, found {len(blocks)}")
    ln, body = blocks[0]
    defined = [(_directive(c) or ("", ""))[1] for _, c, _ in body
               if (_directive(c) or ("", ""))[0] == "define"]
    if defined != [TARGET]:
        raise WapProfileError(
            f"build_config.h:{ln}: the in-file default hardware target is "
            f"{defined or 'nothing'}, not {TARGET} — the Workshop's \"full\" flavor is the "
            f"default XIAO ESP32-S3's, so say which board it describes before changing this")


def _read_values(body, what) -> list[tuple[int, str, str, str, bool]]:
    """(line, name, value expression, note, defaulted) for each #define in a block.

    A block holds `#define NAME value` lines and `#ifndef NAME / #define NAME
    value / #endif` defaults. Anything else is refused.
    """
    out = []
    i = 0
    while i < len(body):
        ln, code, comment = body[i]
        d = _directive(code)
        if not code:
            i += 1
            continue
        if d is None:
            raise WapProfileError(f"build_config.h:{ln}: `{code}` in the {what} is not a directive")
        kind, rest = d
        if kind == "define":
            m = re.fullmatch(r"(\w+)\s+(.+)", rest)
            if not m:
                raise WapProfileError(f"build_config.h:{ln}: `#define {rest}` sets no value")
            out.append((ln, m.group(1), m.group(2).strip(), comment, False))
            i += 1
            continue
        if kind == "ifndef":
            guarded = rest
            inner = body[i + 1:i + 3]
            dd = [(_directive(c) or ("", "")) for _, c, _ in inner]
            if (len(inner) == 2 and dd[0][0] == "define" and dd[1][0] == "endif"
                    and re.fullmatch(r"%s\s+.+" % re.escape(guarded), dd[0][1])):
                value = dd[0][1][len(guarded):].strip()
                note = inner[0][2] or comment
                out.append((inner[0][0], guarded, value, note, True))
                i += 3
                continue
            raise WapProfileError(
                f"build_config.h:{ln}: `#ifndef {guarded}` in the {what} is not the "
                f"`#ifndef X / #define X value / #endif` default this reader understands")
        raise WapProfileError(
            f"build_config.h:{ln}: `#{kind}` in the {what} — this reader resolves "
            f"#define values and #ifndef defaults only, and will not guess past it")
    return out


def read_full_profile(text: str) -> dict:
    lines = _logical_lines(text)
    _check_default_target(lines)

    hw_ln, hw_body = _one_block_defining(lines, TARGET, "HW_", "XIAO ESP32-S3 capability block")
    hw: dict[str, int] = {}
    for ln, name, value, _, _ in _read_values(hw_body, "XIAO ESP32-S3 capability block"):
        if not value.isdigit():
            raise WapProfileError(f"build_config.h:{ln}: {name} = `{value}` is not an integer")
        hw[name] = int(value)

    full_ln, full_body = _one_block_defining(lines, f"BUILD_PROFILE_{PROFILE}", "FEATURE_",
                                             f"BUILD_PROFILE_{PROFILE} block")
    raw: dict[str, tuple[int, str, str, bool]] = {}
    other: dict[str, tuple[int, str]] = {}
    for ln, name, value, note, defaulted in _read_values(full_body, f"BUILD_PROFILE_{PROFILE} block"):
        table = raw if name.startswith("FEATURE_") else other
        if name in raw or name in other:
            raise WapProfileError(
                f"build_config.h:{ln}: {name} is set twice in the BUILD_PROFILE_{PROFILE} block")
        if name.startswith("FEATURE_"):
            table[name] = (ln, value, note, defaulted)
        else:
            table[name] = (ln, value)

    resolved: dict[str, int] = {}
    resolving: set[str] = set()

    def lookup(name: str, ln: int) -> int:
        if name in hw:
            return hw[name]
        if name in resolved:
            return resolved[name]
        if name in raw:
            if name in resolving:
                raise WapProfileError(f"build_config.h:{ln}: {name} resolves to itself")
            resolving.add(name)
            fln, value, _, _ = raw[name]
            resolved[name] = _evaluate(value, fln, lookup)
            resolving.discard(name)
            return resolved[name]
        raise WapProfileError(
            f"build_config.h:{ln}: `{name}` is neither one of the XIAO ESP32-S3's HW_* "
            f"values nor a FEATURE_* the BUILD_PROFILE_{PROFILE} block sets")

    out: dict[str, dict] = {}
    for name, (ln, value, note, _) in raw.items():
        v = lookup(name, ln)
        if v not in (0, 1):
            raise WapProfileError(
                f"build_config.h:{ln}: {name} comes out {v} on the XIAO ESP32-S3; a "
                f"feature flag is 0 or 1")
        entry: dict = {"on": v == 1}
        if note:
            entry["note"] = note
        if value not in ("0", "1"):
            entry["expr"] = value
        out[name] = entry
    if not out:
        raise WapProfileError(f"build_config.h:{full_ln}: the BUILD_PROFILE_{PROFILE} block sets no FEATURE_*")
    return out


def read_full_profile_file(path: Path) -> dict:
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as e:
        raise WapProfileError(f"{path}: {e}") from e
    return read_full_profile(text)


# A note that names a hardware target other than the S3 this flavor is
# resolved for describes another build.
_OTHER_TARGET = re.compile(r"\bC3\b")


def apply_workshop_notes(flags: dict, text: str, notes: dict) -> dict:
    """The profile with a curated note per flag in `notes` (name -> (note or
    None, [patterns])), for a page that shows the notes to a builder.

    A FULL-block line's comment is written for the firmware's developers. Each
    curated note holds only while every pattern beside it still matches the
    header text, so a page cannot keep a caveat the header has dropped; a note
    of None removes the line's comment. Refused, by name: a curated flag the
    profile no longer sets, a pattern the header no longer matches, and any
    note left that names another target than the S3. Each entry's keys keep
    the reader's order (on, note, expr).
    """
    out = {name: dict(entry) for name, entry in flags.items()}
    for name, (note, evidence) in notes.items():
        if name not in out:
            raise WapProfileError(f"the Workshop's note for {name}: the FULL profile no longer sets it")
        for pat in evidence:
            if not re.search(pat, text):
                raise WapProfileError(
                    f"the Workshop's note for {name} rests on /{pat}/, which build_config.h no "
                    "longer says: re-read the header and update or drop the note")
        out[name].pop("note", None)
        if note:
            out[name]["note"] = note
    for name, entry in out.items():
        if _OTHER_TARGET.search(entry.get("note", "")):
            raise WapProfileError(
                f"{name}'s note names another target than the S3 this flavor is resolved "
                f"for ({entry['note']!r}): give it a curated note")
    return {n: {k: e[k] for k in ("on", "note", "expr") if k in e} for n, e in out.items()}
