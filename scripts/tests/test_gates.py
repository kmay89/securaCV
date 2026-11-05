"""Tests for scripts/gates.py — the gate list is derived, and --run proves it.

Pinned, on a fake .github/workflows tree: an unfiltered pull_request workflow
runs on every diff, a `paths` one only on a match, a `paths-ignore` one on
anything outside its ignores, and a push-only one never; a step is fast only
when every non-install line is python3/node/bash/sh with no toolchain, runner
variable, secret or `if:`; install lines are dropped from the command; a
step's working-directory and literal env reach the process; identical
commands run once; a failing fast gate makes --run exit 1 with its output;
an unparseable workflow and a missing diff base are refusals (exit 2).
Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
import contextlib
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "gates.py"
spec = importlib.util.spec_from_file_location("gates", SCRIPT)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

ALWAYS = """
name: Always
on:
  pull_request:
jobs:
  a:
    steps:
      - name: one
        run: python3 -c 'print(1)'
      - name: after install
        run: |
          python3 -m pip install --quiet something
          python3 -c 'print(2)'
      - uses: actions/checkout@v4
      - name: toolchain
        run: cargo test
      - name: conditional
        if: github.event_name == 'push'
        run: python3 -c 'print(3)'
      - name: scoped
        working-directory: sub
        env:
          FOO: bar
          BAZ: ${{ secrets.X }}
        run: python3 -c "import os,sys; sys.exit(0 if os.environ.get('FOO')=='bar' and os.path.basename(os.getcwd())=='sub' else 5)"
      - name: runner only
        run: echo "x=1" >> "$GITHUB_OUTPUT"
"""
FILTERED = "on:\n  pull_request:\n    paths: ['src/**']\njobs:\n  f:\n    steps:\n      - run: python3 -c 'print(1)'\n"
IGNORED = "on:\n  pull_request:\n    paths-ignore: ['docs/**']\njobs:\n  i:\n    steps:\n      - run: node -e '0'\n"
NOPR = "on:\n  push:\njobs:\n  p:\n    steps:\n      - run: python3 -c 'print(1)'\n"
FAILING = ("on:\n  pull_request:\njobs:\n  z:\n    steps:\n      - name: boom\n        run: |\n"
           "          pip install needed-thing\n"
           "          python3 -c 'import sys; print(\"why it broke\"); sys.exit(3)'\n")


class Gates(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.wf = self.root / ".github" / "workflows"
        self.wf.mkdir(parents=True)
        (self.root / "sub").mkdir()
        for name, text in (("always.yml", ALWAYS), ("filtered.yml", FILTERED),
                           ("ignored.yml", IGNORED), ("nopr.yml", NOPR)):
            (self.wf / name).write_text(text)

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                rc = mod.main(["--root", str(self.root), *argv])
            except SystemExit as e:
                rc = e.code
        return rc, out.getvalue(), err.getvalue()

    def triggered(self, *paths):
        rc, out, _ = self.run_main("--paths", *paths)
        self.assertEqual(rc, 0, out)
        return {ln.split()[0] for ln in out.splitlines() if " job(s) " in ln}

    def test_which_workflows_run_is_the_path_filters_answer(self):
        self.assertEqual(self.triggered("docs/x.md"), {"always.yml"})
        self.assertEqual(self.triggered("src/a.rs"), {"always.yml", "filtered.yml", "ignored.yml"})
        self.assertEqual(self.triggered("README.md"), {"always.yml", "ignored.yml"})

    def test_step_classification(self):
        wf = mod.Workflow(self.wf / "always.yml")
        kinds = [(s.name, s.kind) for s in wf.steps]
        self.assertEqual(kinds, [("one", "fast"), ("after install", "fast"), ("toolchain", "manual"),
                                 ("conditional", "manual"), ("scoped", "fast"), ("runner only", "manual")])
        after = wf.steps[1]
        self.assertEqual(after.command, "python3 -c 'print(2)'")  # the install line is dropped
        scoped = wf.steps[4]
        self.assertEqual((scoped.cwd, scoped.env), ("sub", {"FOO": "bar"}))  # the secret never reaches the env
        self.assertIn("cargo", wf.steps[2].why)
        self.assertIn("GITHUB_", wf.steps[5].why)

    def test_run_executes_fast_steps_where_and_how_the_workflow_says(self):
        rc, out, _ = self.run_main("--paths", "x", "--run")
        self.assertEqual(rc, 0, out)
        self.assertIn("ok   always.yml › a › scoped", out)
        self.assertIn("4 of 4 fast gates passed", out)  # always.yml's three and ignored.yml's one

    def test_a_failing_gate_is_reported_with_its_output_and_fails_the_run(self):
        (self.wf / "failing.yml").write_text(FAILING)
        rc, out, _ = self.run_main("--paths", "x", "--run")
        self.assertEqual(rc, 1)
        self.assertIn("FAIL failing.yml › z › boom", out)
        self.assertIn("| why it broke", out)
        self.assertIn("installs skipped here, run it once if that is what failed: pip install needed-thing", out)
        self.assertIn("4 of 5 fast gates passed; 1 FAILED: boom", out)

    def test_identical_commands_run_once(self):
        (self.wf / "dupe.yml").write_text(ALWAYS.replace("name: Always", "name: Dupe"))
        rc, out, _ = self.run_main("--paths", "x")
        self.assertEqual(out.count("python3 -c 'print(1)'"), 1)

    def test_refusals(self):
        (self.wf / "broken.yml").write_text("on: [\n")
        rc, _, err = self.run_main("--paths", "x")
        self.assertEqual(rc, 2)
        self.assertIn("broken.yml", err)
        (self.wf / "broken.yml").unlink()
        rc, _, err = self.run_main()  # no git repository here, so no base
        self.assertEqual(rc, 2)
        self.assertIn("no base", err)


if __name__ == "__main__":
    unittest.main()
