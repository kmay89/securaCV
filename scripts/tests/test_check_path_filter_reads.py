#!/usr/bin/env python3
"""Tests for scripts/check_path_filter_reads.py and its two read recorders.

The gate answers one question — "is every file this job's tests read inside
its path filter?" — and each half of it can answer wrong in a way that reads
as green:

* the MATCHER, if it gets GitHub's pattern rules wrong. `*` stopping at `/`,
  `**/` matching zero directories, `?` meaning "the previous character,
  optional" (not "any one character"), and a later `!` pattern re-excluding
  are all rules a hand-rolled glob gets wrong, and a matcher that says
  "covered" for a path GitHub would not run on is the exact false green the
  gate exists to remove. The cheat-sheet examples from GitHub's docs are
  pinned below.
* the RECORDERS, if they miss a read or invent one. The first run of the
  Python half reported "ssl", "frigate" and "a:b" at the repo root: a
  TemporaryDirectory's cleanup opens entries relative to a directory fd, and
  the audit event carries only the bare name. That is pinned, as are the
  things that must never be recorded (writes, paths outside the repo) and
  the attribution that makes the message useful (a generator a Node test
  spawns is charged to the test; `unittest discover` charges each read to
  the module that made it).
* the CHECKER, if a suite that recorded nothing passes as "no reads outside".

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
The recorder tests start real `node` and `python3` processes.
"""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "check_path_filter_reads", REPO / "scripts" / "check_path_filter_reads.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)

RECORDER = REPO / "scripts" / "path_filter_reads"


def flt(*patterns: str, ignore: bool = False):
    return gate.PathFilter(list(patterns), ignore=ignore)


class GitHubPatternRules(unittest.TestCase):
    """The filter-pattern cheat sheet in GitHub's workflow syntax docs."""

    def test_star_stops_at_a_slash(self):
        f = flt("docs/*")
        self.assertTrue(f.covers_file("docs/README.md"))
        self.assertFalse(f.covers_file("docs/mona/octocat.txt"))
        self.assertFalse(flt("*.js").covers_file("src/app.js"))
        self.assertTrue(flt("*.js").covers_file("app.js"))

    def test_double_star_crosses_directories(self):
        f = flt("docs/**")
        self.assertTrue(f.covers_file("docs/README.md"))
        self.assertTrue(f.covers_file("docs/mona/octocat.txt"))
        self.assertFalse(f.covers_file("src/docs/README.md"))
        self.assertTrue(flt("**.js").covers_file("src/js/app.js"))

    def test_double_star_slash_matches_zero_directories(self):
        f = flt("**/README.md")
        self.assertTrue(f.covers_file("README.md"))
        self.assertTrue(f.covers_file("js/README.md"))
        g = flt("docs/**/*.md")
        self.assertTrue(g.covers_file("docs/README.md"))
        self.assertTrue(g.covers_file("docs/mona/hello-world.md"))
        self.assertTrue(g.covers_file("docs/a/markdown/file.md"))
        self.assertFalse(g.covers_file("docs/a/file.txt"))
        self.assertTrue(flt("**/docs/**").covers_file("docs/hello.md"))
        self.assertTrue(flt("**/docs/**").covers_file("dir/docs/my-file.txt"))
        self.assertTrue(flt("**/*src/**").covers_file("a/src/app.js"))
        self.assertTrue(flt("**/*src/**").covers_file("my-src/code/js/app.js"))

    def test_question_mark_makes_the_previous_character_optional(self):
        f = flt("*.jsx?")
        self.assertTrue(f.covers_file("page.js"))
        self.assertTrue(f.covers_file("page.jsx"))
        self.assertFalse(f.covers_file("page.jsxx"))
        # It is NOT "any one character" (the shell's meaning).
        self.assertFalse(f.covers_file("page.jsz"))

    def test_plus_is_one_or_more_of_the_previous_character(self):
        f = flt("ab+c.txt")
        self.assertTrue(f.covers_file("abc.txt"))
        self.assertTrue(f.covers_file("abbbc.txt"))
        self.assertFalse(f.covers_file("ac.txt"))

    def test_brackets_list_characters_and_ranges(self):
        self.assertTrue(flt("[CB]at").covers_file("Cat"))
        self.assertTrue(flt("[CB]at").covers_file("Bat"))
        self.assertFalse(flt("[CB]at").covers_file("Hat"))
        self.assertTrue(flt("[1-2]00").covers_file("200"))
        self.assertFalse(flt("[1-2]00").covers_file("300"))

    def test_a_later_bang_pattern_excludes_and_a_later_positive_reincludes(self):
        f = flt("*.md", "!README.md")
        self.assertTrue(f.covers_file("hello.md"))
        self.assertFalse(f.covers_file("README.md"))
        g = flt("*.md", "!README.md", "README*")
        self.assertTrue(g.covers_file("README.md"))
        self.assertTrue(g.covers_file("README.doc"))

    def test_backslash_escapes_a_special_character(self):
        f = flt("docs/\\*.md")
        self.assertTrue(f.covers_file("docs/*.md"))
        self.assertFalse(f.covers_file("docs/a.md"))

    def test_regex_metacharacters_are_literal(self):
        self.assertTrue(flt("a.b(c)").covers_file("a.b(c)"))
        self.assertFalse(flt("a.b").covers_file("axb"))

    def test_paths_ignore_inverts(self):
        f = flt("docs/**", ignore=True)
        self.assertFalse(f.covers_file("docs/a.md"))
        self.assertTrue(f.covers_file("src/a.rs"))
        self.assertFalse(f.covers_dir("docs"))
        self.assertTrue(f.covers_dir("src"))


