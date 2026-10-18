#!/usr/bin/env python3
"""Holds canary-vision's flavor config to what something reads (sweep F182).

firmware/configs/canary-vision/default/config.h is on every Vision build's
include path, but the firmware reads its own constants in
firmware/projects/canary-vision/include/canary/config.h (the topic patterns
in include/canary/topics.h), which own the values. The flavor file named
eighteen CONFIG_* settings nothing read, so editing one changed nothing;
they are gone, and so are the four detection defaults gen_flash.py read
there before it read the project config.h's own constants (F182's
follow-up). It still owns the FEATURE_* flags and the watchdog timeout, and
keeps one copy, the device type the fleet figures read. What is pinned and
why:

  • every CONFIG_* macro left in the flavor file has a reader outside it: a
    tracked file of a kind that can read a macro (C/C++ sources and headers,
    sketches, Python, JS, build and workflow config) that names it as a
    whole word anywhere but in the name of its own #define (a second
    definition is not a read). Docs are not readers, and neither is this
    test;
  • every one of them is a seed: a copy of the same-named
    `static constexpr` in the project config.h (CONFIG_ dropped), and it
    must hold the same value. Today there is one, CONFIG_DEVICE_TYPE, which
    gen_figures.mjs reads to key the flavor's figure. A seed with no
    same-named constant, or one whose constant is not a plain literal this
    test can compare, is refused rather than passed;
  • the four detection defaults the flasher catalog used to read here
    (CONFIG_PERSON_TARGET, CONFIG_SCORE_MIN, CONFIG_LOST_TIMEOUT_MS,
    CONFIG_DWELL_START_MS) have no reader left, so putting one back fails
    the first check: gen_flash.py reads PERSON_TARGET, SCORE_MIN,
    LOST_TIMEOUT_MS and DWELL_START_MS from the project config.h, and names
    nothing under firmware/configs/canary-vision/;
  • a tool that reads a macro by pattern rather than by name (gen_figures.mjs
    matches any `#define ...DEVICE_TYPE`) is a reader only through
    PATTERN_READERS, and only while that file still holds the pattern and
    the pattern still matches the macro's line. F182 first deleted
    CONFIG_DEVICE_TYPE as unread, and the Vision boards stopped serving a
    device type in figures.json;
  • the checks fail on what they exist to catch: a planted unread macro, a
    seed that drifted, a constant spelled as an expression. Those self-tests
    run the checkers on synthetic inputs, so they hold the checkers and not
    the tree, and a re-aligned or re-valued project config.h cannot break
    them.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import re
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FLAVOR = Path("firmware/configs/canary-vision/default/config.h")
PROJECT = Path("firmware/projects/canary-vision/include/canary/config.h")
THIS = Path(__file__).resolve().relative_to(REPO)

# The kinds of file that can read a macro: compiled sources and headers,
# sketches, the generators and tests, and the build and CI config that can
# pass a -D. Prose (.md, .txt) is not a reader.
READER_SUFFIXES = {
    ".h", ".hpp", ".c", ".cc", ".cpp", ".ino", ".py", ".js", ".mjs", ".cjs",
    ".ini", ".yml", ".yaml", ".json", ".sh", ".cmake",
}
READER_NAMES = {"Makefile", "CMakeLists.txt"}
# Generated bundles that only echo compiled code.
SKIP_PREFIXES = ("canary-local/emulator/dist/",)
# Readers that match a macro by pattern, so a whole-word search cannot see
# them: macro -> (file, the pattern's source text as the file spells it).
PATTERN_READERS = {
    "CONFIG_DEVICE_TYPE": ("canary-local/tools/figures/gen_figures.mjs",
                           r"#define\s+\w*DEVICE_TYPE\s+"),
}
# The detection defaults gen_flash.py read from the flavor file until it read
# the project config.h's constants instead (F182's follow-up).
FORMER_FLASHER_SEEDS = ("CONFIG_PERSON_TARGET", "CONFIG_SCORE_MIN",
                        "CONFIG_LOST_TIMEOUT_MS", "CONFIG_DWELL_START_MS")

DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(CONFIG_[A-Za-z0-9_]+)\b[ \t]*(.*)$", re.M)
# The defined name of any CONFIG_* #define, which names it without reading it.
DEFINED_NAME = re.compile(r"^([ \t]*#[ \t]*define[ \t]+)CONFIG_[A-Za-z0-9_]+\b", re.M)
CONSTEXPR = re.compile(
    r"^[ \t]*static[ \t]+constexpr[ \t]+[^=;]*?\b([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*([^;]*);", re.M)


def strip_comment(value: str) -> str:
    """The macro body without a trailing // or /* comment."""
    out, i, quote = [], 0, False
    while i < len(value):
        c = value[i]
        if c == '"' and (i == 0 or value[i - 1] != "\\"):
            quote = not quote
        if not quote and value.startswith(("//", "/*"), i):
            break
        out.append(c)
        i += 1
    return "".join(out).strip()


