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
  box) carries an id no other heading carries, under any prefix. Every
  top-level checkbox line is one this suite can read: a heading with an id, or
  an italic note with none (`- [ ] *(...`). And no other line looks like a
  heading (a bold id before a tag anywhere, or a first-column list item or
  Markdown heading that opens with an id and a `[code`, `[human` or
  `[decision` tag): ids are read from headings in the one form, so an id on a
  heading in another form would be an id nothing checks for a twin;
- every row of the design's section 11 table names a home that exists. A
  home is `ledger <id>` (optionally `; part fixed in #N`), an open backlog
  heading whose entry cites the row back as
  "`docs/design/device_details.md` §11, finding P<n>)", or `fixed in #N`,
  with no backlog entry;
- every backlog entry that says "finding P<n>" is the one row P<n> names
  (see the rule below);
- the counts and lists that section 11's opening sentence and
  docs/README.md's index line state are exactly what the table states.

One rule may surprise the next person. In the backlog, the phrase "finding
P<n>" ("Finding P<n>" at the start of a sentence) is reserved: it always
means row P<n> of the design's section 11, and only the entry that row names
as its home may say it, in its title or its text, inside a code span or not.
That is how one finding stays under one id. An entry that only refers to a
finding cites the home's id instead ("see F377"). Prose between entries and
italic notes may not say it at all, because they carry no id to hold it to.
A cite in any other shape (a plural, "findings P3 and P4"; markup around the
number; a hyphen; a leading zero) fails as unreadable, so write one "finding
P<n>" per finding. A bare P<n> with no "finding" before it is not read: the
backlog also writes P0 and P1 for priority rows and privacy ceilings. If
another document ever numbers its findings P<n>, this suite will read a
backlog cite of them as the design's; teach it that document then.

What the next edit most likely trips, and what to do:

- A PR finishes a backlog item that is a finding's home and deletes the
  entry, as the backlog's header asks. The row still says `ledger <id>`.
  Write its Home cell as `fixed in #<the PR>` (or keep `ledger <id>; part
  fixed in #N` while the entry holds a rest), and move the finding from the
  open list to the fixed list, with both counts, in section 11's opening
  sentence and in the index line.
- The design gains a finding: add the row, file its home with the cite, and
  update the same two sentences.

An entry is its heading line and the indented or blank lines under it. A list
is ids separated by commas, with "and" before the last, and a range is
written `F375 to F377`. A heading, a cite, a list, a table row or either
sentence that this suite cannot read FAILS: a checker that skips what it
cannot parse passes on the line that is wrong. Numbers are ASCII digits with
no leading zero.

Not held: any number or id outside the table's Home column, the two
sentences and the backlog's cites (the design's prose cites `ledger <id>` in
other sections too), whether a `fixed in #N` PR exists, and whether a number
was used before by an item since deleted (CHANGELOG.md and `git log -p` on
the backlog are that history). Two false alarms are accepted, and each
message says how to reword: an id-shaped word (A4, say) in an index entry
that links the design reads as an id named outside the two lists, and a bold
id directly before a bracket in backlog text (`**C6 [`) reads as a heading
written another way.

Since #1951 the backlog holds open items only, so a fixed finding has no
entry to point at, and its row says which PR fixed it.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_ledger_numbers.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests), unfiltered
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

# ASCII digits, no leading zero: F01 beside F1 would be two ids for one item,
# and `\d` also takes digits no keyboard here types.
NUM = r"[1-9][0-9]*"
COUNT = rf"(?<![0-9])(?:0|{NUM})"
ID = rf"[A-Z]+{NUM}"
PR = rf"#{NUM}"
PRS = rf"{PR}(?:(?:, {PR})* and {PR})?"

# A top-level checkbox line, and the two shapes one may take. The tag may end
# the line, with the title wrapped onto the next.
CHECKBOX = re.compile(r"^- \[(.)\] (.*)$")
BOXES = " ~xX"
HEADING_REST = re.compile(rf"\*\*({ID}) \[[^\]]+\](?: |$)")
NOTE_REST = re.compile(r"\*\(")
# Anything else that looks like a heading: a bold id before a tag, anywhere on
# a line (the `**` opens the bold: it follows a space, a bracket or nothing), or
# a first-column list item or Markdown heading that opens with an id and one of
# the backlog's three tag words. Deliberately loose about the id. A code span
# is read as one backtick: a heading quoted in one is an example, not a heading.
HEADING_LIKE = re.compile(
    r"(?:^|(?<=[\s(\[\]>]))\*\*\s*[^\W\d_]+\d\w*\s*\["
    r"|^(?:[-*+]|\d+[.)]|#+)\s*(?:\[[^\]]?\]\s*)?[*_]*\s*[^\W\d_]+\d\w*\s*\[\s*(?i:code|human|decision)\b")
CODE_SPAN = re.compile(r"`[^`]*`")
HEADING_FORM = ("A heading is `- [ ] **F375 [code] Title.**` from the first column: a dash, a box (`[ ]`, "
                "or `[~]` for part done), then the id in bold with its tag in brackets after it. An id is "
                "capital letters and a number with no leading zero.")

# How a backlog entry cites a section 11 row back; the reserved phrase; and
# any other way of pointing at a finding by number, which is unreadable.
BACK_CITE = "`docs/design/device_details.md` §11, finding P{n})"
CITE = re.compile(rf"\b[Ff]inding P({NUM})\b")
CITE_LIKE = re.compile(r"\bfindings?\b[\s`*_(\[]*P\s*-?\s*\d", re.I)
CITE_RULE = ("In the backlog the phrase \"finding P<n>\" is reserved for the one entry that row P<n> of "
             "`docs/design/device_details.md` §11 names as its home, so that no finding is filed under two ids.")

# The section 11 table: its header, its rule line, a row, and the forms a
# row's Home cell takes.
TABLE_HEAD = re.compile(r"^\| # \| Home \|")
TABLE_RULE = re.compile(r"^\|(?: ?:?-+:? ?\|)+$")
ROW = re.compile(rf"^\| P({NUM}) \| ([^|]*) \|")
ROW_LIKE = re.compile(r"^\s*\|\s*[`*_]*P\d")
PIPE = re.compile(r"(?<!\\)\|")
ROW_FORM = ("A row is `| P<n> | <home> | <finding> | <what a fix did> |` from the first column, with as many "
            "cells as the header; write a pipe inside a cell as `\\|`.")
