"""The policy checker's workflow_run check: a `branches:` filter is not a
check on who triggered the run.

`workflow_run: branches: [main]` matches the triggering run's head branch
NAME, and a pull request from a fork's branch called `main` carries it. Two
workflows trusted the filter (a staging deploy, a macOS build); these cases
pin what the checker now refuses, what it accepts, and that the repo's own
two workflow_run workflows carry the check.
"""
from __future__ import annotations

import os
import sys
import tempfile
import textwrap
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ci_policy_check as cpc  # noqa: E402

POLICY = {"timeout_max": 120, "unfiltered_ok": ["wf.yml"]}

HEAD = """
    on:
      workflow_run:
        workflows: ["Build"]
        types: [completed]
        %s
    permissions: {contents: read}
    jobs:
"""

JOB = """
      %s:
        %s
        runs-on: ubuntu-24.04
        timeout-minutes: 5
        steps:
          - run: echo deploy
"""


def problems(branches: str, *jobs: tuple[str, str]) -> list[str]:
    body = textwrap.dedent(HEAD % branches) + "".join(textwrap.dedent(JOB % job).replace("\n", "\n  ") for job in jobs)
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "wf.yml")
        with open(path, "w", encoding="utf-8") as f:
            f.write(body)
        return [p for p in cpc.check_workflow(path, POLICY) if "R12 —" in p]


SUCCESS = "if: github.event.workflow_run.conclusion == 'success'"
PUSH = "if: github.event.workflow_run.conclusion == 'success' && github.event.workflow_run.event == 'push'"
SAME_REPO = "if: github.event.workflow_run.head_repository.full_name == github.repository"


class BranchesIsNotAnOriginCheck(unittest.TestCase):
    def test_a_job_that_trusts_the_filter_is_refused(self):
        for cond in (SUCCESS, "name: no condition at all"):
            with self.subTest(cond=cond):
                probs = problems("branches: [main]", ("deploy", cond))
                self.assertEqual(len(probs), 1, probs)
                self.assertIn("job `deploy`", probs[0])
                self.assertIn("github.event.workflow_run.event == 'push'", probs[0])

    def test_the_event_or_the_repository_satisfies_it(self):
        for cond in (PUSH, SAME_REPO,
                     "if: github.event_name != 'workflow_run' || github.event.workflow_run.event == 'push'"):
            with self.subTest(cond=cond):
                self.assertEqual(problems("branches: [main]", ("deploy", cond)), [])

    def test_a_job_that_waits_for_a_gated_job_is_skipped_with_it(self):
        self.assertEqual(problems("branches: [main]", ("deploy", PUSH), ("after", "needs: deploy")), [])
        # ...unless its own `if:` asks to run regardless.
        probs = problems("branches: [main]", ("deploy", PUSH),
                         ("after", "needs: deploy\n        if: always()"))
        self.assertEqual(len(probs), 1, probs)
        self.assertIn("job `after`", probs[0])
        # ...and waiting on an ungated job changes nothing for that job.
        probs = problems("branches: [main]", ("deploy", SUCCESS), ("after", "needs: deploy"))
        self.assertEqual(len(probs), 1, probs)
        self.assertIn("job `deploy`", probs[0])

    def test_a_trigger_with_no_branches_filter_is_not_this_rules(self):
        self.assertEqual(problems("# every branch, on purpose", ("deploy", SUCCESS)), [])


class TheReposOwnWorkflows(unittest.TestCase):
    def test_both_workflow_run_workflows_check_the_event(self):
        import yaml

        seen = []
        for name in sorted(os.listdir(cpc.WORKFLOWS_DIR)):
            if not name.endswith((".yml", ".yaml")):
                continue
            with open(os.path.join(cpc.WORKFLOWS_DIR, name), encoding="utf-8") as f:
                wf = yaml.safe_load(f)
            triggers = cpc.triggers_of(wf)
            if "workflow_run" not in triggers:
                continue
            seen.append(name)
            self.assertEqual(cpc.check_workflow_run_origin(name, triggers, wf.get("jobs") or {}), [])
            for job, spec in (wf.get("jobs") or {}).items():
                with self.subTest(workflow=name, job=job):
                    self.assertIn("github.event.workflow_run.event == 'push'", spec.get("if", ""))
        self.assertEqual(seen, ["backoffice-deploy.yml", "ios-selfheal.yml"],
                         "a workflow_run workflow was added or removed: look at its jobs' `if:`")


if __name__ == "__main__":
    unittest.main()
