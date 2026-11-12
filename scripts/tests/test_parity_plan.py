#!/usr/bin/env python3
"""firmware/PARITY_PLAN.md section 1 lists the dashboard rows the trees differ on.

Section 1 of the parity plan is the list of `firmware/FEATURES.md`
Feature-Parity Dashboard rows where the canary (PIO) column (the plan's
ACTIVE) and the canary-wap column (the plan's Arduino) disagree, leaving out
a row with a ➖ in either column. It used to say it was "pulled directly"
from the dashboard and that the core SD/health suite was "already at
parity", and by wave 25 it missed nine of the dashboard's 26 such rows and
still listed two (OTA A/B, acoustic) the dashboard read at parity (sweep
D23). Nothing read the plan, so nothing noticed. This test holds the two
group tables of section 1 to the dashboard:

* every group row names one dashboard row: its capability text whole, or
  the text before its closing parenthetical;
* the row's ACTIVE and Arduino cells carry that dashboard row's canary (PIO)
  and canary-wap symbols;
* the dashboard row is a parity row: the two symbols differ and neither is ➖;
* the row sits in the group of the tree that is ahead (Group A: Arduino,
  Group B: ACTIVE; ✅ over ⚠️ over ❌), and is listed once;
* every parity row of the dashboard is listed.

So a PR that flips one of those dashboard cells restates section 1 in the
same commit. The dashboard is read as `features_dashboard_guard.py` reads it
(the first table under the "Feature-Parity Dashboard" heading), and a test
below holds the two readers to the same cells.

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


def table_after(lines: list[str], start: int, stop: int) -> list[tuple[int, str]]:
    """(line number, text) of the first contiguous table in lines[start:stop]."""
    rows: list[tuple[int, str]] = []
    for i in range(start, stop):
        if lines[i].lstrip().startswith("|"):
            rows.append((i + 1, lines[i]))
        elif rows:
            break
    return rows


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
    """Section 1's group tables, and what could not be read."""
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
        table = table_after(lines, at + 1, end)
        if len(table) < 2:
            problems.append(f"PARITY_PLAN.md:{at + 1}: Group {group} has no table")
            groups[group] = []
            continue
        header = split_row(table[0][1])
        if header != PLAN_HEADER:
            problems.append(
                f"PARITY_PLAN.md:{table[0][0]}: Group {group}'s header reads "
                f"{header}, not {PLAN_HEADER}"
            )
            groups[group] = []
            continue
        rows = []
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

def set_dashboard_cell(features_text: str, capability: str, column: str,
                       value: str) -> str:
    lines = features_text.splitlines(keepends=True)
    header = None
    for i, ln in enumerate(lines):
        if ln.startswith(f"| Capability | {PIO} |"):
            header = split_row(ln)
            continue
        if header and ln.startswith("|") and split_row(ln)[0] == capability:
            row = split_row(ln)
            row[header.index(column)] = value
            lines[i] = "| " + " | ".join(row) + " |\n"
            return "".join(lines)
    raise AssertionError(f"no dashboard row {capability!r}")


def drop_plan_row(plan_text: str, capability: str) -> str:
    kept = [ln for ln in plan_text.splitlines(keepends=True)
            if not (ln.startswith("|") and split_row(ln)[0] == capability)]
    if len(kept) == len(plan_text.splitlines(keepends=True)):
        raise AssertionError(f"no plan row {capability!r}")
    return "".join(kept)


def add_plan_row(plan_text: str, group: str, row: str) -> str:
    """Append a row to the end of a group's table."""
    lines = plan_text.splitlines(keepends=True)
    at = next(i for i, ln in enumerate(lines) if ln.startswith(f"### Group {group}"))
    i = at + 1
    while not lines[i].startswith("|"):
        i += 1
    while i < len(lines) and lines[i].startswith("|"):
        i += 1
    lines.insert(i, row + "\n")
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

    def test_both_groups_hold_rows(self) -> None:
        groups, _ = plan_groups(self.plan)
        for group in GROUP_LEAD:
            self.assertTrue(groups.get(group), f"Group {group} is empty")

    # The check bites: each edit below must be caught, by row.

    def test_a_flipped_dashboard_cell_fails(self) -> None:
        cap = "RF presence detection"
        found = problems(self.plan, set_dashboard_cell(self.features, cap, WAP, "⚠️"))
        self.assertTrue(any(cap in p and "❌ / ⚠️" in p for p in found), found)

    def test_a_dashboard_row_that_starts_to_differ_fails(self) -> None:
        cap = "Watchdog timer"
        found = problems(self.plan, set_dashboard_cell(self.features, cap, WAP, "❌"))
        self.assertTrue(any(cap in p and "neither group" in p for p in found), found)

    def test_a_dropped_group_row_fails(self) -> None:
        cap = "Remote identify blink (HA `Identify` button on MQTT variants; HTTP `/api/identify` on WAP)"
        found = problems(drop_plan_row(self.plan, cap), self.features)
        self.assertTrue(
            any("'Remote identify blink' reads" in p and "neither group" in p
                and "Group A" in p for p in found), found)

    def test_the_stale_ota_row_fails(self) -> None:
        cap = "OTA A/B with rollback safety"
        plan = add_plan_row(self.plan, "B", f"| {cap} | ✅ | ❌ | code + bench (rollback) |")
        found = problems(plan, self.features)
        self.assertTrue(any(cap in p and "reads ACTIVE / Arduino ✅ / ❌" in p for p in found), found)
        self.assertTrue(any(cap in p and "at parity" in p for p in found), found)

    def test_a_row_at_parity_fails_even_with_its_cells_right(self) -> None:
        cap = "OTA A/B with rollback safety"
        plan = add_plan_row(self.plan, "B", f"| {cap} | ✅ | ✅ | bench (Track E) |")
        found = problems(plan, self.features)
        self.assertTrue(any(cap in p and "at parity" in p for p in found), found)

    def test_a_row_in_the_wrong_group_fails(self) -> None:
        cap = "SD endurance wear estimate"
        plan = add_plan_row(drop_plan_row(self.plan, cap), "A",
                            f"| {cap} | ✅ | ❌ | code + host test |")
        found = problems(plan, self.features)
        self.assertTrue(any(cap in p and "belongs in Group B" in p for p in found), found)

    def test_a_misnamed_row_fails_and_names_the_row_it_means(self) -> None:
        plan = self.plan.replace("| RF presence detection |", "| RF presence |", 1)
        found = problems(plan, self.features)
        self.assertTrue(
            any("'RF presence' names no dashboard row; the closest is "
                "'RF presence detection'" in p for p in found), found)

    def test_a_row_with_a_na_cell_is_not_required(self) -> None:
        # Appended after the dashboard's last row, so no other line moves.
        last = dashboard_rows(self.features)[-1].line
        lines = self.features.splitlines(keepends=True)
        lines.insert(last, "| Made-up capability | ❌ | ➖ | ✅ | ✅ | ✅ | ➖ | ❌ |\n")
        features = "".join(lines)
        self.assertEqual(dashboard_rows(features)[-1].capability, "Made-up capability")
        self.assertEqual(problems(self.plan, features), problems(self.plan, self.features))
        plan = add_plan_row(self.plan, "B", "| Made-up capability | ❌ | ➖ | n/a |")
        found = problems(plan, features)
        self.assertTrue(any("Made-up capability" in p and "a ➖ cell" in p for p in found), found)


if __name__ == "__main__":
    unittest.main()
