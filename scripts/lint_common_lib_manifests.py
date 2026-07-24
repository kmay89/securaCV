#!/usr/bin/env python3
"""Guard the PlatformIO manifests under firmware/common/.

Why this exists
---------------
A shared library in firmware/common/ is reached two different ways at once:

  * the compiler finds its *header* through `-I .../firmware/common`, so
    sources include it path-prefixed, e.g. `#include "color/look_engine.h"`;
  * PlatformIO's Library Dependency Finder decides whether to *compile* the
    library's .cpp files by matching those include strings against the
    library.

Those two can disagree silently. If library.json declares a `headers` list,
the LDF matches against that list *exclusively*. Declaring the bare filename
(`look_engine.h`) while every caller writes the prefixed form
(`color/look_engine.h`) means nothing ever matches, the library is never
built, and the failure does not appear until the linker reports an undefined
reference — long after the compile step everyone watches has gone green.

That is not hypothetical: it is exactly how `canary_color` shipped to main in
#1222 and broke the canary-display-nightstand-s3 link with

    undefined reference to `canary::color::wash_stops(...)'

while `boot_banner` — same directory layout, same prefixed include style, same
build node, but no `headers` key — linked fine the whole time.

The rule
--------
A common/ library whose `build.includeDir` is ".." is consumed through the
shared include root, so its callers write prefixed includes. Such a library
must not pin a `headers` list, because the bare names in it can never match.
Omit the key and let the LDF scan the library normally.

`library.properties` is deliberately not checked: its `includes=` field is an
Arduino IDE convenience hint and is inert for the LDF. boot_banner carries a
bare `includes=boot_banner.h` and still links correctly, which is the proof.
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
COMMON = ROOT / "firmware" / "common"


def main() -> int:
    problems = []
    checked = 0

    for manifest in sorted(COMMON.glob("*/library.json")):
        lib = manifest.parent.name
        try:
            data = json.loads(manifest.read_text())
        except json.JSONDecodeError as exc:
            problems.append(f"{manifest.relative_to(ROOT)}: invalid JSON — {exc}")
            continue

        checked += 1
        include_dir = (data.get("build") or {}).get("includeDir")
        headers = data.get("headers")

        if include_dir == ".." and headers:
            problems.append(
                f"{manifest.relative_to(ROOT)}: declares headers {headers} while "
                f'build.includeDir is "..".\n'
                f'    Callers reach this library as "{lib}/<header>", so those '
                f"bare names can never match and the LDF will silently skip\n"
                f"    compiling its .cpp files — an undefined reference at link "
                f"time, not a compile error. Drop the \"headers\" key."
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
