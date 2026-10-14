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
import re
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


class NameLists(unittest.TestCase):
    # `force:` / `--only` are typed by a human into an Actions input box and
    # arrive with the spaces humans type. A token lost to whitespace would
    # silently force one app and not the other, with no error anywhere.
    def test_spaces_around_commas_are_not_part_of_a_name(self):
        self.assertEqual(rp.split_names("flasher, lab"), {"flasher", "lab"})
        self.assertEqual(rp.split_names(" all "), {"all"})

    def test_blank_means_nothing(self):
        self.assertEqual(rp.split_names(""), set())
        self.assertEqual(rp.split_names(" , "), set())


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

    def test_force_on_an_already_released_version_names_the_overwrite(self):
        # The retired one-click launcher's preflight, carried into the plan:
        # re-cutting a tagged app version rewrites that release's assets, and
        # the summary row is the only place a presser will read it.
        d = self.decide(source_version="0.2.0", changed=False, force=True)
        self.assertEqual(d.decision, rp.RELEASE)
        self.assertIn("OVERWRITE", d.reason)
        self.assertIn("0.2.0", d.reason)

    def test_force_on_a_newer_version_does_not_cry_wolf(self):
        # Forcing a version that is genuinely ahead is an ordinary release.
        d = self.decide(source_version="0.3.0", changed=False, force=True)
        self.assertEqual(d.decision, rp.RELEASE)
        self.assertNotIn("OVERWRITE", d.reason)

    def test_an_unreadable_version_is_a_note_not_a_crash(self):
        d = self.decide(source_version=None)
        self.assertEqual(d.decision, rp.NEEDS_BUMP)
        self.assertFalse(d.actionable)

    def test_unselected_targets_are_skipped(self):
        d = self.decide(selected=False)
        self.assertEqual(d.decision, rp.SKIPPED)
        self.assertFalse(d.actionable)


