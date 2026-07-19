#!/usr/bin/env python3
"""scripts/lint_docs_index.py — the documentation stays a map, not a pile.

Two promises, enforced:

  1. NO STRAGGLERS — every Markdown file under docs/ is reachable from the
     index (docs/README.md): either linked there directly, or linked from a
     README.md in its own directory that the index links. A doc nobody can
     navigate to is a doc that rots; this gate makes "add the doc, skip the
     index" a red X instead of a habit.

  2. NO DEAD ENDS — every relative .md link in docs/README.md and in the
     per-directory READMEs resolves to a file that exists. Renames and
     deletions must update the map in the same commit.

Exempt: nothing. New directory of docs? Give it a README.md, link that from
the index, and the directory becomes self-indexing.

Run:  python3 scripts/lint_docs_index.py
CI:   .github/workflows/lint.yml (Repo Lints → scripts/lint_*.sh)
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DOCS = REPO / "docs"
INDEX = DOCS / "README.md"

# Markdown + HTML links; anchors and titles stripped. Bare-path mentions in
# backticks don't count — a straggler must be *clickable* from the map.
LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s#]+?\.md)(?:#[^)]*)?\)|href=\"([^\"#]+?\.md)(?:#[^\"]*)?\"")


def links_of(md_file: Path) -> set[Path]:
    out = set()
    for m in LINK_RE.finditer(md_file.read_text(encoding="utf-8")):
        target = m.group(1) or m.group(2)
        if target.startswith(("http://", "https://")):
            continue
        resolved = (md_file.parent / target).resolve()
        out.add(resolved)
    return out


def main() -> int:
    if not INDEX.exists():
        print("lint_docs_index.py: docs/README.md is missing — the docs have no map")
        return 1

    all_docs = {p.resolve() for p in DOCS.rglob("*.md")}

    # BFS from the index: a linked README.md makes its directory
    # self-indexing, and that recurses (hardware/README → enclosure/README
    # → its members). Every hop must itself be reachable, so a chain can't
    # dangle from an unlinked page.
    reachable = {INDEX.resolve()}
    dir_readmes = []
    queue = [INDEX]
    while queue:
        src = queue.pop(0)
        for target in links_of(src):
            if target in reachable:
                continue
            reachable.add(target)
            if target.name == "README.md" and target.exists():
                dir_readmes.append(target)
                queue.append(target)

    failures = []

    stragglers = sorted(p for p in all_docs if p not in reachable)
    for p in stragglers:
        rel = p.relative_to(REPO)
        failures.append(
            f"STRAGGLER  {rel} — not linked from docs/README.md "
            f"(or from a directory README the index links). Add it to the map "
            f"or fold it into an existing doc.")

    for src in [INDEX, *sorted(dir_readmes)]:
        for target in sorted(links_of(src)):
            if not target.exists() and str(target).startswith(str(REPO)):
                failures.append(
                    f"DEAD LINK  {src.relative_to(REPO)} → "
                    f"{target.relative_to(REPO) if target.is_relative_to(REPO) else target}")

    if failures:
        print(f"lint_docs_index.py: {len(failures)} problem(s):\n")
        print("\n".join(failures))
        return 1

    print(f"lint_docs_index.py: OK — {len(all_docs)} docs, all reachable from the index; no dead links.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
