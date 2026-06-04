#!/usr/bin/env python3
"""Feature-Parity Dashboard regression guard (FEATURES.md contract).

Enforces the CI contract stated in firmware/FEATURES.md: a PR that downgrades a
cell in the **Feature-Parity Dashboard** from ✅ to ⚠️/❌ must reference an issue
in the PR body (e.g. `Regresses FEATURES.md: <cell> (#1234)`). This makes a
silent capability regression impossible to merge unacknowledged — the firmware
analogue of scripts/lint_feature_flags.sh.

It compares the dashboard table in the base revision against the working tree,
and reports any ✅→⚠️/❌ downgrade. If downgrades exist and the PR body carries
no `#<number>` issue reference, it fails.

Usage:
    firmware/scripts/features_dashboard_guard.py [--base <git-ref>]

Environment (set by CI):
    GITHUB_BASE_REF  base branch name (PR); falls back to HEAD^ on push.
    GITHUB_SHA       head sha (informational).
    PR_BODY          pull-request description, scanned for an issue reference.

Exit codes: 0 = no regression / acknowledged; 1 = unacknowledged regression;
2 = usage / parse error.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

FEATURES_PATH = "firmware/FEATURES.md"
DASHBOARD_HEADING = "Feature-Parity Dashboard"


def classify(cell: str) -> str:
    """Map a dashboard cell to one of good/warn/bad/na/unknown."""
    c = cell.strip()
    if "✅" in c:
        return "good"
    if "❌" in c:
        return "bad"
    if "⚠" in c:  # ⚠️ carries a trailing variation selector
        return "warn"
    if "➖" in c:
        return "na"
    return "unknown"


def parse_dashboard(text: str) -> dict[tuple[str, str], str]:
    """Return {(capability, column_name): cell} for the dashboard table only.

    The dashboard is the first markdown table after the "Feature-Parity
    Dashboard" heading and before the next "## " heading; later per-subsystem
    tables are deliberately ignored.
    """
    lines = text.splitlines()
    # Find the dashboard section bounds.
    start = None
    for i, ln in enumerate(lines):
        if ln.startswith("## ") and DASHBOARD_HEADING in ln:
            start = i + 1
            break
    if start is None:
        return {}
    end = len(lines)
    for j in range(start, len(lines)):
        if lines[j].startswith("## "):
            end = j
            break
    section = lines[start:end]

    # Collect the first contiguous run of table rows (lines starting with '|').
    rows: list[list[str]] = []
    in_table = False
    for ln in section:
        if ln.lstrip().startswith("|"):
            in_table = True
            cells = [c.strip() for c in ln.strip().strip("|").split("|")]
            rows.append(cells)
        elif in_table:
            break  # table ended

    if len(rows) < 2:
        return {}

    header = rows[0]
    # rows[1] is the |---|---| separator; data rows follow.
    out: dict[tuple[str, str], str] = {}
    for row in rows[2:]:
        if not row or all(c == "" for c in row):
            continue
        capability = row[0]
        for col_idx in range(1, min(len(header), len(row))):
            out[(capability, header[col_idx])] = row[col_idx]
    return out


def git_show(ref: str, path: str) -> str | None:
    try:
        return subprocess.run(
            ["git", "show", f"{ref}:{path}"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    except subprocess.CalledProcessError:
        return None  # path didn't exist on base → nothing to regress from


def resolve_base(explicit: str | None) -> str:
    if explicit:
        return explicit
    base_ref = os.environ.get("GITHUB_BASE_REF")
    if base_ref:
        # Best-effort fetch so the ref is present in shallow CI checkouts.
        subprocess.run(
            ["git", "fetch", "origin", base_ref, "--depth=1"],
            capture_output=True,
            text=True,
        )
        return f"origin/{base_ref}"
    return "HEAD^"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", help="git ref to diff against (default: PR base or HEAD^)")
    args = ap.parse_args()

    base = resolve_base(args.base)

    base_text = git_show(base, FEATURES_PATH)
    if base_text is None:
        print(f"[dashboard-guard] {FEATURES_PATH} not present at base {base}; nothing to compare.")
        return 0

    try:
        with open(FEATURES_PATH, encoding="utf-8") as fh:
            head_text = fh.read()
    except FileNotFoundError:
        print(f"::error::{FEATURES_PATH} is missing from the working tree.")
        return 2

    base_cells = parse_dashboard(base_text)
    head_cells = parse_dashboard(head_text)
    if not head_cells:
        print("::error::Could not parse the Feature-Parity Dashboard from the working tree.")
        return 2

    regressions: list[str] = []
    for key, base_cell in base_cells.items():
        if classify(base_cell) != "good":
            continue
        head_cell = head_cells.get(key)
        if head_cell is None:
            continue  # row/column removed — not a cell downgrade
        if classify(head_cell) in ("warn", "bad"):
            capability, column = key
            regressions.append(f"{capability} [{column}]: {base_cell} → {head_cell}")

    if not regressions:
        print("[dashboard-guard] OK: no ✅→⚠️/❌ downgrades in the Feature-Parity Dashboard.")
        return 0

    pr_body = os.environ.get("PR_BODY", "")
    has_issue_ref = re.search(r"#\d+", pr_body) is not None

    print("[dashboard-guard] Detected capability downgrade(s):")
    for r in regressions:
        print(f"  - {r}")

    if has_issue_ref:
        print("[dashboard-guard] PR body references an issue (#<number>) — downgrade acknowledged.")
        return 0

    print("::error::This PR downgrades a ✅ cell in the Feature-Parity Dashboard "
          "(firmware/FEATURES.md) without acknowledgement.")
    print("::error::Add an issue reference to the PR body, e.g. "
          "'Regresses FEATURES.md: <cell> (#1234)'.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
