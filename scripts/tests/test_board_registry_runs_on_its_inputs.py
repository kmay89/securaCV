#!/usr/bin/env python3
"""firmware/scripts/check_board_registry.py runs on an edit to any file it reads.

The check reads outside firmware/. Check 9 joins each board-port env to its
board through devices/<slug>/device.json (`board.board_id`, `board.envs`),
and check 10 holds CANARY_PRODUCT_NAME to devices/canary/device.json's
`name` and finds the canary board ports through every manifest's
`board.envs`. It used to run only in firmware.yml, whose path filter lists
firmware/** and no devices/ path, so a PR that edited a manifest alone ran
no check, and the red landed on the next firmware PR (.github/CI.md, "What a
path filter must cover"; review round 2 of device-details PR 6).

This test records what the check really reads, with the path-filter read
recorder canary-local.yml arms (scripts/path_filter_reads), and holds every
workflow that runs the check on a pull request to covering each of those
paths: with no pull_request path filter (lint.yml), or with one that lists
the path (scripts/check_path_filter_reads.py's PathFilter, GitHub's pattern
rules). The recorder's own limit applies: CPython raises no audit event for
a stat or an exists check, so a path the check only tests for existence is
not seen; what it opens and lists is.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
CHECK = "firmware/scripts/check_board_registry.py"
RECORDER = REPO / "scripts" / "path_filter_reads"
WORKFLOWS = REPO / ".github" / "workflows"

_spec = importlib.util.spec_from_file_location(
    "check_path_filter_reads", REPO / "scripts" / "check_path_filter_reads.py")
cpfr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cpfr)  # type: ignore[union-attr]


def runners() -> list[tuple[str, str, dict]]:
    """(workflow file, job id, workflow) for every job with a step that runs
    the check."""
    out = []
    for path in sorted(WORKFLOWS.glob("*.yml")):
        wf = yaml.safe_load(path.read_text(encoding="utf-8"))
        if not isinstance(wf, dict):
            continue
        for job_id, job in (wf.get("jobs") or {}).items():
            for step in (job or {}).get("steps") or []:
                run = step.get("run") if isinstance(step, dict) else None
                if isinstance(run, str) and CHECK in run:
                    out.append((path.name, job_id, wf))
                    break
    return out


def recorded_reads() -> tuple[subprocess.CompletedProcess, dict[str, set[str]]]:
    """Run the check with the recorder armed: (the run, {repo path: ops})."""
    with tempfile.TemporaryDirectory() as d:
        env = dict(os.environ, PATH_FILTER_READS_DIR=d, PYTHONPATH=str(RECORDER))
        env.pop("PATH_FILTER_READS_SUITE", None)
        proc = subprocess.run([sys.executable, CHECK], cwd=REPO, env=env,
                              capture_output=True, text=True, timeout=120)
        reads: dict[str, set[str]] = {}
        for rec in cpfr.load_records(Path(d)):
            reads.setdefault(rec["path"], set()).add(str(rec.get("op")))
    return proc, reads


def uncovered(flt, reads: dict[str, set[str]]) -> list[str]:
    """The recorded paths a PR could edit without the filter running the job."""
    misses = []
    for path, ops in sorted(reads.items()):
        is_dir = bool(ops & cpfr.LISTING_OPS) or (REPO / path).is_dir()
        if not (flt.covers_dir(path) if is_dir else flt.covers_file(path)):
            misses.append(path)
    return misses


class BoardRegistryRunsOnItsInputs(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.proc, cls.reads = recorded_reads()

    def test_the_recorded_run_is_a_whole_one(self):
        # A run that stopped early would have read less than the check reads.
        self.assertEqual(self.proc.returncode, 0, self.proc.stdout + self.proc.stderr)
        self.assertIn(CHECK, self.reads, "the recorder was armed in the check's process")

    def test_it_reads_the_device_manifests(self):
        # Checks 9 and 10: the reads this test exists for. If the check stops
        # reading them, this test should be revisited, not silently pass.
        self.assertIn("devices/canary/device.json", self.reads)
        manifests = [p for p in self.reads
                     if p.startswith("devices/") and p.endswith("/device.json")]
        self.assertGreaterEqual(len(manifests), 2, manifests)

    def test_every_workflow_that_runs_it_on_a_pull_request_covers_what_it_reads(self):
        gates = [(name, job, wf) for name, job, wf in runners()
                 if "pull_request" in cpfr.triggers_of(wf)]
        self.assertTrue(gates, f"no workflow runs {CHECK} on a pull request")
        for name, job, wf in gates:
            flt = cpfr.pr_filter(wf)
            if flt is None:
                continue  # no pull_request path filter: every edit runs it
            misses = uncovered(flt, self.reads)
            self.assertEqual(
                misses, [],
                f".github/workflows/{name} (job `{job}`) runs {CHECK}, but its "
                f"pull_request path filter does not cover {len(misses)} path(s) "
                f"the check reads, so a PR that edits one of them alone runs no "
                f"check. Run the check in lint.yml (no path filter), or add each "
                f"path to BOTH of {name}'s path lists (CI.md R6).")

    def test_the_matcher_reads_a_filtered_miss_as_one(self):
        # The assertion above can only fail if `uncovered` can say "missed":
        # a firmware-only filter misses the manifests, a devices/ one does not.
        reads = {"devices/canary/device.json": {"open"}, "devices": {"listdir"},
                 CHECK: {"open"}}
        self.assertEqual(uncovered(cpfr.PathFilter(["firmware/**"]), reads),
                         ["devices", "devices/canary/device.json"])
        self.assertEqual(uncovered(cpfr.PathFilter(["firmware/**", "devices/**"]), reads), [])


if __name__ == "__main__":
    unittest.main()