HOME_OPEN = re.compile(rf"ledger ({ID})(?:; part fixed in {PRS})?")
HOME_FIXED = re.compile(rf"fixed in {PRS}")
MOVE_TO_FIXED = ("write the row's Home cell as `fixed in #<that PR>`, and move the finding from the open list "
                 "to the fixed list, with both counts, in section 11's opening sentence and in "
                 "docs/README.md's index line")

# The two sentences that restate the table.
OPENING = re.compile(
    rf"({COUNT}) privacy findings, P1 to P({COUNT})\. Of them, ({COUNT}) (?:is|are) open in the backlog, "
    r"`docs/audit/repo_todo_sweep_2026-09\.md` \(\*\*([^*()]+)\*\*\), "
    rf"and ({COUNT}) (?:is|are) fixed \(\*\*([^*()]+)\*\*\)\."
)
OPENING_FORM = ("\"N privacy findings, P1 to PN. Of them, N are open in the backlog, "
                "`docs/audit/repo_todo_sweep_2026-09.md` (**ids**), and N are fixed (**Ps**).\"")
INDEX_LINK = "(design/device_details.md)"
INDEX_LINE = re.compile(
    rf"({COUNT}) privacy findings, ({COUNT}) open in the backlog \(([^()]+)\) and ({COUNT}) fixed \(([^()]+)\)"
)
INDEX_FORM = "\"N privacy findings, N open in the backlog (ids) and N fixed (Ps)\""
ITEM = rf"{ID}(?: to {ID})?"
LIST = re.compile(rf"{ITEM}(?:(?:, {ITEM})* and {ITEM})?")
LIST_ITEM = re.compile(r"([A-Z]+)([0-9]+)(?: to ([A-Z]+)([0-9]+))?")
LIST_FORM = ("Write ids separated by \", \" with \" and \" before the last and no comma before it, a range as "
             "\"F375 to F377\", each id once, or \"none\".")


def flat(text: str) -> str:
    """One line: the backlog and the index are hard-wrapped at different widths."""
    return " ".join(text.split())


def ledger_entries(text: str) -> tuple[list[tuple[str, str, str]], list[str], list[str]]:
    """([(id, box, flattened entry)], [checkbox lines this suite cannot read],
    [other lines that look like a heading]).

    An entry is its heading line and the indented or blank lines under it, so
    prose that starts in the first column belongs to no entry.
    """
    entries, unread, lookalikes = [], [], []
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = CHECKBOX.match(line)
        if not m:
            if HEADING_LIKE.search(CODE_SPAN.sub("`", line)):
                lookalikes.append(line)
            continue
        box, rest = m.group(1), m.group(2)
        head = HEADING_REST.match(rest)
        if box not in BOXES or not (head or NOTE_REST.match(rest)):
            unread.append(line)
            continue
        if not head:
            continue
        end = i + 1
        while end < len(lines) and (not lines[end].strip() or lines[end][0] in " \t"):
            end += 1
        entries.append((head.group(1), box.lower(), flat("\n".join(lines[i:end]))))
    return entries, unread, lookalikes


def cites(text: str) -> tuple[list[int], list[str]]:
    """([n for each "finding P<n>"], [each pointer at a finding this suite cannot read, with context])."""
    read = {m.start(): int(m.group(1)) for m in CITE.finditer(text)}
    odd = [text[max(0, m.start() - 30):m.end() + 12] for m in CITE_LIKE.finditer(text) if m.start() not in read]
    return list(read.values()), odd


def parse_list(text: str) -> list[str]:
    """`F357, F375 to F377 and A132` -> its ids, ranges expanded; `none` -> [].

    Raises ValueError on anything else: a dash for a range, a missing "and",
    a range across two prefixes or running backward, an id listed twice.
    """
    if text == "none":
        return []
    if not LIST.fullmatch(text):
        raise ValueError(f"not a list this test can read: {text!r}. {LIST_FORM}")
    out: list[str] = []
    for m in LIST_ITEM.finditer(text):
        prefix, a, prefix_b, b = m.group(1), int(m.group(2)), m.group(3), m.group(4)
        if prefix_b is None:
            out.append(f"{prefix}{a}")
        elif prefix_b != prefix or int(b) <= a:
            raise ValueError(f"not a list this test can read: {m.group(0)!r} is not a range. {LIST_FORM}")
        else:
            out.extend(f"{prefix}{n}" for n in range(a, int(b) + 1))
    twice = sorted(i for i, c in Counter(out).items() if c > 1)
    if twice:
        raise ValueError(f"not a list this test can read: {twice} listed twice in {text!r}. {LIST_FORM}")
    return out


def section_11(design_text: str) -> str | None:
    """The design's section 11, or None when it is not between `## 11. ` and `## 12. `."""
    start = design_text.find("\n## 11. ")
    end = design_text.find("\n## 12. ", start + 1) if start >= 0 else -1
    return design_text[start:end] if end >= 0 else None


