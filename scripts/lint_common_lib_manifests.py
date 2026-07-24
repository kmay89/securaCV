#!/usr/bin/env python3
"""Guard how firmware/common/ sources get compiled into PlatformIO builds.

The rule
--------
A library under firmware/common/ can be reached two ways, and only one of
them actually compiles its .cpp files:

  * **Bare include, standard layout** — `#include "securacv_ota.h"` resolves
    through the library's own include dir, PlatformIO's LDF matches it, and
    the library gets built. common/ota and common/csi work this way.

  * **Path-prefixed include** — `#include "color/look_engine.h"` resolves
    through `-I .../firmware/common`, i.e. through a plain include path rather
    than through the library. The LDF does not follow that, so the headers
    compile fine and the .cpp files are never built. The only symptom is an
    undefined reference at link time, long after the compile step everyone
    watches has gone green.

So: **anything included path-prefixed must be compiled explicitly**, by naming
its .cpp in a `build_src_filter`. That is what canary-sense.ini does for
boot_banner.cpp, canary-vision.ini for the identity signer, canary-sentinel.ini
for sentinel_fusion.cpp — and now canary-display.ini for the color engine.
The rule holds for every prefixed library in the tree with no exceptions.

Why it is written this way
--------------------------
This started as a manifest-shape check, and that framing was wrong twice.

canary_color shipped to main in #1222 with a library.json declaring bare
`headers` names, and canary-display-nightstand-s3 failed to link
`canary::color::wash_stops`. The first fix dropped the `headers` key so the
manifest exactly matched common/boot's, on the theory that boot proved the LDF
would then discover it. **That theory was wrong: the link failed again,
identically.** common/boot is not evidence for LDF discovery at all — its .cpp
is compiled explicitly by canary-sense.ini, and canary-wap keeps a
sketch-local copy.

The manifest was never the deciding factor. How the source includes the header
is. Hence this check ignores manifests entirely and asks the question that
actually predicts a link error.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
COMMON = ROOT / "firmware" / "common"
ENVS = ROOT / "firmware" / "envs" / "platformio"
PROJECTS = ROOT / "firmware" / "projects"
SRC_EXT = {".cpp", ".c", ".h", ".hpp", ".ino"}


def project_includes():
    """Every `#include "..."` spelling used by the PlatformIO project trees.

    tests_host is excluded on purpose: it is a plain host Makefile that
    compiles common/ sources directly, so its bare spellings say nothing about
    what the LDF resolves on-device — counting them would mask the bug.
    """
    seen = set()
    if not PROJECTS.exists():
        return seen
    for path in PROJECTS.rglob("*"):
        if path.suffix not in SRC_EXT or not path.is_file():
            continue
        if "_archive" in path.parts or "arduino" in path.parts:
            continue
        try:
            seen.update(re.findall(r'#\s*include\s+"([^"]+)"',
                                   path.read_text(errors="ignore")))
        except OSError:
            pass
    return seen


def env_blob():
    """All env .ini text with `;` comments stripped.

    The comments matter: canary-wap.ini contains the line "Do NOT also pull in
    ../../common/boot/boot_banner.cpp here", and matching that would count a
    warning *against* compiling a file as evidence that it is compiled.
    """
    if not ENVS.exists():
        return ""
    out = []
    for path in ENVS.glob("*.ini"):
        for line in path.read_text(errors="ignore").splitlines():
            code = line.split(";", 1)[0]
            if code.strip():
                out.append(code)
    return "\n".join(out)


def main() -> int:
    includes = project_includes()
    envs = env_blob()
    problems, checked = [], 0

    for lib in sorted(p for p in COMMON.iterdir() if p.is_dir()):
        name = lib.name
        sources = [p for p in lib.rglob("*.cpp")
                   if "test" not in p.name.lower()]
        if not sources:
            continue  # header-only: nothing to link, nothing to lose

        prefixed = sorted(i for i in includes if i.startswith(f"{name}/"))
        if not prefixed:
            continue  # bare include / standard layout — the LDF handles it

        checked += 1
        # Match on the path relative to firmware/common/, not name+basename —
        # common/sensors nests its drivers (sensors/bh1750/bh1750.cpp), and a
        # basename-only match would miss the build_src_filter entry that
        # actually compiles it.
        compiled = [p for p in sources
                    if f"common/{p.relative_to(COMMON).as_posix()}" in envs]
        if compiled:
            continue

        problems.append(
            f"common/{name}: included path-prefixed as {prefixed[0]!r}, which "
            f"resolves through -I firmware/common rather than through the\n"
            f"    library, so the LDF will not compile "
            f"{sorted(p.name for p in sources)}. That is an undefined "
            f"reference at link time,\n"
            f"    not a compile error. Name the .cpp in a build_src_filter, as "
            f"canary-sense.ini does for boot_banner.cpp."
        )

    if problems:
        print("firmware/common/ sources that will not link:\n", file=sys.stderr)
        for p in problems:
            print(f"  - {p}\n", file=sys.stderr)
        return 1

    print(f"✓ {checked} path-prefixed common/ libraries are explicitly compiled.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
