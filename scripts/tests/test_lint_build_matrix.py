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
    manifest a lane belongs to;
  • a lane's feature claims are held to its own tree's flag (sweep C36): the
    old `thermal` entry (FEATURE_DIAGNOSTICS, the canary's self-test flag)
    fails, a thermal claim on an env that sets FEATURE_THERMAL_WATCHDOG=0
    fails, and the canary-wap's thermal passes only through its alias
    (FEATURE_SYS_MONITOR); the alias and unmapped tables cannot rot.

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

    def test_a_shipping_product_without_a_lane_fails(self):
        matrix, flavors, _manifests = _tree()
        extra = flavors + [{"name": "canary-nope", "build_envs": ["x"]}]
        errors = lbm.lane_coverage_errors(matrix, extra)
        self.assertTrue(any("'canary-nope' has no lane" in e for e in errors), errors)

    def test_an_unreleased_product_needs_no_lane(self):
        matrix, flavors, _manifests = _tree()
        extra = flavors + [{"name": "canary-nope", "build_envs": ["x"],
                            "unreleased": "bench pending"}]
        self.assertEqual(lbm.lane_coverage_errors(matrix, extra), [])

    def test_an_unreleased_product_may_not_have_a_lane(self):
        matrix, flavors, _manifests = _tree()
        mutated = copy.deepcopy(flavors)
        next(f for f in mutated if f["name"] == "canary-sense")["unreleased"] = "x"
        errors = lbm.lane_coverage_errors(matrix, mutated)
        self.assertTrue(any("'canary-sense' `unreleased`" in e for e in errors), errors)

    def test_the_tree_carries_an_unreleased_product(self):
        # canary-sentinel is compiled (flavors.json) and deliberately laneless;
        # if it ships, this test is the reminder to give it a real lane.
        matrix, flavors, _manifests = _tree()
        sentinel = next(f for f in flavors if f["name"] == "canary-sentinel")
        self.assertTrue(sentinel["unreleased"].strip())
        self.assertNotIn("canary-sentinel",
                         {p.get("flavor", p["id"]) for p in matrix["products"]})

    def test_manifest_family_outside_flavors_json_fails(self):
        matrix, flavors, manifests = _tree()
        mutated = copy.deepcopy(manifests)
        mutated[0]["family"] = "canary-nope"
        errors = lbm.device_join_errors(matrix, flavors, mutated, REPO)
        self.assertTrue(any("canary-nope" in e and "flavors.json" in e for e in errors), errors)


def _claims(matrix, **kw):
    suite = lbm.tree_suite()
    return lbm.feature_flag_errors(matrix, suite.tree_facts, suite.env_flags, **kw)


def _lane(matrix, pid):
    return next(p for p in matrix["products"] if p["id"] == pid)


class FeatureClaimsHeldToTheirTree(unittest.TestCase):
    """Sweep C36: featureCatalog's flags are the canary tree's spellings, so
    each claim is held to the flag its own tree spells the feature with."""

    def test_the_committed_claims_pass(self):
        matrix, _flavors, _manifests = _tree()
        self.assertEqual(_claims(matrix), [])
        self.assertEqual(matrix["featureCatalog"]["thermal"]["flag"], "FEATURE_THERMAL_WATCHDOG")

    def test_the_old_diagnostics_thermal_entry_fails(self):
        # FEATURE_DIAGNOSTICS is #define'd in the canary tree (the heap / SD /
        # self-test flag), so only the reading's own guard tells it apart.
        matrix, _flavors, _manifests = _tree()
        matrix["featureCatalog"]["thermal"]["flag"] = "FEATURE_DIAGNOSTICS"
        errors = _claims(matrix)
        for where in ("product canary.dev", "product canary.release", "product canary.full",
                      "product canary-freenove-s3"):
            self.assertIn(f"{where}: lists 'thermal' as FEATURE_DIAGNOSTICS, but no "
                          "#if FEATURE_DIAGNOSTICS in the canary tree encloses a "
                          "die-temperature read", errors)
        self.assertFalse([e for e in errors if "canary-wap" in e], "the WAP is read through its alias")

    def test_a_thermal_claim_on_an_env_that_turns_the_watchdog_off_fails(self):
        # The classic-ESP32 envs set FEATURE_THERMAL_WATCHDOG=0 (no usable
        # die-temperature sensor); the camera and the WROOM DevKit list no thermal.
        matrix, _flavors, _manifests = _tree()
        for pid, env in (("canary-esp32cam", "esp32cam"), ("canary-wroom", "esp32-wroom")):
            with self.subTest(pid=pid):
                m = copy.deepcopy(matrix)
                _lane(m, pid)["build"]["features"].append("thermal")
                self.assertEqual(_claims(m), [
                    f"product {pid}: lists 'thermal', but [env:{env}] builds the canary tree with "
                    "-DFEATURE_THERMAL_WATCHDOG=0 — the build does not have it"])

    def test_the_wap_thermal_passes_through_its_alias_and_only_through_it(self):
        matrix, _flavors, _manifests = _tree()
        self.assertIn("thermal", _lane(matrix, "canary-wap")["build"]["features"])
        self.assertEqual(lbm.FLAG_ALIASES[("canary-wap", "thermal")], "FEATURE_SYS_MONITOR")
        self.assertEqual(_claims(matrix), [])
        errors = _claims(matrix, aliases={})
        self.assertEqual(errors, [
            "product canary-wap: lists 'thermal' as FEATURE_THERMAL_WATCHDOG, but no source in "
            "the canary-wap tree #defines FEATURE_THERMAL_WATCHDOG — the tree spells it some other "
            "way (map it in FLAG_ALIASES) or does not have it (drop the claim)"])
        # An alias to a flag that guards no die-temperature read is refused too.
        errors = _claims(matrix, aliases={("canary-wap", "thermal"): "FEATURE_BLE_STATUS"})
        self.assertEqual(errors, [
            "product canary-wap: lists 'thermal' as FEATURE_BLE_STATUS, but no #if "
            "FEATURE_BLE_STATUS in the canary-wap tree encloses a die-temperature read"])

    def test_a_claim_the_tree_cannot_spell_fails(self):
        # The 2026-09-05 bug: canary-sense advertised a battery gauge and a
        # die-temperature sensor it has neither of.
        matrix, _flavors, _manifests = _tree()
        _lane(matrix, "canary-sense")["build"]["features"] += ["battery", "thermal"]
        errors = _claims(matrix)
        self.assertEqual(len(errors), 2, errors)
        self.assertTrue(all(e.startswith("product canary-sense: lists '") and
                            "no source in the canary-sense tree #defines" in e for e in errors), errors)

    def test_the_tables_cannot_rot(self):
        matrix, _flavors, _manifests = _tree()
        unmapped = set(lbm.UNMAPPED)
        self.assertEqual(_claims(matrix, unmapped=unmapped | {("canary-vision", "gps")}),
                         ["UNMAPPED lists ('canary-vision', 'gps'), which no lane claims — drop it"])
        self.assertEqual(_claims(matrix, unmapped=unmapped | {("canary", "selftest")}),
                         ["UNMAPPED lists ('canary', 'selftest'), but the canary tree #defines "
                          "FEATURE_DIAGNOSTICS — the claim can be held to it; drop it from UNMAPPED"])
        self.assertEqual(_claims(matrix, unmapped=unmapped | {("canary-wap", "thermal")}),
                         ["('canary-wap', 'thermal') is both in UNMAPPED and in FLAG_ALIASES — "
                          "drop it from UNMAPPED"])
        aliases = {**lbm.FLAG_ALIASES, ("canary-sense", "thermal"): "FEATURE_SYS_MONITOR"}
        self.assertEqual(_claims(matrix, aliases=aliases),
                         ["FLAG_ALIASES maps ('canary-sense', 'thermal'), which no lane claims — drop it"])
        # And dropping an UNMAPPED entry turns its claim into a positive check.
        errors = _claims(matrix, unmapped=unmapped - {("canary-wap", "gps")})
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("product canary-wap: lists 'gps' as FEATURE_GNSS, but no source in the "
                      "canary-wap tree #defines FEATURE_GNSS", errors[0])

    def test_a_lane_env_its_tree_does_not_define_fails(self):
        matrix, _flavors, _manifests = _tree()
        _lane(matrix, "canary")["levels"]["dev"]["env"] = "dev-nope"
        self.assertIn("product canary.dev: env 'dev-nope' is not an [env:dev-nope] of the canary "
                      "tree's PlatformIO config (its platformio.ini and extra_configs)", _claims(matrix))

    def test_the_tree_reader_is_the_thermal_suites_own(self):
        # One definition of a tree and of a die-temperature read.
        self.assertEqual(Path(lbm.tree_suite().__file__).resolve(),
                         (REPO / "scripts/tests/test_board_thermal_notes.py").resolve())


if __name__ == "__main__":
    sys.exit(unittest.main())