class DirectoryCoverage(unittest.TestCase):
    """A listed or stat'ed directory is covered when a pattern reaches inside."""

    def test_a_single_star_segment_reaches_through(self):
        f = flt("firmware/boards/*/pins/pins.h")
        self.assertTrue(f.covers_dir("firmware"))
        self.assertTrue(f.covers_dir("firmware/boards"))
        self.assertTrue(f.covers_dir("firmware/boards/xiao"))
        self.assertTrue(f.covers_dir("firmware/boards/xiao/pins"))
        self.assertFalse(f.covers_dir("firmware/boards/xiao/src"))
        self.assertFalse(f.covers_dir("firmware/common"))
        # the file itself is not a directory the pattern reaches into
        self.assertFalse(f.covers_dir("firmware/boards/xiao/pins/pins.h"))

    def test_a_name_glob_reaches_its_directory(self):
        f = flt("docs/hardware/bom_*.csv")
        self.assertTrue(f.covers_dir("docs/hardware"))
        self.assertFalse(f.covers_dir("docs/design"))

    def test_a_tree_glob_covers_its_own_root(self):
        self.assertTrue(flt("canary-local/**").covers_dir("canary-local"))
        self.assertTrue(flt("canary-local/**").covers_dir("canary-local/a/b"))
        self.assertFalse(flt("canary-local/**").covers_dir("canary"))

    def test_a_bang_pattern_reaches_nothing(self):
        self.assertFalse(flt("!docs/**").covers_dir("docs"))


WORKFLOW = """\
name: demo
on:
  push:
    branches: [main]
    paths:
      - "src/**"
      - "tests/**"
      - "tools/**"
      - ".github/workflows/demo.yml"
  pull_request:
    paths:
      - "src/**"
      - "tests/**"
      - "tools/**"
      # a comment between entries
      - ".github/workflows/demo.yml"

permissions:
  contents: read

jobs:
  logic:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v7
      - name: node suites
        run: |
          node --test tests/a.test.js
          # node --test tests/commented.test.js
          node --test --test-reporter=dot tests/b.test.mjs
      - name: python suites
        run: |
          python3 -m unittest discover -p 'test_*.py' \\
            -s tools/tests
  other:
    runs-on: ubuntu-latest
    steps:
      - run: node --test tests/elsewhere.test.js
"""