class DraftReleases(unittest.TestCase):
    """A draft release has no git tag (GitHub creates it on publish), so the
    tag list never shows it. Before drafts were read from the API, a built
    but unpublished Lab draft re-planned as RELEASE on every press and the
    same build was dispatched again; four stale drafts piled up that way."""

    LAB = dict(VERSIONED, name="lab", tag_prefix="app-v",
               version={"file": "v.json", "json_path": "version"},
               draft_publish='Actions → "Publish the Lab (draft → live)"')

    def plan(self, tags, drafts, **kwargs):
        kwargs.setdefault("changed_resolver", lambda target, latest: True)
        (decision,) = rp.build_plan([self.LAB], tags, draft_tags=drafts, **kwargs)
        return decision

    def test_a_draft_at_the_source_version_waits_for_its_publish(self):
        with tempfile.TemporaryDirectory() as root:
            self._version(root, "0.2.5")
            d = self.plan(["app-v0.2.4"], ["app-v0.2.5"], publish=True, repo_root=root)
        self.assertEqual(d.decision, rp.DRAFT_PENDING)
        self.assertFalse(d.actionable)
        self.assertEqual(d.inputs, {})
        self.assertIn("Publish the Lab", d.reason)
        self.assertIn("app-v0.2.5", d.reason)

    def test_the_first_release_waiting_as_a_draft_is_not_cut_again(self):
        with tempfile.TemporaryDirectory() as root:
            self._version(root, "0.1.0")
            d = self.plan([], ["app-v0.1.0"], publish=True, repo_root=root)
        self.assertEqual(d.decision, rp.DRAFT_PENDING)

    def test_stale_drafts_below_the_published_version_change_nothing(self):
        with tempfile.TemporaryDirectory() as root:
            self._version(root, "0.2.4")
            d = self.plan(["app-v0.2.4"], ["app-v0.1.1", "app-v0.1.2", "app-v0.2.0", "app-v0.2.1"],
                          publish=True, repo_root=root,
                          changed_resolver=lambda target, latest: False)
        self.assertEqual(d.decision, rp.UP_TO_DATE)

    def test_a_draft_of_an_older_version_does_not_hold_back_a_newer_one(self):
        with tempfile.TemporaryDirectory() as root:
            self._version(root, "0.2.6")
            d = self.plan(["app-v0.2.4"], ["app-v0.2.5"], publish=True, repo_root=root)
        self.assertEqual(d.decision, rp.RELEASE)
        self.assertEqual(d.inputs, {"dry_run": "false"})

    def test_force_rebuilds_the_draft_on_purpose(self):
        with tempfile.TemporaryDirectory() as root:
            self._version(root, "0.2.5")
            d = self.plan(["app-v0.2.4"], ["app-v0.2.5"], publish=True, force={"lab"}, repo_root=root)
        self.assertEqual(d.decision, rp.RELEASE)

    def test_a_build_only_run_still_builds(self):
        # A smoke run cuts no tag and publishes nothing, so a pending draft is
        # no reason to skip it.
        with tempfile.TemporaryDirectory() as root:
            self._version(root, "0.2.5")
            d = self.plan(["app-v0.2.4"], ["app-v0.2.5"], publish=False, repo_root=root)
        self.assertEqual(d.decision, rp.RELEASE)
        self.assertEqual(d.inputs, {"dry_run": "true"})

    def test_without_a_draft_publish_hint_the_reason_still_says_where(self):
        d = rp.decide(VERSIONED, source_version="0.3.0", latest_version="0.2.0",
                      changed=True, draft_pending=True)
        self.assertEqual(d.decision, rp.DRAFT_PENDING)
        self.assertIn("Releases page", d.reason)

    def test_the_cli_reads_the_drafts_file(self):
        import io
        from contextlib import redirect_stdout
        from unittest import mock

        lab = next(t for t in rp.load_catalog() if t["name"] == "lab")
        version = rp.read_source_version(lab)
        with tempfile.TemporaryDirectory() as root:
            drafts = os.path.join(root, "drafts.json")
            with open(drafts, "w", encoding="utf-8") as handle:
                json.dump([f"app-v{version}"], handle)
            out = io.StringIO()
            with mock.patch.object(rp.sys, "stdin", io.StringIO("[]")), redirect_stdout(out):
                code = rp.main(["--publish", "--only=lab", f"--drafts-file={drafts}"])
            self.assertEqual(code, 0)
            row = next(d for d in json.loads(out.getvalue()) if d["name"] == "lab")
            self.assertEqual(row["decision"], rp.DRAFT_PENDING)
            with open(drafts, "w", encoding="utf-8") as handle:
                json.dump({"not": "a list"}, handle)
            with mock.patch.object(rp.sys, "stdin", io.StringIO("[]")), \
                    redirect_stdout(io.StringIO()), \
                    mock.patch.object(rp.sys, "stderr", io.StringIO()):
                self.assertEqual(rp.main([f"--drafts-file={drafts}"]), 2)

    @staticmethod
    def _version(root: str, version: str) -> None:
        with open(os.path.join(root, "v.json"), "w", encoding="utf-8") as handle:
            json.dump({"version": version}, handle)


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

    def test_a_target_without_dev_inputs_sits_out_a_dev_run(self):
        # The former fallback ran such a target with its REAL inputs, so a
        # "dev build" press could cut a signed firmware release. Not anymore.
        target = dict(VERSIONED)
        target.pop("dev_inputs")
        d = rp.decide(target, source_version="0.3.0", latest_version="0.2.0",
                      changed=True, publish=False)
        self.assertEqual(d.decision, rp.SKIPPED)
        self.assertEqual(d.inputs, {})
        self.assertIn("Tick publish", d.reason)

    def test_force_does_not_override_the_dev_run_skip(self):
        target = dict(VERSIONED)
        target.pop("dev_inputs")
        d = rp.decide(target, source_version="0.3.0", latest_version="0.2.0",
                      changed=True, publish=False, force=True)
        self.assertEqual(d.decision, rp.SKIPPED)

    def test_a_target_without_dev_inputs_still_publishes_when_asked(self):
        target = dict(VERSIONED)
        target.pop("dev_inputs")
        d = rp.decide(target, source_version="0.3.0", latest_version="0.2.0",
                      changed=True, publish=True)
        self.assertEqual(d.decision, rp.RELEASE)

    def test_unknown_force_or_only_names_are_refused(self):
        targets = [dict(VERSIONED, name="flasher"), dict(VERSIONED, name="lab")]
        self.assertEqual(rp.unknown_names({"flahser"}, targets), {"flahser"})
        self.assertEqual(rp.unknown_names({"flasher", "lab", "all"}, targets), set())
        self.assertEqual(rp.unknown_names(set(), targets), set())


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

    def test_a_target_can_explain_its_own_gate(self):
        # The firmware gate is "does the release signing key exist" — a derived
        # condition, not a repo variable. Naming a flag would be useless there;
        # the useful message is the ceremony, so a target may carry its own.
        keyed = dict(
            VERSIONED,
            name="firmware",
            tag_prefix="fw-v",
            gate_var="OTA_SIGNING_KEY_READY",
            gate_reason="No signing key: run setup_release_key.sh, then add the secret.",
        )
        d = rp.decide(keyed, source_version="2.4.0", latest_version="2.3.0",
                      changed=True, gate_enabled=False)
        self.assertEqual(d.decision, rp.GATED)
        self.assertFalse(d.actionable)
        self.assertIn("setup_release_key.sh", d.reason)
        # And it must NOT fall back to the Apple wording, which would send
        # someone to look for a repo variable that was never the problem.
        self.assertNotIn("tvos/README.md", d.reason)
        self.assertNotIn("is not 'true'", d.reason)

    def test_a_gate_without_a_reason_still_gets_the_default_text(self):
        d = rp.decide(GATED_TARGET, source_version="0.3.0", latest_version="0.2.0",
                      changed=True, gate_enabled=False)
        self.assertIn("is not 'true'", d.reason)

    def test_a_keyed_target_ships_once_the_gate_opens(self):
        keyed = dict(
            VERSIONED, name="firmware", tag_prefix="fw-v",
            gate_var="OTA_SIGNING_KEY_READY", gate_reason="…",
        )
        d = rp.decide(keyed, source_version="2.4.0", latest_version="2.3.0",
                      changed=True, gate_enabled=True)
        self.assertEqual(d.decision, rp.RELEASE)


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

    def test_every_publishing_target_actually_publishes_and_tags(self):
        """The button must never report a release that didn't happen.

        Regression guard for a real bug: the iOS row dispatched
        `ios-release.yml` with `export_method: app-store-connect`, but that
        workflow only exported an .ipa as a build artifact — no upload, no
        `ios-v*` tag. The summary claimed iOS shipped while users got nothing,
        and because "shipped" is derived from tags, the target stayed eligible
        and burned a macOS runner on every press.

        So: every versioned target's publish inputs must actually reach a
        publishing path, and its workflow must be able to create the tag this
        planner reads back.
        """
        workflows_dir = os.path.join(rp.REPO_ROOT, ".github", "workflows")
        for target in self.targets:
            if target.get("kind") == "pages":
                continue
            name = target["name"]
            with self.subTest(target=name):
                path = os.path.join(workflows_dir, target["workflow"])
                with open(path, encoding="utf-8") as handle:
                    source = handle.read()

                # It must be able to write the tag the planner reads.
                self.assertIn(
                    "contents: write",
                    source,
                    f"{name}: {target['workflow']} cannot create a {target['tag_prefix']}* tag "
                    f"(no `contents: write`), so a successful publish would be invisible to "
                    f"this planner and re-dispatched forever",
                )
                # And it must know about its own tag namespace at all — whether
                # it creates the tag with `git tag` (the Apple targets) or via
                # the release API's `tag_name` (firmware, and tauri-action for
                # the desktop apps).
                self.assertIn(
                    target["tag_prefix"],
                    source,
                    f"{name}: {target['workflow']} never mentions {target['tag_prefix']}*, "
                    f"so it cannot be producing the tags this planner reads back",
                )

    def test_every_pins_file_a_release_reads_is_in_its_watch(self):
        # RELEASE_LESSONS 2026-09-23 (b): the bundled espflash's version and
        # sha256 pins lived only inside the two desktop release workflows,
        # which no watch named, so a pin bump alone marked neither app as
        # changed and released nothing. A pins file (.github/*.env) that a
        # target's release workflow reads is an input to what it ships, so it
        # must be in that target's watch.
        workflows_dir = os.path.join(rp.REPO_ROOT, ".github", "workflows")
        readers = set()
        for target in self.targets:
            with open(os.path.join(workflows_dir, target["workflow"]), encoding="utf-8") as handle:
                source = handle.read()
            for path in sorted(set(re.findall(r"\.github/[\w.-]+\.env\b", source))):
                with self.subTest(target=target["name"], path=path):
                    self.assertTrue(
                        os.path.isfile(os.path.join(rp.REPO_ROOT, path)),
                        f"{target['workflow']} reads {path}, which does not exist",
                    )
                    self.assertIn(
                        path,
                        target["watch"],
                        f"{target['name']}'s release reads {path} but its watch does not name it — "
                        f"a change to it alone would be reported as nothing to do",
                    )
                if path == ".github/espflash-pins.env":
                    readers.add(target["name"])
        # Not vacuous: both desktop apps bundle espflash from that one file.
        self.assertEqual(readers, {"flasher", "lab"})

    def test_every_file_a_desktop_app_embeds_is_in_its_watch(self):
        # The pins-file lesson above, one layer down (RELEASE_LESSONS
        # 2026-09-23 (c)): a desktop app's binary carries every file that any
        # crate it links by path include_str!s or include_bytes!s by literal
        # path (the Flasher's hub-io embeds the Pi hub's provisioning bundle),
        # so each such file, and each such crate's directory, is an input to
        # what the app ships and must be in that target's watch. Path
        # dependencies are followed through each Cargo.toml, transitively, so
        # a new crate is covered without editing this test. The
        # concat!(env!("OUT_DIR"), ...) form reads a copy build.rs made, which
        # the watch comments tie to build.rs; any other form fails here
        # rather than being skipped unread.
        import tomllib

        apps = {"flasher": "desktop/src-tauri", "lab": "desktop-lab/src-tauri"}
        by_name = {t["name"]: t for t in self.targets}
        root = rp.REPO_ROOT
        embed = re.compile(r"\binclude_(?:str|bytes)!\s*\(\s*")
        literal = re.compile(r'"([^"\\]+)"\s*\)')
        out_dir = re.compile(r'concat!\s*\(\s*env!\s*\(\s*"OUT_DIR"\s*\)')

        def covered(rel: str, watch: list) -> bool:
            return any(rel == w or rel.startswith(w.rstrip("/") + "/") for w in watch)

        def path_deps(crate: str) -> set:
            with open(os.path.join(crate, "Cargo.toml"), "rb") as handle:
                manifest = tomllib.load(handle)
            tables = [manifest.get("dependencies", {})]
            tables += [t.get("dependencies", {}) for t in manifest.get("target", {}).values()]
            return {
                os.path.normpath(os.path.join(crate, spec["path"]))
                for table in tables
                for spec in table.values()
                if isinstance(spec, dict) and "path" in spec
            }

        linked, embedded, copies = {}, {}, {}
        for name, app_dir in apps.items():
            watch = by_name[name]["watch"]
            crates, todo = set(), [os.path.join(root, app_dir)]
            while todo:
                crate = todo.pop()
                if crate not in crates:
                    crates.add(crate)
                    todo.extend(path_deps(crate))
            linked[name] = {os.path.relpath(c, root) for c in crates}
            embedded[name], copies[name] = set(), 0
            for crate in sorted(crates):
                rel_crate = os.path.relpath(crate, root)
                with self.subTest(target=name, links=rel_crate):
                    self.assertTrue(
                        covered(rel_crate, watch),
                        f"{name} links {rel_crate} but its watch does not cover it — "
                        f"a change to that crate alone would be reported as nothing to do",
                    )
                for dirpath, _dirs, names in os.walk(os.path.join(crate, "src")):
                    for rs in sorted(n for n in names if n.endswith(".rs")):
                        path = os.path.join(dirpath, rs)
                        where = os.path.relpath(path, root)
                        with open(path, encoding="utf-8") as handle:
                            source = handle.read()
                        for found in embed.finditer(source):
                            rest = source[found.end():]
                            lit = literal.match(rest)
                            if lit is None:
                                self.assertIsNotNone(
                                    out_dir.match(rest),
                                    f"{where}: an embed this test cannot read — teach it the "
                                    f"form, or its file goes unwatched",
                                )
                                copies[name] += 1
                                continue
                            rel = os.path.relpath(
                                os.path.normpath(os.path.join(dirpath, lit.group(1))), root
                            )
                            embedded[name].add(rel)
                            with self.subTest(target=name, file=where, embeds=rel):
                                self.assertTrue(
                                    os.path.isfile(os.path.join(root, rel)),
                                    f"{where} embeds {rel}, which does not exist",
                                )
                                self.assertTrue(
                                    covered(rel, watch),
                                    f"{name} embeds {rel} but its watch does not name it — "
                                    f"a change to it alone would be reported as nothing to do",
                                )
        # Not vacuous: the Flasher's hub-io embeds the five bundle files it
        # seeds onto a hub's card; the Lab reaches hub-core only through
        # flash-engine (so the walk is transitive) and embeds flash.json
        # through it; and both apps' OUT_DIR copies were seen and set aside.
        self.assertLessEqual(
            {
                "canary-local/devices/hub_seed.json",
                "canary-local/devices/hub_provision_bundle.json",
                "canary-local/tools/hub_seed_apply.py",
                "canary-local/tools/hub_host_provision.sh",
                "homeassistant/frigate/config.yaml",
            },
            embedded["flasher"],
        )
        self.assertIn("desktop/hub-core", linked["lab"])
        self.assertIn("canary-local/devices/flash.json", embedded["lab"])
        self.assertGreater(copies["flasher"], 0)
        self.assertGreater(copies["lab"], 0)

    def test_every_source_the_wall_compiles_is_in_the_tvos_watch(self):
        # The desktop-embed lesson above, on Apple TV: the Witness Wall
        # compiles Swift files from the iPhone tree (project.yml's
        # `- path: ../../ios/...` sources), and the tvos watch said only
        # `tvos`, so a change to those files alone (the device figures were
        # regenerated for a new product) planned the Wall as "unchanged —
        # nothing to do" while tvos.yml rebuilt it. Both directions are held:
        # every source the Wall reaches outside tvos/ is watched, and every
        # watch entry outside tvos/ is something the Wall still compiles, so
        # an iPhone-only edit never asks for a TV release.
        # workflows-lint.yml's path filter lists project.yml for this test.
        project = os.path.join(rp.REPO_ROOT, "tvos", "WitnessWall", "project.yml")
        with open(project, encoding="utf-8") as handle:
            text = handle.read()
        base = os.path.dirname(project)
        compiled = set()
        for raw in re.findall(r"^\s*-\s*path:\s*(\S+)\s*$", text, re.MULTILINE):
            rel = os.path.relpath(os.path.normpath(os.path.join(base, raw)), rp.REPO_ROOT)
            if not rel.startswith("tvos" + os.sep) and rel != "tvos":
                compiled.add(rel.replace(os.sep, "/"))
        tvos = next(t for t in self.targets if t["name"] == "tvos")
        watch = tvos["watch"]

        def covered(rel: str) -> bool:
            return any(rel == w or rel.startswith(w.rstrip("/") + "/") for w in watch)

        for rel in sorted(compiled):
            with self.subTest(compiles=rel):
                self.assertTrue(
                    os.path.exists(os.path.join(rp.REPO_ROOT, rel)),
                    f"tvos/WitnessWall/project.yml compiles {rel}, which does not exist",
                )
                self.assertTrue(
                    covered(rel),
                    f"the Wall compiles {rel} but the tvos watch does not name it — a change "
                    f"to it alone would be reported as nothing to do. Add `- {rel}` under "
                    f"the tvos row in .github/release-targets.yml",
                )
        for entry in watch:
            if entry == "tvos" or entry.startswith("tvos/"):
                continue
            with self.subTest(watches=entry):
                self.assertTrue(
                    any(rel == entry or rel.startswith(entry.rstrip("/") + "/") for rel in compiled),
                    f"the tvos watch names {entry}, which the Wall no longer compiles — an edit "
                    f"to it would ask for a TV release that changes nothing on the TV",
                )
        # Not vacuous: the Wall compiles shared files from both iPhone trees.
        self.assertIn("ios/Shared/FleetFigures.swift", compiled)
        self.assertIn("ios/Sources/SecuraCV/Model/CanaryMoodKeeper.swift", compiled)
        self.assertGreaterEqual(len(compiled), 15)

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


