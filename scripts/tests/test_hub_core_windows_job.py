#!/usr/bin/env python3
"""Hub Core's Windows job lints and tests, on the bytes the repo commits.

desktop-hub-core.yml's `windows-check` job (sweep A68) compiled hub-core and
hub-io on windows-latest and ran nothing else, so a Windows-only clippy
warning and the Windows run of hub_enumerate_windows.rs's tests reached no
gate (sweep A87). The job now runs clippy with warnings as errors and
`cargo test`. Its test run needs one thing the Linux jobs do not: Git for
Windows checks text out as CRLF by default, and hub-io hashes four
include_str! files against pins taken over the committed LF bytes, so the
job turns core.autocrlf off BEFORE actions/checkout. After the checkout the
setting would come too late and the pin test would fail on Windows alone.

This holds those three facts, since no local run can: cargo does not run on
the hosts these suites run on, and the job's first CI run is its own proof.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_hub_core_windows_job.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "desktop-hub-core.yml"


def job_problems(job: dict) -> list[str]:
    found = []
    if "windows" not in str(job.get("runs-on", "")):
        found.append("the job does not run on Windows")
    if set(job.get("strategy", {}).get("matrix", {}).get("crate", [])) != {
            "hub-core", "hub-io"}:
        found.append("the matrix is not hub-core and hub-io")
    steps = job.get("steps", [])
    runs = [" ".join(str(s.get("run", "")).split()) for s in steps]
    checkout = next((i for i, s in enumerate(steps)
                     if str(s.get("uses", "")).startswith("actions/checkout@")),
                    None)
    crlf = next((i for i, r in enumerate(runs)
                 if r == "git config --global core.autocrlf false"), None)
    if checkout is None:
        found.append("no actions/checkout step")
    if crlf is None:
        found.append("core.autocrlf is never turned off")
    elif checkout is not None and crlf > checkout:
        found.append("core.autocrlf is turned off after the checkout")
    for want in ("cargo check --all-targets",
                 "cargo clippy --all-targets -- -D warnings",
                 "cargo test"):
        if want not in runs:
            found.append(f"no step runs `{want}`")
    return found


class WindowsJob(unittest.TestCase):
    def setUp(self):
        self.wf = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))

    def test_the_job_checks_lints_and_tests_on_lf_bytes(self):
        job = self.wf["jobs"]["windows-check"]
        self.assertEqual(job_problems(job), [])

    def test_the_old_job_is_refused(self):
        # The job as A68 left it: check only, no line-ending step.
        old = {"runs-on": "windows-latest",
               "strategy": {"matrix": {"crate": ["hub-core", "hub-io"]}},
               "steps": [{"uses": "actions/checkout@v7"},
                         {"run": "cargo check --all-targets"}]}
        got = job_problems(old)
        self.assertIn("core.autocrlf is never turned off", got)
        self.assertIn("no step runs `cargo clippy --all-targets -- -D warnings`", got)
        self.assertIn("no step runs `cargo test`", got)

    def test_autocrlf_after_checkout_is_refused(self):
        job = dict(self.wf["jobs"]["windows-check"])
        steps = list(job["steps"])
        crlf = steps.pop(0)
        steps.insert(1, crlf)  # behind the checkout
        job["steps"] = steps
        self.assertIn("core.autocrlf is turned off after the checkout",
                      job_problems(job))


if __name__ == "__main__":
    unittest.main()
