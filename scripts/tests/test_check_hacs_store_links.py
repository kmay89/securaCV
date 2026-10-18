#!/usr/bin/env python3
"""scripts/check_hacs_store_links.py — the HACS store page's links land here.

The script lays the mirror's four files out as the mirror has them and runs
the mirror's own checker with --source pointing at this checkout (sweep
HA35). The checker is the mirror's, so these tests drive the plumbing with a
stand-in checker, never the real network: a synthetic mirror directory
(--from) for the run path, and a local HTTP server standing in for
raw.githubusercontent.com for the fetch path.

- the checker is run isolated (-I) with --source <the checkout>, from a
  layout where its ROOT (two levels up) holds the three prose files, and a
  link that does not land fails the step (exit 1, with the rename hint);
- the script fails closed: an exit 0 counts only with the checker's own
  `links into kmay89/securaCV OK — <n> resolve` line, n at least 1, so a
  checker older than securacv-homeassistant#28 (no --source mode), a silent
  one, or one that checked nothing is exit 2, never a pass;
- a mirror file that cannot be fetched (after the retries) or read, or
  comes back empty, is exit 2;
- lint.yml runs the script, unfiltered, with no token left in .git/config.

The live run against the real mirror and a scratch rename of a linked
heading is recorded in the sweep's Done text for HA35; it needs the network.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import contextlib
import http.server
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import textwrap
import threading
import unittest
from pathlib import Path
from unittest import mock

import yaml

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_hacs_store_links.py"
LINT = REPO / ".github" / "workflows" / "lint.yml"

# A stand-in checker with the real one's shape: argparse with --source, ROOT
# two levels up, the three prose files read from ROOT, and the real
# checker's summary line on success. It "checks" one link: README.md names
# a file that must exist under --source. It also reports whether it was run
# isolated (python -I).
CHECKER = textwrap.dedent('''\
    import argparse, sys
    from pathlib import Path
    ROOT = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path)
    a = ap.parse_args()
    for name in ("README.md", "AGENTS.md", "CLAUDE.md"):
        assert (ROOT / name).is_file(), name
    print("isolated=%d" % sys.flags.isolated)
    want = (ROOT / "README.md").read_text().strip()
    if not (a.source / want).is_file():
        print("MISSING " + want + " in " + str(a.source), file=sys.stderr)
        sys.exit(1)
    print("links into kmay89/securaCV OK \\u2014 1 resolve against "
          + str(a.source) + " (files, directories, headings, lines)")
''')
# The mirror's checker before securacv-homeassistant#28: no argv parsing, so
# --source is ignored and it lints the prose instead, and can exit 0.
OLD_CHECKER = "import sys\nprint('mirror prose OK')\nsys.exit(0)\n"
SILENT_CHECKER = "import sys\nsys.exit(0)\n"
ZERO_CHECKER = ("print('links into kmay89/securaCV OK \\u2014 0 resolve "
                "against .')\n")


def checker_exiting(code: int) -> str:
    return f"import sys\nprint('trouble', file=sys.stderr)\nsys.exit({code})\n"


def files(readme: str, checker: str = CHECKER) -> dict[str, bytes]:
    return {
        "README.md": (readme + "\n").encode(),
        "AGENTS.md": b"agents\n",
        "CLAUDE.md": b"claude\n",
        ".github/scripts/lint_readme.py": checker.encode(),
    }


def mirror(root: Path, readme: str, checker: str = CHECKER) -> Path:
    for name, data in files(readme, checker).items():
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_bytes(data)
    return root


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), *args],
                          capture_output=True, text=True)


def load_script():
    spec = importlib.util.spec_from_file_location("check_hacs_store_links",
                                                  SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Source(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.source = self.dir / "monorepo"
        (self.source / "docs").mkdir(parents=True)
        (self.source / "docs" / "setup.md").write_text("# Setup\n")

    def tearDown(self):
        self.tmp.cleanup()


class Plumbing(Source):
    def check(self, readme: str, checker: str = CHECKER):
        return run("--from", str(mirror(self.dir / "m", readme, checker)),
                   "--source", str(self.source))

    def test_a_link_that_lands_passes(self):
        r = self.check("docs/setup.md")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f"OK — 1 resolve against {self.source}", r.stdout)

    def test_the_checker_runs_isolated(self):
        # A downloaded script: no user site, no PYTHON* env, no imports from
        # the current directory.
        r = self.check("docs/setup.md")
        self.assertIn("isolated=1", r.stdout)

    def test_a_link_that_does_not_land_fails_with_the_rename_hint(self):
        r = self.check("docs/renamed.md")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("MISSING docs/renamed.md", r.stderr)
        self.assertIn('<a id="old-slug"></a>', r.stderr)

    def test_a_checker_without_source_mode_is_not_a_pass(self):
        # The pre-#28 checker ignores --source, lints the prose against an
        # empty directory and may exit 0: nothing was vouched for.
        r = self.check("docs/setup.md", OLD_CHECKER)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("mirror prose OK", r.stdout)
        self.assertIn("exited 0 without saying it checked", r.stderr)

    def test_a_silent_exit_0_is_not_a_pass(self):
        r = self.check("docs/setup.md", SILENT_CHECKER)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("exited 0 without saying it checked", r.stderr)

    def test_a_checker_that_checked_nothing_is_not_a_pass(self):
        r = self.check("docs/setup.md", ZERO_CHECKER)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("n at least 1", r.stderr)

    def test_the_checkers_exit_2_stays_2_without_the_hint(self):
        r = self.check("docs/setup.md", checker_exiting(2))
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertNotIn("old-slug", r.stderr)

    def test_any_other_failure_of_the_checker_is_1(self):
        for code in (1, 3, 127):
            with self.subTest(code=code):
                r = self.check("docs/setup.md", checker_exiting(code))
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn("trouble", r.stderr)

    def test_an_unreadable_mirror_is_exit_2(self):
        m = mirror(self.dir / "m", "docs/setup.md")
        (m / "CLAUDE.md").unlink()
        r = run("--from", str(m), "--source", str(self.source))
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("CLAUDE.md: no such file", r.stderr)

    def test_an_empty_mirror_file_is_exit_2(self):
        m = mirror(self.dir / "m", "docs/setup.md")
        (m / ".github" / "scripts" / "lint_readme.py").write_bytes(b"")
        r = run("--from", str(m), "--source", str(self.source))
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("lint_readme.py: empty", r.stderr)


class FakeRaw(http.server.BaseHTTPRequestHandler):
    """raw.githubusercontent.com: /<owner>/<repo>/<ref>/<path>. `plan` maps a
    path to a list of answers, one per request (the last repeats): bytes
    for a 200, an int for that status."""
    plan: dict = {}
    hits: list = []

    def do_GET(self):
        type(self).hits.append(self.path)
        owner, repo, ref, path = self.path.lstrip("/").split("/", 3)
        answers = type(self).plan.get(path, [404])
        n = sum(1 for h in type(self).hits if h == self.path)
        answer = answers[min(n, len(answers)) - 1]
        if isinstance(answer, int):
            self.send_error(answer)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(answer)))
        self.end_headers()
        self.wfile.write(answer)

    def log_message(self, *args):
        pass


class Fetch(Source):
    def setUp(self):
        super().setUp()
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0),
                                                      FakeRaw)
        threading.Thread(target=self.server.serve_forever,
                         daemon=True).start()
        FakeRaw.hits = []
        self.mod = load_script()
        port = self.server.server_address[1]
        self.patches = [
            mock.patch.object(self.mod, "RAW",
                              f"http://127.0.0.1:{port}/{{repo}}/{{ref}}/"
                              "{path}"),
            mock.patch.object(self.mod, "RETRY_PAUSE", 0),
            mock.patch.dict(os.environ, {"no_proxy": "127.0.0.1",
                                         "NO_PROXY": "127.0.0.1"}),
        ]
        for p in self.patches:
            p.start()

    def tearDown(self):
        for p in reversed(self.patches):
            p.stop()
        self.server.shutdown()
        self.server.server_close()
        super().tearDown()

    def serve(self, **overrides):
        plan = {k: [v] for k, v in files("docs/setup.md").items()}
        plan.update(overrides)
        FakeRaw.plan = plan

    def main(self, *args: str):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = self.mod.main([*args, "--source", str(self.source)])
        return rc, out.getvalue(), err.getvalue()

    def requests_for(self, path: str) -> list[str]:
        return [h for h in FakeRaw.hits if h.endswith("/" + path)]

    def test_the_four_files_are_fetched_from_the_ref_and_checked(self):
        self.serve()
        rc, out, err = self.main("--ref", "some-branch")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("OK — 1 resolve", out)
        self.assertEqual(sorted(FakeRaw.hits), sorted(
            f"/kmay89/securacv-homeassistant/some-branch/{p}"
            for p in self.mod.FILES))

    def test_a_404_is_exit_2_after_the_retries(self):
        self.serve(**{"CLAUDE.md": [404]})
        rc, out, err = self.main()
        self.assertEqual(rc, 2, out + err)
        self.assertIn("could not read the mirror's files", err)
        self.assertIn("/main/CLAUDE.md: HTTP Error 404", err)
        self.assertIn(f"after {self.mod.ATTEMPTS} tries", err)
        self.assertEqual(len(self.requests_for("CLAUDE.md")),
                         self.mod.ATTEMPTS)

    def test_an_empty_body_is_exit_2(self):
        self.serve(**{".github/scripts/lint_readme.py": [b""]})
        rc, out, err = self.main()
        self.assertEqual(rc, 2, out + err)
        self.assertIn("lint_readme.py: an empty body", err)
        self.assertEqual(len(self.requests_for("lint_readme.py")),
                         self.mod.ATTEMPTS)

    def test_a_transient_failure_is_retried(self):
        body = files("docs/setup.md")["README.md"]
        self.serve(**{"README.md": [503, b"", body]})
        rc, out, err = self.main()
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(len(self.requests_for("README.md")), 3)


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
        self.assertFalse(any("continue-on-error" in s for s in steps))
        self.assertNotIn("continue-on-error", job)


if __name__ == "__main__":
    unittest.main()
