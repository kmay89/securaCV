#!/usr/bin/env python3
"""A Lab generator's drift step runs after the drift step of every page it reads.

canary-local.yml's logic job regenerates each devices/*.json page and fails
on a diff, one step per generator. When one generator reads another's page
(gen_homeassistant.py prints the WAP's retained topics from wap.json in its
"Meet the fleet" step, since sweep A27), the page it reads must be checked
first. Otherwise a stale wap.json takes two red rounds: the Hub step passes
against the committed (stale but self-consistent) wap.json, the WAP step
fails, and only after gen_wap.py alone is rerun does the Hub step fail.
That is what the A27 review found: "Hub data drift" ran ~90 lines before
"WAP data drift".

This reads the job's steps (the generator each runs and the pages it
diffs) and each generator's source (the devices/*.json pages it names), and
requires producer-before-reader for every pair. One pair reads both ways
and is named below with its reason.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_lab_drift_order.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests), unfiltered
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github/workflows/canary-local.yml"
JOB = "logic-tests"

GEN_RUN = re.compile(r"python3 (canary-local/tools/gen_[a-z_]+\.py)(?![^\n]*--check)")
DIFFED = re.compile(r"git diff --exit-code -- ([^;\n]+?);")
PAGE = re.compile(r"devices/([a-z_]+\.json)")

# (reader, page): reads that may come before the page's own drift step,
# each with why that cannot leave a stale page green.
CYCLES = {
    ("canary-local/tools/gen_vision.py", "flash.json"):
        "gen_vision.py reads only flash.json's canary-vision product ids, names, chips and "
        "boards, which gen_flash.py builds from the firmware tree; gen_flash.py reads "
        "vision.json's room presets and model_load wire. Neither reads what the other "
        "derives from it, so the two are checked in either order.",
}


def drift_steps(workflow: dict) -> list[tuple[int, str, list[str]]]:
    """[(step index, generator, [pages it diffs])] in the job's order."""
    out = []
    for i, step in enumerate(workflow["jobs"][JOB]["steps"]):
        run = step.get("run") or ""
        gens = GEN_RUN.findall(run)
        diffs = DIFFED.findall(run)
        if len(gens) == 1 and diffs:
            pages = [Path(p).name for p in diffs[0].split() if p.startswith("canary-local/devices/")]
            out.append((i, gens[0], pages))
    return out


def order_problems(steps: list[tuple[int, str, list[str]]], reads: dict[str, set[str]]) -> list[str]:
    produced_at = {page: (i, gen) for i, gen, pages in steps for page in pages}
    problems = []
    for i, gen, pages in steps:
        for page in sorted(reads.get(gen, set()) - set(pages)):
            if page not in produced_at:
                continue  # hand-written (registry.json) or checked elsewhere
            j, producer = produced_at[page]
            if j > i and (gen, page) not in CYCLES:
                problems.append(f"{gen} reads devices/{page}, but {producer}'s drift step runs after it "
                                f"(step {j} > {i}): move it before, and name it in the error")
    return problems


def generator_reads(gens: list[str]) -> dict[str, set[str]]:
    return {g: set(PAGE.findall((REPO / g).read_text(encoding="utf-8"))) for g in gens}


class TheRule(unittest.TestCase):
    def test_a_reader_checked_before_its_producer_fails(self):
        steps = [(13, "canary-local/tools/gen_homeassistant.py", ["homeassistant.json"]),
                 (21, "canary-local/tools/gen_wap.py", ["wap.json"])]
        reads = {"canary-local/tools/gen_homeassistant.py": {"homeassistant.json", "wap.json", "registry.json"}}
        self.assertEqual(len(order_problems(steps, reads)), 1)
        self.assertEqual(order_problems(list(reversed([(21, *steps[0][1:]), (13, *steps[1][1:])])), reads), [])


class TheWorkflow(unittest.TestCase):
    def test_every_drift_step_follows_the_pages_its_generator_reads(self):
        wf = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
        steps = drift_steps(wf)
        gens = [g for _, g, _ in steps]
        self.assertIn("canary-local/tools/gen_wap.py", gens)
        self.assertIn("canary-local/tools/gen_homeassistant.py", gens)
        reads = generator_reads(gens)
        self.assertIn("wap.json", reads["canary-local/tools/gen_homeassistant.py"],
                      "gen_homeassistant.py no longer reads wap.json; this pair's reason is gone")
        for (reader, page), why in CYCLES.items():
            self.assertIn(page, reads.get(reader, set()), f"a dead exemption: {reader} no longer reads {page} ({why})")
        self.assertEqual(order_problems(steps, reads), [])

    def test_the_wap_error_names_the_generator_that_reads_it(self):
        text = WORKFLOW.read_text(encoding="utf-8")
        wap = text.split("- name: WAP data drift", 1)[1].split("- name:", 1)[0]
        self.assertIn("then canary-local/tools/gen_homeassistant.py", wap)


if __name__ == "__main__":
    unittest.main()
