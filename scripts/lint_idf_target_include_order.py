#!/usr/bin/env python3
"""A chip-target macro tested before anything defines it is silently false.

IDF defines CONFIG_IDF_TARGET_* in sdkconfig.h and ESP_IDF_VERSION in
esp_idf_version.h, never on the compiler's command line. So in every .cpp,
.h and .ino under firmware/, the first #if on either must follow an include
that reaches them: sdkconfig.h, Arduino.h, or an esp_*, driver/ or freertos/
header. A file that tests first compiles its "no such chip" branch on every
chip, and nothing warns.

Usage: lint_idf_target_include_order.py [--root DIR]
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SCAN_DIR = "firmware"
SUFFIXES = (".cpp", ".h", ".ino")

MACRO = re.compile(r"\b(?:CONFIG_IDF_TARGET|ESP_IDF_VERSION)\w*")
# Comments and raw strings are blanked; ordinary literals are matched only so
# a "//" or "/*" inside one is not read as a comment.
_SKIP = re.compile(
    r'R"(?P<d>[^()\\\s]{0,16})\(.*?\)(?P=d)"'
    r'|"(?:\\.|[^"\\\n])*"'
    r"|'(?:\\.|[^'\\\n])*'"
    r"|/\*.*?\*/"
    r"|//(?:\\\n|[^\n])*",
    re.S,
)
_DIRECTIVE = re.compile(r"\s*#\s*(include|if|ifdef|ifndef|elif|elifdef|elifndef)\b(.*)", re.S)
_INCLUDED = re.compile(r'\s*[<"]([^>"]+)[>"]')


def _blank(m: re.Match) -> str:
    text = m.group(0)
    if text[0] in "\"'":
        return text
    return re.sub(r"[^\n]", " ", text)


def reaches_sdkconfig(header: str) -> bool:
    """True for an include that defines the macros, directly or on its way."""
    name = header.rsplit("/", 1)[-1]
    return (name in ("sdkconfig.h", "Arduino.h") or name.startswith("esp_")
            or header.startswith(("driver/", "freertos/")))


def first_early_test(text: str) -> tuple[int, str] | None:
    """(line, macro) of a target test no earlier include covers, else None."""
    if not MACRO.search(text):
        return None
    lines = _SKIP.sub(_blank, text.replace("\r\n", "\n")).split("\n")
    i = 0
    while i < len(lines):
        start, logical = i, lines[i]
        while logical.endswith("\\") and i + 1 < len(lines):
            i += 1
            logical = logical[:-1] + " " + lines[i]
        i += 1
        m = _DIRECTIVE.match(logical)
        if not m:
            continue
        if m.group(1) == "include":
            inc = _INCLUDED.match(m.group(2))
            if inc and reaches_sdkconfig(inc.group(1)):
                return None
            continue
        tested = MACRO.search(m.group(2))
        if tested:
            return start + 1, tested.group(0)
    return None


def sources(root: Path) -> list[Path]:
    """Tracked sources under firmware/; every one on disk outside a checkout."""
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--", SCAN_DIR],
            capture_output=True, check=True).stdout.decode("utf-8", "surrogateescape")
        paths = [root / p for p in out.split("\0") if p]
    except (OSError, subprocess.CalledProcessError):
        paths = []
    if not paths:
        paths = list((root / SCAN_DIR).rglob("*"))
    return sorted(p for p in paths if p.suffix in SUFFIXES and p.is_file())


def lint(root: Path) -> list[str]:
    problems = []
    for path in sources(root):
        hit = first_early_test(path.read_text(encoding="utf-8", errors="replace"))
        if hit:
            problems.append(
                f"{path.relative_to(root).as_posix()}:{hit[0]}: {hit[1]} is tested "
                "before any include that defines it")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args(argv)
    files = sources(args.root)
    if not files:
        print(f"lint_idf_target_include_order: no sources under {SCAN_DIR}/", file=sys.stderr)
        return 2
    problems = lint(args.root)
    for p in problems:
        print(p)
    if problems:
        print(f"\n{len(problems)} file(s) test a chip-target macro too early. Include "
              "<sdkconfig.h> (or <esp_idf_version.h> for ESP_IDF_VERSION) above the test.",
              file=sys.stderr)
        return 1
    print(f"lint_idf_target_include_order: OK ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
