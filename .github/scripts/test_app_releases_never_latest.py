#!/usr/bin/env python3
"""No desktop app release may be born as `releases/latest`, even for a moment.

`releases/latest` is the URL every Canary polls for its OTA manifest
(`releases/latest/download/manifest-*.json`), so it must always be a fw-v*
release. On 2026-10-10 the Flasher's Linux leg let tauri-action create
flasher-v0.12.0 as an ordinary published release, GitHub made it `latest` on
the spot, and the fleet's update URL pointed at the Flasher for three minutes
until another workflow's guard put it back (.github/RELEASE_LESSONS.md
2026-10-10). The repair job (keep-firmware-latest) runs after the builds by
design, so it cannot close a window that opens first.

These tests hold the shapes that close it:

  * the Flasher creates its release itself, with `--latest=false`, before any
    tauri-action step runs, and hands tauri-action the release's id, never a
    tag (given a tag it cannot find, tauri-action creates the release);
  * the Flasher's own edits of that release pass `--latest=false`, a step
    after publishing fails the run if `latest` is not fw-v*, and the
    keep-firmware-latest backstop still runs under always();
  * the Lab's tauri-action steps build DRAFTS (a draft is never `latest`),
    its draft edits keep it one, and the button that publishes it does so
    with `--latest=false`.

Run:  python3 -m unittest discover -s .github/scripts -p 'test_*.py' -v
"""

from __future__ import annotations

import os
import re
import unittest

import yaml

WORKFLOWS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "workflows")


def load(name: str) -> dict:
    with open(os.path.join(WORKFLOWS, name), encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def is_tauri_action(step: dict) -> bool:
    return str(step.get("uses", "")).startswith("tauri-apps/tauri-action@")


def gh_release_edits(run: str) -> list[str]:
    """Each `gh release edit …` command in a run block, continuation lines joined."""
    joined = re.sub(r"\\\n\s*", " ", run)
    return [line.strip() for line in joined.splitlines() if "gh release edit" in line]


class FlasherReleaseIsNeverBornLatest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.wf = load("desktop-flasher-release.yml")
        cls.build = cls.wf["jobs"]["build"]["steps"]
        cls.finalize = cls.wf["jobs"]["finalize"]["steps"]

    def test_tauri_action_gets_a_release_id_never_a_tag(self):
        tauri = [s for s in self.build if is_tauri_action(s)]
        self.assertGreaterEqual(len(tauri), 2, "expected the unsigned and the signed build steps")
        for step in tauri:
            with self.subTest(step=step["name"]):
                inputs = step.get("with", {})
                self.assertNotIn("tagName", inputs,
                                 "a tag lets tauri-action create the release itself, as releases/latest")
                self.assertEqual(inputs.get("releaseId"), "${{ steps.release.outputs.id }}")

    def test_the_release_is_created_with_latest_false_before_any_upload(self):
        ids = [s.get("id") for s in self.build]
        self.assertIn("release", ids, "the build job no longer creates its own release")
        create_at = ids.index("release")
        create = self.build[create_at]
        self.assertIn("gh release create", create["run"])
        self.assertIn("--latest=false", create["run"], "the release must be created with --latest=false")
        first_tauri = min(i for i, s in enumerate(self.build) if is_tauri_action(s))
        self.assertLess(create_at, first_tauri, "the release must exist before tauri-action uploads")

    def test_edits_of_the_versioned_release_never_ask_for_latest(self):
        edits = [cmd for s in self.finalize for cmd in gh_release_edits(s.get("run", ""))
                 if '"$TAG"' in cmd]
        self.assertTrue(edits, "finalize no longer edits the release body — update this test")
        for cmd in edits:
            with self.subTest(cmd=cmd):
                self.assertIn("--latest=false", cmd)

    def test_a_step_after_publishing_fails_unless_latest_is_firmware(self):
        guard = self.finalize[-1]
        self.assertIn("releases/latest", guard["run"])
        self.assertIn("fw-v*", guard["run"])
        self.assertIn("exit 1", guard["run"])
        self.assertEqual(guard.get("if"), "success() || failure()",
                         "the guard must run even when an earlier finalize step failed")

    def test_the_keep_firmware_latest_backstop_still_runs_under_always(self):
        job = self.wf["jobs"]["keep-latest"]
        self.assertTrue(str(job["if"]).startswith("always()"))
        self.assertIn("./.github/actions/keep-firmware-latest", [s.get("uses") for s in job["steps"]])


class LabReleaseIsADraftUntilPublishedNotLatest(unittest.TestCase):
    def test_every_tauri_action_step_builds_a_draft(self):
        steps = load("desktop-release.yml")["jobs"]["build"]["steps"]
        tauri = [s for s in steps if is_tauri_action(s)]
        self.assertGreaterEqual(len(tauri), 2)
        for step in tauri:
            with self.subTest(step=step["name"]):
                self.assertIn(step["with"].get("releaseDraft"), (True, "true"))

    def test_the_draft_body_edit_keeps_it_a_draft(self):
        steps = load("desktop-release.yml")["jobs"]["finalize"]["steps"]
        edits = [cmd for s in steps for cmd in gh_release_edits(s.get("run", ""))]
        self.assertTrue(edits)
        for cmd in edits:
            with self.subTest(cmd=cmd):
                self.assertRegex(cmd, r"--draft(\s|$)")

    def test_the_publish_button_never_asks_for_latest(self):
        steps = load("lab-publish.yml")["jobs"]["publish"]["steps"]
        publishes = [cmd for s in steps for cmd in gh_release_edits(s.get("run", ""))
                     if "--draft=false" in cmd]
        self.assertTrue(publishes, "lab-publish.yml no longer publishes with gh release edit — update this test")
        for cmd in publishes:
            with self.subTest(cmd=cmd):
                self.assertIn("--latest=false", cmd)


if __name__ == "__main__":
    unittest.main()
