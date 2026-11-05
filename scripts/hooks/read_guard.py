#!/usr/bin/env python3
"""scripts/hooks/read_guard.py — a whole read of a huge file is refused, with an outline.

Claude Code runs this before every Read (PreToolUse, `.claude/settings.json`).
It reads the tool call as JSON on stdin and exits 0 to allow it. For a Read of
a file over the limit with neither `offset` nor `limit`, it exits 2, and what
it prints on stderr is shown to the agent instead of the file: the size, the
cost, and an outline (headings, definitions, top-level keys) with line
numbers, so the next call is a ranged read of the part that matters.

WHY. Twelve hand-edited files here exceed 150 KB; `canary_wap.ino` is 685 KB,
about 170,000 tokens. Nothing told an agent not to open one whole, and the
read guard is the first thing that does. A ranged read is always allowed;
raster images are always allowed (they cannot be ranged); anything that is
not a Read, or has no file path, or cannot be parsed, is allowed — the guard
refuses only the one call it understands.

Knobs: READ_GUARD_LIMIT (bytes, default 120000), READ_GUARD_OFF=1 disables.
"""
import json
import os
import re
import sys
from pathlib import Path

DEFAULT_LIMIT = 120_000
OUTLINE_LINES = 40
IMAGES = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"}

# One outline pattern per family of file; the first group is what we print.
OUTLINE = {
    (".md", ".markdown"): re.compile(r"^(#{1,3} .+)$"),
    (".py",): re.compile(r"^((?:async )?def \w+|class \w+)"),
    (".rs",): re.compile(r"^(\s*(?:pub(?:\([^)]*\))? )?(?:fn|struct|enum|impl|mod|trait|type) [^{;(]+)"),
    (".c", ".cpp", ".cc", ".h", ".hpp", ".ino"): re.compile(
        r"^((?:class|struct|namespace|enum) \w+|[A-Za-z_][\w:<>*&, ]*\s[*&]?\w+\([^;]*\)\s*(?:const)?\s*\{?)\s*$"
    ),
    (".js", ".mjs", ".cjs", ".ts"): re.compile(
        r"^((?:export )?(?:async )?function\s*\*?\s*\w+|(?:export )?(?:const|let|class) \w+)"
    ),
    (".json",): re.compile(r'^(  "[^"]+")\s*:'),
    (".scad",): re.compile(r"^((?:module|function) \w+)"),
    (".swift",): re.compile(r"^(\s*(?:public |private |internal )?(?:final )?(?:func|struct|class|enum|extension|protocol) \w+)"),
    (".yml", ".yaml"): re.compile(r"^([a-zA-Z_][\w-]*):\s*$"),
    (".html",): re.compile(r"^\s*(<(?:section|h[1-3]|script|style|template)\b[^>]*>)"),
}


def pattern_for(path: Path):
    for exts, rx in OUTLINE.items():
        if path.suffix.lower() in exts:
            return rx
    return None


def outline(path: Path, limit: int = OUTLINE_LINES) -> list[str]:
    rx = pattern_for(path)
    if rx is None:
        return []
    out = []
    try:
        with path.open("r", encoding="utf-8", errors="replace") as fh:
            for n, line in enumerate(fh, 1):
                m = rx.match(line.rstrip("\n"))
                if m:
                    out.append(f"{n:>6}  {m.group(1).strip()[:100]}")
                    if len(out) >= limit:
                        out.append("        …")
                        break
    except OSError:
        return []
    return out


def decide(call: dict, limit: int) -> str | None:
    """None to allow; otherwise the refusal text."""
    if call.get("tool_name") != "Read":
        return None
    tool_input = call.get("tool_input") or {}
    raw = tool_input.get("file_path")
    if not isinstance(raw, str) or not raw:
        return None
    if tool_input.get("offset") is not None or tool_input.get("limit") is not None:
        return None
    path = Path(raw)
    if path.suffix.lower() in IMAGES:
        return None
    try:
        size = path.stat().st_size
    except OSError:
        return None
    if not path.is_file() or size <= limit:
        return None
    lines = [
        f"read_guard: {raw} is {size // 1024} KB (~{size // 4:,} tokens); a whole read is refused.",
        "Read a range instead — Read(file_path, offset=<line>, limit=<lines>) — or search it first.",
    ]
    ol = outline(path)
    if ol:
        lines.append("Outline (line  item):")
        lines.extend(ol)
    return "\n".join(lines)


def main() -> int:
    if os.environ.get("READ_GUARD_OFF") == "1":
        return 0
    try:
        limit = int(os.environ.get("READ_GUARD_LIMIT", DEFAULT_LIMIT))
    except ValueError:
        limit = DEFAULT_LIMIT
    try:
        call = json.load(sys.stdin)
    except (json.JSONDecodeError, OSError):
        return 0
    if not isinstance(call, dict):
        return 0
    refusal = decide(call, limit)
    if refusal is None:
        return 0
    print(refusal, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
