#!/usr/bin/env python3
"""check_common_build_reachability.py — every shared .cpp is actually compiled.

THE BUG THIS EXISTS TO CATCH
============================

A new module lands in `firmware/common/<lib>/` as a header + a .cpp, a project
includes the header, the host test suite passes, review passes — and the .cpp
is never compiled by anything. The failure surfaces far from the cause: an
undefined-reference at LINK time, in one board's build job, minutes into CI.

That is exactly how `firmware/common/color` shipped in the nightstand look
engine: nothing compiled its sources, the `-I../../common` in the env still
made the *headers* resolve, and the only symptom was
`canary-display-nightstand-s3` failing to link against
`canary::color::wash_stops`.

The header resolving is not evidence that the translation unit is built. This
guard checks the second thing directly.

WHICH ROUTE TO PICK (learned the hard way on that same bug)
===========================================================

Reach for `build_src_filter` FIRST. It is deterministic: the file is named, so
it is compiled.

A `library.json` only helps when the LDF already decided to look at the
library, and it will not look if the only `#include` sits behind a flavor or
feature conditional. These projects inherit `lib_ldf_mode = deep+` from
common.ini, which EVALUATES preprocessor conditionals — and the flavor symbols
(`CD_FLAVOR_NIGHTSTAND`, `FEATURE_AMBIENT_LED`, …) are not defined during that
scan. So a conditionally-included library is invisible to the LDF no matter how
correct its manifest is. Adding a manifest to common/color did not fix the
link error; naming its two .cpp files in the nightstand env's build_src_filter
did.

`common/boot` is the contrast that makes the rule clear: it works through the
LDF only because `boot/boot_banner.h` is included UNCONDITIONALLY from
canary-display's main.cpp.

Never satisfy both routes for the same file — that compiles the translation
unit twice and fails the link on duplicate symbols (canary-sense.ini carries
the same warning).

THE RULE
========

Every non-test `.cpp` under `firmware/common/` must be reachable by at least
one of the three routes this repo actually uses to compile shared code:

  1. PlatformIO LDF — a `library.json` manifest inside the library directory.
     Projects that pull common/ in via `lib_extra_dirs` rely on this; without
     the manifest the directory is not a library and its sources are skipped.
     (See common/boot and common/fleet_link for the canonical shape: headers
     and .cpp at the dir root, `includeDir: ".."` so `"<lib>/<name>.h"`
     resolves, `srcDir: "."` + `srcFilter: ["+<*.cpp>"]` so the TUs build.)

     A manifest that EXISTS is not automatically a manifest that WORKS. The
     optional `headers` field is what the LDF matches `#include` directives
     against, and it is interpreted relative to `includeDir` — so declaring
     `headers: ["look_engine.h"]` alongside `includeDir: ".."` tells the LDF
     to look for `common/look_engine.h`, which does not exist, and the library
     is silently never linked. That is the second half of the common/color
     bug: adding the manifest was necessary but the extra `headers` metadata
     made it inert, and the link error did not change at all. The guard below
     therefore checks that every declared header actually resolves.
     If in doubt, declare no `headers` field — boot and fleet_link don't.

  2. Explicit `build_src_filter` — the .cpp is named in a PlatformIO env, the
     pattern canary-sense / canary-sentinel use for the modules they compile
     directly rather than through the LDF.

  3. Arduino staged copy — a flat, committed copy of the file lives in a
     sketch directory (`firmware/projects/*/arduino/*/`), so the Arduino build
     compiles it as part of the sketch.

A file reachable by none of these is dead weight at best and a link error
waiting for the right build flavor at worst.

Run:  python3 firmware/scripts/check_common_build_reachability.py
CI:   .github/workflows/firmware.yml (Regression Guards)
"""
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
COMMON = REPO / "firmware" / "common"
ENV_DIRS = [REPO / "firmware" / "envs" / "platformio"]
PROJECTS = REPO / "firmware" / "projects"

# Files that are knowingly compiled by nothing, with the reason. An entry here
# is a deliberate, reviewed statement — "this is not wired up yet and we know
# it" — not a way to silence the guard. Wire the module up or delete it, and
# drop the waiver in the same PR.
#
# (bluetooth/ble_debug_beacon.cpp carried the only waiver for a year: orphaned
# since it landed, nothing ever included its header. It was deleted rather than
# wired up, and its waiver left with it.)
WAIVERS: dict[str, str] = {}


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def manifest_dirs() -> tuple[set[Path], list[tuple[Path, str]]]:
    """Library dirs declaring a PlatformIO manifest, plus any broken ones.

    "Broken" covers both ways a manifest can be present but useless:
      * it doesn't parse — PlatformIO ignores it and skips the sources;
      * it declares `headers` that don't resolve relative to `includeDir`,
        so the LDF cannot match the project's #include and never links the
        library. This one is nastier: the build failure is identical to
        having no manifest at all, so it looks like the fix didn't apply.
    """
    good: set[Path] = set()
    bad: list[tuple[Path, str]] = []
    for man in sorted(COMMON.rglob("library.json")):
        try:
            data = json.loads(read(man))
        except json.JSONDecodeError as exc:
            bad.append((man, f"not valid JSON ({exc})"))
            continue

        build = data.get("build") or {}
        declared = data.get("headers") or []

        # Where the LDF resolves declared headers.
        #
        # An EXPLICIT includeDir is authoritative — that is the single path a
        # dependent project sees, which is why common/color's `includeDir: ".."`
        # meant its headers had to be declared as "color/<name>.h".
        #
        # With no includeDir, PlatformIO falls back through include/ then the
        # source dir then the library root — which is how common/csi resolves
        # `#include <csi_types.h>` from its src/ directory.
        if "includeDir" in build:
            roots = [(man.parent / build["includeDir"]).resolve()]
            where = f"includeDir '{build['includeDir']}'"
        else:
            roots = [
                (man.parent / "include").resolve(),
                (man.parent / build.get("srcDir", "src")).resolve(),
                man.parent.resolve(),
            ]
            where = "the default include/ → srcDir → library-root fallback"

        unresolved = [
            h for h in declared
            if not any((r / h).is_file() for r in roots)
        ]
        if unresolved:
            def show(p: Path) -> str:
                try:
                    return str(p.relative_to(REPO))
                except ValueError:
                    return str(p)
            bad.append((
                man,
                f"declares headers the LDF cannot resolve under {where} "
                f"(looked in {', '.join(show(r) for r in roots)}): "
                f"{', '.join(unresolved)}. The `headers` field is what the LDF "
                f"matches #include directives against, so an entry that does "
                f"not resolve means the library is never linked — the same "
                f"link error as having no manifest at all. Declare them the "
                f"way projects include them, or omit the field entirely, as "
                f"common/boot and common/fleet_link do."
            ))
            continue

        good.add(man.parent)
    return good, bad