def table_rows(section: str) -> tuple[dict[int, tuple[str, str | None]], list[str]]:
    """({P: ("open", id) or ("fixed", None)}, [problems])."""
    rows: dict[int, tuple[str, str | None]] = {}
    problems: list[str] = []
    order: list[int] = []
    all_read = True
    lines = section.splitlines()
    heads = [i for i, line in enumerate(lines) if TABLE_HEAD.match(line)]
    if len(heads) != 1:
        return rows, [f"section 11 has {len(heads)} tables headed `| # | Home |`, not one: this test reads the "
                      f"findings from the table whose first two columns are the P-number and its home. If the "
                      f"design's sections were renumbered, section 11 is no longer the findings: update "
                      f"section_11() and BACK_CITE in this test and the `§11` in each backlog cite."]
    end = heads[0]
    while end < len(lines) and lines[end].startswith("|"):
        end += 1
    width = len(PIPE.findall(lines[heads[0]]))
    body = lines[heads[0] + 1:end]
    if body and TABLE_RULE.match(body[0]):
        body = body[1:]
    else:
        problems.append("section 11's table has no `|---|` rule line under its header, so it is not a table")
    for line in body:
        m = ROW.match(line)
        cells = len(PIPE.findall(line)) - 1
        if not m or cells != width - 1:
            why = f" (it has {cells} cells; the header has {width - 1})" if m else ""
            problems.append(f"a row this test cannot read{why}: {line[:70]!r}. {ROW_FORM}")
            all_read = False
            continue
        p, home = int(m.group(1)), m.group(2).strip()
        order.append(p)
        opened = HOME_OPEN.fullmatch(home)
        if opened:
            rows[p] = ("open", opened.group(1))
        elif HOME_FIXED.fullmatch(home):
            rows[p] = ("fixed", None)
        else:
            problems.append(f"P{p}'s Home cell is none of `ledger <id>`, `ledger <id>; part "
                            f"fixed in #N` and `fixed in #N`: {home!r}. Several PRs are written "
                            f"`#1, #2 and #3`.")
    for line in lines[:heads[0]] + lines[end:]:
        if ROW_LIKE.match(line):
            problems.append(f"a findings row outside the table: {line[:70]!r}. Every row starts in the first "
                            f"column, directly under the row before it (a blank line ends a Markdown table).")
            all_read = False
    # A gap that an unread row explains is not a second problem.
    if all_read and order != list(range(1, len(order) + 1)):
        problems.append(f"the rows are not P1 to P{len(order)}, each once and in order: {order}")
    return rows, problems


def index_entries(index_text: str) -> list[str]:
    """Each docs/README.md entry that links the design, as one line: the line
    that opens the entry and the indented lines that continue it."""
    lines = index_text.splitlines()
    starts: list[int] = []
    for i, line in enumerate(lines):
        if INDEX_LINK not in line:
            continue
        while i > 0 and lines[i][:1] in (" ", "\t") and lines[i - 1].strip():
            i -= 1
        if i not in starts:
            starts.append(i)
    out = []
    for i in starts:
        end = i + 1
        while end < len(lines) and lines[end].strip() and lines[end][0] in " \t":
            end += 1
        out.append(flat("\n".join(lines[i:end])))
    return out


def backlog_problems(ledger_text: str) -> list[str]:
    """Every way the backlog's headings fail to give each id one heading."""
    problems: list[str] = []
    entries, unread, lookalikes = ledger_entries(ledger_text)
    problems += [f"a backlog checkbox line that is neither a heading with an id nor an italic note: "
                 f"{line[:70]!r}. {HEADING_FORM} A note with no id is `- [ ] *(...)*`." for line in unread]
    problems += [f"a backlog line that looks like a heading and is not written as one: {line[:70]!r}. "
                 f"{HEADING_FORM} This suite reads ids from headings in that form only, so an id on any other "
                 f"is not checked for a twin. If the line is not a heading, do not open it with an id and a "
                 f"tag, and do not put a bold id before a bracket." for line in lookalikes]
    ids = [i for i, _, _ in entries]
    if not ids:
        problems.append(f"no backlog heading read: the heading pattern no longer fits the backlog. {HEADING_FORM}")
    twice = sorted(i for i, c in Counter(ids).items() if c > 1)
    if twice:
        problems.append(f"ids on more than one backlog heading: {twice}. An id names one item. Keep it on the "
                        f"older entry (ids are stable) and give the newer one a number no item has carried: "
                        f"finished items are deleted, so search CHANGELOG.md and `git log -p` on the backlog "
                        f"before taking one.")
    return problems