def literal(text: str):
    """A plain integer (optionally U/L-suffixed) or string literal as a value,
    else None: anything else cannot be compared and is refused."""
    t = text.strip()
    m = re.fullmatch(r"(\d+)[uUlL]*", t)
    if m:
        return int(m.group(1))
    m = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', t)
    if m:
        return m.group(1)
    return None


def flavor_macros(text: str) -> dict[str, str]:
    return {m.group(1): strip_comment(m.group(2)) for m in DEFINE.finditer(text)}


def project_constants(text: str) -> dict[str, str]:
    return {m.group(1): m.group(2).strip() for m in CONSTEXPR.finditer(text)}


def unread(macros, corpus) -> list[str]:
    """The macros no reader text names as a whole word, outside the name of
    a #define of its own (a body that uses another CONFIG_* still reads it)."""
    found = set()
    pattern = re.compile(r"\bCONFIG_[A-Za-z0-9_]+\b")
    for text in corpus:
        found.update(pattern.findall(DEFINED_NAME.sub(r"\1", text)))
    return sorted(name for name in macros if name not in found)


def pattern_read(name: str, flavor_text: str, readers=PATTERN_READERS, read=None) -> bool:
    """Whether `name` is read by pattern: its PATTERN_READERS file still holds
    the pattern's source text, and that pattern matches the macro's line."""
    if name not in readers:
        return False
    rel, pattern = readers[name]
    text = read(rel) if read else (REPO / rel).read_text(encoding="utf-8")
    if pattern not in text:
        return False
    line = next((m.group(0) for m in DEFINE.finditer(flavor_text) if m.group(1) == name), "")
    return re.search(pattern, line) is not None


def seed_problems(macros: dict[str, str], constants: dict[str, str]) -> list[str]:
    """Each macro must be a literal copy of the same-named project constant."""
    problems = []
    for name, body in sorted(macros.items()):
        const = name[len("CONFIG_"):]
        seed = literal(body)
        if seed is None:
            problems.append(f"{name}: the flavor value `{body}` is not a plain literal")
            continue
        if const not in constants:
            problems.append(f"{name}: {PROJECT} has no `static constexpr ... {const}` to own it")
            continue
        owned = literal(constants[const])
        if owned is None:
            problems.append(f"{name}: {PROJECT}'s {const} is `{constants[const]}`, not a literal "
                            "this test can compare; refusing rather than passing")
        elif owned != seed:
            problems.append(f"{name} = {seed!r} but {PROJECT}'s {const} = {owned!r}: the project "
                            "config owns the value, so the seed must follow it")
    return problems


def reader_corpus() -> list[str]:
    files = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z"], check=True,
                           capture_output=True).stdout.decode().split("\0")
    texts = []
    for rel in files:
        if not rel or rel.startswith(SKIP_PREFIXES):
            continue
        p = Path(rel)
        if p == FLAVOR or p == THIS:
            continue
        if p.suffix not in READER_SUFFIXES and p.name not in READER_NAMES:
            continue
        full = REPO / p
        if full.is_file():
            texts.append(full.read_text(encoding="utf-8", errors="replace"))
    return texts


