"""Tests for scripts/agent_context_budget.py — the gate on what an agent loads at start.

Pinned: the root CLAUDE.md and the files it @imports (not ones mentioned in a
code fence) count toward the start budget, as does an unscoped .claude/rules
file; a `paths:`-scoped rule and a nested AGENTS.md / CLAUDE.md count per
file; --check fails on either budget and passes under both; an @import of a
missing file is a refusal (exit 2), not a pass.
"""
import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "agent_context_budget.py"
spec = importlib.util.spec_from_file_location("agent_context_budget", SCRIPT)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def write(root: Path, path: str, size: int, head: str = ""):
    p = root / path
    p.parent.mkdir(parents=True, exist_ok=True)
    body = head + "x" * max(0, size - len(head))
    p.write_text(body)
    return p


class Budget(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        write(self.root, "CLAUDE.md", 400, "# root\n\n@AGENTS.md\n\n```\n@not/an/import.md\n```\n")
        write(self.root, "AGENTS.md", 1000)

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                rc = mod.main(["--root", str(self.root), *argv])
            except SystemExit as e:
                rc = e.code
        return rc, out.getvalue(), err.getvalue()

    def test_imports_count_at_start_and_fences_do_not(self):
        rows = mod.inventory(self.root)
        start = {mod.rel(p) if p.is_relative_to(mod.REPO) else p.name for w, p, _ in rows if w == "start"}
        self.assertEqual(start, {"CLAUDE.md", "AGENTS.md"})

    def test_under_budget_passes(self):
        write(self.root, "src/AGENTS.md", 500)
        write(self.root, ".claude/rules/scoped.md", 500, "---\npaths:\n  - 'src/**'\n---\n")
        rc, out, _ = self.run_main("--check")
        self.assertEqual(rc, 0, out)
        self.assertIn("touch", out)

    def test_a_fat_nested_brief_fails_check(self):
        write(self.root, "firmware/AGENTS.md", mod.BUDGET_LAZY + 1)
        rc, out, err = self.run_main("--check")
        self.assertEqual(rc, 1)
        self.assertIn("firmware/AGENTS.md", err)
        self.assertEqual(self.run_main()[0], 0)  # without --check the table is informational

    def test_an_unscoped_rule_counts_at_start(self):
        write(self.root, ".claude/rules/always.md", mod.BUDGET_START)
        rc, _, err = self.run_main("--check")
        self.assertEqual(rc, 1)
        self.assertIn("session start loads", err)

    def test_a_missing_import_is_a_refusal(self):
        (self.root / "AGENTS.md").unlink()
        rc, _, _ = self.run_main("--check")
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