def build_filter_text() -> str:
    """Every PlatformIO ini that could name a source explicitly."""
    chunks = []
    for d in ENV_DIRS:
        for ini in sorted(d.glob("*.ini")):
            chunks.append(read(ini))
    for ini in sorted(PROJECTS.glob("*/platformio.ini")):
        chunks.append(read(ini))
    return "\n".join(chunks)


def staged_copy_names() -> set[str]:
    """Basenames of .cpp files committed into Arduino sketch directories."""
    names = set()
    for sketch_cpp in PROJECTS.glob("*/arduino/*/*.cpp"):
        names.add(sketch_cpp.name)
    return names


def main() -> int:
    if not COMMON.is_dir():
        print(f"::error::{COMMON} not found")
        return 1

    manifests, invalid = manifest_dirs()
    if invalid:
        print("::error::library.json present but ineffective — PlatformIO's LDF")
        print("         would skip these libraries' sources entirely, which")
        print("         fails at LINK time exactly as if no manifest existed:")
        for man, err in invalid:
            print(f"::error file={man.relative_to(REPO)}::{err}")
        return 1

    inis = build_filter_text()
    staged = staged_copy_names()

    unreachable = []
    checked = 0

    for cpp in sorted(COMMON.rglob("*.cpp")):
        if cpp.name.startswith("test_"):
            continue
        rel_common = cpp.relative_to(COMMON).as_posix()   # e.g. "boot/boot_banner.cpp"
        checked += 1

        # Route 1 — a manifest at or above this file, inside common/.
        if any(m in cpp.parents for m in manifests):
            continue

        # Route 2 — named in a build_src_filter (match on the common/-relative
        # tail, which is how the ini entries spell it: +<../../../common/...>).
        if f"common/{rel_common}" in inis:
            continue

        # Route 3 — a committed flat copy in an Arduino sketch.
        if cpp.name in staged:
            continue

        if rel_common in WAIVERS:
            print(f"  ~ waived: {rel_common}")
            print(f"      {WAIVERS[rel_common]}")
            continue

        unreachable.append(rel_common)

    stale = [w for w in WAIVERS if not (COMMON / w).is_file()]

    if stale:
        for w in stale:
            print(f"::error::Stale waiver in {Path(__file__).name}: "
                  f"'{w}' no longer exists — remove the WAIVERS entry.")
        return 1

    if unreachable:
        print("::error::Shared source files that NOTHING compiles:")
        for rel in unreachable:
            print(f"           firmware/common/{rel}")
        print()
        print("  Each of these will vanish from every firmware image, and any")
        print("  project that calls into it fails at LINK time with an")
        print("  undefined reference — not at compile time, because an")
        print("  -I../../common include path still resolves the headers.")
        print()
        print("  Fix it one of three ways, in this order of preference:")
        print("    1. PREFERRED — name the .cpp in the consuming env's")
        print("       build_src_filter (see canary-sense.ini, or the")
        print("       nightstand-s3 env in canary-display.ini). Deterministic:")
        print("       the file is named, so it is compiled.")
        print("    2. Add a library.json so the LDF discovers it (copy")
        print("       common/boot: includeDir '..', srcDir '.', srcFilter")
        print("       ['+<*.cpp>']). ONLY works if the header is included")
        print("       UNCONDITIONALLY — these projects run lib_ldf_mode deep+,")
        print("       which evaluates #ifdefs, so an include behind a flavor or")
        print("       feature guard is invisible to the LDF and the manifest")
        print("       changes nothing.")
        print("    3. Stage a flat copy into the Arduino sketch that uses it.")
        print()
        print("  Pick exactly ONE. Satisfying two compiles the TU twice and")
        print("  fails the link on duplicate symbols.")
        print()
        print("  If the module is genuinely not wired up yet, add a WAIVERS")
        print("  entry with the reason instead of leaving it silently dead.")
        return 1

    print(f"✓ common/ build reachability: {checked} shared .cpp files, "
          f"all compiled by a manifest, a build_src_filter, or a staged copy.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