class FlavorConfigReaders(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.flavor_text = (REPO / FLAVOR).read_text(encoding="utf-8")
        cls.project_text = (REPO / PROJECT).read_text(encoding="utf-8")
        cls.macros = flavor_macros(cls.flavor_text)
        cls.corpus = reader_corpus()

    def test_every_config_macro_left_has_a_reader(self):
        self.assertTrue(self.macros, f"no CONFIG_* parsed from {FLAVOR}")
        missing = [n for n in unread(self.macros, self.corpus)
                   if not pattern_read(n, self.flavor_text)]
        self.assertEqual(missing, [],
                         f"{FLAVOR} defines CONFIG_* macros nothing outside it reads; "
                         "the firmware reads include/canary/config.h, so delete them there")

    def test_every_seed_equals_the_project_constant(self):
        self.assertEqual(seed_problems(self.macros, project_constants(self.project_text)), [])

    def test_the_only_seed_is_the_figure_type(self):
        self.assertEqual(sorted(self.macros), ["CONFIG_DEVICE_TYPE"])
        self.assertTrue(pattern_read("CONFIG_DEVICE_TYPE", self.flavor_text),
                        "gen_figures.mjs no longer reads the flavor's DEVICE_TYPE by its pattern")

    def test_gen_flash_names_nothing_in_the_flavor_directory(self):
        # A pin on the source; canary-local/tools/tests/test_flash_read_const.py
        # holds the behavior (which files vision_detect_block reads).
        gen_flash = (REPO / "canary-local/tools/gen_flash.py").read_text(encoding="utf-8")
        naming = [n for n, line in enumerate(gen_flash.splitlines(), 1)
                  if "configs/canary-vision" in line]
        self.assertEqual(naming, [], "gen_flash.py names the Vision flavor directory again (at "
                         "these lines); its detection defaults are the project config.h's constants")

    def test_a_former_flasher_seed_put_back_has_no_reader(self):
        # Against the real tree: each of the four, planted back in the flavor
        # file, is a CONFIG_* nothing reads, so the first check fails on it.
        planted = self.flavor_text + "".join(f"#define {n}  1\n" for n in FORMER_FLASHER_SEEDS)
        macros = flavor_macros(planted)
        missing = [n for n in unread(macros, self.corpus) if not pattern_read(n, planted)]
        self.assertEqual(missing, sorted(FORMER_FLASHER_SEEDS))

    # ── the checks catch what they exist to catch ─────────────────────────

    def test_a_planted_unread_macro_fails(self):
        flavor = ("#define FEATURE_X   1\n"
                  "#define CONFIG_READ_BY_A_TOOL   70   // a seed\n"
                  "#define CONFIG_NOBODY_READS_THIS   7\n")
        corpus = ['read_const(VISION_CFG, "CONFIG_READ_BY_A_TOOL")', "# CONFIG_NOBODY_READS_THESE"]
        self.assertEqual(unread(flavor_macros(flavor), corpus), ["CONFIG_NOBODY_READS_THIS"])

    def test_a_pattern_reader_counts_only_while_it_holds_and_matches(self):
        readers = {"CONFIG_KIND": ("tool.mjs", r"#define\s+\w*KIND\s+")}
        flavor = '#define CONFIG_KIND   "x"\n'
        holds = 'const m = text.match(/#define\\s+\\w*KIND\\s+"([^"]+)"/);'
        self.assertIn(r"#define\s+\w*KIND\s+", holds)
        self.assertTrue(pattern_read("CONFIG_KIND", flavor, readers, read=lambda _: holds))
        self.assertFalse(pattern_read("CONFIG_KIND", flavor, readers, read=lambda _: "// gone"))
        self.assertFalse(pattern_read("CONFIG_KIND", '#define CONFIG_KINDS "x"\n', readers,
                                      read=lambda _: holds))
        self.assertFalse(pattern_read("CONFIG_OTHER", flavor, readers, read=lambda _: holds))

    def test_a_second_definition_is_not_a_reader(self):
        macros = {"CONFIG_A": "1", "CONFIG_B": "2"}
        corpus = ["#define CONFIG_A 1\n", "  #  define CONFIG_B (CONFIG_A + 1)\n"]
        self.assertEqual(unread(macros, corpus), ["CONFIG_B"])

    def test_a_name_inside_a_longer_word_is_not_a_reader(self):
        macros = {"CONFIG_ONLY_IN_DOCS": "1"}
        self.assertEqual(unread(macros, ["see CONFIG_ONLY_IN_DOCSX", "CONFIG_ONLY_IN_DOC"]),
                         ["CONFIG_ONLY_IN_DOCS"])
        self.assertEqual(unread(macros, ['read_const(VISION_CFG, "CONFIG_ONLY_IN_DOCS")']), [])

    def test_a_drifted_seed_fails(self):
        macros = {"CONFIG_SCORE_MIN": "70", "CONFIG_LOST_TIMEOUT_MS": "1500"}
        owner = ("static constexpr int SCORE_MIN     = {score};       // 0-100\n"
                 "static constexpr uint32_t LOST_TIMEOUT_MS = 1500;\n")
        self.assertEqual(seed_problems(macros, project_constants(owner.format(score=70))), [])
        problems = seed_problems(macros, project_constants(owner.format(score=65)))
        self.assertEqual(len(problems), 1)
        self.assertIn("CONFIG_SCORE_MIN = 70", problems[0])
        self.assertIn("SCORE_MIN = 65", problems[0])

    def test_a_seed_with_no_owner_or_an_expression_owner_is_refused(self):
        self.assertIn("has no `static constexpr",
                      seed_problems({"CONFIG_NOT_A_CONSTANT": "3"}, {})[0])
        self.assertIn("not a literal",
                      seed_problems({"CONFIG_X": "4"}, {"X": "VISION_X"})[0])
        self.assertIn("not a plain literal",
                      seed_problems({"CONFIG_X": "(2 * 2)"}, {"X": "4"})[0])

    def test_the_parsers_read_the_real_shapes(self):
        consts = project_constants("static constexpr uint32_t LOST_TIMEOUT_MS = 1500;\n"
                                   "static constexpr const char* MODEL        = \"a // b\";\n")
        self.assertEqual(literal(consts["LOST_TIMEOUT_MS"]), 1500)
        self.assertEqual(literal(consts["MODEL"]), "a // b")
        macros = flavor_macros("#define CONFIG_A   1500u   // ms\n#define CONFIG_B \"x\" /* c */\n")
        self.assertEqual({k: literal(v) for k, v in macros.items()}, {"CONFIG_A": 1500, "CONFIG_B": "x"})


if __name__ == "__main__":
    unittest.main()
