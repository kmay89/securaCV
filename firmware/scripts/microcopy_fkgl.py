#!/usr/bin/env python3
"""
Flesch-Kincaid reading-grade lint for the headline Sensing dashboard.

The plan asks the dashboard's microcopy to read at ≤ 6th grade so it works
for grandmas and 10-year-olds. This script computes the aggregate
Flesch-Kincaid Grade Level across every user-facing string in
csi_dashboard_html.h's COPY object and fails if it scores worse than the
configured ceiling (default 6th grade — matches the spec's target with
no slack, since the corpus reads at ~2.7 today and any future jargon
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


def extract_copy_strings(text: str) -> list[str]:
    """Pull every user-facing string out of the COPY object literal.

    Strategy: find the COPY block, then within it match every double-
    or single-quoted string. We deliberately drop very short strings
    (placeholders like '·', '—', '?') because they distort FKGL.
    """
    m = re.search(r"\bconst\s+COPY\s*=\s*\{", text)
    if not m:
        return []
    # Walk to matching `};`
    i = m.end()
    depth = 1
    end = len(text)
    while i < len(text) and depth > 0:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
        i += 1
    block = text[m.end():end]

    # Match string literals. Tolerate \" and \' escapes inside.
    pattern = r'"((?:\\.|[^"\\])*)"|\'((?:\\.|[^\'\\])*)\''
    out: list[str] = []
    for sm in re.finditer(pattern, block):
        s = sm.group(1) or sm.group(2) or ""
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
        help="Fail if aggregate FKGL exceeds this grade (default 6.0).",
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
