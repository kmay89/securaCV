#!/usr/bin/env python3
"""Pin firmware/build_matrix.json to the real feature flags so it can't rot.

build_matrix.json is the single source of truth the securacv_website /checkup
firmware-type selector mirrors: "which build includes which feature". This lint
proves the load-bearing cells against the ONLY files that actually decide them —
firmware/canary/platformio.ini (per-env -DFEATURE_* lines) and
firmware/canary/include/canary_config.h (the #ifndef defaults) — plus internal
consistency against flavors.json + boards.json (every flavors.json product
must have a lane here, by id or by `flavor`), and against the device manifests
(every product lane resolves to ONE devices/<slug>/device.json, and every
manifest's board.envs is a real [env:NAME] of its family's PlatformIO project
— the resolution and the ini reader are shared with lint_device_manifests.py
through scripts/_device_join.py, so the two lints cannot disagree). Each
product's feature claims are held to its own tree (section 2a-bis): the flag
the tree spells the feature with (featureCatalog's, or FLAG_ALIASES') must be
#define'd there, and a reading must be under it (thermal: a die-temperature
read), read by the sweep-C35 suite's tree reader. A claim no flag is mapped
for (UNMAPPED) names the code it rests on instead (EVIDENCE, section
2a-ter), which must stay in its tree as unconditional code, as far as the
source itself shows (macros are not expanded). Every count of the flagship
self-test's probes a note states is held to the suite itself
(securacv_diagnostics.cpp's TESTS[]: a lane's note reaches the website's
/checkup page). It is a cheap grep-style check (no PlatformIO), run in
.github/workflows/lint.yml.

Exit non-zero on any drift, printing every problem.
"""
import bisect
import functools
import importlib.util
import json
import re
import sys
from pathlib import Path
from typing import NamedTuple

from _device_join import (
    load_manifests,
    load_project_ini,
    manifest_for_matrix_product,
    unknown_envs,
)

ROOT = Path(__file__).resolve().parent.parent

# ── each tree's spelling of a feature (section 2a-bis) ───────────────────────
# featureCatalog's `flag` is the canary tree's spelling. A tree that spells a
# feature its own way says so here, keyed (tree, feature), the tree being the
# flavors.json product whose sources a lane builds (its `flavor`, else its id).
FLAG_ALIASES = {
    # The canary-wap reads die temperature in sys_monitor.h, under
    # FEATURE_SYS_MONITOR (1 in the FULL and DEV profiles build_config.h
    # defines; canary-wap-default names FULL).
    ("canary-wap", "thermal"): "FEATURE_SYS_MONITOR",
}

# A feature that is a reading: its flag must also guard the read itself, in
# the tree, as the sweep-C35 suite holds boards.json's thermal notes.
READINGS = {"thermal": "a die-temperature read"}

# The claims whose tree spells the feature with no flag this lint has mapped:
# the catalog's spelling is #define'd nowhere in that tree, and no alias says
# what it is instead, so these rest on the negative direction alone (no env
# that builds the lane sets the catalog's flag to 0). Each entry must still be
# claimed by a lane and still be unspelled in its tree, or the lint says to
# drop it; mapping one is an alias above, or dropping the claim.
UNMAPPED = {
    ("canary-wap", "selftest"),
    ("canary-wap", "csi"),
    ("canary-wap", "gps"),
    ("canary-vision", "selftest"),
    ("canary-sense", "selftest"),
    ("canary-display", "selftest"),
}

# The code each UNMAPPED claim rests on today (sweep C37): a source of that
# tree (test_board_thermal_notes.tree_files) and a needle that must stand in
# it as code, under no #if / #ifdef / #ifndef (needle_sites says exactly what
# counts, and what it does not model). It names the code; it maps nothing, so
# the claim stays in UNMAPPED and the copy question stays open. When the code
# goes, or goes conditional, the lint says what may be done about the claim
# instead (lost_code). The table and UNMAPPED must hold the same keys, so
# neither can rot.
EVIDENCE = {
    # The CSI library and its HTTP endpoints (csi_integration::init in setup()).
    ("canary-wap", "csi"): ("firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino",
                            '#include "csi_integration.h"'),
    # The wizard's on-demand self-test, GET /api/selftest — not a boot one.
    ("canary-wap", "selftest"): ("firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino",
                                 '#include "selftest_api.h"'),
    # The GNSS UART setup() opens for an optional module (PHASE 4).
    ("canary-wap", "gps"): ("firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino",
                            "Serial1.begin(GPS_BAUD"),
    # The OTA engine's boot self-test: one required wifi probe, then confirm
    # or roll back the image (ota_boot_validate).
    ("canary-vision", "selftest"): ("firmware/projects/canary-vision/src/net/ota_mgr.cpp",
                                    "securacv_ota_boot_self_test();"),
    ("canary-sense", "selftest"): ("firmware/projects/canary-sense/src/net/ota_mgr.cpp",
                                   "securacv_ota_boot_self_test();"),
    # The canary-display-watch lane builds src/ (src_dir = src).
    ("canary-display", "selftest"): ("firmware/projects/canary-display/src/net/ota_mgr.cpp",
                                     "securacv_ota_boot_self_test();"),
}

# The feature every build must list (check_features): its claim can be held
# to code or mapped to a flag, never dropped.
ALWAYS_LISTED = "selftest"


