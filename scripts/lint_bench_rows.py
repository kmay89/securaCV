#!/usr/bin/env python3
"""Fail the build on a benchmark number committed to the tree (AGENTS.md rule 4).

    python3 scripts/lint_bench_rows.py          # every Markdown file in the tree
    python3 scripts/lint_bench_rows.py PATH...  # specific files or directories

tests/bench_harness.rs measures the kernel and prints Markdown tables. The
rule that goes with it (docs/BENCHMARKS.md, "Where a run's output goes") is
that no number a run prints is ever committed: it stays in the terminal, the
CI artifact, or the review thread of the change it was measured for. This is
the guard. It fails on the two shapes the harness prints:

  a table row    first cell one of the harness's row names (backticked, bold
                 or plain), second cell a number (digit groups and a unit
                 allowed); the line may be blockquoted ("> | ...") and may
                 drop its leading pipe
  the trailer    the append row's "total: <n> events in <t> s" line, anywhere
                 in a line

It catches a pasted table, not a number retyped into prose; that part is
review. A row that NAMES a harness row and describes it (docs/BENCHMARKS.md's
"What each row measures" table) is fine: its second cell is prose.

The row names are read from ROW_NAMES in tests/bench_harness.rs, never typed
here, so a new row is guarded the moment it exists; the harness in turn
refuses to print a row whose name is not in ROW_NAMES. A lint that cannot read
its own list has nothing to look for, and passing then would be a false green,
so any source it cannot parse is a refusal (exit 2), named, not a pass.

Why a lint and not a Rust test: the input is Markdown. A Rust test runs only
when rust.yml's path filter sees a Rust change, so a docs-only PR that pasted a
table would merge green, and the first red would land on the next unrelated
Rust PR or on the release tag's `cargo test`. Repo Lints is unfiltered, so the
check runs where its input changes.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "tests" / "bench_harness.rs"
SKIP_DIRS = {".git", "node_modules", "target", "build", "dist", ".venv",
             "__pycache__", ".pytest_cache", "third_party"}


class Refusal(ValueError):
    """The harness source could not be read into a row-name list."""


# `const ROW_APPEND: &str = "append_event_checked";` (a `'static` is allowed).
CONST_RE = re.compile(
    r"""^\s*const\s+(ROW_[A-Z0-9_]+)\s*:\s*&\s*(?:'static\s+)?str\s*=\s*"([^"]*)"\s*;""",
    re.M,
)
# `const ROW_NAMES: &[&str] = &[ ROW_APPEND, ... ];`
NAMES_RE = re.compile(
    r"""const\s+ROW_NAMES\s*:\s*&\s*\[\s*&\s*(?:'static\s+)?str\s*\]\s*=\s*&\s*\[(.*?)\]\s*;""",
    re.S,
)
# Any `const ROW_...:` line, parsed or not: one the CONST_RE shape misses is a
# refusal, not a silently unguarded name.
ANY_CONST_RE = re.compile(r"^\s*const\s+(ROW_[A-Z0-9_]+)\s*:", re.M)
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def parse_row_names(src: str) -> list[str]:
    """The row names ROW_NAMES lists, or Refusal naming what could not be read."""
    consts: dict[str, str] = {}
    for m in CONST_RE.finditer(src):
        ident, value = m.group(1), m.group(2)
        if "\\" in value or "|" in value or not value.strip():
            raise Refusal(f"{ident} = {value!r}: a row name with an escape, a pipe "
                          "or no text cannot be matched as a table cell")
        consts[ident] = value
    declared = {m.group(1) for m in ANY_CONST_RE.finditer(src)} - {"ROW_NAMES"}
    unread = sorted(declared - consts.keys())
    if unread:
        raise Refusal(f"const(s) {', '.join(unread)} are not a plain "
                      "`const ROW_X: &str = \"...\";` string this lint can read")
    block = NAMES_RE.search(src)
    if not block:
        raise Refusal("no `const ROW_NAMES: &[&str] = &[...];` block found")
    names: list[str] = []
    listed: set[str] = set()
    body = re.sub(r"//[^\n]*", "", block.group(1))
    for raw in body.split(","):
        item = raw.strip()
        if not item:
            continue
        if IDENT_RE.fullmatch(item) and item in consts:
            listed.add(item)
            names.append(consts[item])
        else:
            raise Refusal(f"ROW_NAMES entry {item!r} is not one of the ROW_* "
                          "string constants")
    if not names:
        raise Refusal("ROW_NAMES is empty")
    dupes = sorted({n for n in names if names.count(n) > 1})
    if dupes:
        raise Refusal(f"ROW_NAMES lists {dupes} more than once")
    unlisted = sorted(consts.keys() - listed)
    if unlisted:
        raise Refusal(f"{', '.join(unlisted)} declared but not in ROW_NAMES: a "
                      "row printed under that name would escape this guard")
    return names


