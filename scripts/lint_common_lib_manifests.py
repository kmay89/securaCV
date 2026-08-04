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

...with ONE exception, which is the second half of this file. An env whose
effective config is `lib_ldf_mode = chain` **plus** `lib_extra_dirs` reaching
firmware/common already resolves those directories as ordinary LDF libraries.
Naming the sources there too compiles each translation unit twice, and the
link dies on `multiple definition of ...`. Both failures are link-time and
neither shows up while the files are compiling:

    too few sources  ->  undefined reference to canary::color::wash_stops
    too many sources ->  multiple definition of canary::color::gamma8

canary-display-nightstand7 shipped the second one to main by copying the
block from nightstand-s3, where it is genuinely required, into an env that
inherits both halves through `extends` and states neither. Hence
`duplicate_compile_problems()` below, and the fixtures in
scripts/tests/test_lint_common_lib_manifests.py — the tree only ever holds
one shape at a time, so the shapes that are NOT in it have to be written down.

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


def ini_sections(envs_dir):
    """{section name: {'file','text','extends'}} across every env .ini.

    Section names keep their `[env:...]` spelling; `extends` is normalized to
    the bare section name so a chain can be walked without caring whether a
    parent was written `env:canary-display-dash` or `canary_display_base`.
    """
    out = {}
    for path in sorted(pathlib.Path(envs_dir).glob("*.ini")):
        section = None
        for line in path.read_text(errors="ignore").splitlines():
            code = line.split(";", 1)[0]
            header = re.match(r"\s*\[([^\]]+)\]", code)
            if header:
                section = header.group(1)
                out.setdefault(section, {"file": path.name, "lines": []})
                continue
            if section:
                out[section]["lines"].append(code)
    for name, sec in out.items():
        sec["text"] = "\n".join(sec["lines"])
        m = re.search(r"^\s*extends\s*=\s*(\S+)", sec["text"], re.M)
        # Kept VERBATIM. `extends = env:canary-display-dash` names the section
        # `[env:canary-display-dash]`, so stripping the prefix here produced a
        # key that matches nothing and silently ended the walk — which made
        # this check blind to the very configuration it exists to catch (both
        # halves inherited). resolve() below handles either spelling instead.
        sec["extends"] = m.group(1) if m else None
    return out


def resolve(sections, name):
    """The section key for `name`, tolerating the env: prefix either way."""
    if name in sections:
        return name
    for alt in (f"env:{name}", name.split("env:")[-1]):
        if alt in sections:
            return alt
    return None


def option_value(sec, option):
    """The VALUE of `option` in this section, or None if it doesn't set it.

    Returns the assignment's own text plus any indented continuation lines,
    which is how PlatformIO spells multi-value options:

        lib_extra_dirs =
            ../../common

    Reading the value rather than the whole section matters twice over. These
    files carry long comments that name the settings they discuss, and — the
    bug that shipped in the first version of this check — a `build_src_filter`
    entry like `+<../../../common/color/color_engine.cpp>` literally contains
    the substring `../../common`, so "is `../../common` anywhere in this
    section?" answered yes for a section whose lib_extra_dirs pointed
    somewhere else entirely. That rejects a correct config and tells the
    author to delete sources they need.
    """
    lines = sec["lines"]
    for i, line in enumerate(lines):
        m = re.match(rf"\s*{re.escape(option)}\s*=(.*)$", line)
        if not m:
            continue
        value = [m.group(1)]
        for cont in lines[i + 1:]:
            if not cont.strip():
                break
            if re.match(r"\s*[A-Za-z_][A-Za-z0-9_.]*\s*=", cont):
                break          # the next option starts
            if not cont[:1].isspace():
                break          # unindented: no longer this value
            value.append(cont)
        return "\n".join(value)
    return None


def inherited_option(sections, name, option, _seen=None):
    """`option`'s effective value, resolved the way PlatformIO resolves it.

    The FIRST section in the `extends` chain that sets the option wins — a
    child setting it overrides the parent rather than merging, so the walk
    must stop at "is it set here?", never at "does it say what I expected?".
    Reading only the section's own lines sees a different build than the one
    that runs, which is how the duplicate got in: that env inherited both
    halves of the problem and stated neither.
    """
    _seen = _seen or set()
    key = resolve(sections, name) if name else None
    if not key or key in _seen:
        return None
    _seen.add(key)
    got = option_value(sections[key], option)
    if got is not None:
        return got
    return inherited_option(sections, sections[key]["extends"], option, _seen)


