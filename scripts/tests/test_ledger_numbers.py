"""The backlog files each id once, and the device-details design's findings
table, the backlog and the docs index name each other.

docs/audit/repo_todo_sweep_2026-09.md is the one place an open item gets its
id (a prefix such as F, A, HA, W, C, CI, D or U, and a number), and other
documents cite those ids. Nothing held either side. The device-details
design's reviews found one finding filed under two ids, a findings row naming
an id the backlog did not hold, one id on two headings, and an index line
whose range took in an unused number; a reader caught each, no gate did.

This suite holds:

- every backlog heading (`- [ ] **F375 [code] ...`, `[~]` for a part-done
  box) carries an id no other heading carries, and every top-level checkbox
  line is one this suite can read: a heading with an id, or an italic note
  with none (`- [ ] *(...`);
- every row of the design's section 11 table names a home that exists. A
  home is `ledger <id>` (optionally `; part fixed in #N`), an open backlog
  heading whose entry cites the row back as
  "`docs/design/device_details.md` §11, finding P<n>)", or `fixed in #N`,
  with no backlog entry. And every backlog entry that says "finding P<n>" is
  the one row P<n> names;
- the counts and lists that section 11's opening sentence and
  docs/README.md's index line state are exactly what the table states.

A list is ids separated by commas, with "and" before the last, and a range
is written `F375 to F377`. A list, a table row or either sentence that this
suite cannot read FAILS: a checker that skips what it cannot parse passes on
the line that is wrong.

Since #1951 the backlog holds open items only, so a fixed finding has no
entry to point at, and its row says which PR fixed it.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import re
import unittest
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LEDGER = REPO / "docs" / "audit" / "repo_todo_sweep_2026-09.md"
DESIGN = REPO / "docs" / "design" / "device_details.md"
INDEX = REPO / "docs" / "README.md"

ID = r"[A-Z]+\d+"
PRS = r"#\d+(?:, #\d+)*(?: and #\d+)?"
# A top-level checkbox line, and the two shapes one may take.
CHECKBOX = re.compile(r"^- \[(.)\] (.*)$", re.M)
HEADING_REST = re.compile(rf"\*\*({ID}) \[[^\]]+\] ")
NOTE_REST = re.compile(r"\*\(")
END_OF_ENTRY = re.compile(r"^(?:- \[|#|---)", re.M)
# How a backlog entry cites a section 11 row back, and any mention of a finding.
BACK_CITE = "`docs/design/device_details.md` §11, finding P{n})"
ANY_CITE = re.compile(r"\bfinding P(\d+)\b")
# A section 11 table row, and the forms its Home cell takes.
ROW = re.compile(r"^\| (P\d+) \| ([^|]*) \|")
HOME_OPEN = re.compile(rf"ledger ({ID})(?:; part fixed in {PRS})?")
HOME_FIXED = re.compile(rf"fixed in {PRS}")
# The two sentences that restate the table.
OPENING = re.compile(
    r"(\d+) privacy findings, P1 to P(\d+)\. Of them, (\d+) (?:is|are) open in the backlog, "
    r"`docs/audit/repo_todo_sweep_2026-09\.md` \(\*\*([^*()]+)\*\*\), "
    r"and (\d+) (?:is|are) fixed \(\*\*([^*()]+)\*\*\)\."
)
INDEX_LINE = re.compile(
    r"(\d+) privacy findings, (\d+) open in the backlog \(([^()]+)\) and (\d+) fixed \(([^()]+)\)"
)
ITEM = rf"{ID}(?: to {ID})?"
LIST = re.compile(rf"{ITEM}(?:(?:, {ITEM})* and {ITEM})?")
LIST_ITEM = re.compile(r"([A-Z]+)(\d+)(?: to ([A-Z]+)(\d+))?")


def flat(text: str) -> str:
    """One line: the backlog and the index are hard-wrapped at different widths."""
    return " ".join(text.split())


def ledger_entries(text: str) -> tuple[list[tuple[str, str, str]], list[str]]:
    """([(id, box, flattened entry)], [checkbox lines this suite cannot read])."""
    entries, unread = [], []
    for m in CHECKBOX.finditer(text):
        box, rest = m.group(1), m.group(2)
        head = HEADING_REST.match(rest)
        if box not in " x~" or not (head or NOTE_REST.match(rest)):
            unread.append(m.group(0))
            continue
        if not head:
            continue
        nxt = END_OF_ENTRY.search(text, m.end())
        entries.append((head.group(1), box, flat(text[m.start():nxt.start() if nxt else len(text)])))
    return entries, unread


def parse_list(text: str) -> list[str]:
    """`F357, F375 to F377 and A132` -> its ids, ranges expanded; `none` -> [].

    Raises ValueError on anything else: a dash for a range, a missing "and",
    a range across two prefixes or running backward, an id listed twice.
    """
    if text == "none":
        return []
    if not LIST.fullmatch(text):
        raise ValueError(f"not a list this test can read: {text!r}")
    out: list[str] = []
    for m in LIST_ITEM.finditer(text):
        prefix, a, prefix_b, b = m.group(1), int(m.group(2)), m.group(3), m.group(4)
        if prefix_b is None:
            out.append(f"{prefix}{a}")
        elif prefix_b != prefix or int(b) <= a:
            raise ValueError(f"not a list this test can read: {m.group(0)!r} is not a range")
        else:
            out.extend(f"{prefix}{n}" for n in range(a, int(b) + 1))
    twice = sorted(i for i, c in Counter(out).items() if c > 1)
    if twice:
        raise ValueError(f"not a list this test can read: {twice} listed twice in {text!r}")
    return out


def section_11(design_text: str) -> str:
    start = design_text.index("\n## 11. ")
    return design_text[start:design_text.index("\n## 12. ", start)]


def table_rows(section: str) -> tuple[dict[int, tuple[str, str | None]], list[str]]:
    """({P: ("open", id) or ("fixed", None)}, [problems])."""
    rows: dict[int, tuple[str, str | None]] = {}
    problems: list[str] = []
    order: list[int] = []
    for line in section.splitlines():
        if not line.startswith("| P"):
            continue
        m = ROW.match(line)
        if not m:
            problems.append(f"a row this test cannot read: {line[:70]}")
            continue
        p, home = int(m.group(1)[1:]), m.group(2).strip()
        order.append(p)
        opened = HOME_OPEN.fullmatch(home)
        if opened:
            rows[p] = ("open", opened.group(1))
        elif HOME_FIXED.fullmatch(home):
            rows[p] = ("fixed", None)
        else:
            problems.append(f"P{p}'s Home cell is none of `ledger <id>`, `ledger <id>; part "
                            f"fixed in #N` and `fixed in #N`: {home!r}")
    if order != list(range(1, len(order) + 1)):
        problems.append(f"the rows are not P1 to P{len(order)}, each once and in order: {order}")
    return rows, problems


def check(ledger_text: str, design_text: str, index_text: str, min_headings: int = 100) -> list[str]:
    """Every way the three files disagree, as sentences. Empty when they agree."""
    problems: list[str] = []
    entries, unread = ledger_entries(ledger_text)
    problems += [f"a backlog checkbox line that is neither a heading with an id nor an italic "
                 f"note: {line[:70]}" for line in unread]
    ids = [i for i, _, _ in entries]
    twice = sorted(i for i, c in Counter(ids).items() if c > 1)
    if twice:
        problems.append(f"ids on more than one backlog heading: {twice}")
    if len(ids) < min_headings:
        problems.append(f"only {len(ids)} backlog headings read: the heading pattern no longer fits")
    by_id = {i: (box, body) for i, box, body in entries}

    section = section_11(design_text)
    rows, row_problems = table_rows(section)
    problems += row_problems
    if not rows:
        problems.append("no section 11 rows read")

    # Each open row's home exists, is open, and cites the row back.
    for p, (kind, home) in sorted(rows.items()):
        if kind != "open":
            continue
        if home not in by_id:
            problems.append(f"P{p} names ledger {home}, which heads no backlog entry")
            continue
        box, body = by_id[home]
        if box == "x":
            problems.append(f"P{p} names ledger {home}, whose box is ticked: the backlog holds open "
                            f"items only, so the row says `fixed in #N` and the entry goes")
        if BACK_CITE.format(n=p) not in body:
            problems.append(f"{home} does not cite P{p} back as {BACK_CITE.format(n=p)!r}")
    # Each backlog entry that cites a finding is the one that finding's row names.
    cited_anywhere = Counter(int(n) for n in ANY_CITE.findall(flat(ledger_text)))
    cited_in_entries: Counter[int] = Counter()
    for entry_id, _, body in entries:
        for n in ANY_CITE.findall(body):
            cited_in_entries[int(n)] += 1
            named = rows.get(int(n))
            if named != ("open", entry_id):
                where = ("no row" if named is None else
                         "a fixed row" if named[1] is None else f"ledger {named[1]}")
                problems.append(f"{entry_id} cites finding P{n}, whose row names {where}")
    if cited_anywhere != cited_in_entries:
        problems.append("the backlog says \"finding P<n>\" outside any entry with an id: "
                        f"{sorted((cited_anywhere - cited_in_entries).elements())}")

    open_rows = {p: home for p, (kind, home) in rows.items() if kind == "open"}
    fixed_rows = sorted(p for p, (kind, _) in rows.items() if kind == "fixed")

    def held(where: str, total: str, n_open: str, open_list: str, n_fixed: str, fixed_list: str) -> None:
        try:
            stated_open, stated_fixed = parse_list(open_list), parse_list(fixed_list)
        except ValueError as exc:
            problems.append(f"{where}: {exc}")
            return
        if int(total) != len(rows):
            problems.append(f"{where} says {total} findings; the table has {len(rows)}")
        if int(n_open) != len(open_rows) or int(n_fixed) != len(fixed_rows):
            problems.append(f"{where} says {n_open} open and {n_fixed} fixed; the table has "
                            f"{len(open_rows)} and {len(fixed_rows)}")
        if sorted(stated_open) != sorted(set(open_rows.values())):
            problems.append(f"{where} lists open ids {sorted(stated_open)}; the table's homes are "
                            f"{sorted(set(open_rows.values()))}")
        if stated_fixed != [f"P{p}" for p in fixed_rows]:
            problems.append(f"{where} lists fixed findings {stated_fixed}; the table's are "
                            f"{[f'P{p}' for p in fixed_rows]}")

    opening = OPENING.search(flat(section))
    if not opening:
        problems.append("section 11's opening sentence is not in the form this test reads: "
                        "\"N privacy findings, P1 to PN. Of them, N are open in the backlog, "
                        "`docs/audit/repo_todo_sweep_2026-09.md` (**ids**), and N are fixed (**Ps**).\"")
    else:
        total, last, n_open, open_list, n_fixed, fixed_list = opening.groups()
        if total != last:
            problems.append(f"section 11's opening sentence says {total} findings and P1 to P{last}")
        held("section 11's opening sentence", total, n_open, open_list, n_fixed, fixed_list)

    lines = [ln for ln in index_text.splitlines() if "(design/device_details.md)" in ln]
    if len(lines) != 1:
        problems.append(f"docs/README.md indexes the design {len(lines)} times, not once")
    else:
        stated = INDEX_LINE.search(lines[0])
        if not stated:
            problems.append("docs/README.md's index line does not state the findings in the form "
                            "this test reads: \"N privacy findings, N open in the backlog (ids) "
                            "and N fixed (Ps)\"")
        else:
            held("docs/README.md's index line", *stated.groups())
            # An id or a finding named anywhere else on the line is a second,
            # unheld statement of the same thing.
            rest = lines[0][:stated.start()] + lines[0][stated.end():]
            prefixes = sorted({re.match(r"[A-Z]+", home).group(0) for home in open_rows.values()} | {"P"})
            loose = re.findall(rf"\b(?:{'|'.join(prefixes)})\d+\b", rest)
            if loose:
                problems.append(f"docs/README.md's index line names {loose} outside its two lists")
    return problems


class TheBacklogAndTheDeviceDetailsFindings(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ledger = LEDGER.read_text(encoding="utf-8")
        cls.design = DESIGN.read_text(encoding="utf-8")
        cls.index = INDEX.read_text(encoding="utf-8")

    def test_every_backlog_id_is_on_one_heading(self):
        entries, unread = ledger_entries(self.ledger)
        self.assertEqual(unread, [], "checkbox lines that are neither a heading with an id nor an "
                                     "italic note")
        ids = [i for i, _, _ in entries]
        self.assertGreater(len(ids), 100, "the heading pattern no longer fits the backlog")
        # Every prefix the backlog uses is read, not only F.
        self.assertGreaterEqual({re.match(r"[A-Z]+", i).group(0) for i in ids},
                                {"A", "C", "CI", "D", "F", "HA", "U", "W"})
        twice = sorted(i for i, c in Counter(ids).items() if c > 1)
        self.assertEqual(twice, [], f"ids on more than one backlog heading: {twice}")

    def test_the_table_the_backlog_and_the_index_agree(self):
        self.assertEqual(check(self.ledger, self.design, self.index), [])


# A backlog, a design and an index in miniature, in the real files' shapes.
# Six findings: four open (one of them part fixed), two fixed.
MINI_LEDGER = """# Open work