def lost_code(feat: str) -> str:
    """The words a row's failure ends with: what may be done about the claim
    whose code went, and the sweep item that asks it. A claim may be dropped
    or mapped (a FLAG_ALIASES spelling, or a flag the tree grows), except the
    ALWAYS_LISTED one, which every build must keep."""
    if feat == ALWAYS_LISTED:
        return (f"the claim lost the code it rests on: restore the code, or map the claim "
                f"(a flag the tree #defines, in FLAG_ALIASES); a '{feat}' claim cannot be "
                f"dropped, every build must list it (sweep C37)")
    return "the claim lost the code it rests on: drop it or map it (sweep C37)"

_TREE_SUITE = ROOT / "scripts" / "tests" / "test_board_thermal_notes.py"


@functools.lru_cache(maxsize=None)
def tree_suite():
    """scripts/tests/test_board_thermal_notes.py (sweep C35), for its reader of
    a product's tree: tree_facts(product) is (the FEATURE_* flags #define'd
    in the tree, the flags whose `#if` encloses a die-temperature read, whether
    the tree reads die temperature at all), and env_flags(product, env) the
    FEATURE_* -D values an env compiles with, through `extends`, `${...}` and
    build_unflags. One definition of a tree, of a die-temperature read and of
    an env's flags, so this lint and that suite cannot disagree about them."""
    spec = importlib.util.spec_from_file_location("_board_thermal_tree", _TREE_SUITE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)  # type: ignore[union-attr]
    return module


def lanes(prod: dict):
    """(where, env, features) for each buildable lane of a matrix product: its
    levels, or its one build."""
    if prod.get("hasLevels"):
        for lvl, b in (prod.get("levels") or {}).items():
            yield f"product {prod['id']}.{lvl}", b.get("env"), b.get("features", [])
    else:
        b = prod.get("build") or {}
        yield f"product {prod['id']}", b.get("env"), b.get("features", [])


def feature_flag_errors(matrix: dict, tree_facts, env_flags, aliases=None,
                        unmapped=None, trees=None) -> list[str]:
    """Each lane's feature claims, held to the tree that lane builds.

    For every feature a lane lists, the flag is the one its tree spells it
    with (`aliases`, else featureCatalog's). Positive direction: the flag is
    #define'd in the tree's sources (`tree_facts`), and a reading (READINGS)
    has an `#if` on it enclosing the read. Negative direction: no env that
    builds the lane sets it to 0 (`env_flags`, `extends` resolved), and the
    lane's env exists in its tree's PlatformIO config. A claim in `unmapped`
    is held to the negative direction only, with the catalog's flag; the
    table must stay true (claimed by a lane, unspelled in its tree), and an
    alias no lane uses is refused, so neither table rots. `trees` is the
    trees there are (flavors.json's products; the suite's flavor_dirs by
    default): a lane whose tree is not one is named, its claims unchecked,
    where the first version looked it up and crashed."""
    aliases = FLAG_ALIASES if aliases is None else aliases
    unmapped = UNMAPPED if unmapped is None else unmapped
    trees = frozenset(tree_suite().flavor_dirs() if trees is None else trees)
    catalog = matrix.get("featureCatalog", {})
    errors: list[str] = []
    claimed: set[tuple[str, str]] = set()
    for prod in matrix.get("products", []):
        # The lane's tree as the suite's own board_envs reads it: its
        # `flavor`, else (absent or null) its id.
        tree = prod.get("flavor") or prod["id"]
        if tree not in trees:
            errors.append(f"product {prod['id']}: builds the '{tree}' tree, which flavors.json "
                          f"does not list — its feature claims cannot be held to any tree")
            continue
        for where, env, feats in lanes(prod):
            flags = env_flags(tree, env) if env else None
            if env and flags is None:
                errors.append(f"{where}: env '{env}' is not an [env:{env}] of the {tree} tree's "
                              f"PlatformIO config (its platformio.ini and extra_configs)")
            for feat in feats:
                if feat not in catalog:
                    continue  # check_features names it
                claimed.add((tree, feat))
                flag = aliases.get((tree, feat), catalog[feat].get("flag"))
                if not flag:
                    continue
                if (flags or {}).get(flag) == "0":
                    errors.append(f"{where}: lists '{feat}', but [env:{env}] builds the {tree} tree "
                                  f"with -D{flag}=0 — the build does not have it")
                if (tree, feat) in unmapped:
                    continue
                defined, gauges, _reads = tree_facts(tree)
                if flag not in defined:
                    errors.append(f"{where}: lists '{feat}' as {flag}, but no source in the {tree} "
                                  f"tree #defines {flag} — the tree spells it some other way (map "
                                  f"it in FLAG_ALIASES) or does not have it (drop the claim)")
                elif feat in READINGS and flag not in gauges:
                    errors.append(f"{where}: lists '{feat}' as {flag}, but no #if {flag} in the "
                                  f"{tree} tree encloses {READINGS[feat]}")
    for tree, feat in sorted(unmapped):
        flag = catalog.get(feat, {}).get("flag")
        if (tree, feat) not in claimed:
            errors.append(f"UNMAPPED lists ({tree!r}, {feat!r}), which no lane claims — drop it")
        elif (tree, feat) in aliases:
            errors.append(f"({tree!r}, {feat!r}) is both in UNMAPPED and in FLAG_ALIASES — "
                          f"drop it from UNMAPPED")
        elif flag and flag in tree_facts(tree)[0]:
            errors.append(f"UNMAPPED lists ({tree!r}, {feat!r}), but the {tree} tree #defines "
                          f"{flag} — the claim can be held to it; drop it from UNMAPPED")
    for tree, feat in sorted(set(aliases) - claimed):
        errors.append(f"FLAG_ALIASES maps ({tree!r}, {feat!r}), which no lane claims — drop it")
    return errors


