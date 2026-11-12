#!/usr/bin/env python3
"""firmware/PARITY_PLAN.md section 1 lists the dashboard rows the trees differ on.

Section 1 of the parity plan is the list of `firmware/FEATURES.md`
Feature-Parity Dashboard rows where the canary (PIO) column (the plan's
ACTIVE) and the canary-wap column (the plan's Arduino) disagree, leaving out
a row with a ➖ in either column. It used to say it was "pulled directly"
from the dashboard and that the core SD/health suite was "already at
parity", and by wave 25 it missed nine of the dashboard's 26 such rows and
still listed three the dashboard read at parity (MQTT publish + HA
Discovery, OTA A/B, acoustic), two of them with stale Arduino ❌ cells
(sweep D23). Nothing read the plan, so nothing noticed. This test holds the
group tables of section 1 to the dashboard:

* every row of every table under a "### Group A" or "### Group B" heading
  (up to the next heading) names one dashboard row: its capability text
  whole, or the text before its closing parenthetical;
* the row's ACTIVE and Arduino cells carry that dashboard row's canary (PIO)
  and canary-wap symbols;
* the dashboard row is a parity row: the two symbols differ and neither is ➖;
* the row sits in the group of the tree that is ahead (Group A: Arduino,
  Group B: ACTIVE; ✅ over ⚠️ over ❌), and is listed once;
* every parity row of the dashboard is listed.

So a PR that flips one of those dashboard cells restates section 1 in the
same commit, and that is all it has to do: the tests that prove the check
bites edit a small fixture below (FIXTURE_FEATURES, FIXTURE_PLAN), never a
live row, so closing a parity item never breaks them. The dashboard is read
as `features_dashboard_guard.py` reads it (the first table under the
"Feature-Parity Dashboard" heading), and a test below holds the two readers
to the same cells.

Run:  python3 -m unittest scripts/tests/test_parity_plan.py -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import difflib
import importlib.util
import re
import unittest
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FEATURES = REPO / "firmware" / "FEATURES.md"
PLAN = REPO / "firmware" / "PARITY_PLAN.md"
GUARD = REPO / "firmware" / "scripts" / "features_dashboard_guard.py"

DASHBOARD_HEADING = "Feature-Parity Dashboard"
PIO, WAP = "canary (PIO)", "canary-wap"
# The plan's names for the two dashboard columns: ACTIVE is canary (PIO),
# Arduino is canary-wap.
PLAN_HEADER = ["Capability", "ACTIVE", "Arduino", "Closure is…"]
# Which tree is ahead in each group.
GROUP_LEAD = {"A": WAP, "B": PIO}

NA = "➖"
RANK = {"❌": 0, "⚠️": 1, "✅": 2}


def rank(sym: str | None) -> int:
    """✅ over ⚠️ over ❌; anything else (➖, unreadable) ranks below all three."""
    return RANK.get(sym or "", -1)


def symbol(cell: str) -> str | None:
    """The dashboard symbol a cell opens with, or None.

    A dashboard cell may carry a note after its symbol ("⚠️ (`/HEALTH` made
    and rotated, never written; ...)"), and the note may hold another symbol,
    so the cell is read by its first one only. ⚠️ is matched with or without
    its variation selector.
    """
    c = cell.strip()
    for mark, canonical in (("✅", "✅"), ("❌", "❌"), ("⚠", "⚠️"), (NA, NA)):
        if c.startswith(mark):
            return canonical
    return None


def split_row(line: str) -> list[str]:
    """A Markdown table row's cells; a `|` inside backticks or escaped stays."""
    body = line.strip()
    if body.startswith("|"):
        body = body[1:]
    if body.endswith("|") and not body.endswith("\\|"):
        body = body[:-1]
    cells, cur, tick, i = [], [], False, 0
    while i < len(body):
        ch = body[i]
        if ch == "\\" and i + 1 < len(body) and body[i + 1] == "|":
            cur.append("|")
            i += 2
            continue
        if ch == "`":
            tick = not tick
        if ch == "|" and not tick:
            cells.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
        i += 1
    cells.append("".join(cur).strip())
    return cells


