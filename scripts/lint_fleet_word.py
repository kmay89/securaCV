#!/usr/bin/env python3
"""Fail the build on the banned bird-group word. A group of Canaries is a fleet.

    python3 scripts/lint_fleet_word.py          # check the whole tree
    python3 scripts/lint_fleet_word.py PATH...  # check specific paths

AGENTS.md rule 3 bans the f-word for a group of birds in copy, UI strings,
identifiers, and comments — a company by that name soured it. Until this
gate existed the rule was enforced only by prose and an advisory review:
a PR using the word in a new doc, comment, or identifier merged green.
(Guard tests existed for a handful of surfaces — the Lab pages, the tvOS
wall — but a per-surface guard is not a guard on the word.)

WHAT IS ALLOWED, MECHANICALLY
-----------------------------
Three narrow shapes survive, because each is a real name rather than the
bird word:

  flock(          the Unix flock(2) syscall and calls to it — flock(2),
                  libc::flock(fd, LOCK_EX), fcntl.flock(...)
  `flock`         the same syscall named in prose/doc comments, backticked
                  the way an API name should be
  flock/PID       the storage flight-rules phrase for the lock we don't take
  Flock Safety /  the company, named where research must name it
  FlockOS

Everything else needs the file to be on the ALLOW_FILES list below — which
exists for exactly two kinds of file: the ones that STATE the rule (and so
must name the word once, in quotes), and the guard tests that assert its
absence (their regexes contain it by necessity). If this lint just went red
on your new file, the fix is to write "fleet", not to grow the list.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTS = {".rs", ".py", ".js", ".mjs", ".ts", ".c", ".h", ".cpp", ".hpp", ".md",
        ".html", ".css", ".swift", ".json", ".yml", ".yaml", ".sh", ".toml",
        ".scad", ".ino", ".txt",
        # Copy-bearing formats the rule binds just as hard: an SVG badge
        # label, a plist display string, a CSV column of user-facing text.
        ".svg", ".plist", ".ini", ".properties", ".entitlements", ".csv"}
EXTRA_NAMES = {"Makefile", "makefile", "GNUmakefile", "Dockerfile"}
SKIP_DIRS = {".git", "node_modules", "target", "build", "dist", ".venv",
             "__pycache__", ".pytest_cache", "third_party"}
# This file names the banned word, so it cannot police itself.
SKIP_FILES = {"lint_fleet_word.py"}

# A substring, not a \b word — identifiers are banned too, and \b would wave
# `flockSummary` through. Substring is safe here: the chemistry lookalikes
# (flocculent, deflocculant) spell the stem with a double c, so they never
# contain this sequence. The MUST_PASS/MUST_FAIL self-test pins both claims.
BANNED = re.compile(r"flock", re.IGNORECASE)

# Masked out BEFORE the ban runs, in the same spirit as lint_spelling.py's
# API_EXEMPT: real names are not ours to rename.
EXEMPT_PATTERNS = [
    r"\bflock\((?!s\))",  # the syscall and calls to it: flock(2), libc::flock( —
                          # but never parenthetical pluralization, "flock(s)"
    r"`flock`",         # the syscall named in prose, backticked
    r"\bflock/PID\b",   # the flight-rules phrase (no flock/PID lock)
    r"\bFlock Safety\b",  # the company (research docs must name it)
    r"\bFlockOS\b",
]

# Files that may contain the bare word: the rule stating itself (quoting the
# word once is how a rule gets stated), and guard tests whose regexes assert
# its absence. Paths relative to the repo root. Add here ONLY for one of
# those two reasons, with the reason.
ALLOW_FILES = {
    # The rule, in the canonical brief and every generated pointer copy.
    "AGENTS.md", "CLAUDE.md", "GEMINI.md", "QWEN.md",
    ".clinerules", ".windsurfrules",
    ".cursor/rules/securacv.mdc", ".github/copilot-instructions.md",
    "scripts/gen_agent_entrypoints.py",   # docstring describes the brief
    # Workflows that describe what the advisory review looks for.
    ".github/workflows/claude-review.yml", ".github/workflows/lint.yml",
    # Docs that state or teach the rule.
    "docs/BRAND.md", "docs/FAQ.md", "docs/GLOSSARY.md",
    "docs/design/self_star_roadmap.md", "docs/hardware/dev_playground_todo.md",
    "docs/legal-audit-2026-07.md", "docs/tvos/README.md",
    # Research that names the company beyond the exact "Flock Safety" form.
    "docs/research/enterprise_surveillance_landscape.md",
    # Guard tests asserting the ban (their regexes contain the word).
    "canary-local/tests/mic.test.js", "canary-local/tests/mode.test.js",
    "tvos/WitnessWall/Tests/WitnessWallTests/WallSurfaceTests.swift",
    "tvos/WitnessWall/Tests/WitnessWallTests/WitnessCoreTests.swift",
}

# The guard on the guard: strings that must always pass, so an edit to the
# exemptions that stops protecting them fails loudly here instead of in a
# red build on somebody's syscall comment.
MUST_PASS = [
    "no flock/PID lock",
    "the Unix flock(2) syscall",
    "libc::flock(file.as_raw_fd(), libc::LOCK_EX)",
    "take `flock` on the sibling lock file",
    "Flock Safety's Falcon ALPR network",
    "a flocculent precipitate, treated with deflocculant",  # double-c stem
]
# And strings that must always fail — the ban losing its teeth is the other
# failure mode.
MUST_FAIL = ["a flock of Canaries", "your flock", "the whole Flock",
             "flockSummary", "flocking to the feeder",
             "your flock(s) of Canaries"]


def masked(line: str) -> str:
    for pat in EXEMPT_PATTERNS:
        line = re.sub(pat, lambda m: "\x00" * len(m.group(0)), line)
    return line


def walk(paths):
    for base in paths:
        p = Path(base).resolve()
        if not p.exists():
            print(f"lint_fleet_word.py: no such path: {base}", file=sys.stderr)
            raise SystemExit(2)
        if p.is_file():
            if p.name not in SKIP_FILES:
                yield p
            continue
        for f in p.rglob("*"):
            if (f.is_file()
                    and (f.suffix in EXTS or f.name in EXTRA_NAMES)
                    and f.name not in SKIP_FILES
                    and not any(d in f.parts for d in SKIP_DIRS)):
                yield f


def main() -> int:
    broken_guard = ([s for s in MUST_PASS if BANNED.search(masked(s))]
                    + [s for s in MUST_FAIL if not BANNED.search(masked(s))])
    if broken_guard:
        print("lint_fleet_word.py: the exemption patterns no longer behave — "
              "these self-test strings get the wrong verdict:",
              file=sys.stderr)
        for s in broken_guard:
            print(f"  {s!r}", file=sys.stderr)
        return 2
    paths = sys.argv[1:] or [ROOT]
    offenders = []
    for f in walk(paths):
        rel = f.relative_to(ROOT).as_posix() if f.is_relative_to(ROOT) \
            else str(f)
        if rel in ALLOW_FILES:
            continue
        try:
            text = f.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for i, line in enumerate(text.splitlines(), 1):
            if BANNED.search(masked(line)):
                offenders.append(f"{rel}:{i}")
    if offenders:
        print(f"lint_fleet_word.py: {len(offenders)} use(s) of the banned "
              "bird-group word — a group of Canaries is a FLEET "
              "(AGENTS.md rule 3):", file=sys.stderr)
        for o in offenders[:40]:
            print(f"  {o}", file=sys.stderr)
        if len(offenders) > 40:
            print(f"  … and {len(offenders) - 40} more", file=sys.stderr)
        return 1
    print("fleet OK — the bird-group word appears only where the rule "
          "states itself")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