# ── the code an UNMAPPED claim rests on (sweep C37) ──────────────────────────

_LEX_RE = re.compile(r"//|/\*|[\"']")
_WORD_BEFORE_RE = re.compile(r"\w{1,16}\Z")
_LITERAL_PREFIXES = ("u8", "u", "U", "L")
_RAW_PREFIXES = ("R", "u8R", "uR", "UR", "LR")
# Translation phase 2: a backslash that ends a line joins the next line to it.
# GCC and Clang take spaces or tabs between the backslash and the newline as
# well (with a warning), and so does this.
_SPLICE_RE = re.compile(r"\\[ \t\f\v]*\r?\n")
# A directive's `#` (or its digraph `%:`) and keyword, from the start of a
# logical line: whitespace, and comments blanked to it, may come first.
_DIRECTIVE_RE = re.compile(r"\s*(#|%:)\s*(\w*)")
# What each offset of lex_c's text is.
CODE, LITERAL, COMMENT = 0, 1, 2
# A translation unit: what a build compiles on its own. A header is compiled
# only where something includes it, so it cannot be a claim's evidence.
_UNIT_SUFFIXES = (".ino", ".c", ".cc", ".cpp")


class Lexed(NamedTuple):
    """lex_c's reading of a source: `text` is the source after translation
    phase 2 (every line splice taken out) with every comment blanked to
    spaces (a block comment's newlines kept), and `kind[i]` says what offset
    i of `text` is (CODE, LITERAL or COMMENT). `src` is the source as read,
    and `joins` / `taken` map an offset of `text` back to it: the k-th
    splice came out at text offset joins[k], and taken[k] characters had
    come out by then."""
    text: str
    kind: bytearray
    src: str
    joins: list
    taken: list

    def src_offset(self, at: int) -> int:
        """The offset in `src` of offset `at` of `text`."""
        k = bisect.bisect_right(self.joins, at)
        return at + (self.taken[k - 1] if k else 0)

    def text_offset(self, at: int) -> int:
        """The offset in `text` of offset `at` of `src`, which is not inside
        a splice (lex_c maps only the end of a raw string, just past its
        closing quote)."""
        lo, hi = 0, len(self.joins)
        while lo < hi:  # the splices that end at or before `at` in src
            mid = (lo + hi) // 2
            if self.joins[mid] + self.taken[mid] <= at:
                lo = mid + 1
            else:
                hi = mid
        return at - (self.taken[lo - 1] if lo else 0)


@functools.lru_cache(maxsize=8)  # three rows read the one 685 KB sketch
def lex_c(src: str) -> Lexed:
    """C/C++ source read through translation phases 1 to 3, as far as a
    needle needs: a byte-order mark is not a character of the source, every
    line splice comes out first (so a `//` comment whose line ends in a
    backslash takes the next line with it, and a string can run on across
    one), and then comments and literals are told apart. A raw string
    (R"d(...)d", with the u8/u/U/L prefixes) runs to its own close in the
    source as read, since the standard undoes a splice inside one; an
    ordinary literal stops before an unescaped newline, so one stray quote
    (an apostrophe in an #if 0 block) cannot swallow the rest of the file. A
    quote after a digit is a C++14 digit separator, not a literal. A
    literal's prefix is part of it. Trigraphs, gone from C++17 and off in
    GCC's gnu modes, are not read."""
    if src.startswith("\ufeff"):
        src = " " + src[1:]  # the mark is not a token; offsets hold
    parts: list[str] = []
    joins: list[int] = []
    taken: list[int] = []
    pos = gone = 0
    for m in _SPLICE_RE.finditer(src):
        parts.append(src[pos:m.start()])
        joins.append(m.start() - gone)
        gone += m.end() - m.start()
        taken.append(gone)
        pos = m.end()
    parts.append(src[pos:])
    text = "".join(parts)
    lexed = Lexed(text, bytearray(len(text)), src, joins, taken)
    out = list(text)
    kind = lexed.kind
    n = len(text)
    i = 0
    while True:
        m = _LEX_RE.search(text, i)
        if m is None:
            break
        i, tok = m.start(), m.group()
        if tok == "//":
            # No newline is left inside a line comment: phase 2 took them out.
            j = text.find("\n", i)
            j = n if j < 0 else j
            out[i:j] = " " * (j - i)
            kind[i:j] = bytes([COMMENT]) * (j - i)
            i = j
            continue
        if tok == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out[i:j] = ["\n" if ch == "\n" else " " for ch in text[i:j]]
            kind[i:j] = bytes([COMMENT]) * (j - i)
            i = j
            continue
        before = _WORD_BEFORE_RE.search(text, max(0, i - 16), i)
        word = before.group() if before else ""
        if tok == "'" and word[:1].isdigit():
            i += 1  # 1'000'000: a digit separator
            continue
        start = i - len(word) if word in _LITERAL_PREFIXES + _RAW_PREFIXES else i
        k = text.find("(", i + 1, i + 18) if tok == '"' and word in _RAW_PREFIXES else -1
        if k >= 0 and not re.search(r"[\s\\)\"]", text[i + 1:k]):
            close = ")" + text[i + 1:k] + '"'
            at = src.find(close, lexed.src_offset(k) + 1)
            j = n if at < 0 else lexed.text_offset(at + len(close))
        else:
            j = i + 1
            while j < n and text[j] != tok and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            if j < n and text[j] == tok:
                j += 1
            j = min(j, n)
        kind[start:j] = bytes([LITERAL]) * (j - start)
        i = j
    return lexed._replace(text="".join(out))


