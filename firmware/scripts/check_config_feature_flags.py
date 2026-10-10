#!/usr/bin/env python3
"""check_config_feature_flags.py — every FEATURE_* a config sets gates code.

THE BUG THIS EXISTS TO CATCH
============================

A FEATURE_* flag is a promise: set it to 1 and something compiles in, set it
to 0 and it compiles out. A flag that no source reads keeps the promise's
shape with nothing behind it, and everything downstream believes it:

  * FEATURE_PROOF_QR, FEATURE_CHAIN_VERIFY and FEATURE_NIGHT_BLACKOUT sat in
    the canary-display configs read by no source. The docs described a
    "first-boot dark seed" that did not exist (every flavor boots DIM), a
    size-cut plan named "then FEATURE_PROOF_QR" as a cut that would have
    removed nothing, and the Lab Workshop listed all three as switches.
  * The canary's [env:full] set FEATURE_BLUETOOTH, _SYS_MONITOR,
    _WIFI_PRESENCE, _AUDIBLE_CHIRP, _RF_PRESENCE and _CHIRP to 1, and the
    secure env carried `-DFEATURE_BLUETOOTH=0 ; DISABLED FOR SECURITY`.
    No file in firmware/canary read any of them: the "security" line was a
    no-op, and firmware/FEATURES.md credited a property to it.

scripts/lint_feature_flags.sh polices Cargo features and the Home Assistant
const.py lists; nothing looked at firmware macros. This does (audit,
2026-10-09), and that lint runs it, so one feature-flag gate covers every layer.

THE RULE
========

1. firmware/configs/<project>/*/config.h, for each project built from
   firmware/projects/<project>/: every `#define FEATURE_X` is READ by that
   project's src/ or include/, or by firmware/common/, or it is a
   DESCRIPTORS entry for the project, with the reason.
2. firmware/canary: every FEATURE_X that canary/include/canary_config.h
   defines, or that canary/platformio.ini or provisioning/platformio_secure.ini
   sets with -D, is READ by canary/src, canary/lib, canary/include or
   firmware/common, or it is a CANARY_DESCRIPTORS entry, with the reason.

A READ is a mention outside comments that is not the flag's own definition
(`#define X`, `#undef X`, or the `#ifndef X` that guards a default). Never
counted as readers: firmware/configs/ itself, canary_config.h, the generated
Arduino mirrors (projects/*/arduino/, copies of the trees read here), and
common/core/feature_sanity.h, except for a project that includes it: its
FEATURE_* vs HAS_* #errors give a flag a compile-time meaning there.

SKIPPED, with the reason: firmware/configs/canary-wap/, which no build
compiles (its README says why it still exists; the WAP's flags are its
sketch's build_config.h).

A DESCRIPTORS entry that no longer applies (the flag is read now, or no config
sets it) fails the check, so the list cannot outlive what it excuses.

Run:  python3 firmware/scripts/check_config_feature_flags.py
CI:   scripts/lint_feature_flags.sh (check D), from .github/workflows/lint.yml
      on every PR
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FW = REPO / "firmware"
CONFIGS = FW / "configs"
SOURCE_EXTS = {".h", ".hpp", ".c", ".cc", ".cpp", ".ino"}
FEATURE_SANITY = FW / "common" / "core" / "feature_sanity.h"
NEVER_READERS = {
    FEATURE_SANITY,  # counted only for the projects that include it (below)
    FW / "canary" / "include" / "canary_config.h",
}

SKIPPED_PROJECTS = {
    "canary-wap": "compiled by no build: no env puts firmware/configs/canary-wap/ "
                  "on an include path and the sketch includes no config.h (see "
                  "firmware/configs/canary-wap/default/README.md)",
}

# Flags a config sets that no #if reads, kept on purpose: each is a
# descriptor (what this product has or lacks) that the Lab's generators or a
# reader of the config uses, while the code it describes compiles
# unconditionally or not at all. (project, flag) → reason.
DESCRIPTORS: dict[tuple[str, str], str] = {
    ("canary-display", "FEATURE_HA_DISCOVERY"):
        "descriptor (0): the display publishes no Home Assistant discovery; "
        "it consumes the fleet's",
    ("canary-display", "FEATURE_MMWAVE_RADAR"):
        "descriptor (0): no display board carries a radar",
    ("canary-sense", "FEATURE_HA_DISCOVERY"):
        "descriptor (1): src/ha/ha_discovery.cpp compiles unconditionally",
    ("canary-sense", "FEATURE_MMWAVE_RADAR"):
        "descriptor (1): the MR60 radar driver compiles unconditionally "
        "(main.cpp includes sensors/mmwave_mr60)",
    ("canary-sentinel", "FEATURE_HA_DISCOVERY"):
        "descriptor (1): src/ha/ha_discovery.cpp compiles unconditionally",
    ("canary-sentinel", "FEATURE_MQTT"):
        "descriptor (1, door): src/net/mqtt_mgr.cpp compiles unconditionally",
    ("canary-sentinel", "FEATURE_WIFI_STA"):
        "descriptor (1, door): src/net/wifi_mgr.cpp compiles unconditionally",
    ("canary-sentinel", "FEATURE_MESH_NETWORK"):
        "Phase 1b placeholder (perimeter-demo): the ESP-NOW head<->hub link "
        "it names is not built (the project README's phase table, "
        "'[BENCH]'); the port that builds it reads the flag",
    ("canary-sentinel", "FEATURE_STATUS_LED"):
        "descriptor (1): no canary-sentinel source reads it (canary-sense's "
        "main.cpp reads its own config's copy)",
    ("canary-vision", "FEATURE_HA_DISCOVERY"):
        "descriptor (1): src/ha/ha_discovery.cpp compiles unconditionally",
    ("canary-vision", "FEATURE_RF_PRESENCE"):
        "descriptor (0): the Vision has no RF-presence code",
}

# The same, for the canary tree: flag → reason.
CANARY_DESCRIPTORS: dict[str, str] = {
    "FEATURE_TEST_CONSOLE":
        "reserved gate, default 0 and set by no env (scripts/lint_build_matrix.py "
        "pins both): no demo/mutating console command exists yet, and the first "
        "one goes behind #if FEATURE_TEST_CONSOLE (docs/design/test_console.md)",
}

DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(FEATURE_\w+)\b", re.M)
DFLAG_RE = re.compile(r"(?:^|[\s'\"])-D(FEATURE_\w+)\b", re.M)


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def strip_comments(src: str) -> str:
    """Drop // and /* */ comments, keeping string and char literals intact."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def sources(roots: list[Path]) -> list[Path]:
    files = []
    for root in roots:
        if not root.is_dir():
            continue
        for p in sorted(root.rglob("*")):
            if (p.suffix in SOURCE_EXTS and p.is_file() and p not in NEVER_READERS
                    and "arduino" not in p.relative_to(FW).parts[:3]
                    and ".pio" not in p.parts):
                files.append(p)
    return files


