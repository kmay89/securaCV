#!/usr/bin/env python3
"""Pins the device-join half of scripts/lint_build_matrix.py (devices/README.md,
wave 2 "Consume").

What is pinned and why:
  • the committed tree is green — the lint must pass on the tree it ships in;
  • the lint actually FAILS on each mistake the join exists for: a build-matrix
    lane no manifest resolves to, and a manifest naming an env its family's
    PlatformIO project never defines — a guard that reads as covered while
    catching nothing is worse than no guard;
  • the resolution is the SAME function lint_device_manifests.py uses
    (scripts/_device_join.py), so the two lints cannot disagree about which
    manifest a lane belongs to.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import copy
import importlib.util
import io
import json
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "lint_build_matrix.py"

# The lint imports scripts/_device_join.py the way it runs in CI
# (`python3 scripts/lint_build_matrix.py` puts scripts/ at sys.path[0]).
if str(SCRIPT.parent) not in sys.path:
    sys.path.insert(0, str(SCRIPT.parent))
spec = importlib.util.spec_from_file_location("lint_build_matrix", SCRIPT)
lbm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lbm)  # type: ignore[union-attr]

import _device_join as dj  # noqa: E402  (needs scripts/ on sys.path first)


def _tree():
    matrix = json.loads((REPO / "firmware/build_matrix.json").read_text(encoding="utf-8"))
    flavors = json.loads((REPO / "firmware/flavors.json").read_text(encoding="utf-8"))
    manifests, errs = dj.load_manifests(REPO / "devices")
    assert errs == [], errs
    return matrix, flavors, manifests


class CommittedTreeIsGreen(unittest.TestCase):
    def test_collect_is_empty(self):
        self.assertEqual(lbm.collect(), [])

    def test_cli_exit_code(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(lbm.main(), 0)

    def test_every_lane_resolves_to_one_manifest(self):
        matrix, _flavors, manifests = _tree()
        for prod in matrix["products"]:
            target, why = dj.manifest_for_matrix_product(prod, manifests)
            self.assertIsNone(why, why)
            self.assertIsNotNone(target, prod["id"])


class LintCatchesRealMistakes(unittest.TestCase):
    def test_lane_with_no_manifest_fails(self):
        matrix, flavors, manifests = _tree()
        bad = {"products": [{"id": "canary-nope", "flavor": "canary-display",
                             "build": {"env": "canary-display-nope"}}]}
        errors = lbm.device_join_errors(bad, flavors, manifests, REPO)
        self.assertTrue(any("canary-nope" in e and "no manifest" in e for e in errors), errors)

    def test_lane_resolved_by_flavor_and_env_not_just_id(self):
        # The board-specialized lanes (no devices/<id>/ of their own) resolve
        # through flavor + env — the same rule the manifest linter applies.
        matrix, flavors, manifests = _tree()
        by_id = [p for p in matrix["products"] if p.get("flavor")]
        self.assertTrue(by_id, "the tree has board-specialized lanes to exercise")
        for prod in by_id:
            target, why = dj.manifest_for_matrix_product(prod, manifests)
            self.assertIsNone(why, why)
            self.assertEqual(target["family"], prod["flavor"])
            for env in dj.matrix_product_envs(prod):
                self.assertIn(env, target["board"]["envs"])

    def test_manifest_env_missing_from_platformio_ini_fails(self):
        matrix, flavors, manifests = _tree()
        mutated = copy.deepcopy(manifests)
        dash = next(m for m in mutated if m["slug"] == "canary-display-dash")
        dash["board"]["envs"].append("canary-display-nope")
        errors = lbm.device_join_errors(matrix, flavors, mutated, REPO)
        self.assertTrue(any("canary-display-nope" in e and "[env:" in e for e in errors), errors)

    def test_manifest_family_outside_flavors_json_fails(self):
        matrix, flavors, manifests = _tree()
        mutated = copy.deepcopy(manifests)
        mutated[0]["family"] = "canary-nope"
        errors = lbm.device_join_errors(matrix, flavors, mutated, REPO)
        self.assertTrue(any("canary-nope" in e and "flavors.json" in e for e in errors), errors)


if __name__ == "__main__":
    sys.exit(unittest.main())
