#!/usr/bin/env python3
"""Unit tests for the "Update everything" decision engine.

The button's promise is narrow and testable: **release what genuinely needs
releasing, touch nothing that doesn't, and never fail for saying so.** These
tests are what make that a fact rather than an intention — every branch of
`decide()` is pinned here, including the two that exist to STOP work
(UP_TO_DATE, which is what saves the compute, and NEEDS_BUMP, which is what
stops a second tree shipping under a published version).

Run:  python3 -m unittest discover -s .github/scripts -p 'test_*.py'
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import release_plan as rp  # noqa: E402


VERSIONED = {
    "name": "flasher",
    "label": "SecuraCV Flasher",
    "kind": "versioned",
    "tag_prefix": "flasher-v",
    "workflow": "desktop-flasher-release.yml",
    "inputs": {"dry_run": "false"},
    "dev_inputs": {"dry_run": "true"},
    "watch": ["desktop"],
}

PAGES = {
    "name": "web",
    "label": "the site",
    "kind": "pages",
    "workflow": "pages.yml",
    "inputs": {},
    "watch": ["canary-local"],
}

GATED_TARGET = dict(VERSIONED, name="tvos", tag_prefix="tvos-v", gate_var="ENABLE_TVOS_BUILD")


class VersionOrdering(unittest.TestCase):
    def test_ordinary_triples(self):
        self.assertEqual(rp.compare("2.3.0", "2.2.9"), 1)
        self.assertEqual(rp.compare("2.3.0", "2.3.0"), 0)
        self.assertEqual(rp.compare("2.3.0", "2.4.0"), -1)

    def test_numeric_not_lexicographic(self):
        # The classic: "2.10.0" must beat "2.9.0".
        self.assertEqual(rp.compare("2.10.0", "2.9.0"), 1)

    def test_a_prerelease_sorts_before_its_release(self):
        # Otherwise a dev build looks "newer" than stable and re-cuts it.
        self.assertEqual(rp.compare("2.3.0-dev.1", "2.3.0"), -1)
        self.assertEqual(rp.compare("2.3.0-dev.1", "2.2.9"), 1)

    def test_rubbish_is_rejected_loudly(self):
        with self.assertRaises(ValueError):
            rp.version_key("not-a-version")

    def test_newest_published_ignores_prereleases_as_a_baseline(self):
        tags = ["flasher-v0.1.0", "flasher-v0.2.0", "flasher-v0.3.0-dev.1", "app-v9.9.9"]
        self.assertEqual(rp.newest_published(tags, "flasher-v"), "0.2.0")

    def test_newest_published_is_none_when_nothing_matches(self):
        self.assertIsNone(rp.newest_published(["app-v1.0.0"], "flasher-v"))

    def test_other_targets_tags_do_not_leak(self):
        tags = ["fw-v9.0.0", "flasher-v0.2.0"]
        self.assertEqual(rp.newest_published(tags, "flasher-v"), "0.2.0")


class DecideVersioned(unittest.TestCase):
    def decide(self, **kwargs):
        base = dict(source_version="0.3.0", latest_version="0.2.0", changed=True)
        base.update(kwargs)
        return rp.decide(VERSIONED, **base)

    def test_newer_source_releases(self):
        d = self.decide()
        self.assertEqual(d.decision, rp.RELEASE)
        self.assertTrue(d.actionable)

    def test_first_ever_release_is_cut(self):
        d = self.decide(latest_version=None, changed=False)
        self.assertEqual(d.decision, rp.RELEASE)

    def test_unchanged_and_already_released_does_nothing(self):
        # THE compute-saving case: pressing the button repeatedly is free.
        d = self.decide(source_version="0.2.0", changed=False)
        self.assertEqual(d.decision, rp.UP_TO_DATE)
        self.assertFalse(d.actionable)
        self.assertEqual(d.inputs, {})

    def test_changed_without_a_bump_asks_for_a_bump_and_does_not_ship(self):
        d = self.decide(source_version="0.2.0", changed=True)
        self.assertEqual(d.decision, rp.NEEDS_BUMP)
        self.assertFalse(d.actionable)
        self.assertIn("Bump it", d.reason)

    def test_source_behind_the_release_is_reported_not_shipped(self):
        d = self.decide(source_version="0.1.0", changed=False)
        self.assertEqual(d.decision, rp.BEHIND)
        self.assertFalse(d.actionable)

    def test_force_recuts_even_when_up_to_date(self):
        d = self.decide(source_version="0.2.0", changed=False, force=True)
        self.assertEqual(d.decision, rp.RELEASE)
        self.assertEqual(d.inputs, {"dry_run": "false"})

    def test_an_unreadable_version_is_a_note_not_a_crash(self):
        d = self.decide(source_version=None)
        self.assertEqual(d.decision, rp.NEEDS_BUMP)
        self.assertFalse(d.actionable)

    def test_unselected_targets_are_skipped(self):
        d = self.decide(selected=False)
        self.assertEqual(d.decision, rp.SKIPPED)
        self.assertFalse(d.actionable)


class DecideInputs(unittest.TestCase):
    def test_publish_uses_the_real_inputs(self):
        d = rp.decide(VERSIONED, source_version="0.3.0", latest_version="0.2.0",
                      changed=True, publish=True)
        self.assertEqual(d.inputs, {"dry_run": "false"})

    def test_dev_run_uses_the_build_only_inputs(self):
        # The dry-run mode must not be able to publish by accident.
        d = rp.decide(VERSIONED, source_version="0.3.0", latest_version="0.2.0",
                      changed=True, publish=False)
        self.assertEqual(d.inputs, {"dry_run": "true"})

    def test_version_is_substituted_into_inputs(self):
        target = dict(VERSIONED, inputs={"channel": "release", "version": "${version}"})
        d = rp.decide(target, source_version="2.3.0", latest_version="2.2.0",
                      changed=True, publish=True)
        self.assertEqual(d.inputs, {"channel": "release", "version": "2.3.0"})

    def test_inputs_are_all_strings(self):
        # workflow_dispatch rejects a bare boolean over the API.
        target = dict(VERSIONED, inputs={"publish": True})
        d = rp.decide(target, source_version="0.3.0", latest_version="0.2.0",
                      changed=True, publish=True)
        self.assertEqual(d.inputs, {"publish": "True"})
        self.assertTrue(all(isinstance(v, str) for v in d.inputs.values()))

    def test_a_target_without_dev_inputs_falls_back_to_its_real_inputs(self):
        target = dict(VERSIONED)
        target.pop("dev_inputs")
        d = rp.decide(target, source_version="0.3.0", latest_version="0.2.0",
                      changed=True, publish=False)
        self.assertEqual(d.inputs, {"dry_run": "false"})


class DecidePages(unittest.TestCase):
    def test_changed_pages_redeploy(self):
        d = rp.decide(PAGES, source_version=None, latest_version=None, changed=True)
        self.assertEqual(d.decision, rp.RELEASE)

    def test_unchanged_pages_are_left_alone(self):
        d = rp.decide(PAGES, source_version=None, latest_version=None, changed=False)
        self.assertEqual(d.decision, rp.UP_TO_DATE)

    def test_pages_never_ask_for_a_version_bump(self):
        # A versionless target must not fall into the versioned branches.
        for changed in (True, False):
            d = rp.decide(PAGES, source_version=None, latest_version=None, changed=changed)
            self.assertIn(d.decision, {rp.RELEASE, rp.UP_TO_DATE})


class Gating(unittest.TestCase):
    def test_a_gated_target_is_not_dispatched_when_its_var_is_off(self):
        d = rp.decide(GATED_TARGET, source_version="0.3.0", latest_version="0.2.0",
                      changed=True, gate_enabled=False)
        self.assertEqual(d.decision, rp.GATED)
        self.assertFalse(d.actionable)
        self.assertIn("ENABLE_TVOS_BUILD", d.reason)

    def test_a_gated_target_ships_normally_once_enabled(self):
        d = rp.decide(GATED_TARGET, source_version="0.3.0", latest_version="0.2.0",
                      changed=True, gate_enabled=True)
        self.assertEqual(d.decision, rp.RELEASE)

    def test_the_gate_beats_force(self):
        # Forcing a target whose workflow would no-op just burns a runner.
        d = rp.decide(GATED_TARGET, source_version="0.3.0", latest_version="0.2.0",
                      changed=True, gate_enabled=False, force=True)
        self.assertEqual(d.decision, rp.GATED)


class BuildPlan(unittest.TestCase):
    targets = [VERSIONED, PAGES, GATED_TARGET]

    def plan(self, **kwargs):
        kwargs.setdefault("changed_resolver", lambda target, latest: True)
        return rp.build_plan(self.targets, kwargs.pop("tags", []), **kwargs)

    def test_every_target_gets_exactly_one_decision(self):
        decisions = self.plan()
        self.assertEqual([d.name for d in decisions], ["flasher", "web", "tvos"])

    def test_only_narrows_the_run_without_dropping_rows(self):
        decisions = self.plan(only={"web"})
        by_name = {d.name: d for d in decisions}
        self.assertEqual(by_name["web"].decision, rp.RELEASE)
        # The others are still reported — as skipped, so the summary is complete.
        self.assertEqual(by_name["flasher"].decision, rp.SKIPPED)
        self.assertEqual(by_name["tvos"].decision, rp.SKIPPED)

    def test_gates_default_to_off_so_an_unset_variable_never_burns_a_runner(self):
        decisions = {d.name: d for d in self.plan()}
        self.assertEqual(decisions["tvos"].decision, rp.GATED)

    def test_nothing_actionable_is_a_valid_plan_not_an_error(self):
        # Pressing the button with nothing to do must be a calm no-op.
        decisions = rp.build_plan(
            [VERSIONED], ["flasher-v9.9.9"],
            changed_resolver=lambda target, latest: False,
        )
        self.assertFalse(any(d.actionable for d in decisions))


class ReadingVersionsFromDisk(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()

    def write(self, relative: str, content: str) -> None:
        path = os.path.join(self.root, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(content)

    def test_reads_a_json_key(self):
        self.write("desktop/src-tauri/tauri.conf.json", json.dumps({"version": "0.2.2"}))
        target = {"version": {"file": "desktop/src-tauri/tauri.conf.json", "json_path": "version"}}
        self.assertEqual(rp.read_source_version(target, self.root), "0.2.2")

    def test_reads_a_nested_json_key(self):
        self.write("a.json", json.dumps({"package": {"version": "1.2.3"}}))
        target = {"version": {"file": "a.json", "json_path": "package.version"}}
        self.assertEqual(rp.read_source_version(target, self.root), "1.2.3")

    def test_reads_a_regex_capture(self):
        self.write("tvos/WitnessWall/project.yml", 'settings:\n  base:\n    MARKETING_VERSION: "0.1.0"\n')
        target = {"version": {
            "file": "tvos/WitnessWall/project.yml",
            "regex": r'^\s*MARKETING_VERSION:\s*"?([0-9][0-9.]*)"?\s*$',
        }}
        self.assertEqual(rp.read_source_version(target, self.root), "0.1.0")

    def test_a_missing_file_is_none_not_an_exception(self):
        target = {"version": {"file": "nope.json", "json_path": "version"}}
        self.assertIsNone(rp.read_source_version(target, self.root))

    def test_malformed_json_is_none_not_an_exception(self):
        self.write("a.json", "{ not json")
        target = {"version": {"file": "a.json", "json_path": "version"}}
        self.assertIsNone(rp.read_source_version(target, self.root))

    def test_a_missing_key_is_none(self):
        self.write("a.json", json.dumps({"other": 1}))
        target = {"version": {"file": "a.json", "json_path": "version"}}
        self.assertIsNone(rp.read_source_version(target, self.root))


class TheRealCatalog(unittest.TestCase):
    """The catalog is data, so it gets the same scrutiny as code."""

    @classmethod
    def setUpClass(cls):
        cls.targets = rp.load_catalog()

    def test_it_parses_and_has_targets(self):
        self.assertTrue(self.targets)

    def test_every_target_is_complete(self):
        for target in self.targets:
            with self.subTest(target=target.get("name")):
                self.assertIn("label", target)
                self.assertIn("workflow", target)
                self.assertIn("watch", target)
                if target.get("kind") != "pages":
                    self.assertIn("tag_prefix", target, "a versioned target needs a tag prefix")
                    self.assertIn("version", target)

    def test_every_workflow_it_names_actually_exists(self):
        # The exact rot this button exists to avoid: dispatching a workflow
        # that was renamed months ago and failing at the worst moment.
        workflows_dir = os.path.join(rp.REPO_ROOT, ".github", "workflows")
        for target in self.targets:
            with self.subTest(target=target["name"]):
                self.assertTrue(
                    os.path.exists(os.path.join(workflows_dir, target["workflow"])),
                    f"{target['name']} points at a missing workflow: {target['workflow']}",
                )

    def test_every_versioned_targets_version_is_readable_right_now(self):
        for target in self.targets:
            if target.get("kind") == "pages":
                continue
            with self.subTest(target=target["name"]):
                version = rp.read_source_version(target)
                self.assertIsNotNone(version, f"{target['name']}'s version file/selector is wrong")
                rp.version_key(version)  # raises if it isn't a version

    def test_tag_prefixes_are_unique(self):
        prefixes = [t["tag_prefix"] for t in self.targets if t.get("tag_prefix")]
        self.assertEqual(len(prefixes), len(set(prefixes)))

    def test_the_apple_targets_are_gated(self):
        by_name = {t["name"]: t for t in self.targets}
        self.assertEqual(by_name["tvos"].get("gate_var"), "ENABLE_TVOS_BUILD")
        self.assertEqual(by_name["ios"].get("gate_var"), "ENABLE_IOS_BUILD")

    def test_the_pages_target_watches_what_pages_actually_deploys(self):
        # If pages.yml's path filter and the catalog's watch list drift apart,
        # the button either redeploys for nothing or misses a real doc change.
        import yaml as _yaml

        with open(os.path.join(rp.REPO_ROOT, ".github", "workflows", "pages.yml"), encoding="utf-8") as handle:
            workflow = _yaml.safe_load(handle)
        triggers = workflow.get("on", workflow.get(True, {}))
        deployed_paths = triggers["push"]["paths"]
        # Normalize: the workflow uses globs, the catalog uses directories.
        deployed = {p.replace("/**", "").rstrip("/") for p in deployed_paths}
        deployed.discard(".github/workflows/pages.yml")

        web = next(t for t in rp.load_catalog() if t["name"] == "web")
        self.assertEqual(set(web["watch"]), deployed)


if __name__ == "__main__":
    unittest.main()