class Checker(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / ".github/workflows").mkdir(parents=True)
        self.wf = self.root / ".github/workflows/demo.yml"
        self.wf.write_text(WORKFLOW)
        (self.root / "tools/tests").mkdir(parents=True)
        (self.root / "tools/tests/test_one.py").write_text("")
        (self.root / "tools/tests/helper.py").write_text("")
        (self.root / "docs/sub").mkdir(parents=True)
        self.reads = self.root / "reads"
        self.reads.mkdir()

    def tearDown(self):
        self._tmp.cleanup()

    def record(self, *recs: dict) -> None:
        with open(self.reads / f"r{len(list(self.reads.iterdir()))}.jsonl", "w") as fh:
            for r in recs:
                fh.write(json.dumps(r) + "\n")

    def all_suites_heard(self) -> None:
        self.record(
            {"suite": "tests/a.test.js", "op": "exec", "path": "tests/a.test.js"},
            {"suite": "tests/b.test.mjs", "op": "exec", "path": "tests/b.test.mjs"},
            {"suite": "tools/tests/test_one.py", "op": "open", "path": "src/x.py"},
        )

    def run_check(self):
        return gate.check(self.root, self.wf, "logic", self.reads)

    def test_the_job_suites_are_read_from_its_run_blocks(self):
        wf = gate.yaml.safe_load(WORKFLOW)
        node, discover = gate.job_suites(wf, "logic")
        self.assertEqual(node, ["tests/a.test.js", "tests/b.test.mjs"])
        self.assertEqual(discover, [("tools/tests", "test_*.py")])
        self.assertEqual(
            gate.expected_files(self.root, node, discover),
            ["tests/a.test.js", "tests/b.test.mjs", "tools/tests/test_one.py"])

    def test_green_when_every_read_is_covered(self):
        self.all_suites_heard()
        self.record({"suite": "tests/a.test.js", "op": "readFileSync", "path": "src/lib.js"},
                    {"suite": "tests/a.test.js", "op": "readdirSync", "path": "src"})
        problems, summary = self.run_check()
        self.assertEqual(problems, [])
        self.assertIn("3 suites", summary)

    def test_a_read_outside_names_test_file_line_and_the_line_to_add(self):
        self.all_suites_heard()
        self.record({"suite": "tests/a.test.js", "op": "existsSync", "path": "docs/guide.md",
                     "at": "tests/a.test.js:42", "proc": "tests/a.test.js"})
        problems, _ = self.run_check()
        self.assertEqual(len(problems), 1, problems)
        p = problems[0]
        self.assertIn("docs/guide.md", p)
        self.assertIn("tests/a.test.js (existsSync at tests/a.test.js:42)", p)
        self.assertIn('- "docs/guide.md"', p)
        self.assertIn("BOTH on.push.paths and on.pull_request.paths", p)
        # the annotation points at the last pull_request paths entry
        last = WORKFLOW.splitlines().index('      - ".github/workflows/demo.yml"', 10) + 1
        self.assertIn(f"file=.github/workflows/demo.yml,line={last}::", p)

    def test_a_spawned_generator_is_charged_to_its_test(self):
        self.all_suites_heard()
        self.record({"suite": "tests/b.test.mjs", "op": "open", "path": "docs/table.md",
                     "at": "tools/gen.py:7", "proc": "tools/gen.py"})
        problems, _ = self.run_check()
        self.assertEqual(len(problems), 1)
        self.assertIn("tests/b.test.mjs (open at tools/gen.py:7, via tools/gen.py)", problems[0])

    def test_a_listed_directory_outside_asks_for_its_tree(self):
        self.all_suites_heard()
        self.record({"suite": "tests/a.test.js", "op": "readdirSync", "path": "docs/sub"},
                    {"suite": "tests/a.test.js", "op": "statSync", "path": "docs"})
        problems, _ = self.run_check()
        self.assertEqual(len(problems), 2, problems)
        self.assertIn('- "docs/sub/**"', problems[1])
        self.assertIn('- "docs/**"', problems[0])  # a directory on disk, stat'ed

    def test_a_suite_that_left_no_record_fails(self):
        self.record({"suite": "tests/a.test.js", "op": "exec", "path": "tests/a.test.js"})
        problems, _ = self.run_check()
        silent = sorted(p.split("::")[2].split(" ")[0] for p in problems)
        self.assertEqual(silent, ["tests/b.test.mjs", "tools/tests/test_one.py"])
        self.assertIn("left no read record", problems[0])

    def test_a_python_module_heard_only_through_its_import_counts(self):
        self.record(
            {"suite": "tests/a.test.js", "op": "exec", "path": "tests/a.test.js"},
            {"suite": "tests/b.test.mjs", "op": "exec", "path": "tests/b.test.mjs"},
            {"suite": "python3 -m unittest discover", "op": "open",
             "path": "tools/tests/test_one.py"},
        )
        problems, _ = self.run_check()
        self.assertEqual(problems, [])

    def test_a_bytecode_read_is_charged_to_its_source(self):
        # A cached module is imported from its .pyc alone (the .py is only
        # stat'ed). Allowing __pycache__ as "output" hid scripts/bom_pricing.py.
        self.all_suites_heard()
        self.record({"suite": "tools/tests/test_one.py", "op": "open",
                     "path": "scripts/__pycache__/bom_pricing.cpython-311.pyc"},
                    {"suite": "tools/tests/test_one.py", "op": "open",
                     "path": "src/__pycache__/ok.cpython-312.opt-1.pyc"})
        problems, _ = self.run_check()
        self.assertEqual(len(problems), 1, problems)
        self.assertIn('- "scripts/bom_pricing.py"', problems[0])
        self.assertEqual(gate.source_of("a/__pycache__/m.cpython-311.pyc"), "a/m.py")
        self.assertEqual(gate.source_of("__pycache__/m.cpython-311.pyc"), "m.py")
        self.assertEqual(gate.source_of("a/m.pyc"), "a/m.pyc")

    def test_a_test_module_imported_from_its_cache_is_heard(self):
        self.record(
            {"suite": "tests/a.test.js", "op": "exec", "path": "tests/a.test.js"},
            {"suite": "tests/b.test.mjs", "op": "exec", "path": "tests/b.test.mjs"},
            {"suite": "python3 -m unittest discover", "op": "open",
             "path": "tools/tests/__pycache__/test_one.cpython-311.pyc"},
        )
        problems, _ = self.run_check()
        self.assertEqual(problems, [])

    def test_a_torn_last_line_is_skipped_not_fatal(self):
        self.all_suites_heard()
        (self.reads / "torn.jsonl").write_text('{"suite": "x", "op": "rea')
        problems, _ = self.run_check()
        self.assertEqual(problems, [])

    def test_a_job_with_no_recognized_suite_is_not_a_pass(self):
        wf = self.root / ".github/workflows/empty.yml"
        wf.write_text(WORKFLOW.replace("node --test", "echo").replace("python3 -m unittest", "true"))
        problems, _ = gate.check(self.root, wf, "logic", self.reads)
        self.assertTrue(any("runs no suite" in p for p in problems), problems)

    def test_main_refuses_a_missing_recording_dir(self):
        rc = gate.main(["--workflow", str(self.wf), "--job", "logic",
                        "--reads", str(self.root / "nowhere")])
        self.assertEqual(rc, 1)

    def test_the_real_workflow_names_every_suite_it_runs(self):
        # canary-local.yml's logic-tests job: every `node --test` line and the
        # tools discover are what the gate expects records for.
        wf = gate.yaml.safe_load((REPO / ".github/workflows/canary-local.yml").read_text())
        node, discover = gate.job_suites(wf, "logic-tests")
        self.assertIn("canary-local/tests/desktop_parity.test.js", node)
        self.assertIn("canary-local/tests/figures.test.js", node)  # the Witness Wall step's
        self.assertEqual(discover, [("canary-local/tools/tests", "test_*.py")])
        self.assertIsNotNone(gate.pr_filter(wf))


