#!/usr/bin/env python3
"""scripts/agent_context_budget.py — what an agent loads at session start stays small.

    python3 scripts/agent_context_budget.py          # the table
    python3 scripts/agent_context_budget.py --check  # CI gate: exit 1 over budget

WHY. Claude Code loads the root CLAUDE.md, every file it @imports, and every
.claude/rules/*.md without a `paths:` scope into every session before the
first prompt; Codex and the rest load AGENTS.md the same way. On 2026-11-03
that was 55 KB (~14,000 tokens), 68% of it a narrative about generated files
that only a CAD or emulator task needs. A tree's AGENTS.md / CLAUDE.md, a
`paths:`-scoped rule, a skill body and a subagent definition load only when
touched or invoked, so the cure is to move a rule to where it applies (a
tree's one-line CLAUDE.md imports the AGENTS.md beside it, because a root
CLAUDE.md turns off Claude Code's own AGENTS.md discovery; the pair is
measured as one load). This
gate is what keeps it there: a rule for one tree goes in that tree's brief, a
story about why goes in docs/, and the root does not grow back.

Budgets are bytes (tokens are estimated at four bytes each, for the table
only), so the gate is deterministic. Refuses (exit 2): a missing root
CLAUDE.md, or an @import naming a file that does not exist — a broken import
is the brief silently not loading.
"""
import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

# Bytes. START is everything loaded before the first prompt; LAZY is per file
# that loads on touch or invocation (the per-file cap Claude Code reads whole
# is 40,000 characters on a 200K window; a brief should be far under it).
BUDGET_START = 20_000
BUDGET_LAZY = 8_000

IMPORT_RE = re.compile(r"^@(\S+)")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
PATHS_RE = re.compile(r"^paths\s*:", re.M)
SKIP_DIRS = {".git", "node_modules", "target", "build", "dist", ".venv"}


def imports_of(md: Path, seen: set[Path] | None = None, depth: int = 0) -> list[Path]:
    """Files an instruction file pulls in with `@path` lines, recursively (4 hops)."""
    seen = set() if seen is None else seen
    found: list[Path] = []
    if depth > 4:
        return found
    in_fence = False
    for line in md.read_text(encoding="utf-8", errors="replace").splitlines():
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        m = IMPORT_RE.match(line)
        if not m or m.group(1).startswith("~"):
            continue
        target = (md.parent / m.group(1)).resolve()
        if target in seen:
            continue
        seen.add(target)
        if not target.is_file():
            refuse(f"{rel(md)} imports {m.group(1)}, which does not exist")
        found.append(target)
        found.extend(imports_of(target, seen, depth + 1))
    return found


def refuse(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(2)


def rel(p: Path) -> str:
    try:
        return p.relative_to(REPO).as_posix()
    except ValueError:
        return p.as_posix()


def has_paths_scope(rule: Path) -> bool:
    text = rule.read_text(encoding="utf-8", errors="replace")
    if not text.startswith("---"):
        return False
    end = text.find("\n---", 3)
    return end != -1 and bool(PATHS_RE.search(text[3:end]))


def walk(root: Path, name: str):
    for p in sorted(root.rglob(name)):
        if p.is_file() and not any(d in p.parts for d in SKIP_DIRS):
            yield p


def inventory(root: Path) -> list[tuple[str, Path, int]]:
    """(when-loaded, path, budget) for every instruction file an agent can meet."""
    rows: list[tuple[str, Path, int]] = []
    root_claude = root / "CLAUDE.md"
    if not root_claude.is_file():
        refuse("no CLAUDE.md at the repo root")
    start = [root_claude, *imports_of(root_claude)]
    agents = root / "AGENTS.md"
    if agents.is_file() and agents.resolve() not in {p.resolve() for p in start}:
        start.append(agents)  # the vendor-neutral brief, loaded whole by every other tool
    for p in start:
        rows.append(("start", p, BUDGET_START))
    for rule in walk(root / ".claude" / "rules", "*.md"):
        rows.append(("touch" if has_paths_scope(rule) else "start", rule, BUDGET_LAZY if has_paths_scope(rule) else BUDGET_START))
    imported: set[Path] = set()
    for p in walk(root, "CLAUDE.md"):
        if p.parent != root and ".claude" not in p.parts:
            imported.update(q.resolve() for q in imports_of(p))
            rows.append(("touch", p, BUDGET_LAZY))
    for p in walk(root, "AGENTS.md"):
        if p.parent != root and ".claude" not in p.parts and p.resolve() not in imported:
            rows.append(("touch", p, BUDGET_LAZY))  # read by Codex and friends; by Claude only via a sibling CLAUDE.md
    for p in walk(root / ".claude" / "skills", "SKILL.md"):
        rows.append(("invoke", p, BUDGET_LAZY))
    for p in walk(root / ".claude" / "agents", "*.md"):
        rows.append(("spawn", p, BUDGET_LAZY))
    return rows


def report(root: Path) -> tuple[list[str], list[str]]:
    rows = inventory(root)
    lines = [f"{'loaded':<7} {'bytes':>7} {'~tokens':>8}  {'budget':>7}  file"]
    failures: list[str] = []
    start_total = 0
    for when, p, budget in rows:
        size = p.stat().st_size + (sum(q.stat().st_size for q in imports_of(p)) if when == "touch" and p.name == "CLAUDE.md" else 0)
        if when == "start":
            start_total += size
        over = when != "start" and size > budget
        if over:
            failures.append(f"{rel(p)}: {size:,} bytes, budget {budget:,} — move what applies to one tree into that tree's brief")
        lines.append(f"{when:<7} {size:>7,} {size // 4:>8,}  {budget if when != 'start' else '':>7}  {rel(p)}{'  OVER' if over else ''}")
    lines.append(f"{'':<7} {start_total:>7,} {start_total // 4:>8,}  {BUDGET_START:>7,}  at session start, in total"
                 + ("  OVER" if start_total > BUDGET_START else ""))
    if start_total > BUDGET_START:
        failures.append(f"session start loads {start_total:,} bytes, budget {BUDGET_START:,} — a rule that applies to one tree goes in that tree's brief, a story about why goes in docs/")
    return lines, failures


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 when a budget is exceeded")
    ap.add_argument("--root", type=Path, default=REPO, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    lines, failures = report(args.root.resolve())
    print("\n".join(lines))
    if failures:
        print("\nagent_context_budget.py: over budget:", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1 if args.check else 0
    print("agent context budget OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
