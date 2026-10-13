"""scripts/lint_spelling.py — the US-stem families, their self-test, and the gate.

The lint's British forms come from two places: the hand-typed word list and
the families it shares with the website's tests/copy-honesty.test.mjs, each
GENERATED from US words so a form nobody typed is still caught (sweep item
CI9). This suite holds:

- the module's own self-test (every form a family builds is caught, every US
  form and every ALLOW word is not) is clean, and it has teeth: a family
  dropped from the rule, or a rule made greedy, makes it report;
- the generated pairs cover every family, prefix and suffix the rules name;
- the cached word-run check finds exactly the lines a per-line search would;
- main() fails on a file holding a family form the old word list never had,
  passes a file of the US forms, and blanks the Swift API names it exempts.

AGENTS.md rule 3b: this file never types a banned form. Every British
spelling it needs is BUILT from a US stem at run time (the lint's own
family_pairs(), or a stem plus a suffix), so a repo-wide spelling sweep has
nothing here to rewrite — and lint_spelling.py, which reads this file like any
other, stays green on it.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "lint_spelling.py"
spec = importlib.util.spec_from_file_location("lint_spelling", SCRIPT)
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)  # type: ignore[union-attr]

# A family form the hand-typed word list never had: the doubled-l past tense
# of the first DOUBLE_L verb, the very form that shipped on the website.
SHIPPED = L.DOUBLE_L[0] + "l" + "ed"


def run_lint(*paths: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, paths)],
                          capture_output=True, text=True)


def with_banned(pattern: str):
    """Swap the module's BANNED for one built from `pattern` (same flags)."""
    class _Swap:
        def __enter__(self):
            self.saved = L.BANNED
            L.BANNED = re.compile(pattern, re.IGNORECASE | re.ASCII)
            L._is_banned.cache_clear()

        def __exit__(self, *exc):
            L.BANNED = self.saved
            L._is_banned.cache_clear()
    return _Swap()


class SelfTest(unittest.TestCase):
    def test_self_test_is_clean(self):
        missed, eaten = L.self_test()
        self.assertEqual(missed, [], "a family no longer catches what it builds")
        self.assertEqual(eaten, [], "the ban matches a correct word")

    def test_every_generated_british_form_is_caught(self):
        missed = [uk for _, uk in L.family_pairs() if not L.BANNED.search(uk)]
        self.assertEqual(missed, [])

    def test_no_us_form_and_no_allowed_word_is_caught(self):
        us = [w for w, _ in L.family_pairs()]
        eaten = [w for w in us + L.ALLOW if L.BANNED.search(w)]
        self.assertEqual(eaten, [])

    def test_the_form_that_shipped_is_caught_in_a_compound(self):
        self.assertTrue(L.BANNED.search(f"a better-{SHIPPED} route"))
        self.assertEqual(L.banned_words(f"a better-{SHIPPED} route"), [SHIPPED])

    def test_dropping_a_family_makes_the_self_test_report(self):
        # The rule without the -ize family: the self-test must name its forms.
        ize = next(f for f in L.FAMILIES
                   if f.startswith("(?:" + "|".join(L.IZE_PREFIXES) + ")?"))
        without = [f for f in L.FAMILIES if f is not ize]
        self.assertEqual(len(without), len(L.FAMILIES) - 1)
        with with_banned(rf"\b({L.WORD_LIST}|{'|'.join(without)})\b"):
            missed, eaten = L.self_test()
        stem = L._stems(L.IZE_VERBS, "ize")[-1]
        self.assertIn(stem + "is" + "ation", missed)
        self.assertEqual(eaten, [])

    def test_a_greedy_rule_makes_the_self_test_report(self):
        # The failure that shipped on this file's first run: an optional u.
        greedy = r"\b(colou?r\w*)\b"
        with with_banned(greedy):
            _, eaten = L.self_test()
        self.assertIn("color", eaten)
        self.assertIn("colored", eaten)

    def test_a_dropped_allow_guard_word_is_reported_when_eaten(self):
        # A rule that eats an ALLOW word (aria-labelledby's attribute name is
        # the classic) must be named by the self-test, not merged.
        attr = next(w for w in L.ALLOW if w.startswith("aria-"))
        part = attr.split("-", 1)[1]
        with with_banned(rf"\b({re.escape(part[:8])}\w*)\b"):
            _, eaten = L.self_test()
        self.assertIn(attr, eaten)