def short_name(capability: str) -> str:
    """The capability text before its closing parenthetical.

    "Hub failover election (lowest fingerprint wins, ...)" reads "Hub failover
    election"; a parenthetical that holds a link or another parenthetical is
    dropped whole. Text that does not end in a balanced group is returned as
    it is.
    """
    s = capability.rstrip()
    if not s.endswith(")"):
        return s
    depth = 0
    for i in range(len(s) - 1, -1, -1):
        if s[i] == ")":
            depth += 1
        elif s[i] == "(":
            depth -= 1
            if depth == 0:
                return s[:i].rstrip() or s
    return s


@dataclass(frozen=True)
class DashRow:
    line: int
    capability: str
    pio: str | None
    wap: str | None

    @property
    def is_parity_row(self) -> bool:
        return (
            self.pio in RANK and self.wap in RANK and self.pio != self.wap
        )

    @property
    def lead(self) -> str:
        """The group of the tree that is ahead (parity rows only)."""
        return "A" if rank(self.wap) > rank(self.pio) else "B"


@dataclass(frozen=True)
class PlanRow:
    line: int
    group: str
    capability: str
    active: str | None
    arduino: str | None
    closure: str


def tables_in(lines: list[str], start: int, stop: int) -> list[list[tuple[int, str]]]:
    """Every contiguous table in lines[start:stop], as (line number, text) rows."""
    tables: list[list[tuple[int, str]]] = []
    cur: list[tuple[int, str]] = []
    for i in range(start, stop):
        if lines[i].lstrip().startswith("|"):
            cur.append((i + 1, lines[i]))
        elif cur:
            tables.append(cur)
            cur = []
    if cur:
        tables.append(cur)
    return tables


def table_after(lines: list[str], start: int, stop: int) -> list[tuple[int, str]]:
    """(line number, text) of the first contiguous table in lines[start:stop]."""
    tables = tables_in(lines, start, stop)
    return tables[0] if tables else []


def dashboard_rows(features_text: str) -> list[DashRow]:
    lines = features_text.splitlines()
    start = next(
        (i + 1 for i, ln in enumerate(lines)
         if ln.startswith("## ") and DASHBOARD_HEADING in ln),
        None,
    )
    if start is None:
        raise AssertionError(f"FEATURES.md has no '## {DASHBOARD_HEADING}' heading")
    stop = next(
        (j for j in range(start, len(lines)) if lines[j].startswith("## ")),
        len(lines),
    )
    table = table_after(lines, start, stop)
    if len(table) < 3:
        raise AssertionError("the FEATURES.md dashboard has no table rows")
    header = split_row(table[0][1])
    for column in (PIO, WAP):
        if column not in header:
            raise AssertionError(
                f"the FEATURES.md dashboard has no {column!r} column: {header}"
            )
    pio_at, wap_at = header.index(PIO), header.index(WAP)
    out = []
    for number, text in table[2:]:
        cells = split_row(text)
        if len(cells) <= max(pio_at, wap_at):
            raise AssertionError(f"FEATURES.md:{number}: a dashboard row with too few cells")
        out.append(DashRow(number, cells[0], symbol(cells[pio_at]), symbol(cells[wap_at])))
    return out


