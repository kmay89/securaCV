#!/usr/bin/env python3
"""scripts/check_hacs_store_links.py — the HACS store page's links land here.

The script lays the mirror's four files out as the mirror has them and runs
the mirror's own checker with --source pointing at this checkout (sweep
HA35). The checker is the mirror's, so these tests drive the plumbing with a
stand-in checker in a synthetic mirror directory (--from), never the network:

- the checker is run with --source <the checkout>, from a layout where its
  ROOT (two levels up) holds the three prose files, and its exit code is the
  script's, so a link that does not land fails the step;
- a checker with no --source mode (the mirror's main before
  securacv-homeassistant#28) is NOT run: the script warns and passes;
- a mirror file that cannot be read is exit 2, never a pass;
- lint.yml runs the script, unfiltered, with no token left in .git/config.

The live run against the real mirror and a scratch rename of a linked
heading is recorded in the sweep's Done text for HA35; it needs the network.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_hacs_store_links.py"
LINT = REPO / ".github" / "workflows" / "lint.yml"

# A stand-in checker with the real one's shape: argparse with --source, ROOT
# two levels up, the three prose files read from ROOT. It "checks" one link:
# README.md names a file that must exist under --source.
CHECKER = textwrap.dedent('''\
    import argparse, sys
    from pathlib import Path
    ROOT = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path)
    a = ap.parse_args()
    for name in ("README.md", "AGENTS.md", "CLAUDE.md"):
        assert (ROOT / name).is_file(), name
    want = (ROOT / "README.md").read_text().strip()
    ok = (a.source / want).is_file()
    print(("OK " if ok else "MISSING ") + want + " in " + str(a.source))
    sys.exit(0 if ok else 1)
''')
OLD_CHECKER = "import sys\nprint('prose OK')\nsys.exit(1)\n"


def mirror(root: Path, readme: str, checker: str = CHECKER) -> Path:
    (root / ".github" / "scripts").mkdir(parents=True)
    (root / "README.md").write_text(readme + "\n", encoding="utf-8")
    (root / "AGENTS.md").write_text("agents\n", encoding="utf-8")
    (root / "CLAUDE.md").write_text("claude\n", encoding="utf-8")
    (root / ".github" / "scripts" / "lint_readme.py").write_text(
        checker, encoding="utf-8")
    return root


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), *args],
                          capture_output=True, text=True)


class Plumbing(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.source = self.dir / "monorepo"
        (self.source / "docs").mkdir(parents=True)
        (self.source / "docs" / "setup.md").write_text("# Setup\n")

    def tearDown(self):
        self.tmp.cleanup()

    def test_a_link_that_lands_passes(self):
        m = mirror(self.dir / "m", "docs/setup.md")
        r = run("--from", str(m), "--source", str(self.source))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f"OK docs/setup.md in {self.source}", r.stdout)

    def test_a_link_that_does_not_land_fails(self):
        m = mirror(self.dir / "m", "docs/renamed.md")
        r = run("--from", str(m), "--source", str(self.source))
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("MISSING docs/renamed.md", r.stdout)

    def test_a_checker_without_source_mode_is_not_run(self):
        # The pre-#28 checker ignores --source and would lint the prose's
        # relative links against an empty directory; it must not run.
        m = mirror(self.dir / "m", "docs/setup.md", OLD_CHECKER)
        r = run("--from", str(m), "--source", str(self.source))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("::warning title=HACS store page links not checked::",
                      r.stdout)
        self.assertNotIn("prose OK", r.stdout)

    def test_an_unreadable_mirror_is_exit_2(self):
        m = mirror(self.dir / "m", "docs/setup.md")
        (m / "CLAUDE.md").unlink()
        r = run("--from", str(m), "--source", str(self.source))
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("CLAUDE.md: no such file", r.stderr)


class Wiring(unittest.TestCase):
    def test_lint_yml_runs_it_without_a_token(self):
        wf = yaml.safe_load(LINT.read_text(encoding="utf-8"))
        # lint.yml is unfiltered: every PR runs it, whatever it touches.
        on = wf.get("on", wf.get(True))
        self.assertNotIn("paths", on["pull_request"])
        job = wf["jobs"]["hacs-store-page-links"]
        steps = job["steps"]
        checkout = [s for s in steps
                    if str(s.get("uses", "")).startswith("actions/checkout@")]
        self.assertEqual(len(checkout), 1)
        self.assertIs(checkout[0].get("with", {}).get("persist-credentials"),
                      False)
        runs = [str(s.get("run", "")).strip() for s in steps]
        self.assertIn("python3 scripts/check_hacs_store_links.py", runs)


if __name__ == "__main__":
    unittest.main()
