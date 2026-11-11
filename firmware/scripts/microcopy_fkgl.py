#!/usr/bin/env python3
"""
Flesch-Kincaid reading-grade lint for the headline Sensing dashboard.

The plan asks the dashboard's microcopy to read at ≤ 6th grade so it works
for grandmas and 10-year-olds. This script computes the aggregate
Flesch-Kincaid Grade Level across every user-facing string in
csi_dashboard_html.h's COPY object and fails if it scores worse than the
configured ceiling (default 6th grade — matches the spec's target with
no slack, since the corpus reads at ~3.4 today and any future jargon
that pushes the aggregate past 6 deserves to be caught).

We aggregate into one corpus rather than per-string because:
  - Many strings are short (1-3 words) where FKGL is meaningless.
  - The user reads the corpus as a whole; aggregate grade is what
    matters in practice.

Usage:
    python3 firmware/scripts/microcopy_fkgl.py [path/to/dashboard.h] [--max-grade N]
"""

from __future__ import annotations
import argparse
import re
import sys
from pathlib import Path

DEFAULT_PATH = Path(
    "firmware/projects/canary-wap/arduino/canary_wap/csi_dashboard_html.h"
)
DEFAULT_MAX_GRADE = 6.0


# ── Syllable counting ─────────────────────────────────────────────────────
# Heuristic: count vowel-group runs, with adjustments for silent trailing
# 'e', 'es', 'ed', and leading 'y'. Good enough for English microcopy at
# corpus scale; matches textstat's behavior on common words within ±1.

VOWELS = "aeiouy"


def syllables(word: str) -> int:
    word = re.sub(r"[^A-Za-z']", "", word).lower()
    if len(word) <= 3:
        return 1 if word else 0
    # Strip silent trailing endings.
    word = re.sub(r"e$", "", word)
    word = re.sub(r"es$", "", word)
    word = re.sub(r"ed$", "", word)
    # Leading 'y' generally consonantal.
    if word.startswith("y"):
        word = word[1:]
    # Count vowel groups.
    groups = re.findall(r"[aeiouy]+", word)
    return max(1, len(groups))


# ── Corpus extraction from COPY ────────────────────────────────────────────
#
# COPY is a data literal: strings, numbers, booleans, arrays and objects, with
# comments between them. Comments are not copy. Graded as quoted spans, an
# apostrophe in one comment paired with the next one and swallowed every string
# between them into one bogus "string" (sweep F465: the calibrate overlay's
# comment and the tier note's took the calibrate, today and tier labels with
# them). So the block is cut out and stripped of comments by one scanner that
# knows where strings are, and only then matched.
#
# The scanner knows the three JavaScript string forms ("...", '...' and `...`,
# with backslash escapes and a template's ${...} code) and the two comment forms
# (// to the end of the line, /* to */). A // or /* inside a string is the
# string's, so "https://..." survives whole; a quote or brace inside a comment is
# the comment's, so it neither opens a string nor moves the brace walk. It does
# not know regular-expression literals; COPY holds none, and a / that is not
# followed by / or * is kept as code.


def _skip_quoted(src: str, i: int) -> int:
    """Index just past the '...' or "..." string that opens at src[i].

    A raw newline ends it too: JavaScript allows none inside these two forms
    (a backslash-newline continuation is an escape and is skipped), so a stray
    quote in code cannot swallow the lines after it.
    """
    quote = src[i]
    j = i + 1
    n = len(src)
    while j < n:
        c = src[j]
        if c == "\\":
            j += 2
            continue
        if c == quote:
            return j + 1
        if c == "\n":
            return j
        j += 1
    return n


def _scan_code(src: str, i: int, out: list[str], *, until_close: bool) -> int:
    """Copy code from src[i:] into out with its comments dropped.

    Strings are copied whole. With until_close, stop at the } that closes the
    brace open before src[i] (braces counted only in code, never inside a string
    or a comment) and return its index without copying it; otherwise run to the
    end. Returns where the scan stopped.
    """
    n = len(src)
    depth = 0
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c in "\"'":
            j = _skip_quoted(src, i)
            out.append(src[i:j])
            i = j
        elif c == "`":
            i = _scan_template(src, i, out)
        elif c == "/" and nxt == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j  # the newline itself is kept
        elif c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            out.append(" ")  # a comment still separates the tokens around it
            i = n if j < 0 else j + 2
        else:
            if until_close:
                if c == "{":
                    depth += 1
                elif c == "}":
                    if depth == 0:
                        return i
                    depth -= 1
            out.append(c)
            i += 1
    return n