def plan_groups(plan_text: str) -> tuple[dict[str, list[PlanRow]], list[str]]:
    """Section 1's group rows, and what could not be read.

    A group's rows are those of every table between its "### Group X"
    heading and the next heading, so a row in a second table under the same
    heading is read and checked like the rest; each table carries the plan's
    header.
    """
    lines = plan_text.splitlines()
    problems: list[str] = []
    start = next((i for i, ln in enumerate(lines) if re.match(r"## 1\.", ln)), None)
    if start is None:
        return {}, ["PARITY_PLAN.md has no '## 1.' section"]
    stop = next(
        (j for j in range(start + 1, len(lines)) if lines[j].startswith("## ")),
        len(lines),
    )
    heads = [
        (i, m.group(1))
        for i in range(start, stop)
        if (m := re.match(r"### Group ([A-Z])\b", lines[i]))
    ]
    groups: dict[str, list[PlanRow]] = {}
    for k, (at, group) in enumerate(heads):
        if group not in GROUP_LEAD:
            problems.append(
                f"PARITY_PLAN.md:{at + 1}: Group {group} is not one this test "
                f"knows (A: Arduino ahead, B: ACTIVE ahead)"
            )
            continue
        if group in groups:
            problems.append(f"PARITY_PLAN.md:{at + 1}: a second Group {group} heading")
            continue
        end = heads[k + 1][0] if k + 1 < len(heads) else stop
        end = next(
            (j for j in range(at + 1, end) if lines[j].startswith("### ")), end
        )
        tables = tables_in(lines, at + 1, end)
        if not tables:
            problems.append(f"PARITY_PLAN.md:{at + 1}: Group {group} has no table")
        rows: list[PlanRow] = []
        for table in tables:
            header = split_row(table[0][1])
            if len(table) < 2:
                problems.append(
                    f"PARITY_PLAN.md:{table[0][0]}: a table under Group {group} "
                    f"has no separator row, so its rows are not read"
                )
                continue
            if header != PLAN_HEADER:
                problems.append(
                    f"PARITY_PLAN.md:{table[0][0]}: a table under Group {group} "
                    f"opens with {header}, not the plan's header {PLAN_HEADER}, "
                    f"so its rows are not read"
                )
                continue
            for number, text in table[2:]:
                cells = split_row(text)
                if len(cells) != len(PLAN_HEADER):
                    problems.append(
                        f"PARITY_PLAN.md:{number}: Group {group} row has "
                        f"{len(cells)} cells, not {len(PLAN_HEADER)}"
                    )
                    continue
                row = PlanRow(number, group, cells[0], symbol(cells[1]),
                              symbol(cells[2]), cells[3])
                if not row.closure:
                    problems.append(
                        f"PARITY_PLAN.md:{number}: {row.capability!r} has an empty "
                        f"'Closure is…' cell"
                    )
                rows.append(row)
        groups[group] = rows
    for group in GROUP_LEAD:
        if group not in groups:
            problems.append(f"PARITY_PLAN.md section 1 has no Group {group} table")
    return groups, problems


def closest(name: str, dash: list[DashRow]) -> DashRow | None:
    """The dashboard row a misnamed plan row most likely means (messages only)."""
    best, score = None, 0.0
    for row in dash:
        for candidate in {row.capability, short_name(row.capability)}:
            ratio = difflib.SequenceMatcher(None, name, candidate).ratio()
            if ratio > score:
                best, score = row, ratio
    return best if score >= 0.6 else None


def cells(a: str | None, b: str | None) -> str:
    return f"{a or '?'} / {b or '?'}"