def duplicate_compile_problems(envs_dir):
    """Envs that compile a common/ library TWICE — a link-time duplicate.

    The sibling check above guards the UNDEFINED direction: a path-prefixed
    include whose .cpp nobody compiles. This guards the opposite, which is
    just as fatal and looks nothing alike:

        `lib_ldf_mode = chain` + `lib_extra_dirs` reaching firmware/common
        already resolves those directories as ordinary LDF libraries. An env
        in that state that ALSO names the .cpp in a build_src_filter compiles
        each translation unit twice, and the link dies on
        `multiple definition of ...`.

    Measured on the tree, the three shapes that link cleanly are:
        deep+ + lib_extra_dirs + explicit sources   (nightstand-s3, touch169, vision)
        chain + no lib_extra_dirs + explicit sources (nightstand-c6, sense, sentinel)
        chain + lib_extra_dirs + NO explicit sources (dash family, incl. nightstand7)
    Only chain + lib_extra_dirs + explicit is broken, so that exact triple is
    what this flags — narrow on purpose, because a false positive here blocks
    correct work and teaches people to route around the check.
    """
    sections = ini_sections(envs_dir)
    problems = []
    for name in sorted(sections):
        # build_src_filter inherits too: an env can pick up the explicit
        # sources from a parent while adding only `lib_ldf_mode` itself, and
        # reading just this section's lines would see none and skip it — a
        # false negative on the very combination being guarded.
        src_filter = inherited_option(sections, name, "build_src_filter") or ""
        srcs = sorted(set(re.findall(r"\+<[^>]*common/([A-Za-z0-9_/]+\.cpp)>",
                                     src_filter)))
        if not srcs:
            continue

        ldf = (inherited_option(sections, name, "lib_ldf_mode") or "").strip()
        extra_raw = inherited_option(sections, name, "lib_extra_dirs")
        # Only the option's OWN value, and only an entry that really is
        # firmware/common — `../../common`, not any path containing it.
        extra = bool(extra_raw) and any(
            re.search(r"(^|/)common/?$", entry.strip())
            for entry in (extra_raw or "").splitlines() if entry.strip()
        )
        if ldf == "chain" and extra:
            problems.append(
                f"{sections[name]['file']}:[{name}]: compiles "
                f"{srcs} explicitly, but its effective config is "
                f"`lib_ldf_mode = chain` + `lib_extra_dirs = ../../common`,\n"
                f"    which ALREADY builds firmware/common/* as LDF libraries. "
                f"Each of those translation units would be compiled twice and "
                f"the link fails with\n"
                f"    'multiple definition of ...'. Drop the build_src_filter "
                f"entries — this env inherits the sources from the LDF. (Both "
                f"halves are usually INHERITED via\n"
                f"    `extends`, so read the parent chain, not just this "
                f"section.)"
            )
    return problems


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


def env_src_filters():
    """{section name: set of common/<path>.cpp it compiles}, comments stripped.

    Per-section, not one blob: whether a translation unit is compiled is a
    property of the individual env, and the closure check below has to reason
    about one env's set at a time.
    """
    out = {}
    if not ENVS.exists():
        return out
    for path in ENVS.glob("*.ini"):
        section = None
        for line in path.read_text(errors="ignore").splitlines():
            code = line.split(";", 1)[0]
            header = re.match(r"\s*\[([^\]]+)\]", code)
            if header:
                section = f"{path.name}:{header.group(1)}"
                continue
            for m in re.finditer(r"\+<[^>]*common/([A-Za-z0-9_/]+\.cpp)>", code):
                if section:
                    out.setdefault(section, set()).add(m.group(1))
    return out


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

    # Naming one .cpp of a library is not enough. build_src_filter compiles
    # exactly the TUs it lists, so an env that compiles look_engine.cpp but
    # not color_engine.cpp still fails to link — and the check above, which
    # only asks whether *any* source is named, would call that green.
    #
    # Requiring every source is too strong: common/sensors is a set of
    # independent drivers, and canary-sentinel legitimately compiles three of
    # the four (it has no use for mr60_vitals.cpp). So the rule is closure —
    # if a compiled source includes a sibling header that has its own .cpp,
    # that .cpp has to be compiled by the same env too.
    for section, compiled in sorted(env_src_filters().items()):
        for rel in sorted(compiled):
            src = COMMON / rel
            if not src.is_file():
                continue
            try:
                text = src.read_text(errors="ignore")
            except OSError:
                continue
            lib = COMMON / rel.split("/")[0]
            # Walk includes transitively through the library's own headers:
            # look_engine.cpp includes only look_engine.h, and the dependency
            # on color_engine.cpp arrives through look_engine.h including
            # color_engine.h. A direct-includes-only walk misses it entirely.
            seen, queue, reached = set(), [text], {}
            while queue:
                for inc in re.findall(r'#\s*include\s+"([^"]+)"', queue.pop()):
                    stem = pathlib.Path(inc).stem
                    if stem in seen:
                        continue
                    seen.add(stem)
                    reached[stem] = inc
                    for hdr in lib.rglob(f"{stem}.h"):
                        try:
                            queue.append(hdr.read_text(errors="ignore"))
                        except OSError:
                            pass
            for stem, inc in sorted(reached.items()):
                for sibling in lib.rglob(f"{stem}.cpp"):
                    if "test" in sibling.name.lower():
                        continue
                    need = sibling.relative_to(COMMON).as_posix()
                    if need != rel and need not in compiled:
                        problems.append(
                            f"{section}: compiles {rel}, which pulls in "
                            f"{inc!r}, but does not compile {need}.\n"
                            f"    build_src_filter compiles only what it "
                            f"names, so this links against symbols from a "
                            f"translation unit\n    that is never built — "
                            f"undefined reference at link time. Add it "
                            f"alongside {rel}."
                        )

    # The other direction: compiled twice rather than not at all.
    problems.extend(duplicate_compile_problems(ENVS))

    if problems:
        print("firmware/common/ sources that will not link:\n", file=sys.stderr)
        for p in problems:
            print(f"  - {p}\n", file=sys.stderr)
        return 1

    print(f"✓ {checked} path-prefixed common/ libraries are explicitly compiled.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
