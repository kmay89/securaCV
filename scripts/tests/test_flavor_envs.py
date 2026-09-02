#!/usr/bin/env python3
"""Pins firmware/scripts/flavor_envs.py — the one place the release workflows
get the Canary Display env list from.

What is pinned and why:
  • the release build ORDER: named core-dir groups first, then the default
    core dir, then isolated envs — a PLATFORMIO_CORE_DIR switch changes the
    project checksum and wipes .pio/build, so this order is load-bearing;
  • the current flavors.json validates (release_envs ⊆ build_envs, each with
    a flasher product) and the current workflows name only declared envs —
    the lint must be green on the tree it ships in;
  • the lint actually FAILS on a typo'd env and on a release env CI never
    builds — a guard that reads as covered while catching nothing is worse
    than no guard.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "firmware" / "scripts" / "flavor_envs.py"

spec = importlib.util.spec_from_file_location("flavor_envs", SCRIPT)
fe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fe)  # type: ignore[union-attr]


def display_entry(core_dir_groups, isolated, build, release):
    return {
        "name": "canary-display",
        "build_envs": build,
        "release_envs": release,
        "core_dir_groups": core_dir_groups,
        "isolated_core_envs": isolated,
    }


class ReleaseOrder(unittest.TestCase):
    def test_groups_then_default_then_isolated_in_build_order(self):
        entry = display_entry(
            {"canary-display-a": "pio3", "canary-display-b": "pio3"},
            ["canary-display-e"],
            ["canary-display-c", "canary-display-e", "canary-display-a",
             "canary-display-d", "canary-display-b"],
            # deliberately shuffled: the JSON order must not leak through
            ["canary-display-e", "canary-display-d", "canary-display-b",
             "canary-display-c", "canary-display-a"],
        )
        self.assertEqual(
            fe.ordered_release_envs(entry),
            ["canary-display-a", "canary-display-b",   # named group, build order
             "canary-display-c", "canary-display-d",   # default core, build order
             "canary-display-e"],                       # isolated last
        )
        self.assertEqual(fe.core_of(entry, "canary-display-a"), "pio3")
        self.assertEqual(fe.core_of(entry, "canary-display-c"), "default")
        self.assertEqual(fe.core_of(entry, "canary-display-e"), "isolated")

    def test_current_tree_release_order_matches_the_release_steps(self):
        # The order the release workflows always built in — the pioarduino
        # core-3 pair first, the espressif32 envs next, the C6 last.
        entry = fe.find_product(fe.load_flavors(), "canary-display")
        self.assertEqual(
            fe.ordered_release_envs(entry),
            ["canary-display-dash7", "canary-display-nightstand7",
             "canary-display-nightstand-s3", "canary-display-touch169",
             "canary-display-amoled241", "canary-display-nightlight-c3",
             "canary-display-nightstand-c6"],
        )

    def test_missing_release_envs_is_an_error_not_an_empty_list(self):
        entry = display_entry({}, [], ["canary-display-a"], None)
        del entry["release_envs"]
        with self.assertRaises(SystemExit):
            fe.ordered_release_envs(entry)


class CurrentTreeIsGreen(unittest.TestCase):
    def test_flavors_json_validates(self):
        self.assertEqual(fe.validate(fe.load_flavors()), [])

    def test_workflows_name_only_declared_envs(self):
        self.assertEqual(fe.check_workflows(fe.load_flavors()), [])

    def test_cli_check_mode_exit_code(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(fe.main(["--check-workflows"]), 0)


class LintCatchesRealMistakes(unittest.TestCase):
    def test_release_env_ci_never_builds_is_rejected(self):
        entry = display_entry({}, [], ["canary-display-a"],
                              ["canary-display-a", "canary-display-zz"])
        problems = fe.validate([entry])
        self.assertTrue(any("canary-display-zz" in p and "not in build_envs" in p
                            for p in problems), problems)

    def test_release_env_without_flasher_product_is_rejected(self):
        # An env CI builds but the flasher catalog has never heard of.
        entry = display_entry({}, [], ["canary-display-playground"],
                              ["canary-display-playground"])
        problems = fe.validate([entry])
        self.assertTrue(any("securacv-canary-display-playground" in p
                            for p in problems), problems)

    def test_typoed_env_in_a_workflow_is_flagged_and_templates_are_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            wf = Path(tmp) / "x.yml"
            wf.write_text(
                "run: |\n"
                "  pio run -e canary-display-amoled214\n"       # typo
                "  cp .pio/build/canary-display-${E} out/\n"    # template: fine
                "  ls canary-display-dash7-${VERSION}.bin\n"    # trailing hyphen: fine
                "  echo securacv-canary-display-nightstand-c6\n",  # product id: fine
                encoding="utf-8",
            )
            saved = fe.WORKFLOWS
            fe.WORKFLOWS = Path(tmp)
            try:
                problems = fe.check_workflows(fe.load_flavors())
            finally:
                fe.WORKFLOWS = saved
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("canary-display-amoled214", problems[0])
        self.assertIn("x.yml:2", problems[0])


if __name__ == "__main__":
    sys.exit(unittest.main())