def problems(plan_text: str, features_text: str) -> list[str]:
    """Every way section 1 of the plan disagrees with the dashboard."""
    dash = dashboard_rows(features_text)
    found: list[str] = []
    for row in dash:
        for column, value in ((PIO, row.pio), (WAP, row.wap)):
            if value is None:
                found.append(
                    f"FEATURES.md:{row.line}: {row.capability!r}'s {column} cell "
                    f"opens with no dashboard symbol"
                )
    index: dict[str, list[DashRow]] = {}
    for row in dash:
        for name in {row.capability, short_name(row.capability)}:
            index.setdefault(name, []).append(row)

    groups, unread = plan_groups(plan_text)
    found += unread
    listed: dict[int, PlanRow] = {}
    for group in sorted(groups):
        for pr in groups[group]:
            where = f"PARITY_PLAN.md:{pr.line}: Group {group} row {pr.capability!r}"
            hits = index.get(pr.capability, [])
            match: DashRow | None
            if len(hits) > 1:
                found.append(
                    f"{where} names {len(hits)} dashboard rows (FEATURES.md:"
                    f"{', '.join(str(h.line) for h in hits)}); use the full text"
                )
                continue
            if hits:
                match = hits[0]
            else:
                match = closest(pr.capability, dash)
                hint = (f"; the closest is {match.capability!r} (FEATURES.md:{match.line})"
                        if match else "")
                found.append(f"{where} names no dashboard row{hint}")
                if match is None:
                    continue
            if match.line in listed:
                found.append(
                    f"{where} repeats FEATURES.md:{match.line}, already listed at "
                    f"PARITY_PLAN.md:{listed[match.line].line}"
                )
                continue
            listed[match.line] = pr
            if (pr.active, pr.arduino) != (match.pio, match.wap):
                found.append(
                    f"{where} reads ACTIVE / Arduino {cells(pr.active, pr.arduino)}, "
                    f"but FEATURES.md:{match.line} reads {PIO} / {WAP} "
                    f"{cells(match.pio, match.wap)}"
                )
            if not match.is_parity_row:
                why = ("a ➖ cell, not a parity item" if NA in (match.pio, match.wap)
                       else "the two trees at parity")
                found.append(
                    f"{where}: FEATURES.md:{match.line} reads "
                    f"{cells(match.pio, match.wap)}, {why}, so section 1 does not list it"
                )
            elif match.lead != group:
                found.append(
                    f"{where}: FEATURES.md:{match.line} reads "
                    f"{cells(match.pio, match.wap)}, so it belongs in Group {match.lead}"
                )
    for row in dash:
        if row.is_parity_row and row.line not in listed:
            found.append(
                f"FEATURES.md:{row.line}: {short_name(row.capability)!r} reads {PIO} / {WAP} "
                f"{cells(row.pio, row.wap)} and is in neither group of "
                f"PARITY_PLAN.md section 1 (it belongs in Group {row.lead})"
            )
    return found


# --- synthetic edits, for the tests that prove the check bites -------------
#
# The edits run on the fixture below, never on a live row: a PR that closes
# a parity item (the port lands, the dashboard cell moves, section 1 is
# restated) changes the live files these tests would otherwise pick their
# rows from. The fixture keeps the live files' shapes: the dashboard heading
# and its first table among others, a symbol cell with a note, a ⚠ with no
# variation selector, a `|` in backticks, a closing parenthetical with a
# link, a ➖ row, a group table followed by prose and a table that is no
# group's.

FIXTURE_FEATURES = """\
# Fixture features

## Build lanes

| Capability | canary (PIO) | canary-wap |
|---|:---:|:---:|
| Not the dashboard | ❌ | ✅ |

## Feature-Parity Dashboard

| Capability | canary (PIO) | canary-wap | canary-vision |
|---|:---:|:---:|:---:|
| Alpha storage (append-only, `/A` `/B`) | ⚠️ (writer pending; ✅ once landed) | ✅ | ❌ |
| Beta radio | ❌ | ✅ | ➖ |
| Gamma sensor ([note](notes.md); host-tested) | ✅ | ❌ | ✅ |
| Delta TLS — pinned socket | ✅ | ⚠ | ❌ |
| Eta pipe `a|b` reader | ⚠️ | ✅ | ✅ |
| Epsilon reboot | ✅ | ✅ | ✅ |
| Zeta lamp | ✅ | ➖ | ✅ |
| Theta shared gap | ⚠️ | ⚠️ | ❌ |

A note under the dashboard.

## Next section
"""

FIXTURE_PLAN = """\
# Fixture plan

## 1. Where the two trees disagree today

The lead.

### Group A — Arduino has it, ACTIVE is partial/absent

| Capability | ACTIVE | Arduino | Closure is… |
|---|:---:|:---:|---|
| Alpha storage | ⚠️ | ✅ | code + bench |
| Beta radio | ❌ | ✅ | code + bench |
| Eta pipe `a|b` reader | ⚠️ | ✅ | code (compile) |

### Group B — ACTIVE has it, Arduino partial/absent

| Capability | ACTIVE | Arduino | Closure is… |
|---|:---:|:---:|---|
| Gamma sensor | ✅ | ❌ | code + host test |
| Delta TLS — pinned socket | ✅ | ⚠️ | code + bench (a TLS broker) |

**At parity on the dashboard, so not listed:** Epsilon reboot.

### Shared gap (no group's table, so not read)

| Capability | ACTIVE | Arduino | Closure is… |
|---|:---:|:---:|---|
| Theta shared gap | ⚠️ | ⚠️ | bench, once, in both trees |
| Epsilon reboot | ✅ | ❌ | a stale row this test does not read |

## 2. Next section
"""