def reads(flag: str, texts: list[str]) -> bool:
    """Does any (comment-stripped) text mention `flag` other than defining it?"""
    word = re.compile(r"\b%s\b" % re.escape(flag))
    own = re.compile(r"^[ \t]*#[ \t]*(?:define|undef|ifndef)[ \t]+%s\b" % re.escape(flag))
    for text in texts:
        for line in text.splitlines():
            if word.search(line) and not own.match(line):
                return True
    return False


def rel(p: Path) -> str:
    return p.relative_to(REPO).as_posix()


def main() -> int:
    problems: list[str] = []
    common = sources([FW / "common"])
    common_texts = [strip_comments(read(p)) for p in common]
    seen_descriptors: set[tuple[str, str]] = set()

    # ── 1. firmware/configs/<project>/*/config.h ──
    checked_projects = 0
    for proj_dir in sorted(p for p in CONFIGS.iterdir() if p.is_dir()):
        project = proj_dir.name
        if project in SKIPPED_PROJECTS:
            print(f"  ~ skipped firmware/configs/{project}: {SKIPPED_PROJECTS[project]}")
            continue
        tree = FW / "projects" / project
        if not tree.is_dir():
            problems.append(f"firmware/configs/{project}/ has no firmware/projects/{project}/ to read it "
                            f"(add it to SKIPPED_PROJECTS with the reason, or delete it)")
            continue
        checked_projects += 1
        flags: dict[str, list[str]] = {}
        for cfg in sorted(proj_dir.glob("*/config.h")):
            for flag in DEFINE_RE.findall(read(cfg)):
                flags.setdefault(flag, []).append(cfg.parent.name)
        own = sources([tree / "src", tree / "include"])
        texts = [strip_comments(read(p)) for p in own] + common_texts
        # feature_sanity.h is a reader for a project that includes it: its
        # FEATURE_* vs HAS_* #errors give the flag a compile-time meaning
        # there (a camera flag on a camera-less board fails the build).
        if any("feature_sanity.h" in read(p) for p in own):
            texts.append(strip_comments(read(FEATURE_SANITY)))
        for flag, flavors in sorted(flags.items()):
            key = (project, flag)
            if reads(flag, texts):
                continue
            if key in DESCRIPTORS:
                seen_descriptors.add(key)
                continue
            problems.append(f"{flag} is set in firmware/configs/{project}/{{{','.join(sorted(set(flavors)))}}}"
                            f"/config.h but no {project} or common/ source reads it")

    # ── 2. the canary tree ──
    canary_flags: dict[str, set[str]] = {}
    cfg = FW / "canary" / "include" / "canary_config.h"
    for flag in DEFINE_RE.findall(read(cfg)):
        canary_flags.setdefault(flag, set()).add(rel(cfg))
    for ini in (FW / "canary" / "platformio.ini", FW / "provisioning" / "platformio_secure.ini"):
        body = "\n".join(line.split(";", 1)[0] for line in read(ini).splitlines())
        for flag in DFLAG_RE.findall(body):
            canary_flags.setdefault(flag, set()).add(rel(ini))
    canary_texts = [strip_comments(read(p)) for p in sources(
        [FW / "canary" / "src", FW / "canary" / "lib", FW / "canary" / "include"])] + common_texts
    for flag, where in sorted(canary_flags.items()):
        if not reads(flag, canary_texts):
            if flag in CANARY_DESCRIPTORS:
                seen_descriptors.add(("canary", flag))
                continue
            problems.append(f"{flag} is set for firmware/canary ({', '.join(sorted(where))}) but no "
                            f"canary/src, canary/lib, canary/include or common/ source reads it")

    for project in sorted(SKIPPED_PROJECTS):
        if not (CONFIGS / project).is_dir():
            problems.append(f"stale SKIPPED_PROJECTS entry {project}: firmware/configs/{project}/ "
                            f"is gone; remove the entry")
    listed = set(DESCRIPTORS) | {("canary", f) for f in CANARY_DESCRIPTORS}
    stale = sorted(listed - seen_descriptors)
    for project, flag in stale:
        problems.append(f"stale DESCRIPTORS entry ({project}, {flag}): it is read now, or no "
                        f"config sets it; remove the entry")

    if problems:
        print("::error::FEATURE_* flags that gate no code:")
        for p in problems:
            print(f"  - {p}")
        print()
        print("  A flag nothing reads is a switch connected to nothing: docs, the Lab")
        print("  Workshop and size plans take it at its word. Read it where the code")
        print("  it names compiles, or delete it from the config (and the docs that")
        print("  advertise it). If it is a deliberate descriptor, add a DESCRIPTORS")
        print("  entry in this script saying what it describes.")
        return 1
    print(f"✓ config FEATURE_* flags: every flag in {checked_projects} configs/ project(s) and the "
          f"canary tree ({len(canary_flags)} flags) gates code or is a listed descriptor ({len(listed)}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
