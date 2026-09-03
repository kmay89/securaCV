#!/usr/bin/env python3
"""firmware/scripts/lint_platform_pins.py — the espressif32 platform pin has ONE home.

firmware/envs/platformio/platforms.ini is the only file allowed to carry a
literal `platform =` pin for the ESP32 toolchain — an `espressif32…` version
spec or a pioarduino release URL. Every other .ini under firmware/ takes its
platform by interpolation from one of that file's sections:

    platform = ${platform_core3.platform}

Why: the same pioarduino URL was typed in six files and the official pin in
three more, each with its own "bump in lockstep" comment — and lockstep held
only as long as every editor remembered every copy. One literal per distinct
pin, referenced everywhere, makes the lockstep structural and makes "which
platform does this env build on?" a one-file question (firmware/PLATFORMS.md).

Checks, on every .ini under firmware/ except .pio/ build dirs:
  1. no literal espressif32 pin or pioarduino URL on a `platform =` line
     outside platforms.ini (comment lines are ignored — prose may quote one);
  2. every `${platform_x.platform}` reference names a section platforms.ini has;
  3. platforms.ini holds only `[platform_*]` sections, each with exactly one
     option, `platform`, whose value IS a literal pin (never an interpolation);
  4. no two sections carry the same literal (one section per DISTINCT pin);
  5. every section is referenced by at least one env (no dead pins).

Exit 1 with one line per finding; 0 with a one-line summary.
Run:   python3 firmware/scripts/lint_platform_pins.py [--root <checkout>]
Tests: scripts/tests/test_lint_platform_pins.py (feeds it doctored trees and
       watches it go red — a lint that only ever passes proves nothing).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

PLATFORMS_INI = Path("firmware") / "envs" / "platformio" / "platforms.ini"

SECTION_RE = re.compile(r"^\s*\[([^\]]+)\]\s*$")
OPTION_RE = re.compile(r"^\s*([A-Za-z0-9_.\-]+)\s*=\s*(.*?)\s*$")
REF_RE = re.compile(r"^\$\{(platform_[A-Za-z0-9_]+)\.platform\}$")
PIN_LITERAL_RE = re.compile(r"espressif32|pioarduino", re.IGNORECASE)
SECTION_NAME_RE = re.compile(r"^platform_[a-z0-9_]+$")


def _strip_inline_comment(value: str) -> str:
    # PlatformIO (configparser) treats ' ;' / ' #' after a value as a comment.
    for marker in (" ;", " #", "\t;", "\t#"):
        idx = value.find(marker)
        if idx >= 0:
            value = value[:idx]
    return value.strip()


def _parse(path: Path) -> list[tuple[int, str, str, str]]:
    """(lineno, section, option, value) for every option line; comments and
    continuation lines skipped. A deliberately small parser: PlatformIO's own
    would resolve the interpolation we are here to inspect."""
    out: list[tuple[int, str, str, str]] = []
    section = ""
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.rstrip("\n")
        if not line.strip() or line.lstrip().startswith((";", "#")):
            continue
        m = SECTION_RE.match(line)
        if m:
            section = m.group(1).strip()
            continue
        if line[:1] in (" ", "\t"):
            continue  # continuation of a multi-line value
        m = OPTION_RE.match(line)
        if m:
            out.append((lineno, section, m.group(1), _strip_inline_comment(m.group(2))))
    return out


def _is_pin_literal(value: str) -> bool:
    return bool(PIN_LITERAL_RE.search(value)) or value.startswith(("http://", "https://", "file://"))


def lint(root: Path) -> list[str]:
    errors: list[str] = []
    pins_path = root / PLATFORMS_INI
    fw = root / "firmware"
    rel = lambda p: p.relative_to(root).as_posix()  # noqa: E731 — one-liner used four times

    if not pins_path.is_file():
        return [f"{PLATFORMS_INI.as_posix()}: missing — it is the one home for the platform pins"]

    # ── platforms.ini itself ──────────────────────────────────────────────────
    sections: dict[str, str] = {}
    per_section: dict[str, list[str]] = {}
    for lineno, section, option, value in _parse(pins_path):
        per_section.setdefault(section, []).append(option)
        if not SECTION_NAME_RE.match(section):
            errors.append(f"{rel(pins_path)}:{lineno}: section [{section}] must be named platform_<name> (lowercase)")
            continue
        if option != "platform":
            errors.append(f"{rel(pins_path)}:{lineno}: [{section}] carries '{option}' — only `platform` belongs here")
            continue
        if "${" in value:
            errors.append(f"{rel(pins_path)}:{lineno}: [{section}] interpolates {value!r} — platforms.ini holds LITERALS")
            continue
        if not _is_pin_literal(value):
            errors.append(f"{rel(pins_path)}:{lineno}: [{section}] = {value!r} is not an espressif32 pin or pioarduino URL")
            continue
        if section in sections:
            errors.append(f"{rel(pins_path)}:{lineno}: [{section}] sets `platform` twice")
            continue
        sections[section] = value
    for section, options in per_section.items():
        if SECTION_NAME_RE.match(section) and "platform" not in options:
            errors.append(f"{rel(pins_path)}: [{section}] has no `platform` option")
    by_literal: dict[str, list[str]] = {}
    for section, value in sections.items():
        by_literal.setdefault(value, []).append(section)
    for value, names in by_literal.items():
        if len(names) > 1:
            errors.append(
                f"{rel(pins_path)}: {value!r} appears in {len(names)} sections ({', '.join(names)}) — "
                "one section per DISTINCT pin; consumers that need the same bytes share it"
            )

    # ── every other .ini under firmware/ ──────────────────────────────────────
    referenced: dict[str, list[str]] = {}
    for ini in sorted(fw.rglob("*.ini")):
        if ".pio" in ini.parts or ini == pins_path:
            continue
        for lineno, section, option, value in _parse(ini):
            if option != "platform":
                continue
            where = f"{rel(ini)}:{lineno}"
            m = REF_RE.match(value)
            if m:
                referenced.setdefault(m.group(1), []).append(where)
                if m.group(1) not in sections:
                    errors.append(f"{where}: references [{m.group(1)}], which platforms.ini does not define")
                continue
            if _is_pin_literal(value):
                errors.append(
                    f"{where}: literal platform pin {value!r} in [{section}] — move it to "
                    f"{PLATFORMS_INI.as_posix()} and write `platform = ${{platform_<name>.platform}}`"
                )
            # any other platform (e.g. `native`) is not this lint's business

    for section in sections:
        if section not in referenced:
            errors.append(f"{rel(pins_path)}: [{section}] is referenced by no env — a dead pin; delete it or use it")

    if not errors:
        n_refs = sum(len(v) for v in referenced.values())
        print(
            f"lint_platform_pins OK — {n_refs} `platform =` lines interpolate "
            f"{len(sections)} pins from {PLATFORMS_INI.as_posix()}; no literal elsewhere"
        )
    return errors


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2],
                    help="checkout root (default: this file's repo)")
    args = ap.parse_args(argv)
    errors = lint(args.root.resolve())
    for e in errors:
        print(f"lint_platform_pins: {e}")
    if errors:
        print(f"lint_platform_pins: {len(errors)} finding(s) — the platform pin lives ONCE, in "
              f"{PLATFORMS_INI.as_posix()} (firmware/PLATFORMS.md)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