def findings_problems(ledger_text: str, design_text: str, index_text: str) -> list[str]:
    """Every way the section 11 table, the backlog's cites and the two sentences disagree."""
    problems: list[str] = []
    entries, _, _ = ledger_entries(ledger_text)
    # An id on two headings is backlog_problems' to report, once: here its
    # entries read as one, so the twin is not also a home with no cite.
    by_id: dict[str, tuple[str, str]] = {}
    for i, box, body in entries:
        first_box, text = by_id.get(i, (box, ""))
        by_id[i] = (first_box, f"{text} {body}".strip())

    section = section_11(design_text)
    if section is None:
        return ["docs/design/device_details.md has no `## 11. ` heading with a `## 12. ` heading after it, and "
                "this test reads the findings table between them. If the sections were renumbered, update "
                "section_11() and BACK_CITE in this test and the `§11` in each backlog cite."]
    rows, row_problems = table_rows(section)
    if not rows and not row_problems:
        row_problems = ["no section 11 rows read: the table has its header and no row under it"]
    if row_problems:
        # One cause, not the cite, count and list mismatches that follow from it.
        return row_problems + ["the backlog's cites, section 11's opening sentence and docs/README.md's index "
                               "line were not compared with a table this test could not read in full: fix "
                               "the table first."]

    # Each open row's home exists, is open, and cites the row back.
    for p, (kind, home) in sorted(rows.items()):
        if kind != "open":
            continue
        cite = BACK_CITE.format(n=p)
        if home not in by_id:
            problems.append(f"P{p} names ledger {home}, which heads no backlog entry. If a PR finished {home} "
                            f"and deleted its entry, as the backlog's header asks, the finding is fixed: "
                            f"{MOVE_TO_FIXED}. If the id is mistyped, the home is the entry that says "
                            f"\"finding P{p}\".")
            continue
        box, body = by_id[home]
        if box == "x":
            problems.append(f"P{p} names ledger {home}, whose box is ticked: the backlog holds open "
                            f"items only, so the row says `fixed in #N` and the entry goes. That is: delete "
                            f"the entry, {MOVE_TO_FIXED}.")
        if cite not in body:
            problems.append(f"{home} does not cite P{p} back as {cite!r}. Add ({cite} to the indented text "
                            f"of {home}'s entry, or, if another entry is the finding's home, name that one "
                            f"in the row.")
    # Each backlog entry that cites a finding is the one that finding's row names.
    cited_anywhere, odd = cites(flat(ledger_text))
    problems += [f"the backlog points at a finding in a form this test cannot read: {snip!r}. Write "
                 f"\"finding P<n>\" with one space and plain digits, once for each finding (\"finding P3 and "
                 f"finding P4\"). {CITE_RULE}" for snip in odd]
    cited_in_entries: Counter[int] = Counter()
    for entry_id, _, body in entries:
        for n in cites(body)[0]:
            cited_in_entries[n] += 1
            named = rows.get(n)
            if named == ("open", entry_id):
                continue
            if named is None:
                problems.append(f"{entry_id} cites finding P{n}, whose row names no row: section 11's table "
                                f"has no P{n}. {CITE_RULE} Add the row with `ledger {entry_id}` as its home, "
                                f"or correct the number.")
            elif named[1] is None:
                problems.append(f"{entry_id} cites finding P{n}, whose row names a fixed row: its Home cell "
                                f"says `fixed in #N`, and a fixed finding has no backlog entry. {CITE_RULE} "
                                f"If {entry_id} holds what the fix left, write the Home cell as `ledger "
                                f"{entry_id}; part fixed in #N`; otherwise take the phrase out of {entry_id}.")
            else:
                problems.append(f"{entry_id} cites finding P{n}, whose row names ledger {named[1]}. "
                                f"{CITE_RULE} If {entry_id} only refers to the finding, cite its home's id "
                                f"({named[1]}) in place of the phrase; if {entry_id} is the real home, change "
                                f"the row's Home cell and take the phrase out of {named[1]}.")
    stray = Counter(cited_anywhere) - cited_in_entries
    if stray:
        problems.append("the backlog says \"finding P<n>\" outside any entry with an id: "
                        f"{[f'P{n}' for n in sorted(stray.elements())]}. An entry is its heading line and the "
                        f"indented lines under it; prose that starts in the first column and italic notes "
                        f"carry no id to hold the cite to. {CITE_RULE}")

    open_rows = {p: home for p, (kind, home) in rows.items() if kind == "open"}
    fixed_rows = sorted(p for p, (kind, _) in rows.items() if kind == "fixed")

    def held(where: str, total: str, n_open: str, open_list: str, n_fixed: str, fixed_list: str) -> None:
        try:
            stated_open, stated_fixed = parse_list(open_list), parse_list(fixed_list)
        except ValueError as exc:
            problems.append(f"{where}: {exc}")
            return
        fix = f"The table is what this test trusts: edit {where} to match it, or the row that is wrong."
        if int(total) != len(rows):
            problems.append(f"{where} says {total} findings; the table has {len(rows)}. {fix}")
        if int(n_open) != len(open_rows) or int(n_fixed) != len(fixed_rows):
            problems.append(f"{where} says {n_open} open and {n_fixed} fixed; the table has "
                            f"{len(open_rows)} and {len(fixed_rows)}. {fix}")
        homes = set(open_rows.values())
        if set(stated_open) != homes:
            problems.append(f"{where} lists open ids that are not the table's homes: only in the sentence "
                            f"{sorted(set(stated_open) - homes)}, only in the table "
                            f"{sorted(homes - set(stated_open))}. {fix}")
        if stated_fixed != [f"P{p}" for p in fixed_rows]:
            problems.append(f"{where} lists fixed findings {', '.join(stated_fixed) or 'none'}; the table's "
                            f"are {', '.join(f'P{p}' for p in fixed_rows) or 'none'}, in that order. {fix}")

    openings = list(OPENING.finditer(flat(section)))
    if not openings:
        problems.append(f"section 11's opening sentence is not in the form this test reads: {OPENING_FORM}")
    elif len(openings) > 1:
        problems.append(f"section 11 states its counts {len(openings)} times in the opening sentence's form; "
                        f"this test holds one, so say it once")
    else:
        total, last, n_open, open_list, n_fixed, fixed_list = openings[0].groups()
        if total != last:
            problems.append(f"section 11's opening sentence says {total} findings and P1 to P{last}")
        held("section 11's opening sentence", total, n_open, open_list, n_fixed, fixed_list)

    indexed = index_entries(index_text)
    stated = [(k, m) for k, entry in enumerate(indexed) for m in INDEX_LINE.finditer(entry)]
    if not indexed:
        problems.append(f"docs/README.md indexes the design 0 times, not once: this test reads the findings' "
                        f"counts from the entry that links `{INDEX_LINK}`")
    elif not stated:
        problems.append("docs/README.md's index line does not state the findings in the form "
                        f"this test reads: {INDEX_FORM}")
    elif len(stated) > 1:
        problems.append(f"docs/README.md indexes the design {len(stated)} times with its findings stated, not "
                        f"once: state the counts in one entry (a second link that names no finding is fine)")
    else:
        held("docs/README.md's index line", *stated[0][1].groups())
    if len(stated) == 1:
        # An id or a finding named anywhere else in an entry that links the
        # design is a second, unheld statement of the same thing.
        prefixes = sorted({re.match(r"[A-Z]+", home).group(0) for home in open_rows.values()} | {"P"})
        for k, entry in enumerate(indexed):
            m = stated[0][1] if k == stated[0][0] else None
            rest = entry[:m.start()] + entry[m.end():] if m else entry
            # Not part of a longer token: a chip is named ESP32-P4.
            loose = re.findall(rf"(?<![\w-])(?:{'|'.join(prefixes)})[0-9]+(?![\w-])", rest)
            if loose:
                problems.append(f"docs/README.md's index line names {loose} outside its two lists: an id or a "
                                f"finding named anywhere else in an entry that links the design is a second "
                                f"statement this test cannot hold to the table")
    return problems


def check(ledger_text: str, design_text: str, index_text: str) -> list[str]:
    """Every way the three files disagree, as sentences. Empty when they agree."""
    return backlog_problems(ledger_text) + findings_problems(ledger_text, design_text, index_text)


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise AssertionError(f"{path.relative_to(REPO)} is gone and this test reads it: if it moved, update "
                             f"the path at the top of {Path(__file__).name} (and BACK_CITE and "
                             f"INDEX_LINK, for the design)") from None