def needle_sites(src: str, needle: str) -> tuple[list[int], list[str]]:
    """Where `needle` stands in `src` as compiled code: (the 1-based lines of
    `src` it does, why each other occurrence does not count).

    The source is read as lex_c reads it, splices out, and the needle counts
    only where it starts in code (not in a comment, nor in a string or
    character literal, its prefix included), whole at both ends (`foo()` is
    not in `my_foo()`), on a logical line no #if / #ifdef / #ifndef / #elif /
    #else encloses (an #if 0 block, an #if 1 block and an include guard all
    count as conditional: the needle belongs at a call site in a .ino /
    .cpp, and a false red is the safe way to be wrong), and not inside a
    directive unless it IS that directive (`#include "x.h"` counts; the same
    text in a #define body compiles only where the macro is used, so it does
    not). A logical line ends at a newline in code: one inside a block
    comment or a raw string does not end it (phase 3 makes the comment one
    space), so the rest of the line a comment closes on still belongs to the
    directive it opened in.

    It holds the needle to the ways code stops compiling that show in the
    source itself. It does not expand macros (a #define that turns the
    needle's name into nothing would fool it), nor know which files a build
    compiles beyond the tree reader's list, nor whether anything calls the
    code at run time."""
    lexed = lex_c(src)
    text, kind = lexed.text, lexed.kind
    line_starts = [0] + [m.end() for m in re.finditer("\n", lexed.src)]

    def line_of(at: int) -> int:
        return bisect.bisect_right(line_starts, lexed.src_offset(at))

    ends = [m.start() for m in re.finditer("\n", text) if kind[m.start()] == CODE]
    starts = [0] + [e + 1 for e in ends]
    ends.append(len(text))
    # Each directive (by its logical line), and the conditionals open after
    # it: (the `#if` as written, its line, the branch now in force, its line).
    frames: list[tuple[str, int, str, int]] = []
    directive: dict[int, tuple[int, int, str]] = {}
    after: list[int] = []
    open_after: list[tuple[tuple[str, int, str, int], ...]] = []
    for idx, (ls, le) in enumerate(zip(starts, ends)):
        m = _DIRECTIVE_RE.match(text, ls, le)
        if m is None:
            continue
        no, kw = line_of(m.start(1)), m.group(2)
        directive[idx] = (m.start(1), no, "".join(text[m.start(1):m.end()].split()))
        said = " ".join(text[m.start(1):le].split())
        if kw in ("if", "ifdef", "ifndef"):
            frames.append((said, no, said, no))
        elif kw in ("elif", "elifdef", "elifndef", "else") and frames:
            frames[-1] = (frames[-1][0], frames[-1][1], said, no)
        elif kw == "endif" and frames:
            frames.pop()
        after.append(idx)
        open_after.append(tuple(frames))
    head = r"(?<!\w)" if re.match(r"\w", needle) else ""
    tail = r"(?!\w)" if re.search(r"\w$", needle) else ""
    good: list[int] = []
    why: list[str] = []
    for m in re.finditer(head + re.escape(needle) + tail, text):
        at = m.start()
        if kind[at] != CODE:
            continue  # in a literal (a comment is blanked): not code at all
        idx = bisect.bisect_right(starts, at) - 1
        no = line_of(at)
        e = bisect.bisect_left(after, idx) - 1
        ctx = open_after[e] if e >= 0 else ()
        d = directive.get(idx)
        if d and d[0] != at:
            body = (" (a macro body compiles only where the macro is used)"
                    if d[2].endswith("define") else "")
            why.append(f"line {no} is inside the {d[2]} on line {d[1]}, not code{body}")
        elif ctx:
            chain = ", inside ".join(
                f"`{branch}` (line {bno}) of `{opened}` (line {ono})" if bno != ono
                else f"`{opened}` (line {ono})"
                for opened, ono, branch, bno in reversed(ctx))
            why.append(f"line {no} is under {chain}")
        else:
            good.append(no)
    return good, why