def _scan_template(src: str, i: int, out: list[str]) -> int:
    """Copy the `...` template that opens at src[i] into out; return the index
    past its closing backtick. Each ${...} is code: its own strings and
    comments are scanned as code (comments dropped), its braces counted."""
    n = len(src)
    out.append("`")
    j = i + 1
    while j < n:
        c = src[j]
        if c == "\\":
            out.append(src[j:j + 2])
            j += 2
        elif c == "`":
            out.append("`")
            return j + 1
        elif c == "$" and j + 1 < n and src[j + 1] == "{":
            out.append("${")
            j = _scan_code(src, j + 2, out, until_close=True)
            if j < n:
                out.append("}")
                j += 1
        else:
            out.append(c)
            j += 1
    return n


def strip_js_comments(src: str) -> str:
    """src with every // and /* */ comment removed, strings untouched."""
    out: list[str] = []
    _scan_code(src, 0, out, until_close=False)
    return "".join(out)


def copy_block(text: str) -> str | None:
    """The body of `const COPY = { ... }`, comments removed, or None.

    The brace walk and the comment strip are one scan, so a brace in a comment
    or a string cannot end the block early or late."""
    m = re.search(r"\bconst\s+COPY\s*=\s*\{", text)
    if not m:
        return None
    out: list[str] = []
    _scan_code(text, m.end(), out, until_close=True)
    return "".join(out)


# A string literal in the stripped block, as the scanner reads one: a '...' or
# "..." form does not cross a raw newline, and a backslash escapes the next
# character whatever it is. The template form is matched so a quote inside one
# cannot pair with a quote outside it; its ${...} is kept as text, the
# {bpm}-style placeholder the plain forms already carry.
_STRING_LITERAL = re.compile(
    r'"((?:\\[\s\S]|[^"\\\n])*)"'
    r"|'((?:\\[\s\S]|[^'\\\n])*)'"
    r"|`((?:\\[\s\S]|[^`\\])*)`"
)


def extract_copy_strings(text: str) -> list[str]:
    """Pull every user-facing string out of the COPY object literal.

    Strategy: cut out the COPY block with its comments removed (copy_block),
    then match every string literal in what is left. We deliberately drop
    very short strings (placeholders like '·', '—', '?') because they distort
    FKGL.
    """
    block = copy_block(text)
    if block is None:
        return []
    out: list[str] = []
    for sm in _STRING_LITERAL.finditer(block):
        s = sm.group(1) or sm.group(2) or sm.group(3) or ""
        # Skip identifier-only strings and very short fragments.
        if len(s.strip()) < 4:
            continue
        # Skip placeholders we know aren't sentences (e.g. "{bpm}").
        if re.fullmatch(r"\{?[a-zA-Z_]+\}?", s.strip()):
            continue
        out.append(s)
    return out


# ── FKGL ──────────────────────────────────────────────────────────────────


def fkgl(corpus: str) -> tuple[float, dict[str, int]]:
    sentences = max(1, len(re.findall(r"[.!?]+", corpus)))
    words = re.findall(r"[A-Za-z']+", corpus)
    word_count = len(words)
    if word_count == 0:
        return 0.0, {"words": 0, "sentences": sentences, "syllables": 0}
    syl = sum(syllables(w) for w in words)
    grade = (
        0.39 * (word_count / sentences)
        + 11.8 * (syl / word_count)
        - 15.59
    )
    return grade, {"words": word_count, "sentences": sentences, "syllables": syl}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("path", nargs="?", default=str(DEFAULT_PATH))
    p.add_argument(
        "--max-grade",
        type=float,
        default=DEFAULT_MAX_GRADE,
        help="Fail if aggregate FKGL exceeds this grade (default %(default)s).",
    )
    args = p.parse_args()

    path = Path(args.path)
    if not path.exists():
        print(f"[fkgl] file not found: {path}", file=sys.stderr)
        return 2
    text = path.read_text(encoding="utf-8")

    strings = extract_copy_strings(text)
    if not strings:
        print(f"[fkgl] no COPY strings extracted from {path}", file=sys.stderr)
        return 1

    corpus = " ".join(strings)
    grade, stats = fkgl(corpus)

    print(
        f"[fkgl] {len(strings)} strings, {stats['words']} words, "
        f"{stats['sentences']} sentences, {stats['syllables']} syllables"
    )
    print(f"[fkgl] aggregate Flesch-Kincaid grade: {grade:.2f}")
    print(f"[fkgl] ceiling: {args.max_grade}")

    if grade > args.max_grade:
        print(
            f"[fkgl] FAIL — corpus reads at grade {grade:.2f}, above "
            f"the {args.max_grade} ceiling.",
            file=sys.stderr,
        )
        print(
            "Tighten sentences, swap multi-syllable words for shorter "
            "synonyms, split 'and' clauses.",
            file=sys.stderr,
        )
        return 1

    print(f"[fkgl] OK — corpus reads at grade {grade:.2f} (≤ {args.max_grade}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