class SbomsRideEveryRelease(unittest.TestCase):
    """sbom.yml attached the SBOMs on `release: published`, an event GitHub
    never sends for a release created with the GITHUB_TOKEN — which is every
    release CI cuts. By 2026-10-09 only 3 of 64 published releases carried
    them, while the README said every release did. Each workflow that
    publishes a release now CALLS sbom.yml with the tag; this holds every one
    of them to it."""

    PUBLISHERS = {
        "firmware-release.yml",       # fw-v*  (the firmware row)
        "desktop-flasher-release.yml",  # flasher-v*  (the flasher row)
        "lab-publish.yml",            # app-v*: desktop-release.yml only drafts
        "release.yml",                # v*: the kernel's binaries
    }

    @classmethod
    def setUpClass(cls):
        import yaml as _yaml

        cls.dir = os.path.join(rp.REPO_ROOT, ".github", "workflows")
        cls.docs = {}
        for name in cls.PUBLISHERS | {"sbom.yml"}:
            with open(os.path.join(cls.dir, name), encoding="utf-8") as handle:
                cls.docs[name] = _yaml.safe_load(handle)

    def test_sbom_yml_takes_a_tag_and_attaches_for_it(self):
        doc = self.docs["sbom.yml"]
        triggers = doc.get("on", doc.get(True, {}))
        self.assertEqual(triggers["workflow_call"]["inputs"]["tag"]["type"], "string")
        self.assertTrue(triggers["workflow_call"]["inputs"]["tag"]["required"])
        attach = doc["jobs"]["attach-to-release"]
        self.assertIn("inputs.tag", attach["if"])
        self.assertIn("github.event_name == 'release'", attach["if"])
        checkout = doc["jobs"]["generate-sbom"]["steps"][0]
        self.assertEqual(checkout["with"]["ref"], "${{ inputs.tag || '' }}")

    def test_every_release_publisher_calls_sbom_with_its_tag(self):
        for name in sorted(self.PUBLISHERS):
            with self.subTest(workflow=name):
                calls = [
                    job for job in self.docs[name]["jobs"].values()
                    if job.get("uses") == "./.github/workflows/sbom.yml"
                ]
                self.assertEqual(len(calls), 1, f"{name} publishes a release but never calls sbom.yml")
                call = calls[0]
                self.assertTrue(call.get("needs"), f"{name}: the sbom call must wait for the publish")
                self.assertIn("${{", str(call.get("with", {}).get("tag", "")))
                self.assertEqual(call.get("permissions", {}).get("contents"), "write")

    def test_the_catalogs_release_workflows_are_covered(self):
        # Not vacuous: the catalog's GitHub-release targets publish through
        # these files (the Lab's own workflow only builds the draft).
        by_name = {t["name"]: t for t in rp.load_catalog()}
        self.assertIn(by_name["firmware"]["workflow"], self.PUBLISHERS)
        self.assertIn(by_name["flasher"]["workflow"], self.PUBLISHERS)
        self.assertEqual(by_name["lab"]["workflow"], "desktop-release.yml")


if __name__ == "__main__":
    unittest.main()