@functools.lru_cache(maxsize=None)
def _read_source(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


@functools.lru_cache(maxsize=None)
def _tree_paths(tree: str) -> frozenset:
    """The sweep-C35 suite's files of `tree`, resolved (read once a run)."""
    return frozenset(Path(p).resolve() for p in tree_suite().tree_files(tree))


def evidence_errors(evidence=None, unmapped=None, tree_files=None, read=None,
                    trees=None) -> list[str]:
    """Each UNMAPPED claim still has the code EVIDENCE says it rests on.

    The two tables hold the same keys (a claim with no row, or a row for a
    claim UNMAPPED does not list, is named). For each row, the source path is
    a translation unit (a .ino / .c / .cc / .cpp: a header compiles only
    where it is included) among its tree's files (`tree_files`, the
    sweep-C35 suite's reader by default), and the needle stands in it as
    compiled code (needle_sites). `read` reads a source (the file on disk by
    default) and `trees` is the trees there are (the suite's flavor_dirs by
    default): seams the tests inject sources through."""
    evidence = EVIDENCE if evidence is None else evidence
    unmapped = UNMAPPED if unmapped is None else unmapped
    read = _read_source if read is None else read
    trees = frozenset(tree_suite().flavor_dirs() if trees is None else trees)
    errors: list[str] = []
    files_of: dict[str, frozenset | set[Path]] = {}
    for tree, feat in sorted(set(unmapped) - set(evidence)):
        errors.append(f"UNMAPPED lists ({tree!r}, {feat!r}), but EVIDENCE names no code it rests "
                      f"on — add its (source path, needle) row, or map the claim (sweep C37)")
    for tree, feat in sorted(set(evidence) - set(unmapped)):
        errors.append(f"EVIDENCE has a row for ({tree!r}, {feat!r}), which UNMAPPED does not "
                      f"list — drop the row (sweep C37)")
    for (tree, feat), (rel, needle) in sorted(evidence.items()):
        if (tree, feat) not in unmapped:
            continue
        where = f"EVIDENCE ({tree!r}, {feat!r})"
        if tree not in trees:
            errors.append(f"{where}: '{tree}' is not a flavors.json product, so it has no tree "
                          f"to hold {rel} to — {lost_code(feat)}")
            continue
        path = (ROOT / rel).resolve()
        if path.suffix not in _UNIT_SUFFIXES:
            units = ", ".join(_UNIT_SUFFIXES)
            errors.append(f"{where}: {rel} is not a translation unit ({units}): a header "
                          f"compiles only where it is included, so name the source that "
                          f"holds the code — {lost_code(feat)}")
            continue
        if tree not in files_of:
            files_of[tree] = (_tree_paths(tree) if tree_files is None else
                              {Path(p).resolve() for p in tree_files(tree)})
        if path not in files_of[tree]:
            errors.append(f"{where}: {rel} is not a source of the {tree} tree (its flavor dir, "
                          f"the firmware/common files it includes, its build_src_filter) — "
                          f"{lost_code(feat)}")
            continue
        good, why = needle_sites(read(path), needle)
        if good:
            continue
        if why:
            more = f"; and {len(why) - 3} more" if len(why) > 3 else ""
            errors.append(f"{where}: {rel} has {needle!r} only where it is not unconditional "
                          f"code ({'; '.join(why[:3])}{more}) — {lost_code(feat)}")
        else:
            errors.append(f"{where}: {rel} has no {needle!r} outside comments and string "
                          f"literals — {lost_code(feat)}")
    return errors


def env_sections(ini_text):
    """Split a platformio.ini into {section_name: body_text}. The base table is
    keyed 'env'; environments are keyed by their full '[env:NAME]' minus brackets."""
    sections = {}
    current = None
    buf = []
    for line in ini_text.splitlines():
        m = re.match(r"^\[([^\]]+)\]\s*$", line)
        if m:
            if current is not None:
                sections[current] = "\n".join(buf)
            current = m.group(1)
            buf = []
        elif current is not None:
            buf.append(line)
    if current is not None:
        sections[current] = "\n".join(buf)
    return sections


def flag_set(body, flag):
    """Return the int value FLAG is set to in this section body, or None if the
    section doesn't set it. Matches `-DFEATURE_X=1` (ignoring trailing comments)."""
    m = re.search(rf"-D{re.escape(flag)}=(\d+)", body)
    return int(m.group(1)) if m else None


def device_join_errors(matrix: dict, flavors: list, manifests: list, root: Path) -> list[str]:
    """The build matrix's side of the device join (devices/README.md, wave 2).

    Two rules, both cheap and both fatal:
      • every product lane resolves to exactly one manifest — devices/<id>/,
        else the single manifest of its flavor claiming every env it builds;
        a lane no manifest owns is a product the flasher catalog, the
        figures and the board registry have no way to agree about;
      • every manifest's board.envs is an [env:NAME] of its family's
        PlatformIO project (platformio.ini with extra_configs resolved) — a
        manifest naming an env CI cannot build is a join to nothing.
    The manifest linter proves the rest (board, chip, figure, flasher, CAD).
    """
    errors: list[str] = []
    for prod in matrix.get("products", []):
        _target, why = manifest_for_matrix_product(prod, manifests)
        if why:
            errors.append(why)
    flavor_dir = {f["name"]: f.get("dir") for f in flavors}
    ini_cache: dict[str, dict] = {}
    for m in manifests:
        d = flavor_dir.get(m["family"])
        if not d:
            errors.append(f"devices/{m['slug']}: family '{m['family']}' is not a flavors.json "
                          f"product — its envs cannot be checked against any platformio.ini")
            continue
        if d not in ini_cache:
            ini_cache[d] = load_project_ini(root / d)
        for env in unknown_envs(m, ini_cache[d]):
            errors.append(f"devices/{m['slug']}: board.envs names '{env}', which is not an "
                          f"[env:{env}] in {d}/platformio.ini (extra_configs included)")
    return errors


# The flagship's boot self-test (FEATURE_DIAGNOSTICS, the catalog's
# `selftest` flag): its table lists every row, and a row with no probe
# function (nullptr) is listed and never run, so it is not a probe.
DIAG_CPP = "firmware/canary/lib/securacv_diagnostics/src/securacv_diagnostics.cpp"
_COUNT_WORDS = {w: n for n, w in enumerate(
    ("zero one two three four five six seven eight nine ten eleven twelve "
     "thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty").split())}
# "10-probe", "nine-probe", "10 probes", "ten probes": a count of probes.
_PROBE_COUNT_RE = re.compile(
    r"\b(\d+|" + "|".join(_COUNT_WORDS) + r")(?:-probe\b|\s+probes\b)", re.I)


def flagship_probe_count(src: str):
    """The probes in the flagship self-test's TESTS[] table (rows that name a
    probe function), or None when the table cannot be read."""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    m = re.search(r"\bTESTS\s*\[\s*SELFTEST_COUNT\s*\]\s*=\s*\{(.*?)\n\s*\};", src, re.S)
    if not m:
        return None
    fns = re.findall(r'\{\s*"[^"]+"\s*,\s*([A-Za-z_]\w*)\s*\}', m.group(1))
    if not fns:
        return None
    return sum(1 for fn in fns if fn not in ("nullptr", "NULL"))


def _strings(node, where="build_matrix.json"):
    """(where, text) for every string in the matrix, a product named by id."""
    if isinstance(node, str):
        yield where, node
    elif isinstance(node, dict):
        for k, v in node.items():
            yield from _strings(v, f"{where}.{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            tag = v.get("id") if isinstance(v, dict) and isinstance(v.get("id"), str) else i
            yield from _strings(v, f"{where}[{tag}]")


def probe_count_errors(matrix: dict, probes: int) -> list[str]:
    """Every count of probes a matrix string states is the flagship
    self-test's. The selftest feature is FEATURE_DIAGNOSTICS, the canary
    tree's suite, and scripts/carry_to_site.py copies each lane's `note` to
    the website's /checkup page: the display lane's note said "the canary
    tree's 10-probe suite" after the suite became nine probes."""
    errors: list[str] = []
    for where, text in _strings(matrix):
        for m in _PROBE_COUNT_RE.finditer(text):
            word = m.group(1).lower()
            n = int(word) if word.isdigit() else _COUNT_WORDS[word]
            if n != probes:
                errors.append(
                    f'{where}: says "{m.group(0)}", but the flagship self-test has '
                    f"{probes} probes (the rows of TESTS[] in {DIAG_CPP} that name a "
                    f"probe function) — say {probes}, or name the suite without a count")
    return errors


def lane_coverage_errors(matrix: dict, flavors: list) -> list[str]:
    """Every product flavors.json ships has a lane; no unreleased one does.

    A product flavors.json marks `unreleased` (compiled by CI, shipped by
    nothing — canary-sentinel while its bench checklist is open) is the one
    exception to "every product has a lane", and it cuts both ways: it needs
    no lane, and it may not HAVE one, because a lane is a Flash button on
    /checkup for a binary no release publishes. flavor_envs.py validates the
    field itself (a non-empty reason, never beside release_envs).
    """
    errors: list[str] = []
    flavor_names = {f["name"] for f in flavors}
    covered = {p.get("flavor", p["id"]) for p in matrix.get("products", [])}
    unreleased = {f["name"] for f in flavors if "unreleased" in f}
    for name in sorted(flavor_names - covered - unreleased):
        errors.append(
            f"flavors.json product '{name}' has no lane in build_matrix.json — "
            f"add a product with id '{name}', or a board-specialized product "
            f"whose `flavor` is '{name}' (the matrix must cover every product "
            f"that ships)")
    for name in sorted(covered & unreleased):
        errors.append(
            f"flavors.json marks '{name}' `unreleased`, but build_matrix.json "
            f"gives it a lane — a lane offers a flashable build no release "
            f"publishes; drop the lane, or drop `unreleased` when it ships")
    return errors


def collect(root: Path = ROOT) -> list[str]:
    """Every problem in the tree under `root`, as one line each; [] is green."""
    errors: list[str] = []
    fw = root / "firmware"

    def err(msg):
        errors.append(msg)

    def load_json(path):
        try:
            return json.loads(path.read_text())
        except Exception as e:  # noqa: BLE001
            err(f"could not read/parse {path.relative_to(root)}: {e}")
            return None

    matrix = load_json(fw / "build_matrix.json")
    flavors = load_json(fw / "flavors.json")
    boards = load_json(fw / "boards" / "boards.json")
    flash_catalog = load_json(root / "canary-local" / "devices" / "flash.json")
    ini_path = fw / "canary" / "platformio.ini"
    cfg_path = fw / "canary" / "include" / "canary_config.h"
    if matrix is None or flavors is None or boards is None:
        return errors
    ini = ini_path.read_text()
    cfg = cfg_path.read_text()
    sections = env_sections(ini)

    catalog = matrix.get("featureCatalog", {})
    board_ids = {b["id"] for b in boards}
    flavor_names = {f["name"] for f in flavors}

    # ── 1. platformio.ini ground truth: BLE is [env:full]-only ────────────────
    base = sections.get("env", "")
    full = sections.get("env:full", "")
    dev = sections.get("env:dev", "")
    release = sections.get("env:release", "")

    if flag_set(base, "FEATURE_BLE_STATUS") != 0:
        err("platformio.ini base [env] must set -DFEATURE_BLE_STATUS=0 "
            "(the whole BLE-only-in-full invariant rests on this)")
    if flag_set(full, "FEATURE_BLE_STATUS") != 1:
        err("[env:full] must set -DFEATURE_BLE_STATUS=1")
    if flag_set(full, "FEATURE_MESH_NETWORK") != 1:
        err("[env:full] must set -DFEATURE_MESH_NETWORK=1")
    for name, body in (("env:dev", dev), ("env:release", release)):
        if flag_set(body, "FEATURE_BLE_STATUS") == 1:
            err(f"[{name}] must NOT enable BLE (it doesn't fit the OTA slots)")
        if flag_set(body, "FEATURE_MESH_NETWORK") not in (0, None):
            err(f"[{name}] must not enable mesh (full-only)")

    # FEATURE_TEST_CONSOLE must never ship: not enabled in ANY env, default off.
    if re.search(r"-DFEATURE_TEST_CONSOLE=1", ini):
        err("FEATURE_TEST_CONSOLE=1 found in platformio.ini — the demo/mutate "
            "test console must never be compiled into a shippable image")
    m = re.search(r"#\s*define\s+FEATURE_TEST_CONSOLE\s+(\d+)", cfg)
    if not m or m.group(1) != "0":
        err("canary_config.h FEATURE_TEST_CONSOLE default must be 0")
    m = re.search(r"#\s*define\s+FEATURE_DIAGNOSTICS\s+(\d+)", cfg)
    if not m or m.group(1) != "1":
        err("canary_config.h FEATURE_DIAGNOSTICS default must be 1 "
            "(self-test is expected in every build)")

    # ── 2. build_matrix.json internal + cross-file consistency ────────────────
    def check_features(where, feats):
        for f in feats:
            if f not in catalog:
                err(f"{where}: feature '{f}' not in featureCatalog")
        if ALWAYS_LISTED not in feats:
            err(f"{where}: every build must include '{ALWAYS_LISTED}' "
                "(FEATURE_DIAGNOSTICS is always on)")

    canary = None
    for p in matrix.get("products", []):
        if p["board"] not in board_ids:
            err(f"product {p['id']}: board '{p['board']}' not in boards.json")
        # Every product must resolve to a real flavor. Most ARE one (id ==
        # flavor). A board-specialized product — one env of a flavor, built
        # for a board with a different feature posture, e.g. the classic-ESP32
        # reach ports — says so with `flavor` instead of inventing a flavor
        # that firmware/flavors.json would have to grow for no build reason.
        flavor_of = p.get("flavor", p["id"])
        if flavor_of not in flavor_names:
            err(f"product {p['id']}: flavor '{flavor_of}' is not in flavors.json")
        if p.get("flavor") and not p.get("build", {}).get("env"):
            err(f"product {p['id']}: declares flavor '{p['flavor']}' but no "
                "build.env — a board-specialized product must name the env it is")
        if p["id"] == "canary":
            canary = p
        if p.get("hasLevels"):
            for lvl, b in p.get("levels", {}).items():
                check_features(f"{p['id']}.{lvl}", b["features"])
        else:
            b = p.get("build", {})
            check_features(f"{p['id']}.build", b.get("features", []))
            # serial-only / non-S3 products can't carry BLE or mesh
            if p["flow"] in ("serial", "vision"):
                for banned in ("bluetooth", "mesh"):
                    if banned in b.get("features", []):
                        err(f"product {p['id']} (flow={p['flow']}) must not list "
                            f"'{banned}' — it's a serial/vision board with no BLE radio in use")

    # ── 2a-bis. a product's feature claim is held to its OWN tree's flag ─────
    # The matrix exists to answer "which build includes which feature", so a
    # tile claiming a feature the build does not have is the exact lie it is
    # for. featureCatalog's `flag` names are the canary tree's spellings, and
    # the other trees name the same capability differently (the canary-wap's
    # die-temperature reader is FEATURE_SYS_MONITOR), so each claim is held to
    # the flag its own tree spells it with (FLAG_ALIASES above), in both
    # directions: the flag is #define'd in that tree (a reading, thermal,
    # also guards the read itself), and no env that builds the lane sets it
    # to 0. A tree is the sweep-C35 suite's: the flavor's directory, the
    # firmware/common headers it includes and the sources beside them, and
    # the common sources its build_src_filter names. That gap is how
    # canary-sense advertised a battery gauge and a die-temp sensor it has
    # neither of (fixed 2026-09-05), and how featureCatalog.thermal named
    # FEATURE_DIAGNOSTICS, the canary's self-test flag (sweep C36).
    #
    # KNOWN GAP, stated so nobody assumes otherwise: the claims UNMAPPED
    # lists (the canary-wap's selftest, csi and gps; the Vision's, the
    # Sense's and the display's selftest) have no spelling mapped in their
    # tree, so they still rest on the negative direction alone. A flag an env
    # leaves unset is held to being #define'd in its tree, not to its default
    # being on, and a build profile a sketch header chooses (the canary-wap's
    # build_config.h) is not modeled: only an env's -D flags are.
    for e in feature_flag_errors(matrix, tree_suite().tree_facts, tree_suite().env_flags):
        err(e)

    # ── 2a-ter. an UNMAPPED claim still has the code it rests on (C37) ───────
    # Each UNMAPPED claim names, in EVIDENCE, the code it rests on today, and
    # that code must stay in its tree as unconditional compiled code. Deleting
    # the canary-wap's csi_integration.h include, or wrapping it in an #if,
    # used to leave the csi claim green; now it says to restore the code, or
    # to drop or map the claim (a selftest claim may only be mapped: every
    # build must list it). It holds presence only: whether that code earns
    # the catalog's words (a wizard or OTA self-test as "Boot self-test", a
    # GNSS UART for an optional module as "GPS fix") is the open copy
    # question, not this.
    for e in evidence_errors():
        err(e)

    # ── 2b. flasher deep-link can't rot: every product must resolve to a real
    #        flasher catalog product (both files live in this repo). ───────────
    flasher = matrix.get("flasher", {})
    if not flasher.get("url"):
        err("build_matrix.json: flasher.url is required for the /checkup Flash button")
    prefix = flasher.get("productPrefix", "securacv-")
    if flash_catalog is not None:
        flash_ids = {p.get("id") for p in flash_catalog.get("products", [])}
        for p in matrix.get("products", []):
            want = prefix + p["id"]
            if want not in flash_ids:
                err(f"product {p['id']}: flasher catalog has no '{want}' "
                    f"(canary-local/devices/flash.json) — the Flash-in-browser "
                    f"deep-link would 404; add it or fix productPrefix")

    # ── 2b-bis. every product firmware/flavors.json ships has a lane here ─────
    # The matrix answers "which build includes which feature" for every
    # product a user can flash — so a product that ships with no lane is the
    # dashboard omitting the thing people actually install. canary-display,
    # the most actively released product, had no lane for its whole life.
    # A lane is a product whose id IS the flavor, or a board-specialized
    # product whose `flavor` names it (the display's watch lane is one).
    for e in lane_coverage_errors(matrix, flavors):
        err(e)

    # ── 2b-ter. every product lane is ONE device; every manifest env is real ──
    # The device manifests (devices/<slug>/device.json) join this matrix to the
    # flasher catalog, the figures and the board registry. A lane no manifest
    # resolves to, or a manifest naming an env its project never defines,
    # breaks that join at the source — so the matrix's own lint refuses it.
    manifests, load_errs = load_manifests(root / "devices")
    for e in load_errs:
        err(e)
    for e in device_join_errors(matrix, flavors, manifests, root):
        err(e)

    # ── 2b-quater. a probe count a note states is the flagship self-test's ───
    # A lane's note reaches the website's /checkup page (carry_to_site.py), so
    # a stale count is a false sentence on the site, not just in this file.
    diag = root / DIAG_CPP
    probes = flagship_probe_count(diag.read_text()) if diag.is_file() else None
    if probes is None:
        err(f"cannot read the flagship self-test's TESTS[] table in {DIAG_CPP}: "
            f"the probe counts in build_matrix.json would go unchecked")
    else:
        for e in probe_count_errors(matrix, probes):
            err(e)

    # ── 2c. the default recommendation must resolve to a real product/level ───
    by_id = {p["id"]: p for p in matrix.get("products", [])}
    rec = matrix.get("recommended", {})
    dft = rec.get("default", {})
    if not dft.get("product"):
        err("recommended.default.product is required (the pick when nothing is known)")
    elif dft["product"] not in by_id:
        err(f"recommended.default.product '{dft['product']}' is not a product")
    else:
        dp = by_id[dft["product"]]
        if dp.get("hasLevels"):
            if dft.get("level") not in (dp.get("levels") or {}):
                err(f"recommended.default.level '{dft.get('level')}' not a level of "
                    f"{dft['product']}")
    # forChip: each detected-chip pick must be a product whose mcu is that family.
    for chip, pid in (rec.get("forChip") or {}).items():
        if pid not in by_id:
            err(f"recommended.forChip[{chip}] '{pid}' is not a product")
        elif chip not in (by_id[pid].get("mcu") or ""):
            err(f"recommended.forChip[{chip}] points at {pid}, whose mcu "
                f"'{by_id[pid].get('mcu')}' isn't a {chip}")

    # ── 3. the matrix must agree with the ini on the BLE/mesh full-only cell ───
    if canary:
        lv = canary.get("levels", {})
        for lvl in ("dev", "release"):
            for banned in ("bluetooth", "mesh"):
                if banned in lv.get(lvl, {}).get("features", []):
                    err(f"canary.{lvl} lists '{banned}', but platformio.ini "
                        f"enables it only in [env:full]")
        for needed in ("bluetooth", "mesh"):
            if needed not in lv.get("full", {}).get("features", []):
                err(f"canary.full must list '{needed}' (it's on in [env:full])")

    return errors


def main() -> int:
    errors = collect()
    if errors:
        print("build_matrix.json lint FAILED:", file=sys.stderr)
        for e in errors:
            print(f"  ✗ {e}", file=sys.stderr)
        return 1
    print("build_matrix.json lint OK — matrix matches platformio.ini + config.h, "
          "and every lane is a device manifest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
