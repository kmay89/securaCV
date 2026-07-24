#!/usr/bin/env python3
"""Guard the PlatformIO manifests under firmware/common/.

Why this exists
---------------
A shared library in firmware/common/ is reached two different ways at once,
and the two can disagree in silence:

  * the **compiler** finds its header through `-I .../firmware/common`, so
    callers write the path-prefixed form, e.g. `#include "color/look_engine.h"`;
  * PlatformIO's **LDF** decides separately whether to *compile* the library's
    .cpp files, and when library.json declares a `headers` list it matches
    against that list *exclusively*.

Declaring the bare filename (`look_engine.h`) while every caller writes the
prefixed form means nothing ever matches, the LDF skips the library, and the
break does not surface at compile time — it surfaces as an undefined
reference at link time, long after the compile step everyone watches has gone
green.

That is not hypothetical. It is how `canary_color` shipped to main in #1222
and broke the canary-display-nightstand-s3 link with

    undefined reference to `canary::color::wash_stops(...)'

while `boot_banner` — same layout, same prefixed includes, same build node,
but no `headers` key — linked correctly the whole time.

What this checks
----------------
Manifest shape alone is the wrong test: `sentinel-fusion` declares bare
`headers` and *is* included prefixed, yet builds fine, because
canary-sentinel.ini names its .cpp in `build_src_filter` explicitly. That is
a legitimate second mechanism and must not be flagged.

So the real predicate is the conjunction — a library is broken when:

  1. it ships .cpp files that must be linked, and
  2. its declared `headers` cannot match how sources actually include it, and
  3. no env rescues it by naming those .cpp files in a `build_src_filter`.

Anything failing all three is a link error waiting to happen. Fix it by
dropping the `headers` key (letting the LDF scan normally, as boot_banner
does) or by adding an explicit build_src_filter entry.

`library.properties` is deliberately not consulted: its `includes=` field is
an Arduino IDE convenience hint, inert for the LDF. boot_banner carries a
bare `includes=boot_banner.h` and still links, which is the proof.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
COMMON = ROOT / "firmware" / "common"
ENVS = ROOT / "firmware" / "envs" / "platformio"
# Only the PlatformIO project trees. firmware/tests_host/ is a plain host
# Makefile build that compiles common/ sources directly, so its (bare) include
# spellings say nothing about what the LDF will resolve on-device — counting
# them would mask exactly the bug this lint exists to catch.
SEARCH = [ROOT / "firmware" / "projects"]
SRC_EXT = {".cpp", ".c", ".h", ".hpp", ".ino"}


def include_strings():
    """Every `#include "..."` spelling used across firmware sources."""
    seen = set()
    for base in SEARCH:
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.suffix not in SRC_EXT or not path.is_file():
                continue
            try:
                text = path.read_text(errors="ignore")
            except OSError:
                continue
            seen.update(re.findall(r'#\s*include\s+"([^"]+)"', text))
    return seen


def src_filter_blob():
    """All build_src_filter text, where explicit .cpp rescues are declared."""
    if not ENVS.exists():
        return ""
    return "\n".join(p.read_text(errors="ignore") for p in ENVS.glob("*.ini"))


def main() -> int:
    includes = include_strings()
    rescues = src_filter_blob()
    problems, checked = [], 0

    for manifest in sorted(COMMON.glob("*/library.json")):
        lib = manifest.parent.name
        try:
            data = json.loads(manifest.read_text())
        except json.JSONDecodeError as exc:
            problems.append(f"{manifest.relative_to(ROOT)}: invalid JSON — {exc}")
            continue

        checked += 1
        declared = data.get("headers")
        if not declared:
            continue  # LDF scans normally — the boot_banner pattern, always fine

        sources = [p for p in manifest.parent.rglob("*.cpp")
                   if not p.name.endswith(("_test.cpp", "test.cpp"))]
        if not sources:
            continue  # header-only: nothing to link, nothing to lose

        # The LDF needs only ONE declared header to match a real include to
        # decide the library is wanted; it then builds everything srcFilter
        # selects. So the question is not whether every spelling is declared,
        # it is whether *any* declared spelling is ever actually used.
        own = {p.name for p in manifest.parent.rglob("*.h")}
        used = {inc for inc in includes if inc.split("/")[-1] in own}
        if set(declared) & used:
            continue  # at least one match — the LDF pulls the library in
        unmatchable = sorted(used)
        if not unmatchable:
            continue  # nothing includes it at all; not this lint's business

        # Is it rescued by an explicit build_src_filter naming its .cpp?
        if any(f"common/{lib}/{p.name}" in rescues for p in sources):
            continue

        problems.append(
            f"{manifest.relative_to(ROOT)}: declares headers {declared}, but "
            f"sources include it as {unmatchable}.\n"
            f"    Those spellings can never match, so the LDF will silently skip "
            f"compiling {[p.name for p in sources]} —\n"
            f"    an undefined reference at link time, not a compile error, and "
            f"no env rescues it via build_src_filter.\n"
            f'    Fix: drop the "headers" key (as common/boot does), or name the '
            f".cpp in a build_src_filter."
        )

    if problems:
        print("common/ library manifest problems:\n", file=sys.stderr)
        for p in problems:
            print(f"  - {p}\n", file=sys.stderr)
        return 1

    print(f"✓ {checked} common/ library.json manifests are LDF-consistent.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