## 1. Firmware

- [ ] **U1 [human] One bench session.** Text.

- [ ] **F1 [code] First.** What is wrong, where. Found by the device-details
  design's privacy review (`docs/design/device_details.md` §11, finding P1).

- [ ] **F2 [code+decision] Second.** Text (`docs/design/device_details.md`
  §11, finding P2).

- [~] **F4 [code] Fourth.** Text (`docs/design/device_details.md` §11,
  finding P4).

- [ ] *(A note with no id.)*

---

## 2. Apps

- [ ] **A2 [code] What the fix left (device-details finding P5).** Text
  (`docs/design/device_details.md` §11, finding P5).
"""
MINI_DESIGN = """# Device details

## 11. Privacy findings, and where each stands

The review produced 6 privacy findings, P1 to P6. Of them, 4 are open in the backlog, `docs/audit/repo_todo_sweep_2026-09.md` (**F1 to F2, F4 and A2**), and 2 are fixed (**P3 and P6**).

| # | Home | Finding | What a fix did, and what remains |
|---|---|---|---|
| P1 | ledger F1 | One. | Open. |
| P2 | ledger F2 | Two. | Open. |
| P3 | fixed in #10 | Three. | #10 did it. |
| P4 | ledger F4 | Four. | Open. |
| P5 | ledger A2; part fixed in #11 | Five. | #11 did part. |
| P6 | fixed in #12, #13 and #14 | Six. | Done. |

