#!/usr/bin/env python3
"""Holds canary-vision's flavor config to what something reads (sweep F182).

firmware/configs/canary-vision/default/config.h is on every Vision build's
include path, but the firmware reads its own constants in
firmware/projects/canary-vision/include/canary/config.h, which own the
values. The flavor file named eighteen CONFIG_* settings nothing read, so
editing one changed nothing; they are gone. What is pinned and why:

  • every CONFIG_* macro left in the flavor file has a reader outside it: a
    tracked file of a kind that can read a macro (C/C++ sources and headers,
    sketches, Python, JS, build and workflow config) that names it as a
    whole word. Docs are not readers, and neither is this test;
  • every one of them is a seed: a copy of the same-named
    `static constexpr` in the project config.h (CONFIG_ dropped), and it
    must hold the same value. Today they are the four gen_flash.py reads
    (CONFIG_PERSON_TARGET, CONFIG_SCORE_MIN, CONFIG_LOST_TIMEOUT_MS,
    CONFIG_DWELL_START_MS) for the flasher catalog's detection defaults. A
    seed with no same-named constant, or one whose constant is not a plain
    literal this test can compare, is refused rather than passed;
  • the checks fail on what they exist to catch: a planted unread macro, a
    seed that drifted, a constant spelled as an expression.

Repointing gen_flash.py at the project config.h, which would let the four
seeds go too, waits for the package that owns gen_flash.py (w18-labflash).

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

DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(CONFIG_[A-Za-z0-9_]+)\b[ \t]*(.*)$", re.M)
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
    """The macros no reader text names as a whole word."""
    found = set()
    pattern = re.compile(r"\bCONFIG_[A-Za-z0-9_]+\b")
    for text in corpus:
        found.update(pattern.findall(text))
    return sorted(name for name in macros if name not in found)


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
        self.assertEqual(unread(self.macros, self.corpus), [],
                         f"{FLAVOR} defines CONFIG_* macros nothing outside it reads; "
                         "the firmware reads include/canary/config.h, so delete them there")

    def test_every_seed_equals_the_project_constant(self):
        self.assertEqual(seed_problems(self.macros, project_constants(self.project_text)), [])

    def test_the_seeds_are_the_four_gen_flash_reads(self):
        self.assertEqual(sorted(self.macros), ["CONFIG_DWELL_START_MS", "CONFIG_LOST_TIMEOUT_MS",
                                               "CONFIG_PERSON_TARGET", "CONFIG_SCORE_MIN"])
        gen_flash = (REPO / "canary-local/tools/gen_flash.py").read_text(encoding="utf-8")
        for name in self.macros:
            self.assertIn(f'read_const(VISION_CFG, "{name}")', gen_flash, name)

    # ── the checks catch what they exist to catch ─────────────────────────

    def test_a_planted_unread_macro_fails(self):
        planted = self.flavor_text + "\n#define CONFIG_NOBODY_READS_THIS   7\n"
        self.assertEqual(unread(flavor_macros(planted), self.corpus), ["CONFIG_NOBODY_READS_THIS"])

    def test_a_name_inside_a_longer_word_is_not_a_reader(self):
        macros = {"CONFIG_ONLY_IN_DOCS": "1"}
        self.assertEqual(unread(macros, ["see CONFIG_ONLY_IN_DOCSX", "CONFIG_ONLY_IN_DOC"]),
                         ["CONFIG_ONLY_IN_DOCS"])
        self.assertEqual(unread(macros, ['read_const(VISION_CFG, "CONFIG_ONLY_IN_DOCS")']), [])

    def test_a_drifted_seed_fails(self):
        drifted = self.project_text.replace("SCORE_MIN     = 70;", "SCORE_MIN     = 65;")
        self.assertNotEqual(drifted, self.project_text, "the project config's SCORE_MIN line moved")
        problems = seed_problems(self.macros, project_constants(drifted))
        self.assertEqual(len(problems), 1)
        self.assertIn("CONFIG_SCORE_MIN = 70", problems[0])

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
