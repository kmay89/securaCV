#!/usr/bin/env python3
"""Tests for scripts/lint_bench_rows.py — the guard needs a guard.

The lint keeps a benchmark measurement out of the tree (AGENTS.md rule 4,
docs/BENCHMARKS.md). Its predecessor, a Rust test, was reviewed with four
mutations it waved through: the append row's `total:` trailer, a blockquoted
row, and a row pasted into CHANGELOG.md or eval/README.md (outside the docs/
it scanned). Each shape is pinned below, together with the shapes it must NOT
flag (the doc's own descriptive table names every row), and the refusals: a
lint that cannot read ROW_NAMES has nothing to look for, so it must stop
rather than pass.

The numbers in the fixtures are synthetic cell values, not measurements.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "lint_bench_rows", REPO / "scripts" / "lint_bench_rows.py"
)
lint = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lint)

NAMES = lint.parse_row_names(lint.HARNESS.read_text(encoding="utf-8"))


class TheRealHarness(unittest.TestCase):
    def test_every_row_constant_is_read(self):
        # Every `const ROW_*: &str` in the harness (bar the list itself) is a
        # name the lint looks for; parse_row_names refuses otherwise, so this
        # pins that the real file parses, and which names it yields.
        src = lint.HARNESS.read_text(encoding="utf-8")
        consts = {m.group(2) for m in lint.CONST_RE.finditer(src)}
        self.assertEqual(set(NAMES), consts)
        self.assertIn("append_event_checked", NAMES)
        self.assertIn("execute_sandboxed (no-op module)", NAMES)


class Caught(unittest.TestCase):
    """Shapes of a pasted measurement that MUST fail the lint."""

    def assertCaught(self, line):
        self.assertIsNotNone(lint.offending(line, NAMES), line)

    def test_table_row_as_printed(self):
        self.assertCaught("| append_event_checked | 10000 | 1.5 | 1.25 | 2.5 | 3.5 | 4.5 |")

    def test_backticked_and_bold_names(self):
        self.assertCaught("| `run_full_verify` | 5 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |")
        self.assertCaught("| **verify_envelope** | 5 | 1.0 |")

    def test_name_with_spaces_and_parentheses(self):
        self.assertCaught("| execute_sandboxed (no-op module) | 10000 | 0.5 |")

    def test_blockquoted_rows(self):
        self.assertCaught("> | append_event_checked | 10000 | 1.5 |")
        self.assertCaught("> > | TimeBucket::now_10min | 10000 | 0.25 |")

    def test_no_leading_pipe(self):
        self.assertCaught("ContractEnforcer::enforce | 10000 | 0.25 |")

    def test_retyped_numbers(self):
        self.assertCaught("| append_event_checked | 10,000 | 1.5 |")
        self.assertCaught("| append_event_checked | 10 000 | 1.5 |")
        self.assertCaught("| append_event_checked | 1.5 ms |")
        self.assertCaught("| append_event_checked | 250 µs |")

    def test_trailer(self):
        self.assertCaught("total: 10000 events in 1.234 s (8103 events/s)")
        self.assertCaught("> total: 10000 events in 1.234 s (8103 events/s)")
        self.assertCaught("`total: 10 000 events in 12.5 s`")


class NotCaught(unittest.TestCase):
    """Shapes that MUST pass, or the lint blocks correct docs."""

    def assertClean(self, line):
        self.assertIsNone(lint.offending(line, NAMES), line)

    def test_descriptive_table_row(self):
        # docs/BENCHMARKS.md, "What each row measures".
        self.assertClean("| `append_event_checked` | One event through the checked "
                         "path witnessd uses. | Building the candidate. |")
        self.assertClean("| `TimeBucket::now_10min` | One coarse clock read. | — |")

    def test_headers_and_prose(self):
        self.assertClean("| row | n | mean ms | p50 ms | p95 ms | p99 ms | max ms |")
        self.assertClean("|---|---:|---:|---:|---:|---:|---:|")
        self.assertClean("Run append_event_checked with SECURACV_BENCH_N=10000.")
        self.assertClean("a trailer line gives the loop's wall time and events per second")

    def test_other_first_cells(self):
        self.assertClean("| append_event | 10000 | 1.5 |")
        self.assertClean("| p95 | 5 |")


class Refused(unittest.TestCase):
    """A harness the lint cannot read is a refusal, never an empty pass."""

    GOOD = (
        'const ROW_A: &str = "a_row";\n'
        'const ROW_B: &str = "b_row";\n'
        "const ROW_NAMES: &[&str] = &[\n    ROW_A,\n    ROW_B,\n];\n"
    )

    def test_good_source_parses(self):
        self.assertEqual(lint.parse_row_names(self.GOOD), ["a_row", "b_row"])

    def refuses(self, src):
        with self.assertRaises(lint.Refusal):
            lint.parse_row_names(src)

    def test_no_list(self):
        self.refuses('const ROW_A: &str = "a_row";\n')

    def test_empty_list(self):
        self.refuses("const ROW_NAMES: &[&str] = &[];\n")

    def test_constant_left_out_of_the_list(self):
        self.refuses(self.GOOD.replace("    ROW_B,\n", ""))

    def test_unknown_entry(self):
        self.refuses(self.GOOD.replace("ROW_B,", "ROW_C,"))
        self.refuses(self.GOOD.replace("ROW_B,", '"b_row",'))

    def test_duplicate_name(self):
        self.refuses(self.GOOD.replace('"b_row"', '"a_row"'))

    def test_unreadable_constant(self):
        # A computed or differently typed ROW_* constant is not silently skipped.
        self.refuses(self.GOOD.replace('const ROW_B: &str = "b_row";',
                                       'const ROW_B: &str = concat!("b", "_row");'))
        self.refuses(self.GOOD.replace('"b_row"', '"b\\u{7c}row"'))
        self.refuses(self.GOOD.replace('"b_row"', '"b|row"'))


class TreeScan(unittest.TestCase):
    def test_file_outside_docs_is_scanned(self):
        # CHANGELOG.md and eval/README.md were outside the Rust test's reach.
        with tempfile.TemporaryDirectory() as d:
            nested = Path(d) / "eval"
            nested.mkdir()
            (nested / "README.md").write_text(
                "# Eval\n\n| verify_envelope | 5 | 1.0 |\n", encoding="utf-8")
            found = list(lint.md_files([d]))
            self.assertEqual([p.name for p in found], ["README.md"])
            lines = found[0].read_text(encoding="utf-8").splitlines()
            self.assertIsNotNone(lint.offending(lines[2], NAMES))

    def test_skip_dirs_are_relative_to_the_scan_root(self):
        # A checkout that happens to live under a directory named `build` is
        # still scanned; only a `build/` INSIDE the tree is skipped.
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / "build" / "repo"
            (root / "node_modules").mkdir(parents=True)
            (root / "a.md").write_text("x\n", encoding="utf-8")
            (root / "node_modules" / "b.md").write_text("x\n", encoding="utf-8")
            self.assertEqual([p.name for p in lint.md_files([root])], ["a.md"])


if __name__ == "__main__":
    unittest.main()
