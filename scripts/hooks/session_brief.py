#!/usr/bin/env python3
"""scripts/hooks/session_brief.py — ten lines of orientation, no file reads.

Claude Code runs this at SessionStart (`.claude/settings.json`); what it
prints is added to the session's context. It says which branch this is, how
it stands against main, what is dirty, which trees the branch has touched and
which tree briefs therefore apply — the questions a session otherwise answers
by reading files. Best effort: every step that fails is skipped, and the hook
never exits non-zero.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BRIEFS = ("AGENTS.md", "CLAUDE.md")


def git(*args: str, strip: bool = True) -> str:
    try:
        out = subprocess.run(
            ["git", *args], cwd=ROOT, capture_output=True, text=True, timeout=5, check=False
        ).stdout
        return out.strip() if strip else out.rstrip("\n")
    except (OSError, subprocess.SubprocessError):
        return ""


def brief_for(tree: str) -> str | None:
    """The nearest tree brief for a top-level directory, if one exists."""
    for name in BRIEFS:
        if (ROOT / tree / name).is_file():
            return f"{tree}/{name}"
    return None


def main() -> int:
    out = []
    branch = git("rev-parse", "--abbrev-ref", "HEAD") or "?"
    out.append(f"securaCV · branch {branch}")
    base = git("merge-base", "HEAD", "origin/main")
    if base:
        counts = git("rev-list", "--left-right", "--count", "origin/main...HEAD")
        if counts:
            behind, ahead = counts.split()
            out.append(f"  vs origin/main: {ahead} ahead, {behind} behind")
        touched = git("diff", "--name-only", base, "HEAD")
    else:
        touched = ""
    dirty = [line for line in git("status", "--porcelain", strip=False).splitlines() if line.strip()]
    if dirty:
        out.append(f"  dirty: {len(dirty)} path(s): " + ", ".join(line[3:] for line in dirty[:6])
                   + (" …" if len(dirty) > 6 else ""))
    trees = sorted({p.split("/", 1)[0] for p in touched.splitlines() if "/" in p})
    if trees:
        out.append("  branch touches: " + ", ".join(trees[:10]) + (" …" if len(trees) > 10 else ""))
        briefs = [b for b in (brief_for(t) for t in trees) if b]
        if briefs:
            out.append("  briefs that apply: " + ", ".join(briefs))
    out.append("  before pushing: the checks named in the tree brief; "
               "python3 scripts/agent_context_budget.py --check if you edited a brief")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