# A number as the harness prints it, or as a person might retype it: digit
# groups (comma, space, no-break space, underscore, apostrophe) and a trailing
# unit allowed.
NUMBER_RE = re.compile(
    r"[+-]?(?:\d[\d, \u00a0\u202f_']*\d|\d)?(?:\.\d+)?(?:[eE][+-]?\d+)?"
    r"(?:\s*(?:ms|µs|us|ns|s|%|x|×|events/s))?"
)
TRAILER_RE = re.compile(r"\btotal:\s*\d[\d, \u00a0\u202f_']*\s+events\s+in\s+\d[\d.]*\s*s\b")
QUOTE_RE = re.compile(r"^\s*(?:>\s*)+")


def is_number(cell: str) -> bool:
    return bool(re.search(r"\d", cell)) and bool(NUMBER_RE.fullmatch(cell))


def offending(line: str, names: list[str]) -> str | None:
    """Why `line` is a committed measurement, or None if it is not one."""
    if TRAILER_RE.search(line):
        return "the append row's total/events-per-second trailer"
    body = QUOTE_RE.sub("", line).strip()
    if "|" not in body:
        return None
    cells = [c.strip().strip("`*").strip() for c in body.strip("|").split("|")]
    if len(cells) >= 2 and cells[0] in names and is_number(cells[1]):
        return f"a measured `{cells[0]}` row"
    return None


def md_files(paths):
    for base in paths:
        p = Path(base).resolve()
        if not p.exists():
            print(f"lint_bench_rows.py: no such path: {base}", file=sys.stderr)
            raise SystemExit(2)
        if p.is_file():
            yield p
            continue
        for f in sorted(p.rglob("*.md")):
            if f.is_file() and not SKIP_DIRS.intersection(f.relative_to(p).parts):
                yield f


def rel(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def main(argv: list[str]) -> int:
    try:
        names = parse_row_names(HARNESS.read_text(encoding="utf-8"))
    except (OSError, Refusal) as e:
        print(f"lint_bench_rows.py: refusing — cannot read the row names from "
              f"{rel(HARNESS)}: {e}", file=sys.stderr)
        return 2
    offenders = []
    n = 0
    for f in md_files(argv or [ROOT]):
        n += 1
        try:
            text = f.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for i, line in enumerate(text.splitlines(), 1):
            why = offending(line, names)
            if why:
                offenders.append(f"{rel(f)}:{i}: {why}: {line.strip()!r}")
    if offenders:
        print(f"lint_bench_rows.py: {len(offenders)} benchmark measurement(s) "
              "committed to the tree. AGENTS.md rule 4: a number stays with the "
              "run that produced it (docs/BENCHMARKS.md, \"Where a run's output "
              "goes\") — link the CI artifact or the review thread instead.",
              file=sys.stderr)
        for o in offenders[:40]:
            print(f"  {o}", file=sys.stderr)
        if len(offenders) > 40:
            print(f"  … and {len(offenders) - 40} more", file=sys.stderr)
        return 1
    print(f"bench rows OK — {n} Markdown files, none carries a measured row of "
          f"the {len(names)} harness rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