def line_of(text: str, prefix: str) -> int:
    """The line number of the one line of text that starts with prefix."""
    hits = [i + 1 for i, ln in enumerate(text.splitlines()) if ln.startswith(prefix)]
    if len(hits) != 1:
        raise AssertionError(f"{len(hits)} lines start with {prefix!r}")
    return hits[0]


BETA_AT = line_of(FIXTURE_FEATURES, "| Beta radio |")
EPSILON_AT = line_of(FIXTURE_FEATURES, "| Epsilon reboot |")
GAMMA = "Gamma sensor ([note](notes.md); host-tested)"


def set_dashboard_cell(features_text: str, capability: str, column: str,
                       value: str) -> str:
    """Set one cell of the dashboard row whose capability text is `capability`."""
    lines = features_text.splitlines(keepends=True)
    start = next(i for i, ln in enumerate(lines)
                 if ln.startswith("## ") and DASHBOARD_HEADING in ln)
    header = None
    for i in range(start, len(lines)):
        ln = lines[i]
        if header is None and ln.startswith(f"| Capability | {PIO} |"):
            header = split_row(ln)
            continue
        if header and ln.startswith("|") and split_row(ln)[0] == capability:
            row = split_row(ln)
            row[header.index(column)] = value
            lines[i] = "| " + " | ".join(row) + " |\n"
            return "".join(lines)
    raise AssertionError(f"no dashboard row {capability!r}")


def add_dashboard_row(features_text: str, row: str) -> str:
    """Append a row after the dashboard table's last row."""
    last = dashboard_rows(features_text)[-1].line
    lines = features_text.splitlines(keepends=True)
    lines.insert(last, row + "\n")
    return "".join(lines)


def drop_plan_row(plan_text: str, capability: str) -> str:
    kept = [ln for ln in plan_text.splitlines(keepends=True)
            if not (ln.startswith("|") and split_row(ln)[0] == capability)]
    if len(kept) == len(plan_text.splitlines(keepends=True)):
        raise AssertionError(f"no plan row {capability!r}")
    return "".join(kept)


def add_plan_row(plan_text: str, group: str, row: str) -> str:
    """Append a row to the end of a group's first table."""
    lines = plan_text.splitlines(keepends=True)
    at = next(i for i, ln in enumerate(lines) if ln.startswith(f"### Group {group}"))
    i = at + 1
    while not lines[i].startswith("|"):
        i += 1
    while i < len(lines) and lines[i].startswith("|"):
        i += 1
    lines.insert(i, row + "\n")
    return "".join(lines)


def add_plan_text_after_group(plan_text: str, group: str, text: str) -> str:
    """Insert text after a group's first table (and the blank line after it)."""
    lines = plan_text.splitlines(keepends=True)
    at = next(i for i, ln in enumerate(lines) if ln.startswith(f"### Group {group}"))
    i = at + 1
    while not lines[i].startswith("|"):
        i += 1
    while i < len(lines) and lines[i].startswith("|"):
        i += 1
    lines.insert(i, "\n" + text)
    return "".join(lines)


class ShortNameTest(unittest.TestCase):
    def test_drops_the_closing_parenthetical(self) -> None:
        self.assertEqual(
            short_name("Hub failover election (lowest fingerprint wins, deterministic)"),
            "Hub failover election",
        )

    def test_drops_a_parenthetical_that_holds_a_link(self) -> None:
        self.assertEqual(
            short_name("MQTT broker TLS — pinned ([audit](../a.md); no bench)"),
            "MQTT broker TLS — pinned",
        )

    def test_keeps_text_with_no_closing_parenthetical(self) -> None:
        self.assertEqual(short_name("Witness record export to /EXPORT/"),
                         "Witness record export to /EXPORT/")
        self.assertEqual(short_name("RF presence detection"), "RF presence detection")