class TheBacklogAndTheDeviceDetailsFindings(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ledger, cls.design, cls.index = read(LEDGER), read(DESIGN), read(INDEX)

    def clean(self, problems: list[str]) -> None:
        if problems:
            self.fail(f"{len(problems)} problem(s). The docstring of {Path(__file__).name} says what this "
                      f"suite holds and why.\n\n" + "\n\n".join(f"- {p}" for p in problems))

    def test_every_backlog_id_is_on_one_heading(self):
        self.clean(backlog_problems(self.ledger))

    def test_the_table_the_backlog_and_the_index_agree(self):
        self.clean(findings_problems(self.ledger, self.design, self.index))


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
        problems = check(ledger, design, index)
        self.assertTrue(any(needle in p for p in problems),
                        f"expected a problem containing {needle!r}, got {problems}")

    def still_clean(self, ledger: str = MINI_LEDGER, design: str = MINI_DESIGN, index: str = MINI_INDEX) -> None:
        self.assertEqual(check(ledger, design, index), [])

    def swap(self, text: str, old: str, new: str) -> str:
        self.assertEqual(text.count(old), 1, f"the planted edit's anchor must occur once: {old!r}")
        return text.replace(old, new)

    def test_the_miniature_is_clean(self):
        self.still_clean()

    # -- the backlog's headings ------------------------------------------------

    def test_a_backlog_no_heading_is_read_from(self):
        # A heading pattern that stops matching anything reads as a failure.
        self.assertTrue(any("the heading pattern no longer fits" in p
                            for p in backlog_problems("# Open work\n\nNothing in the form.\n")))

    def test_an_id_on_two_headings_is_one_problem(self):
        # The twin of a finding's home is not also reported as a home that
        # does not cite its finding back.
        problems = check(MINI_LEDGER + "\n- [ ] **F1 [code] Again.** Text.\n", MINI_DESIGN, MINI_INDEX)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("ids on more than one backlog heading: ['F1']", problems[0])

    def test_an_id_on_two_headings_under_any_prefix_and_box(self):
        for twin in ("- [ ] **F1 [code] Again.** Text.", "- [~] **U1 [human] Again.** Text.",
                     "- [x] **A2 [decision] Again.** Text.", "- [X] **A2 [decision] Again.** Text.",
                     # The tag ends the line and the title starts on the next.
                     "- [ ] **F1 [code, gated by U1]\n  Again.** Text."):
            with self.subTest(twin=twin):
                self.broken("ids on more than one backlog heading", ledger=MINI_LEDGER + "\n" + twin + "\n")

    def test_every_prefix_the_backlog_uses_is_read(self):
        # The real backlog's eight, one and two letters long. A prefix this
        # suite did not read would be a clean file here and a missed twin below.
        prefixes = ("A", "C", "CI", "D", "F", "HA", "U", "W")
        ledger = "".join(f"- [ ] **{p}9 [decision] An item.** Text.\n\n" for p in prefixes)
        self.assertEqual(backlog_problems(ledger), [])
        self.assertEqual([i for i, _, _ in ledger_entries(ledger)[0]], [f"{p}9" for p in prefixes])
        for p in prefixes:
            with self.subTest(prefix=p):
                self.assertTrue(any(f"ids on more than one backlog heading: ['{p}9']" in found for found in
                                    backlog_problems(ledger + f"- [~] **{p}9 [code] Its twin.** Text.\n")))

    def test_a_checkbox_line_this_suite_cannot_read(self):
        for line in ("- [ ] **F9** No tag.", "- [ ] F9 [code] No bold.", "- [?] **F9 [code] Odd box.**",
                     "- [ ] **f9 [code] Lower case.**", "- [ ] Plain text.",
                     # An id this suite would take for a second one: a leading
                     # zero, a letter after the number, a digit that is not ASCII.
                     "- [ ] **F01 [code] Again.**", "- [ ] **F1a [code] Again.**", "- [ ] **F\uff11 [code] Again.**",
                     "- [ ]  **F1 [code] Two spaces.**", "- [ ] **F1 [code]** Bold closed early."):
            with self.subTest(line=line):
                self.broken("neither a heading with an id nor an italic note",
                            ledger=MINI_LEDGER + "\n" + line + "\n")

    def test_a_heading_in_another_form_is_not_skipped(self):
        # Each is F1 a second time, on a line the heading pattern does not
        # read. Skipped, the twin would pass.
        for twin in ("- **F1 [code] No box.** Text.", "* [ ] **F1 [code] A star.** Text.",
                     "+ [ ] **F1 [code] A plus.**", "  - [ ] **F1 [code] Indented.** Text.",
                     " - [ ] **F1 [code] One space in.**", "-  [ ] **F1 [code] Two spaces.**",
                     "- [ ]**F1 [code] No space.**", "- [] **F1 [code] Empty box.**",
                     "\t- [ ] **F1 [code] A tab.**", "> - [ ] **F1 [code] Quoted.**",
                     "**F1 [code] No list.** Text.", "1. [ ] **F1 [code] Numbered.**",
                     "### F1 [code] A Markdown heading", "- F1 [code] No box, no bold.",
                     "* [ ] F1 [decision] A star, no bold.", "- F1 [Human] A capital in the tag.",
                     "- ***F1 [code] Bold and italic.***", "- _**F1 [code] Italic around bold.**_",
                     # Code spans in the title do not hide the heading before them.
                     "- **F1 [code] `a_route` and `another`.** Text."):
            with self.subTest(twin=twin):
                self.broken("looks like a heading and is not written as one",
                            ledger=MINI_LEDGER + "\n" + twin + "\n")

    def test_body_text_that_is_not_a_heading_stays_clean(self):
        # Innocent edits: a checklist nested in an entry, an id and its tag
        # named in running text or in a nested list, a bold id with no tag.
        for body in ("  - [x] part one\n  - [ ] part two\n",
                     "  Un-gates:\n  - F2 [code+decision] once its bench row runs\n",
                     "  See F2 [code+decision] and **F4**, and the `[code]` tag.\n",
                     "  F2 [code+decision] waits on this.\n",
                     # A bold run that closes before an id opens no heading.
                     "  **Note.** F2 [code+decision] waits on this.\n",
                     # A heading quoted in a code span is an example, and a
                     # bold run that ends in a code span closes like any other.
                     "  Write a heading as `- [ ] **F1 [code] Title.**`, a dash and a box first.\n",
                     "  **See `x`** F2 [code+decision] waits on this.\n"):
            with self.subTest(body=body):
                self.still_clean(ledger=self.swap(MINI_LEDGER, "- [ ] **U1 [human] One bench session.** Text.\n",
                                                  "- [ ] **U1 [human] One bench session.** Text.\n" + body))

    # -- the table's homes and the backlog's cites -----------------------------

    def test_a_row_naming_an_id_the_backlog_does_not_hold(self):
        design = self.swap(MINI_DESIGN, "| P1 | ledger F1 |", "| P1 | ledger F3 |")
        self.broken("P1 names ledger F3, which heads no backlog entry", design=design)
        # The likeliest cause is a finished item, deleted as the backlog asks:
        # the message says what to write.
        self.broken("write the row's Home cell as `fixed in #<that PR>`", design=design)

    def test_a_home_that_does_not_cite_its_row_back(self):
        self.broken("F2 does not cite P2 back", ledger=self.swap(MINI_LEDGER, "§11, finding P2).", "§11)."))
        # The plain mention in A2's title is not the cite.
        self.broken("A2 does not cite P5 back",
                    ledger=self.swap(MINI_LEDGER, "\n  (`docs/design/device_details.md` §11, finding P5).", ""))
        # Nor is a cite of another finding, or one in a code span of its own.
        self.broken("F1 does not cite P1 back",
                    ledger=self.swap(MINI_LEDGER, "§11, finding P1).", "§11, finding P10)."))
        self.broken("F1 does not cite P1 back",
                    ledger=self.swap(MINI_LEDGER, "(`docs/design/device_details.md` §11, finding P1)",
                                     "`(docs/design/device_details.md §11, finding P1)`"))

    def test_an_entry_citing_a_row_that_names_another_home(self):
        other = "\n- [ ] **F9 [code] Another.** It says finding P1 is its own.\n"
        self.broken("F9 cites finding P1, whose row names ledger F1", ledger=MINI_LEDGER + other)
        # The message states the rule and both ways out.
        self.broken("is reserved for the one entry", ledger=MINI_LEDGER + other)
        self.broken("cite its home's id (F1) in place of the phrase", ledger=MINI_LEDGER + other)

    def test_the_reserved_phrase_in_any_dress_is_a_cite(self):
        # Capitalized at the start of a sentence, inside a code span, across
        # a line break, with a no-break space: each says "finding P1", and F9
        # is not P1's home.
        for text in ("Finding P1 is here too.", "See `finding P1`.", "See finding\n  P1.", "See finding\u00a0P1."):
            with self.subTest(text=text):
                self.broken("F9 cites finding P1, whose row names ledger F1",
                            ledger=MINI_LEDGER + f"\n- [ ] **F9 [code] Another.** {text}\n")

    def test_a_finding_cited_in_a_form_this_suite_cannot_read(self):
        for text in ("See finding `P1`.", "See finding **P1**.", "See findings P1 and P2.", "See finding P-1.",
                     "See finding P 1.", "See finding P01.", "See FINDING P1.", "See finding (P1).",
                     "See finding P1a.", "See finding P\uff11."):
            with self.subTest(text=text):
                self.broken("points at a finding in a form this test cannot read",
                            ledger=MINI_LEDGER + f"\n- [ ] **F9 [code] Another.** {text}\n")
        # The same in prose between entries.
        self.broken("points at a finding in a form this test cannot read",
                    ledger=self.swap(MINI_LEDGER, "## 2. Apps\n", "## 2. Apps\n\nSee findings P1 to P2.\n"))

    def test_text_that_cites_no_finding_stays_clean(self):
        # The phrase with no number, a bare P-number (the backlog writes P0
        # and P1 for priority rows and privacy ceilings), a home's id.
        for text in ("It reads the phrase \"finding P<n>\".", "A P1 row, above P0.", "See F1, the finding's home.",
                     "The sweep's findings were spot-checked.", "The finding (`a/b.md` §2) stands."):
            with self.subTest(text=text):
                self.still_clean(ledger=MINI_LEDGER + f"\n- [ ] **F9 [code] Another.** {text}\n")

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
        # Prose that starts in the first column belongs to no entry, even
        # directly under the finding's own home.
        self.broken("outside any entry with an id: ['P1']",
                    ledger=self.swap(MINI_LEDGER, "§11, finding P1).\n",
                                     "§11, finding P1).\n\nSee finding P1 again.\n"))
        self.broken("outside any entry with an id: ['P1']",
                    ledger=self.swap(MINI_LEDGER, "- [ ] **U1 [human] One bench session.** Text.\n",
                                     "- [ ] **U1 [human] One bench session.** Text.\n\nSee finding P1.\n"))

    def test_a_ticked_home(self):
        for box in ("x", "X"):
            with self.subTest(box=box):
                self.broken("P1 names ledger F1, whose box is ticked",
                            ledger=self.swap(MINI_LEDGER, "- [ ] **F1 [code]", f"- [{box}] **F1 [code]"))

    def test_a_home_cell_in_no_known_form(self):
        for home in ("note on F1", "F1", "ledger F1, part fixed in #1", "ledger F1; fixed in #1",
                     "fixed in 10", "fixed", "ledger", "", "Ledger F1", "ledger  F1", "`ledger F1`",
                     "ledger F01", "fixed in #010", "fixed in #10, #11", "fixed in #10 and #11 and #12",
                     "ledger F1; part fixed in #10, #11"):
            with self.subTest(home=home):
                self.broken("P1's Home cell is none of",
                            design=self.swap(MINI_DESIGN, "| P1 | ledger F1 |", f"| P1 | {home} |"))

    def test_a_row_this_suite_cannot_read(self):
        self.broken("a row this test cannot read",
                    design=self.swap(MINI_DESIGN, "| P4 | ledger F4 |", "| P4| ledger F4 |"))
        # The last row too: no row after it is out of order, so only reading
        # every line of the table catches it.
        for first in ("|P6 |", "| **P6** |", "| `P6` |", "| P06 |", "| P\uff16 |", "| p6 |", "| 6 |"):
            with self.subTest(first=first):
                self.broken("a row this test cannot read",
                            design=self.swap(MINI_DESIGN, "| P6 | fixed in", f"{first} fixed in"))

    def test_a_pipe_in_a_cell(self):
        # Escaped, it is part of the Home cell, which is then no home.
        self.broken("a row this test cannot read",
                    design=self.swap(MINI_DESIGN, "| P1 | ledger F1 |", "| P1 | ledger F1 \\| ledger F9 |"))
        # Unescaped, it adds a cell and moves every cell after it.
        self.broken("a row this test cannot read (it has 5 cells; the header has 4)",
                    design=self.swap(MINI_DESIGN, "| P1 | ledger F1 | One. |", "| P1 | ledger F1 | One | or two. |"))
        # Escaped in a later cell, it is text.
        self.still_clean(design=self.swap(MINI_DESIGN, "| P1 | ledger F1 | One. |",
                                          "| P1 | ledger F1 | `a \\| b`. |"))

    def test_a_table_this_suite_cannot_find(self):
        self.broken("section 11 has 0 tables headed `| # | Home |`",
                    design=self.swap(MINI_DESIGN, "| # | Home | Finding |", "| # | Finding | Home |"))
        self.broken("section 11 has 2 tables headed `| # | Home |`",
                    design=self.swap(MINI_DESIGN, "\n---\n\n## 12.", "\n| # | Home |\n|---|---|\n\n---\n\n## 12."))
        self.broken("no `|---|` rule line under its header",
                    design=self.swap(MINI_DESIGN, "|---|---|---|---|\n", ""))

    def test_a_row_outside_the_table(self):
        # A blank line ends a Markdown table: the rows under it are text.
        self.broken("a findings row outside the table",
                    design=self.swap(MINI_DESIGN, "| P4 | ledger F4 |", "\n| P4 | ledger F4 |"))
        self.broken("a findings row outside the table",
                    design=self.swap(MINI_DESIGN, "| P6 | fixed in", " | P6 | fixed in"))

    def test_section_11_not_where_this_suite_looks(self):
        # A message, not a traceback.
        for design in (MINI_DESIGN.replace("## 11. ", "## 10. "), MINI_DESIGN.replace("## 12. ", "## 13. "),
                       MINI_DESIGN.replace("## 11. ", "## 12. ").replace("## 12. PR plan", "## 13. PR plan")):
            self.broken("has no `## 11. ` heading with a `## 12. ` heading after it", design=design)
        # Renumbered so that another section is the eleventh.
        moved = MINI_DESIGN.replace("## 12. PR plan", "## 13. PR plan").replace("## 11. ", "## 12. ")
        self.broken("section 11 has 0 tables headed `| # | Home |`",
                    design=self.swap(moved, "\n## 12. ", "\n## 11. Lints\n\nText.\n\n## 12. "))
        self.broken("If the design's sections were renumbered",
                    design=self.swap(moved, "\n## 12. ", "\n## 11. Lints\n\nText.\n\n## 12. "))

    def test_a_missing_repeated_or_misplaced_row(self):
        row = "| P3 | fixed in #10 | Three. | #10 did it. |\n"
        self.broken("each once and in order", design=self.swap(MINI_DESIGN, row, ""))
        self.broken("each once and in order", design=self.swap(MINI_DESIGN, row, row + row))
        self.broken("each once and in order",
                    design=self.swap(MINI_DESIGN, row, "").replace("| P6 |", row + "| P6 |"))
        self.broken("no section 11 rows read",
                    design="".join(ln for ln in MINI_DESIGN.splitlines(True) if not ln.startswith("| P")))

    def test_an_unread_table_is_reported_once(self):
        # One cause, one message and a note: not the gap in the numbering,
        # the cite with no row and the six count and list mismatches that
        # follow from one row that did not read.
        for old, new, needle in (("| P4 | ledger F4 |", "| P4| ledger F4 |", "a row this test cannot read"),
                                 ("| P4 | ledger F4 |", "| P4 | F4 |", "P4's Home cell is none of")):
            with self.subTest(new=new):
                problems = check(MINI_LEDGER, self.swap(MINI_DESIGN, old, new), MINI_INDEX)
                self.assertEqual(len(problems), 2, problems)
                self.assertIn(needle, problems[0])
                self.assertIn("were not compared with a table this test could not read in full", problems[1])

    # -- the two sentences --------------------------------------------------------

    def sentence_cases(self, n_open: str, n_fixed: str):
        """(what is planted, old, new, the problem it must raise), for either sentence."""
        ids, unread = "F1 to F2, F4 and A2", "not a list this test can read"
        return [
            ("an id the table does not name", ids, "F1 to F2, F4 and A3", "lists open ids"),
            ("an open id left out", ids, "F1 to F2 and A2", "lists open ids"),
            ("a range that takes in an unfiled id", ids, "F1 to F4 and A2", "lists open ids"),
            ("a fixed finding left out", "P3 and P6", "P6", "lists fixed findings"),
            ("an open finding listed as fixed", "P3 and P6", "P1, P3 and P6", "lists fixed findings"),
            ("the fixed findings out of order", "P3 and P6", "P6 and P3", "lists fixed findings"),
            ("a wrong open count", n_open, n_open.replace("4", "5"), "says 5 open and 2 fixed"),
            ("a wrong fixed count", n_fixed, n_fixed.replace("2", "3"), "says 4 open and 3 fixed"),
            ("a hyphen for a range", ids, "F1-F2, F4 and A2", unread),
            ("an en dash for a range", ids, "F1–F2, F4 and A2", unread),
            ("an em dash for a range", ids, "F1—F2, F4 and A2", unread),
            ("a spaced dash for a range", ids, "F1 – F2, F4 and A2", unread),
            ("another word for a range", ids, "F1 through F2, F4 and A2", unread),
            ("no and before the last id", ids, "F1 to F2, F4, A2", unread),
            ("a comma before the and", ids, "F1 to F2, F4, and A2", unread),
            ("an id listed twice", ids, "F1 to F2, F2 and A2", unread),
            ("a range across two prefixes", ids, "F1 to A2 and F4", unread),
            ("an id with a leading zero", ids, "F1 to F2, F04 and A2", unread),
            # What an unreadable list should be is in the message.
            ("a list with its form stated", ids, "F1-F2, F4 and A2", "a range as \"F375 to F377\""),
        ]

    def test_the_same_ids_written_another_way_stay_clean(self):
        # A range written out, and the ids in another order, are the same list.
        for new in ("F1, F2, F4 and A2", "A2, F4, F2 and F1"):
            with self.subTest(new=new):
                self.still_clean(design=self.swap(MINI_DESIGN, "(**F1 to F2, F4 and A2**)", f"(**{new}**)"),
                                 index=self.swap(MINI_INDEX, "(F1 to F2, F4 and A2)", f"({new})"))

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
                         ("`docs/audit/repo_todo_sweep_2026-09.md` ", ""),
                         ("6 privacy findings, P1 to P6", "06 privacy findings, P1 to P06"),
                         ("6 privacy findings, P1 to P6", "\uff16 privacy findings, P1 to P\uff16")):
            with self.subTest(reworded=reworded):
                self.broken("section 11's opening sentence is not in the form this test reads",
                            design=self.swap(MINI_DESIGN, *reworded))
        # A second copy of the sentence would be one this suite never compared.
        second = opening.replace("6 privacy findings, P1 to P6", "9 privacy findings, P1 to P9")
        self.broken("section 11 states its counts 2 times",
                    design=self.swap(MINI_DESIGN, "\n| # | Home", f"\n{second}\n\n| # | Home"))

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
        # Part of a longer token is not an id: a chip is named ESP32-P4.
        self.still_clean(index=MINI_INDEX.replace(line, line + "; on the ESP32-P4, PR 4 and #11 (22 PRs, `a/v1`)"))
        self.broken("indexes the design 2 times", index=MINI_INDEX + line + "\n")
        self.broken("indexes the design 0 times", index=MINI_INDEX.replace(line + "\n", ""))
        # The statement twice in one entry: the second is one this suite never compared.
        self.broken("indexes the design 2 times with its findings stated",
                    index=MINI_INDEX.replace(line, line + "; 9 privacy findings, 9 open in the backlog (none) "
                                                          "and 0 fixed (none)"))

    def test_a_wrapped_index_entry_reads_as_one(self):
        # docs/README.md wraps some entries onto indented lines. A reflow of
        # this one is not a failure, and its continuation lines are read.
        wrapped = self.swap(MINI_INDEX, " and 2 fixed (P3 and P6)", "\n  and 2 fixed (P3 and P6)")
        self.still_clean(index=wrapped)
        self.still_clean(index=self.swap(MINI_INDEX, " — design, partly", "\n  — design, partly"))
        # The link on a continuation line: the entry still starts at its dash,
        # where this one states the findings.
        line = next(ln for ln in MINI_INDEX.splitlines() if "(design/device_details.md)" in ln)
        self.still_clean(index=MINI_INDEX.replace(
            line, "- The 6 privacy findings, 4 open in the backlog (F1 to F2, F4 and A2) and 2 fixed (P3 and P6),\n"
                  "  are in [Device details (design)](design/device_details.md)"))
        self.broken("names ['F4'] outside its two lists", index=wrapped + "  See F4 first.\n")
        self.broken("says 4 open and 3 fixed", index=self.swap(wrapped, "\n  and 2 fixed", "\n  and 3 fixed"))

    def test_a_second_link_to_the_design(self):
        # docs/README.md links some documents from two places. A second link
        # that states nothing is fine; one that names an id or a finding is not.
        self.still_clean(index=MINI_INDEX + "- Start with [the design](design/device_details.md) and its plan\n")
        self.broken("names ['F4'] outside its two lists",
                    index=MINI_INDEX + "- Start with [the design](design/device_details.md); F4 is the worst\n")
        self.broken("names ['P1'] outside its two lists",
                    index=MINI_INDEX + "- [The design](design/device_details.md)\n  and its finding P1\n")

    def test_a_file_that_moved_is_named(self):
        with self.assertRaises(AssertionError) as raised:
            read(REPO / "docs" / "audit" / "no_such_backlog.md")
        self.assertIn("docs/audit/no_such_backlog.md is gone and this test reads it", str(raised.exception))

    def test_none_is_an_empty_list(self):
        # Every finding fixed: the four homes leave the backlog (each from its
        # heading to the next heading, section or rule), and both lists say so.
        ledger = MINI_LEDGER
        for entry_id in ("F1", "F2", "F4", "A2"):
            a = ledger.index(f"**{entry_id} [")
            a = ledger.rindex("\n- [", 0, a) + 1
            b = re.compile(r"^(?:- \[|#|---)", re.M).search(ledger, a + 3)
            ledger = ledger[:a] + (ledger[b.start():] if b else "")
        design = MINI_DESIGN
        for p, home in ((1, "ledger F1"), (2, "ledger F2"), (4, "ledger F4"), (5, "ledger A2; part fixed in #11")):
            design = self.swap(design, f"| P{p} | {home} |", f"| P{p} | fixed in #2{p} |")
        design = self.swap(design, "4 are open in the backlog", "0 are open in the backlog")
        design = self.swap(design, "(**F1 to F2, F4 and A2**)", "(**none**)")
        design = self.swap(design, "2 are fixed (**P3 and P6**)", "6 are fixed (**P1, P2, P3, P4, P5 and P6**)")
        index = self.swap(MINI_INDEX, "4 open in the backlog (F1 to F2, F4 and A2) and 2 fixed (P3 and P6)",
                          "0 open in the backlog (none) and 6 fixed (P1, P2, P3, P4, P5 and P6)")
        self.assertEqual(check(ledger, design, index), [])

    def test_parse_list(self):
        self.assertEqual(parse_list("none"), [])
        self.assertEqual(parse_list("F357"), ["F357"])
        self.assertEqual(parse_list("P8 and P11"), ["P8", "P11"])
        self.assertEqual(parse_list("F357, F375 to F377, A132 and A133"),
                         ["F357", "F375", "F376", "F377", "A132", "A133"])
        for bad in ("", " ", "F357 F375", "F375-F377", "F375 to A377", "F377 to F375", "F375 to F375",
                    "F357, F357 and A1", "F357 and F375 and A1", "F357, F375", "F357; F375 and A1",
                    "F375 to F377 and F376", "f357 and F375", "F357, and F375", "F357 and", "None",
                    "F357 and none", " F357", "F357 ", "F0357", "F0", "F35\uff17", "F375 to F0377"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                parse_list(bad)


if __name__ == "__main__":
    unittest.main()
