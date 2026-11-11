#!/usr/bin/env python3
"""Pins firmware/scripts/microcopy_fkgl.py's COPY extractor — the corpus the
Sensing dashboard's reading-grade gate (microcopy_lint.sh, firmware.yml's
microcopy-lint job) grades.

What is pinned and why:
  • comments inside COPY are not copy. The extractor used to match every
    quoted span in the block, comments included, so the apostrophe in the
    calibrate overlay's comment paired with the one in the tier note's and
    swallowed every string between them into one bogus "string" (sweep
    F465): 84 strings graded at 3.46, five of them lost and one made of
    comment text. Now it is 89 strings at 3.35, still under the 6.0 ceiling;
  • the strip knows strings: a // or /* inside a string is the string's
    ('https://' survives whole), and a quote or a brace inside a comment
    is the comment's (it opens no string and does not end the brace walk);
  • on the real dashboard the extracted list is exactly the string values
    JavaScript itself reads out of COPY (through node, which lint.yml's
    scripts job sets up), less the short and placeholder strings the
    extractor drops on purpose.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import json
import re
import shutil
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "firmware" / "scripts" / "microcopy_fkgl.py"
DASHBOARD = (
    REPO / "firmware" / "projects" / "canary-wap" / "arduino" / "canary_wap"
    / "csi_dashboard_html.h"
)


def _load():
    spec = importlib.util.spec_from_file_location("microcopy_fkgl", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


fk = _load()

# Every comment here holds something the old matcher read as copy: an
# apostrophe that pairs with the next one, a double-quoted aside long
# enough to keep, and a brace that ended the old brace walk early. Every
# string holds something a naive comment strip would cut: '://', '/*',
# '//', an apostrophe, an escaped quote.
FIXTURE = r"""
<script>
const BEFORE = "Not part of COPY, it comes first.";
const COPY = {
  // The owner's first look. "An aside in quotes" is not copy either.
  greet: "Welcome home, it's quiet here.",
  /* A block comment with a closing brace } and an opening one {,
   * an apostrophe that isn't copy, and "a quoted aside too". */
  link: 'See https://example.com/help for more.',
  slash: "Odd art: /* not a comment */ stays put.",
  nested: {
    tip: 'Don\'t worry, it\'s fine.', // a trailing comment's apostrophe
    more: "Two // slashes inside a string.",
  },
  tpl: `Hello ${ /* a comment in code */ who }, and again.`,
  list: ["First item in the list.", 'Second item in the list.'],
  // The last comment's quote: "it ends here" and the brace } too.
};
const AFTER = "Not part of COPY, it comes after.";
</script>
"""

FIXTURE_STRINGS = [
    "Welcome home, it's quiet here.",
    "See https://example.com/help for more.",
    "Odd art: /* not a comment */ stays put.",
    r"Don\'t worry, it\'s fine.",
    "Two // slashes inside a string.",
    "Hello ${   who }, and again.",
    "First item in the list.",
    "Second item in the list.",
]


class StripKnowsStrings(unittest.TestCase):
    def test_comments_go_and_strings_stay_whole(self):
        src = (
            'a = "x // y"; // gone, it\'s gone\n'
            "b = 'https://h/p'; /* gone \"too\" */ c = `t /* t */ ${d /* gone */}`;\n"
        )
        self.assertEqual(
            fk.strip_js_comments(src),
            'a = "x // y"; \n'
            "b = 'https://h/p';   c = `t /* t */ ${d  }`;\n",
        )

    def test_a_brace_or_quote_in_a_comment_does_not_end_the_block(self):
        block = fk.copy_block("const COPY = { a: 'one', /* } ' */ b: 'two' }; x = '}';")
        self.assertEqual(block, " a: 'one',   b: 'two' ")


class FixtureList(unittest.TestCase):
    def test_exact_extracted_list(self):
        self.assertEqual(fk.extract_copy_strings(FIXTURE), FIXTURE_STRINGS)

    def test_no_copy_block_is_an_empty_list(self):
        self.assertEqual(fk.extract_copy_strings("const OTHER = { a: 'one two' };"), [])


class RealDashboard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = DASHBOARD.read_text(encoding="utf-8")
        cls.strings = fk.extract_copy_strings(cls.text)

    def test_no_extracted_string_spans_a_comment(self):
        # Every COPY string is one line, and every one is a literal the file
        # spells: a span that crossed a comment holds a newline, a comment
        # marker, or text that is no literal at all.
        self.assertTrue(self.strings)
        for s in self.strings:
            with self.subTest(s=s[:60]):
                self.assertNotIn("\n", s)
                self.assertNotIn("/*", s)
                self.assertNotIn("*/", s)
                self.assertTrue(
                    f'"{s}"' in self.text or f"'{s}'" in self.text or f"`{s}`" in self.text,
                    "not a string literal the file spells",
                )

    def test_the_strings_the_comments_used_to_swallow_are_graded(self):
        for s in (
            "The canary is learning your empty room.",
            "Step out for ten seconds.",
            "Calibrate empty room",
            "Got it. The canary knows your room now.",
            "Quiet so far today. The canary is perched, head cocked.",
        ):
            self.assertIn(s, self.strings)

    @unittest.skipUnless(shutil.which("node"), "node is not on PATH (lint.yml's scripts job sets it up)")
    def test_the_list_is_what_javascript_reads_out_of_copy(self):
        # The oracle is JavaScript itself: evaluate the COPY literal, walk its
        # values in order (string keys keep source order), apply the
        # extractor's two documented drops, and compare. The literal is cut
        # out by the file's own layout (it closes on the first `};` at column
        # 0), not by the scanner under test; node refuses a wrong cut.
        m = re.search(r"\bconst\s+COPY\s*=\s*\{", self.text)
        self.assertIsNotNone(m)
        end = self.text.index("\n};", m.end())
        literal = self.text[m.end() - 1:end + 2]
        js = (
            "const COPY = " + literal + ";\n"
            "const out = [];\n"
            "(function walk(v) {\n"
            "  if (typeof v === 'string') out.push(v);\n"
            "  else if (Array.isArray(v)) v.forEach(walk);\n"
            "  else if (v && typeof v === 'object') Object.values(v).forEach(walk);\n"
            "})(COPY);\n"
            "process.stdout.write(JSON.stringify(out));\n"
        )
        run = subprocess.run(
            ["node", "-e", js], capture_output=True, text=True, timeout=60, check=False,
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        values = json.loads(run.stdout)
        kept = [
            v for v in values
            if len(v.strip()) >= 4 and not re.fullmatch(r"\{?[a-zA-Z_]+\}?", v.strip())
        ]
        # No COPY string uses a backslash escape today, so JavaScript's
        # value and the extractor's raw literal body are the same text.
        self.assertFalse(any("\\" in s for s in self.strings))
        self.assertEqual(self.strings, kept)


if __name__ == "__main__":
    unittest.main()