class DashboardReaderTest(unittest.TestCase):
    def setUp(self) -> None:
        self.rows = dashboard_rows(FEATURES.read_text(encoding="utf-8"))

    def test_every_parity_cell_opens_with_a_symbol(self) -> None:
        bad = [f"FEATURES.md:{r.line}: {r.capability!r}" for r in self.rows
               if r.pio is None or r.wap is None]
        self.assertEqual(bad, [], "\n".join(bad))

    def test_short_names_are_unique(self) -> None:
        seen: dict[str, int] = {}
        clashes = []
        for row in self.rows:
            name = short_name(row.capability)
            if name in seen:
                clashes.append(f"{name!r}: FEATURES.md:{seen[name]} and :{row.line}")
            seen[name] = row.line
        self.assertEqual(clashes, [], "\n".join(clashes))

    def test_reads_the_cells_the_dashboard_guard_reads(self) -> None:
        spec = importlib.util.spec_from_file_location("features_dashboard_guard", GUARD)
        assert spec is not None and spec.loader is not None
        guard = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(guard)
        theirs = guard.parse_dashboard(FEATURES.read_text(encoding="utf-8"))
        ours_caps = [r.capability for r in self.rows]
        self.assertEqual(sorted(ours_caps),
                         sorted({cap for cap, _ in theirs}))
        for row in self.rows:
            self.assertEqual(row.pio, symbol(theirs[(row.capability, PIO)]), row.capability)
            self.assertEqual(row.wap, symbol(theirs[(row.capability, WAP)]), row.capability)


class ParityPlanTest(unittest.TestCase):
    """The live plan against the live dashboard."""

    def setUp(self) -> None:
        self.features = FEATURES.read_text(encoding="utf-8")
        self.plan = PLAN.read_text(encoding="utf-8")

    def test_section_one_is_the_dashboard_rows_the_trees_differ_on(self) -> None:
        found = problems(self.plan, self.features)
        self.assertEqual(
            found, [],
            "PARITY_PLAN.md section 1 no longer matches the FEATURES.md "
            "dashboard; restate the groups from it:\n  " + "\n  ".join(found),
        )

    def test_a_group_holds_rows_when_the_dashboard_has_rows_for_it(self) -> None:
        # Not vacuous: when the dashboard has rows for a group, the reader
        # found that group's rows in the plan (and none when it has none, so
        # closing the last item of a group is a restated section 1, too).
        groups, _ = plan_groups(self.plan)
        dash = dashboard_rows(self.features)
        for group in GROUP_LEAD:
            want = sum(1 for r in dash if r.is_parity_row and r.lead == group)
            self.assertEqual(len(groups.get(group, [])), want, f"Group {group}")