---

## 12. PR plan
"""
MINI_INDEX = """# Docs

- [Other (design)](design/other.md) — text
- [Device details (design)](design/device_details.md) — design, partly built (#11). Its §11 lists the 6 privacy findings, 4 open in the backlog (F1 to F2, F4 and A2) and 2 fixed (P3 and P6)
"""


class TheChecksCanFail(unittest.TestCase):
    """Each rule, broken in a miniature of the three files: a check that
    cannot fail holds nothing."""

    def broken(self, needle: str, ledger: str = MINI_LEDGER, design: str = MINI_DESIGN,
               index: str = MINI_INDEX) -> None:
        problems = check(ledger, design, index, min_headings=1)
        self.assertTrue(any(needle in p for p in problems),
                        f"expected a problem containing {needle!r}, got {problems}")

    def swap(self, text: str, old: str, new: str) -> str:
        self.assertEqual(text.count(old), 1, f"the planted edit's anchor must occur once: {old!r}")
        return text.replace(old, new)

    def test_the_miniature_is_clean(self):
        self.assertEqual(check(MINI_LEDGER, MINI_DESIGN, MINI_INDEX, min_headings=1), [])

    def test_too_few_headings_read(self):
        # The real backlog's floor: a heading pattern that stops matching reads as a failure.
        self.assertTrue(any("the heading pattern no longer fits" in p
                            for p in check(MINI_LEDGER, MINI_DESIGN, MINI_INDEX)))

    def test_an_id_on_two_headings_under_any_prefix_and_box(self):
        for twin in ("- [ ] **F1 [code] Again.** Text.", "- [~] **U1 [human] Again.** Text.",
                     "- [x] **A2 [decision] Again.** Text."):
            with self.subTest(twin=twin):
                self.broken("ids on more than one backlog heading", ledger=MINI_LEDGER + "\n" + twin + "\n")

    def test_a_checkbox_line_this_suite_cannot_read(self):
        for line in ("- [ ] **F9** No tag.", "- [ ] F9 [code] No bold.", "- [?] **F9 [code] Odd box.**",
                     "- [ ] **f9 [code] Lower case.**", "- [ ] Plain text."):
            with self.subTest(line=line):
                self.broken("neither a heading with an id nor an italic note",
                            ledger=MINI_LEDGER + "\n" + line + "\n")

    def test_a_row_naming_an_id_the_backlog_does_not_hold(self):
        design = self.swap(MINI_DESIGN, "| P1 | ledger F1 |", "| P1 | ledger F3 |")
        self.broken("P1 names ledger F3, which heads no backlog entry", design=design)

    def test_a_home_that_does_not_cite_its_row_back(self):
        self.broken("F2 does not cite P2 back", ledger=self.swap(MINI_LEDGER, "§11, finding P2).", "§11)."))
        # The plain mention in A2's title is not the cite.
        self.broken("A2 does not cite P5 back",
                    ledger=self.swap(MINI_LEDGER, "\n  (`docs/design/device_details.md` §11, finding P5).", ""))

    def test_an_entry_citing_a_row_that_names_another_home(self):
        other = "\n- [ ] **F9 [code] Another.** It says finding P1 is its own.\n"
        self.broken("F9 cites finding P1, whose row names ledger F1", ledger=MINI_LEDGER + other)

    def test_an_entry_citing_a_fixed_or_unknown_finding(self):
        self.broken("F9 cites finding P3, whose row names a fixed row",
                    ledger=MINI_LEDGER + "\n- [ ] **F9 [code] Another.** See finding P3.\n")
        self.broken("F9 cites finding P7, whose row names no row",
                    ledger=MINI_LEDGER + "\n- [ ] **F9 [code] Another.** See finding P7.\n")

    def test_a_finding_cited_outside_any_entry(self):
        self.broken("outside any entry with an id",
                    ledger=self.swap(MINI_LEDGER, "## 2. Apps\n", "## 2. Apps\n\nSee finding P1.\n"))
        self.broken("outside any entry with an id",
                    ledger=self.swap(MINI_LEDGER, "*(A note with no id.)*", "*(A note on finding P1.)*"))

    def test_a_ticked_home(self):
        self.broken("P1 names ledger F1, whose box is ticked",
                    ledger=self.swap(MINI_LEDGER, "- [ ] **F1 [code]", "- [x] **F1 [code]"))

    def test_a_home_cell_in_no_known_form(self):
        for home in ("note on F1", "F1", "ledger F1, part fixed in #1", "ledger F1; fixed in #1",
                     "fixed in 10", "fixed", "ledger", ""):
            with self.subTest(home=home):
                self.broken("P1's Home cell is none of",
                            design=self.swap(MINI_DESIGN, "| P1 | ledger F1 |", f"| P1 | {home} |"))

    def test_a_row_this_suite_cannot_read(self):
        self.broken("a row this test cannot read",
                    design=self.swap(MINI_DESIGN, "| P4 | ledger F4 |", "| P4| ledger F4 |"))

    def test_a_missing_repeated_or_misplaced_row(self):
        row = "| P3 | fixed in #10 | Three. | #10 did it. |\n"
        self.broken("each once and in order", design=self.swap(MINI_DESIGN, row, ""))
        self.broken("each once and in order", design=self.swap(MINI_DESIGN, row, row + row))
        self.broken("each once and in order",
                    design=self.swap(MINI_DESIGN, row, "").replace("| P6 |", row + "| P6 |"))
        self.broken("no section 11 rows read",
                    design="".join(ln for ln in MINI_DESIGN.splitlines(True) if not ln.startswith("| P")))

    def sentence_cases(self, n_open: str, n_fixed: str):
        """(what is planted, old, new, the problem it must raise), for either sentence."""
        return [
            ("an id the table does not name", "F1 to F2, F4 and A2", "F1 to F2, F4 and A3", "lists open ids"),
            ("an open id left out", "F1 to F2, F4 and A2", "F1 to F2 and A2", "lists open ids"),
            ("a range that takes in an unfiled id", "F1 to F2, F4 and A2", "F1 to F4 and A2", "lists open ids"),
            ("a fixed finding left out", "P3 and P6", "P6", "lists fixed findings"),
            ("an open finding listed as fixed", "P3 and P6", "P1, P3 and P6", "lists fixed findings"),
            ("the fixed findings out of order", "P3 and P6", "P6 and P3", "lists fixed findings"),
            ("a wrong open count", n_open, n_open.replace("4", "5"), "says 5 open and 2 fixed"),
            ("a wrong fixed count", n_fixed, n_fixed.replace("2", "3"), "says 4 open and 3 fixed"),
            ("a hyphen for a range", "F1 to F2, F4 and A2", "F1-F2, F4 and A2", "not a list this test can read"),
            ("an en dash for a range", "F1 to F2, F4 and A2", "F1–F2, F4 and A2", "not a list this test can read"),
            ("no and before the last id", "F1 to F2, F4 and A2", "F1 to F2, F4, A2", "not a list this test can read"),
            ("an id listed twice", "F1 to F2, F4 and A2", "F1 to F2, F2 and A2", "not a list this test can read"),
            ("a range across two prefixes", "F1 to F2, F4 and A2", "F1 to A2 and F4", "not a list this test can read"),
        ]

    def test_the_opening_sentence_out_of_step_with_the_table(self):
        opening = next(ln for ln in MINI_DESIGN.splitlines() if "are open in the backlog" in ln)
        for name, old, new, needle in self.sentence_cases("4 are open", "2 are fixed"):
            with self.subTest(case=name):
                self.broken(needle, design=MINI_DESIGN.replace(opening, self.swap(opening, old, new)))
        self.broken("says 7 findings; the table has 6",
                    design=self.swap(MINI_DESIGN, "6 privacy findings, P1 to P6", "7 privacy findings, P1 to P7"))
        self.broken("says 6 findings and P1 to P7",
                    design=self.swap(MINI_DESIGN, "6 privacy findings, P1 to P6", "6 privacy findings, P1 to P7"))
        for reworded in (("are open in the backlog,", "are still open in the backlog,"),
                         ("(**F1 to F2, F4 and A2**)", "(F1 to F2, F4 and A2)"),
                         ("`docs/audit/repo_todo_sweep_2026-09.md` ", "")):
            with self.subTest(reworded=reworded):
                self.broken("section 11's opening sentence is not in the form this test reads",
                            design=self.swap(MINI_DESIGN, *reworded))

    def test_the_index_line_out_of_step_with_the_table(self):
        line = next(ln for ln in MINI_INDEX.splitlines() if "(design/device_details.md)" in ln)
        for name, old, new, needle in self.sentence_cases("4 open in the backlog", "2 fixed"):
            with self.subTest(case=name):
                self.broken(needle, index=MINI_INDEX.replace(line, self.swap(line, old, new)))
        self.broken("says 5 findings; the table has 6",
                    index=self.swap(MINI_INDEX, "the 6 privacy findings", "the 5 privacy findings"))
        self.broken("does not state the findings in the form this test reads",
                    index=self.swap(MINI_INDEX, "6 privacy findings, 4 open in the backlog (F1 to F2, F4 and A2) "
                                                "and 2 fixed (P3 and P6)", "privacy findings, F1-F4"))
        self.broken("names ['F4'] outside its two lists", index=MINI_INDEX.replace(line, line + ", F4 first"))
        self.broken("names ['A2'] outside its two lists", index=MINI_INDEX.replace(line, line + " (see A2)"))
        self.broken("names ['P1'] outside its two lists", index=MINI_INDEX.replace(line, line + "; P1 is worst"))
        self.broken("indexes the design 2 times", index=MINI_INDEX + line + "\n")
        self.broken("indexes the design 0 times", index=MINI_INDEX.replace(line + "\n", ""))

    def test_none_is_an_empty_list(self):
        ledger = MINI_LEDGER
        for entry_id in ("F1", "F2", "F4", "A2"):
            a = ledger.index(f"**{entry_id} [")
            a = ledger.rindex("\n- [", 0, a)
            b = END_OF_ENTRY.search(ledger, a + 3)
            ledger = ledger[:a + 1] + (ledger[b.start():] if b else "")
        design = MINI_DESIGN
        for p, home in ((1, "ledger F1"), (2, "ledger F2"), (4, "ledger F4"), (5, "ledger A2; part fixed in #11")):
            design = self.swap(design, f"| P{p} | {home} |", f"| P{p} | fixed in #2{p} |")
        design = self.swap(design, "4 are open in the backlog", "0 are open in the backlog")
        design = self.swap(design, "(**F1 to F2, F4 and A2**)", "(**none**)")
        design = self.swap(design, "2 are fixed (**P3 and P6**)", "6 are fixed (**P1, P2, P3, P4, P5 and P6**)")
        index = self.swap(MINI_INDEX, "4 open in the backlog (F1 to F2, F4 and A2) and 2 fixed (P3 and P6)",
                          "0 open in the backlog (none) and 6 fixed (P1, P2, P3, P4, P5 and P6)")
        self.assertEqual(check(ledger, design, index, min_headings=1), [])

    def test_parse_list(self):
        self.assertEqual(parse_list("none"), [])
        self.assertEqual(parse_list("F357"), ["F357"])
        self.assertEqual(parse_list("P8 and P11"), ["P8", "P11"])
        self.assertEqual(parse_list("F357, F375 to F377, A132 and A133"),
                         ["F357", "F375", "F376", "F377", "A132", "A133"])
        for bad in ("", " ", "F357 F375", "F375-F377", "F375 to A377", "F377 to F375", "F375 to F375",
                    "F357, F357 and A1", "F357 and F375 and A1", "F357, F375", "F357; F375 and A1",
                    "F375 to F377 and F376", "f357 and F375", "F357, and F375", "F357 and", "None",
                    "F357 and none", " F357", "F357 "):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                parse_list(bad)


if __name__ == "__main__":
    unittest.main()
