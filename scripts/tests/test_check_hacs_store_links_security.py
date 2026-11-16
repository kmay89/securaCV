#!/usr/bin/env python3
"""scripts/check_hacs_store_links.py lays out the mirror's SECURITY.md.

The mirror's security policy points into this repository (the advisory form,
this repository's SECURITY.md), so its links belong in the same run as the
store page's. The file is optional on this side: the mirror did not carry
one when this was written, and which prose files must exist is the mirror
checker's rule. These cases pin exactly how far that tolerance goes.

- a mirror that has SECURITY.md gets it laid out beside the other prose,
  fetched or copied, where the checker reads it;
- a mirror without it (a 404, or absent from a --from directory) is laid
  out without it, the run says so, and nothing else changes;
- only "absent" is tolerated: an empty file, or a fetch that fails any
  other way, is exit 2 like every other mirror file;
- the three files that were required are still required.

Rides the stand-in checker and the fake raw server of
test_check_hacs_store_links.py. Discovered by lint.yml's
`unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import sys
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_check_hacs_store_links as base  # noqa: E402

# The stand-in checker, reading SECURITY.md too when it is laid out: the
# file names one more path that must exist under --source.
CHECKER = textwrap.dedent('''\
    import argparse, sys
    from pathlib import Path
    ROOT = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path)
    a = ap.parse_args()
    names = ["README.md"] + (["SECURITY.md"] if (ROOT / "SECURITY.md").is_file() else [])
    for name in names:
        want = (ROOT / name).read_text().strip()
        if not (a.source / want).is_file():
            print("MISSING " + want + " (from " + name + ")", file=sys.stderr)
            sys.exit(1)
    print("links into kmay89/securaCV OK \\u2014 %d resolve against %s"
          % (len(names), a.source))
''')

NOTE = "the mirror has no SECURITY.md"


class Layout(base.Source):
    def mirror(self, security: bytes | None) -> Path:
        m = base.mirror(self.dir / "m", "docs/setup.md", CHECKER)
        if security is not None:
            (m / "SECURITY.md").write_bytes(security)
        return m

    def check(self, security: bytes | None):
        return base.run("--from", str(self.mirror(security)), "--source", str(self.source))

    def test_the_policy_is_laid_out_and_its_links_checked(self):
        r = self.check(b"docs/setup.md\n")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("OK — 2 resolve", r.stdout)
        self.assertNotIn(NOTE, r.stdout + r.stderr)

    def test_a_link_in_the_policy_that_does_not_land_fails(self):
        r = self.check(b"SECURITY-moved.md\n")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("MISSING SECURITY-moved.md (from SECURITY.md)", r.stderr)

    def test_a_mirror_without_one_passes_and_says_so(self):
        r = self.check(None)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(NOTE, r.stdout)
        self.assertIn("OK — 1 resolve", r.stdout)

    def test_an_empty_policy_is_exit_2(self):
        r = self.check(b"")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("SECURITY.md: empty", r.stderr)

    def test_only_the_policy_is_optional(self):
        mod = base.load_script()
        self.assertEqual(mod.OPTIONAL, frozenset({"SECURITY.md"}))
        self.assertEqual(mod.PROSE, ("README.md", "AGENTS.md", "CLAUDE.md", "SECURITY.md"))
        for required in ("README.md", "AGENTS.md", "CLAUDE.md", ".github/scripts/lint_readme.py"):
            with self.subTest(file=required):
                m = self.mirror(b"docs/setup.md\n")
                (m / required).unlink()
                r = base.run("--from", str(m), "--source", str(self.source))
                self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
                self.assertIn("no such file", r.stderr)


class Fetched(base.Fetch):
    def serve(self, **overrides):
        plan = {k: [v] for k, v in base.files("docs/setup.md", CHECKER).items()}
        plan.update(overrides)
        base.FakeRaw.plan = plan

    def test_a_404_is_absent_not_a_failure_and_is_asked_once(self):
        self.serve()
        rc, out, err = self.main()
        self.assertEqual(rc, 0, out + err)
        self.assertIn(NOTE, out)
        self.assertIn("/main/SECURITY.md: 404", out)
        self.assertIn("OK — 1 resolve", out)
        self.assertEqual(len(self.requests_for("SECURITY.md")), 1)

    def test_a_served_policy_is_laid_out(self):
        self.serve(**{"SECURITY.md": [b"docs/setup.md\n"]})
        rc, out, err = self.main()
        self.assertEqual(rc, 0, out + err)
        self.assertIn("OK — 2 resolve", out)
        self.assertNotIn(NOTE, out)

    def test_any_other_answer_is_exit_2_after_the_retries(self):
        for answer in (503, b""):
            with self.subTest(answer=answer):
                base.FakeRaw.hits = []
                self.serve(**{"SECURITY.md": [answer]})
                rc, out, err = self.main()
                self.assertEqual(rc, 2, out + err)
                self.assertIn("could not read the mirror's files", err)
                self.assertEqual(len(self.requests_for("SECURITY.md")), self.mod.ATTEMPTS)


if __name__ == "__main__":
    unittest.main()