class CheckBitesTest(unittest.TestCase):
    """Each edit to the fixture must be caught, by row."""

    def test_the_fixture_is_clean(self) -> None:
        self.assertEqual(problems(FIXTURE_PLAN, FIXTURE_FEATURES), [])
        groups, _ = plan_groups(FIXTURE_PLAN)
        self.assertEqual([r.capability for r in groups["A"]],
                         ["Alpha storage", "Beta radio", "Eta pipe `a|b` reader"])
        self.assertEqual([r.capability for r in groups["B"]],
                         ["Gamma sensor", "Delta TLS — pinned socket"])

    def test_a_flipped_dashboard_cell_fails(self) -> None:
        features = set_dashboard_cell(FIXTURE_FEATURES, "Beta radio", WAP, "⚠️")
        found = problems(FIXTURE_PLAN, features)
        self.assertTrue(
            any("'Beta radio' reads ACTIVE / Arduino ❌ / ✅" in p and "❌ / ⚠️" in p
                for p in found), found)

    def test_a_dashboard_row_that_starts_to_differ_fails(self) -> None:
        features = set_dashboard_cell(FIXTURE_FEATURES, "Epsilon reboot", WAP, "❌")
        found = problems(FIXTURE_PLAN, features)
        self.assertEqual(found, [
            f"FEATURES.md:{EPSILON_AT}: 'Epsilon reboot' reads canary (PIO) / canary-wap ✅ / ❌ "
            "and is in neither group of PARITY_PLAN.md section 1 (it belongs in Group B)",
        ])

    def test_a_dropped_group_row_fails(self) -> None:
        found = problems(drop_plan_row(FIXTURE_PLAN, "Beta radio"), FIXTURE_FEATURES)
        self.assertEqual(found, [
            f"FEATURES.md:{BETA_AT}: 'Beta radio' reads canary (PIO) / canary-wap ❌ / ✅ "
            "and is in neither group of PARITY_PLAN.md section 1 (it belongs in Group A)",
        ])

    def test_a_dropped_row_named_by_its_short_name_is_reported_by_it(self) -> None:
        found = problems(drop_plan_row(FIXTURE_PLAN, "Alpha storage"), FIXTURE_FEATURES)
        self.assertTrue(
            any("'Alpha storage' reads" in p and "neither group" in p
                and "Group A" in p for p in found), found)

    def test_a_stale_row_at_parity_fails(self) -> None:
        plan = add_plan_row(FIXTURE_PLAN, "B", "| Epsilon reboot | ✅ | ❌ | code + bench |")
        found = problems(plan, FIXTURE_FEATURES)
        self.assertTrue(any("'Epsilon reboot' reads ACTIVE / Arduino ✅ / ❌" in p
                            and "✅ / ✅" in p for p in found), found)
        self.assertTrue(any("'Epsilon reboot'" in p and "at parity" in p for p in found), found)

    def test_a_row_at_parity_fails_even_with_its_cells_right(self) -> None:
        plan = add_plan_row(FIXTURE_PLAN, "B", "| Epsilon reboot | ✅ | ✅ | bench |")
        found = problems(plan, FIXTURE_FEATURES)
        self.assertEqual(len(found), 1, found)
        self.assertIn("'Epsilon reboot'", found[0])
        self.assertIn("the two trees at parity", found[0])

    def test_a_shared_gap_is_not_a_parity_item(self) -> None:
        plan = add_plan_row(FIXTURE_PLAN, "A", "| Theta shared gap | ⚠️ | ⚠️ | bench |")
        found = problems(plan, FIXTURE_FEATURES)
        self.assertTrue(any("'Theta shared gap'" in p and "at parity" in p for p in found), found)

    def test_a_row_in_the_wrong_group_fails(self) -> None:
        plan = add_plan_row(drop_plan_row(FIXTURE_PLAN, "Gamma sensor"), "A",
                            "| Gamma sensor | ✅ | ❌ | code + host test |")
        found = problems(plan, FIXTURE_FEATURES)
        self.assertEqual(len(found), 1, found)
        self.assertIn("'Gamma sensor'", found[0])
        self.assertIn("belongs in Group B", found[0])

    def test_a_row_listed_twice_fails(self) -> None:
        plan = add_plan_row(FIXTURE_PLAN, "A", "| Beta radio | ❌ | ✅ | code + bench |")
        found = problems(plan, FIXTURE_FEATURES)
        self.assertTrue(any(f"'Beta radio' repeats FEATURES.md:{BETA_AT}" in p for p in found), found)

    def test_a_misnamed_row_fails_and_names_the_row_it_means(self) -> None:
        plan = FIXTURE_PLAN.replace("| Beta radio |", "| Beta radio link |", 1)
        found = problems(plan, FIXTURE_FEATURES)
        self.assertTrue(
            any("'Beta radio link' names no dashboard row; the closest is "
                f"'Beta radio' (FEATURES.md:{BETA_AT})" in p for p in found), found)

    def test_a_name_two_dashboard_rows_answer_to_fails(self) -> None:
        features = add_dashboard_row(FIXTURE_FEATURES, "| Beta radio (a second one) | ✅ | ✅ | ✅ |")
        found = problems(FIXTURE_PLAN, features)
        self.assertTrue(any("'Beta radio' names 2 dashboard rows" in p for p in found), found)

    def test_a_row_with_a_na_cell_is_not_required_and_not_allowed(self) -> None:
        features = add_dashboard_row(FIXTURE_FEATURES, "| Made-up capability | ❌ | ➖ | ✅ |")
        self.assertEqual(dashboard_rows(features)[-1].capability, "Made-up capability")
        self.assertEqual(problems(FIXTURE_PLAN, features), [])
        plan = add_plan_row(FIXTURE_PLAN, "B", "| Zeta lamp | ✅ | ➖ | n/a |")
        found = problems(plan, FIXTURE_FEATURES)
        self.assertTrue(any("'Zeta lamp'" in p and "a ➖ cell" in p for p in found), found)

    def test_a_new_dashboard_row_the_trees_differ_on_is_required(self) -> None:
        features = add_dashboard_row(FIXTURE_FEATURES, "| Made-up capability | ✅ | ❌ | ✅ |")
        found = problems(FIXTURE_PLAN, features)
        self.assertTrue(any("'Made-up capability'" in p and "neither group" in p
                            and "Group B" in p for p in found), found)

    def test_a_cell_with_no_symbol_fails(self) -> None:
        features = set_dashboard_cell(FIXTURE_FEATURES, "Epsilon reboot", PIO, "yes")
        found = problems(FIXTURE_PLAN, features)
        self.assertTrue(any("'Epsilon reboot'" in p and "no dashboard symbol" in p
                            for p in found), found)

    def test_a_row_in_a_second_table_under_a_group_is_read(self) -> None:
        second = ("Rows still to port (continued):\n\n"
                  "| Capability | ACTIVE | Arduino | Closure is… |\n"
                  "|---|:---:|:---:|---|\n"
                  "| Epsilon reboot | ✅ | ❌ | code + bench (rollback) |\n")
        found = problems(add_plan_text_after_group(FIXTURE_PLAN, "B", second),
                         FIXTURE_FEATURES)
        self.assertTrue(any("'Epsilon reboot' reads ACTIVE / Arduino ✅ / ❌" in p
                            for p in found), found)
        self.assertTrue(any("'Epsilon reboot'" in p and "at parity" in p for p in found), found)

    def test_a_second_table_with_another_header_under_a_group_fails(self) -> None:
        second = ("| Row | Note |\n|---|---|\n| Epsilon reboot | ✅ ❌ |\n")
        found = problems(add_plan_text_after_group(FIXTURE_PLAN, "A", second),
                         FIXTURE_FEATURES)
        self.assertTrue(any("a table under Group A opens with ['Row', 'Note']" in p
                            for p in found), found)

    def test_a_table_under_another_heading_is_not_read(self) -> None:
        # The fixture's "Shared gap" table holds a stale Epsilon row, and
        # test_the_fixture_is_clean passes: only group sections are read.
        self.assertIn("| Epsilon reboot | ✅ | ❌ |", FIXTURE_PLAN)
        self.assertEqual(problems(FIXTURE_PLAN, FIXTURE_FEATURES), [])

    def test_only_the_dashboard_table_is_read(self) -> None:
        caps = [r.capability for r in dashboard_rows(FIXTURE_FEATURES)]
        self.assertNotIn("Not the dashboard", caps)
        self.assertEqual(len(caps), 8)

    def test_a_missing_group_fails(self) -> None:
        plan = FIXTURE_PLAN.replace("### Group B", "### Group Bee", 1)
        found = problems(plan, FIXTURE_FEATURES)
        self.assertIn("PARITY_PLAN.md section 1 has no Group B table", found)

    def test_closing_an_item_needs_only_section_one_restated(self) -> None:
        # The port lands: the dashboard cell moves to parity and the plan
        # row goes, in one edit. Nothing else needs to change.
        features = set_dashboard_cell(FIXTURE_FEATURES, "Beta radio", PIO, "✅")
        plan = drop_plan_row(FIXTURE_PLAN, "Beta radio")
        self.assertEqual(problems(plan, features), [])
        features = set_dashboard_cell(FIXTURE_FEATURES, GAMMA, WAP, "✅")
        plan = drop_plan_row(FIXTURE_PLAN, "Gamma sensor")
        self.assertEqual(problems(plan, features), [])


if __name__ == "__main__":
    unittest.main()
