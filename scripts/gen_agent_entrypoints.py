#!/usr/bin/env python3
"""scripts/gen_agent_entrypoints.py — one brief, every assistant reads it.

Every AI coding tool looks for its own filename: Codex and most others read
AGENTS.md, Claude Code reads CLAUDE.md, Gemini CLI reads GEMINI.md, Qwen Code
reads QWEN.md, Copilot reads .github/copilot-instructions.md, Cursor reads
.cursor/rules/*.mdc, Cline reads .clinerules, Windsurf reads .windsurfrules.

Maintaining eight copies of "never say flock, never add face recognition" by
hand guarantees seven of them go stale — and a stale copy is worse than none,
because an agent trusts it. So AGENTS.md is the single source: the block between
its BEGIN/END AGENT-BRIEF markers is generated into every vendor file, and CI
(`--check`) fails if a generated file drifts from it.

This is the same discipline as spec/witness_dictionary.json (one vocabulary,
linted copies) applied to agent instructions.

Run:    python3 scripts/gen_agent_entrypoints.py          # write the files
Check:  python3 scripts/gen_agent_entrypoints.py --check  # CI gate, no writes
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / "AGENTS.md"
BEGIN = "<!-- BEGIN AGENT-BRIEF"
END = "<!-- END AGENT-BRIEF -->"

# Each target: the path a tool looks for, the H1 it gets, and any preamble the
# tool's own format needs (Cursor wants MDC front-matter).
TARGETS = [
    {
        "path": "GEMINI.md",
        "tool": "Gemini CLI",
        "title": "# GEMINI.md — SecuraCV agent brief",
    },
    {
        "path": "QWEN.md",
        "tool": "Qwen Code",
        "title": "# QWEN.md — SecuraCV agent brief",
    },
    {
        "path": ".github/copilot-instructions.md",
        "tool": "GitHub Copilot",
        "title": "# Copilot instructions — SecuraCV",
    },
    {
        "path": ".cursor/rules/securacv.mdc",
        "tool": "Cursor",
        "title": "# Cursor rules — SecuraCV",
        "frontmatter": (
            "---\n"
            "description: SecuraCV project brief — privacy invariants, naming rules, repo map\n"
            "alwaysApply: true\n"
            "---\n"
        ),
    },
    {
        "path": ".clinerules",
        "tool": "Cline",
        "title": "# Cline rules — SecuraCV",
    },
    {
        "path": ".windsurfrules",
        "tool": "Windsurf",
        "title": "# Windsurf rules — SecuraCV",
    },
]

BANNER = (
    "<!-- GENERATED FILE — DO NOT EDIT.\n"
    "     Source: AGENTS.md (the block between its AGENT-BRIEF markers).\n"
    "     Regenerate: python3 scripts/gen_agent_entrypoints.py\n"
    "     CI fails if this file drifts from AGENTS.md. -->\n"
)

# {up} is the path back to the repo root, so links work from a file that lives
# in .github/ or .cursor/rules/ as well as from one at the root.
FOOTER = """
---

## The rest of the brief

This file carries the non-negotiables only. The full brief — the repo map, the
"where do I look for X" index, the CI gates, the detection-backend audit rules,
the code style, and the Beacon/Chirp channel invariants — lives in
[`AGENTS.md`]({up}AGENTS.md). Read it before any non-trivial change.

Also worth having open:

- [`docs/GLOSSARY.md`]({up}docs/GLOSSARY.md) — every proper noun in the project,
  defined once. Read this before answering a question about what something is.
- [`docs/FAQ.md`]({up}docs/FAQ.md) — the questions users actually ask, answered.
- [`docs/README.md`]({up}docs/README.md) — the CI-enforced map of every doc.
- [`docs/CONSOLIDATION.md`]({up}docs/CONSOLIDATION.md) — which similarly-named
  directory is the real one.
- [`docs/FLIGHT_RULES.md`]({up}docs/FLIGHT_RULES.md) — the engineering
  constitution.
"""


def brief() -> str:
    """The shared block from AGENTS.md, between the markers."""
    text = SOURCE.read_text(encoding="utf-8")
    try:
        start = text.index(BEGIN)
        start = text.index("-->", start) + len("-->")
        end = text.index(END)
    except ValueError:
        sys.exit(
            f"error: {SOURCE.name} is missing its AGENT-BRIEF markers.\n"
            f"       Expected a block delimited by:\n"
            f"         {BEGIN} ... -->\n"
            f"         {END}\n"
            "       Restore them — every vendor agent file is generated from it."
        )
    if end < start:
        sys.exit(f"error: {SOURCE.name} has END AGENT-BRIEF before BEGIN.")
    return text[start:end].strip("\n")


def render(target: dict, block: str) -> str:
    parts = []
    if target.get("frontmatter"):
        parts.append(target["frontmatter"])
    parts.append(BANNER)
    parts.append(f"\n{target['title']}\n\n")
    parts.append(
        f"The rules below are shared by every AI assistant working in this\n"
        f"repository; {target['tool']} reads them from this file.\n"
    )
    parts.append(f"\n{block}\n")
    depth = Path(target["path"]).parent.parts
    parts.append(FOOTER.format(up="../" * len(depth)))
    return "".join(parts)


def main() -> int:
    check = "--check" in sys.argv[1:]
    block = brief()
    if not block.strip():
        sys.exit("error: the AGENT-BRIEF block in AGENTS.md is empty.")

    stale = []
    for target in TARGETS:
        path = REPO / target["path"]
        want = render(target, block)
        have = path.read_text(encoding="utf-8") if path.exists() else None
        if have == want:
            continue
        if check:
            stale.append(target["path"])
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(want, encoding="utf-8")
        print(f"  wrote {target['path']}")

    if check:
        if stale:
            print("::error::Agent entrypoint files no longer match AGENTS.md.")
            print("The following are stale (or missing):")
            for p in stale:
                print(f"  - {p}")
            print("\nRegenerate and commit them:")
            print("    python3 scripts/gen_agent_entrypoints.py")
            print(
                "\nWhy this gate exists: every assistant reads a different filename.\n"
                "A stale copy is worse than no copy — the agent trusts it."
            )
            return 1
        print(f"All {len(TARGETS)} agent entrypoints match AGENTS.md. ✅")
        return 0

    print(f"Generated {len(TARGETS)} agent entrypoints from AGENTS.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