def _need(tool: str) -> str:
    path = shutil.which(tool)
    if not path:
        raise AssertionError(f"{tool} is not on PATH; the recorder tests run real processes")
    return path


class Recorders(unittest.TestCase):
    """The two recorders, run for real in a scratch repo that carries a copy."""

    @classmethod
    def setUpClass(cls):
        _need("node")
        cls._tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls._tmp.name) / "repo"
        rec = cls.root / "scripts/path_filter_reads"
        rec.mkdir(parents=True)
        for name in ("record.cjs", "sitecustomize.py"):
            shutil.copy(RECORDER / name, rec / name)
        files = {
            "data/a.txt": "a", "data/b.txt": "b", "data/c.txt": "c", "data/d.txt": "d",
            "data/e.txt": "e", "data/f.txt": "f", "lib/m.mjs": "export const m = 1;\n",
            "t/run.js": textwrap.dedent("""\
                const fs = require("node:fs");
                const os = require("node:os");
                const path = require("node:path");
                const { spawnSync } = require("node:child_process");
                const out = {};
                out.a = fs.readFileSync("data/a.txt", "utf8");
                const read = (p) => fs.readFileSync(p, "utf8");
                out.e = read("data/e.txt");
                out.missing = fs.existsSync("data/missing.txt");
                out.listing = fs.readdirSync("data").length;
                out.bsize = fs.statSync(path.join(__dirname, "..", "data/b.txt")).size;
                fs.createReadStream("data/f.txt").close();
                fs.writeFileSync("out/w.txt", "w");
                fs.closeSync(fs.openSync("out/w2.txt", "w"));
                const tmp = path.join(os.tmpdir(), `pfr-${process.pid}.txt`);
                fs.writeFileSync(tmp, "x"); fs.readFileSync(tmp); fs.unlinkSync(tmp);
                const r = spawnSync("python3", ["py/gen.py"], { encoding: "utf8" });
                out.child = r.status;
                out.esm = spawnSync(process.execPath, ["t/esm.mjs"], { encoding: "utf8" }).stdout;
                import("../lib/m.mjs").then((mod) => {
                  out.m = mod.m;
                  process.stdout.write(JSON.stringify(out));
                });
                """),
            "t/esm.mjs": textwrap.dedent("""\
                import { readFileSync, existsSync } from "node:fs";
                process.stdout.write(readFileSync("data/d.txt", "utf8") + existsSync("data/nope"));
                """),
            "py/gen.py": textwrap.dedent("""\
                import os, tempfile
                open("data/c.txt").read()
                os.listdir("data")
                with open("out/p.txt", "w") as fh:
                    fh.write("p")
                with tempfile.TemporaryDirectory(dir="out") as d:
                    os.mkdir(os.path.join(d, "ssl"))
                    os.mkdir(os.path.join(d, "ssl", "inner"))
                """),
            "tt/test_first.py": textwrap.dedent("""\
                import subprocess, sys, unittest
                class T(unittest.TestCase):
                    def test_reads(self):
                        open("data/d.txt").read()
                        subprocess.run([sys.executable, "py/child.py"], check=True)
                """),
            "tt/test_second.py": textwrap.dedent("""\
                import unittest
                class T(unittest.TestCase):
                    def test_reads(self):
                        open("data/e.txt").read()
                """),
            "py/child.py": "open('data/b.txt').read()\n",
        }
        for rel, text in files.items():
            p = cls.root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text)
        (cls.root / "out").mkdir()

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def env(self, reads: Path | None) -> dict:
        env = dict(os.environ)
        env.pop("PATH_FILTER_READS_SUITE", None)
        env.pop("PATH_FILTER_READS_DIR", None)
        env["NODE_OPTIONS"] = f"--require {self.root / 'scripts/path_filter_reads/record.cjs'}"
        env["PYTHONPATH"] = str(self.root / "scripts/path_filter_reads")
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        if reads is not None:
            env["PATH_FILTER_READS_DIR"] = str(reads)
        return env

    def run_recorded(self, argv: list[str]) -> tuple[subprocess.CompletedProcess, list[dict]]:
        with tempfile.TemporaryDirectory() as reads:
            r = subprocess.run(argv, cwd=self.root, env=self.env(Path(reads) / "made"),
                               capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return r, gate.load_records(Path(reads) / "made")

    def test_node_reads_are_recorded_and_results_unchanged(self):
        r, recs = self.run_recorded(["node", "t/run.js"])
        self.assertEqual(json.loads(r.stdout),
                         {"a": "a", "e": "e", "missing": False, "listing": 6, "bsize": 1,
                          "child": 0, "esm": "dfalse", "m": 1})
        got = {(x["op"], x["path"]) for x in recs if x["proc"] == "t/run.js"}
        for want in [("readFileSync", "data/a.txt"), ("existsSync", "data/missing.txt"),
                     ("readdirSync", "data"), ("statSync", "data/b.txt"),
                     ("createReadStream", "data/f.txt"), ("readFileSync", "lib/m.mjs"),
                     ("exec", "t/run.js")]:
            self.assertIn(want, got)
        paths = {x["path"] for x in recs}
        # writes are not reads, and nothing outside the repo is kept
        self.assertFalse({"out/w.txt", "out/w2.txt", "out/p.txt"} & paths, paths)
        self.assertFalse(any(p.startswith("..") or p.startswith("/") for p in paths))
        # an ES module's named fs imports are the wrapped calls too
        esm = {(x["op"], x["path"]) for x in recs if x["proc"] == "t/esm.mjs"}
        self.assertLessEqual({("readFileSync", "data/d.txt"), ("existsSync", "data/nope")}, esm)
        self.assertEqual({x["suite"] for x in recs if x["proc"] == "t/esm.mjs"}, {"t/run.js"})
        # `at` is the suite's own line; through a helper, the line that called it
        src = (self.root / "t/run.js").read_text().splitlines()
        line = lambda needle: next(i for i, t in enumerate(src, 1) if needle in t)
        at = {x["path"]: x["at"] for x in recs if x["op"] == "readFileSync"}
        self.assertEqual(at["data/a.txt"], f"t/run.js:{line('data/a.txt')}")
        called = line('read("data/e.txt")')
        self.assertEqual(at["data/e.txt"], f"t/run.js:{called}")

    def test_a_python_child_is_charged_to_the_node_script_that_ran_it(self):
        _, recs = self.run_recorded(["node", "t/run.js"])
        child = [x for x in recs if x["proc"] == "py/gen.py"]
        self.assertTrue(child)
        self.assertEqual({x["suite"] for x in child}, {"t/run.js"})
        self.assertIn(("open", "data/c.txt"), {(x["op"], x["path"]) for x in child})
        self.assertIn(("listdir", "data"), {(x["op"], x["path"]) for x in child})
        self.assertEqual({x["at"] for x in child if x["path"] == "data/c.txt"}, {"py/gen.py:2"})

    def test_a_tmpdir_cleanup_reports_no_bare_names(self):
        # rmtree opens "ssl" and "inner" relative to a directory fd; resolved
        # against the cwd they would read as repo-root files.
        _, recs = self.run_recorded(["node", "t/run.js"])
        paths = {x["path"] for x in recs}
        self.assertFalse({"ssl", "inner"} & paths, paths)

    def test_unittest_discover_charges_each_read_to_its_module(self):
        _, recs = self.run_recorded([sys.executable, "-m", "unittest", "discover",
                                     "-s", "tt", "-p", "test_*.py"])
        by = {(x["path"], x["suite"]) for x in recs}
        self.assertIn(("data/d.txt", "tt/test_first.py"), by)
        self.assertIn(("data/e.txt", "tt/test_second.py"), by)
        # the module's own import is heard, through the runner
        self.assertIn("tt/test_second.py", {x["path"] for x in recs})
        # a child a test module starts is that module's
        self.assertIn(("data/b.txt", "tt/test_first.py"), by)
        at = {x["path"]: x["at"] for x in recs if x["suite"] == "tt/test_first.py"}
        self.assertEqual(at["data/d.txt"], "tt/test_first.py:4")

    def test_inert_without_the_directory_variable(self):
        probe = ['node', '-e', 'process.stdout.write(String(require("fs").readFileSync)'
                 '.includes("note(name") ? "wrapped" : "plain")']
        with tempfile.TemporaryDirectory() as reads:
            armed = subprocess.run(probe, cwd=self.root, env=self.env(Path(reads)),
                                   capture_output=True, text=True, timeout=60)
        bare = subprocess.run(probe, cwd=self.root, env=self.env(None),
                              capture_output=True, text=True, timeout=60)
        self.assertEqual((armed.stdout, bare.stdout), ("wrapped", "plain"),
                         armed.stderr + bare.stderr)


if __name__ == "__main__":
    unittest.main()