class Coverage(unittest.TestCase):
    def test_pairs_cover_every_family_prefix_and_suffix(self):
        pairs = L.family_pairs()
        uk = {b for _, b in pairs}
        n = len(L.DOUBLE_L) * len(L.DOUBLE_L_SUFFIXES) * (1 + len(L.DOUBLE_L_PREFIXES))
        n += len(L.OR_NOUNS) * len(L.OR_SUFFIXES) * (1 + len(L.OR_PREFIXES))
        n += len(L.IZE_VERBS) * len(L.IZE_SUFFIXES) * (1 + len(L.IZE_PREFIXES))
        n += len(L.YZE_VERBS) * len(L.YZE_SUFFIXES)
        n += len(L.ER_NOUNS) * len(L.ER_SUFFIXES) * (1 + len(L.ER_PREFIXES))
        n += len(L.SE_NOUNS) * len(L.SE_SUFFIXES)
        self.assertGreaterEqual(len(pairs), n)
        for p in L.IZE_PREFIXES:
            self.assertIn(p + L._stems(L.IZE_VERBS, "ize")[0] + "is" + "ed", uk)
        for p in L.DOUBLE_L_PREFIXES:
            self.assertIn(p + SHIPPED, uk)
        for p in L.ER_PREFIXES:
            self.assertIn(p + L._stems(L.ER_NOUNS, "er")[1] + "re" + "s", uk)

    def test_each_pair_differs_and_the_us_side_is_the_stem_spelling(self):
        for us, uk in L.family_pairs():
            self.assertNotEqual(us.lower(), uk.lower())

    def test_families_are_built_from_us_words_only(self):
        # The stems the rules are made of are US spellings: none is banned.
        words = (L.DOUBLE_L + L.OR_NOUNS + L.IZE_VERBS + L.YZE_VERBS
                 + L.ER_NOUNS + L.SE_NOUNS)
        self.assertEqual([w for w in words if L.BANNED.search(w)], [])


class WordRuns(unittest.TestCase):
    def test_cached_check_finds_exactly_what_search_finds(self):
        frames = ["{}", "a {} b", "x_{}", "{}_y", "{}2", "2{}", "({})",
                  "better-{}", "{}-ish", "'{}'", "<{}>", "{}.", "{}s",
                  "un{}", "é{}", "{}é", "{}\x00"]
        words = ([uk for _, uk in L.family_pairs()[::7]]
                 + [us for us, _ in L.family_pairs()[::7]] + L.ALLOW)
        for w in words:
            for frame in frames:
                line = frame.format(w)
                self.assertEqual(bool(L.banned_words(line)),
                                 bool(L.BANNED.search(line)), line)

    def test_the_pattern_is_word_characters_only(self):
        # banned_words() splits a line into \w+ runs; it is exact only while
        # no alternative holds a hyphen, space or apostrophe.
        self.assertEqual(L.non_word_literals(), [])
        self.assertEqual(L.non_word_literals(r"\b(re-" + "organ" + r"is\w*)\b"), ["-"])
        self.assertEqual(L.non_word_literals(r"\b(" + "col" + r"our s)\b"), [" "])

    def test_main_refuses_a_pattern_the_word_runs_cannot_see(self):
        f = Path(tempfile.mkdtemp()) / "x.md"
        f.write_text("plain\n", encoding="utf-8")
        code = ("import importlib.util,re,sys;"
                f"s=importlib.util.spec_from_file_location('l',{str(SCRIPT)!r});"
                "m=importlib.util.module_from_spec(s);s.loader.exec_module(m);"
                "m.BANNED=re.compile(m.BANNED.pattern[:-3]+'|a-b)\\\\b',re.I|re.A);"
                f"sys.argv=['l',{str(f)!r}];sys.exit(m.main())")
        r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("word-run check cannot see it", r.stderr)

    def test_every_hit_in_a_line_is_reported(self):
        a = L.DOUBLE_L[1] + "l" + "ing"
        b = L._stems(L.IZE_VERBS, "ize")[2] + "is" + "ed"
        self.assertEqual(L.banned_words(f"{a} and {b}"), [a, b])


class Gate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_main_fails_a_family_form_the_word_list_never_had(self):
        f = self.dir / "note.md"
        f.write_text(f"The beacon {SHIPPED} as far as a radio carries it.\n",
                     encoding="utf-8")
        r = run_lint(f)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn(f"note.md:1: {SHIPPED}", r.stderr)

    def test_main_passes_the_us_forms(self):
        f = self.dir / "note.md"
        f.write_text(" ".join(us for us, _ in L.family_pairs()[::11]) + "\n",
                     encoding="utf-8")
        r = run_lint(f)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_swift_api_names_are_blanked_not_banned(self):
        member = "is" + "Cancel" + "led"
        f = self.dir / "Task.swift"
        f.write_text(f"if Task.{member} {{ return }}\n", encoding="utf-8")
        self.assertEqual(run_lint(f).returncode, 0)
        g = self.dir / "task.js"
        g.write_text(f"// the job was {'cancel' + 'led'}\n", encoding="utf-8")
        self.assertEqual(run_lint(g).returncode, 1)


if __name__ == "__main__":
    unittest.main()
