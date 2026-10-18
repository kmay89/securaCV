#!/usr/bin/env python3
"""firmware/FEATURES.md's Last updated line reads once, newest first.

The line is the dashboard's running history: a header date and its note,
then one `YYYY-MM-DD:` entry per earlier pass, newest first. A merge of
#1746 appended its OTA entry after the oldest one and pasted a second copy
of the run from 2026-09-23 to 2026-07-02 behind it, and the line carried
both copies for two weeks (sweep D13). Neither defect broke anything a gate
read, so nothing noticed. This test holds the two properties that broke:

* the dated entries run newest first (an entry appended out of place puts
  an older date before a newer one), and
* no dated entry appears twice (a pasted copy repeats its whole run).

Run:  python3 -m unittest discover -s scripts/tests -p 'test_features_last_updated.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FEATURES = REPO / "firmware" / "FEATURES.md"

# An entry starts at a date followed by a colon. The header date is followed
# by " (" instead, so it is not an entry; inline dates in prose ("on
# 2026-09-05 pass") carry no colon and are not entries either.
ENTRY = re.compile(r"(?<![\w-])(\d{4}-\d{2}-\d{2}): ")


def last_updated_line(text: str) -> str:
    for line in text.splitlines():
        if line.startswith("**Last updated:**"):
            return line
    raise AssertionError("FEATURES.md has no **Last updated:** line")


def entries(line: str) -> list[tuple[str, str]]:
    """(date, passage) for each dated entry, passage running to the next."""
    marks = list(ENTRY.finditer(line))
    out = []
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(line)
        out.append((m.group(1), line[m.start():end].strip()))
    return out


def problems(line: str) -> list[str]:
    found = []
    header = re.match(r"\*\*Last updated:\*\* (\d{4}-\d{2}-\d{2}) \(", line)
    if not header:
        found.append("the line does not open with '**Last updated:** <date> ('")
    rows = entries(line)
    dates = ([header.group(1)] if header else []) + [d for d, _ in rows]
    for older, newer in zip(dates, dates[1:]):
        if newer > older:
            found.append(f"{newer} is listed after {older}: newest first")
    seen: dict[str, int] = {}
    for i, (_, passage) in enumerate(rows):
        if passage in seen:
            found.append(
                f"entry {i + 1} repeats entry {seen[passage] + 1} word for word: "
                f"{passage[:60]!r}")
        else:
            seen[passage] = i
    return found


class LastUpdatedLine(unittest.TestCase):
    def test_reads_once_newest_first(self):
        line = last_updated_line(FEATURES.read_text(encoding="utf-8"))
        self.assertGreater(len(entries(line)), 3, "the entry parser found no history")
        self.assertEqual(problems(line), [])

    # The guard's own guards: each shape the test exists to catch.
    GOOD = ("**Last updated:** 2026-10-13 (note. 2026-10-09: b. "
            "2026-09-30: c, on 2026-09-05 inline. 2026-07-02: d)")

    def test_accepts_newest_first(self):
        self.assertEqual(problems(self.GOOD), [])

    def test_refuses_an_entry_out_of_order(self):
        bad = self.GOOD.replace("2026-07-02: d)", "2026-07-02: d; 2026-09-29: e)")
        self.assertTrue(any("newest first" in p for p in problems(bad)), problems(bad))

    def test_refuses_an_entry_newer_than_the_header(self):
        bad = self.GOOD.replace("2026-10-09: b", "2026-10-14: b")
        self.assertTrue(any("newest first" in p for p in problems(bad)), problems(bad))

    def test_refuses_a_pasted_copy(self):
        bad = self.GOOD.replace("2026-10-09: b. ", "2026-10-09: b. 2026-10-09: b. ")
        self.assertEqual([p for p in problems(bad) if "word for word" not in p], [])
        self.assertTrue(any("word for word" in p for p in problems(bad)), problems(bad))


if __name__ == "__main__":
    unittest.main()
