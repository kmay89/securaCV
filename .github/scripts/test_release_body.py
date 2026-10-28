"""release_body.py runs the release workflows' own body steps — held to that.

The real workflow files are read from the tree; the notes and the changelog
they turn into a body are fixtures in a scratch root, so a CHANGELOG edit
never runs (or breaks) this suite. The one script the steps call from
outside .github/, desktop/scripts/release_notes.py, is copied into the root
and listed in workflows-lint.yml's path filters.
"""
from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_body as rb  # noqa: E402

REPO = Path(__file__).resolve().parents[2]

FLASHER_NOTES = """# SecuraCV Flasher — release notes

Heading grammar is `## <version> — <YYYY-MM-DD>`.

## 1.2.3 — 2026-01-02

**A fixture line the Flasher says.**

- One fixture bullet.

## 1.2.2 — 2026-01-01

**An older section that must not appear.**
"""

LAB_NOTES = FLASHER_NOTES.replace("Flasher", "Lab")

CHANGELOG = """# Changelog

## [Unreleased]

## [9.9.9] - 2026-01-02

> **A fixture notice.**

### A fixture section

- A fixture entry.

## [9.9.8] - 2026-01-01

- An older entry that must not appear.
"""


class Root:
    """A scratch tree with the real workflows and fixture notes."""

    def __init__(self, changelog: str = CHANGELOG):
        self._tmp = tempfile.TemporaryDirectory()
        self.path = Path(self._tmp.name)
        wf = self.path / ".github" / "workflows"
        wf.mkdir(parents=True)
        for workflow, *_ in rb.STEPS.values():
            shutil.copy(REPO / ".github" / "workflows" / workflow, wf / workflow)
        scripts = self.path / "desktop" / "scripts"
        scripts.mkdir(parents=True)
        shutil.copy(REPO / "desktop" / "scripts" / "release_notes.py", scripts / "release_notes.py")
        (self.path / "desktop" / "RELEASE_NOTES.md").write_text(FLASHER_NOTES, encoding="utf-8")
        (self.path / "desktop-lab").mkdir()
        (self.path / "desktop-lab" / "RELEASE_NOTES.md").write_text(LAB_NOTES, encoding="utf-8")
        (self.path / "CHANGELOG.md").write_text(changelog, encoding="utf-8")

    def close(self):
        self._tmp.cleanup()


class TheRealSteps(unittest.TestCase):
    def test_each_kind_names_exactly_one_runnable_step(self):
        for prefix, (workflow, step, _ver, _handoff) in rb.STEPS.items():
            with self.subTest(prefix=prefix):
                script = rb.step_script(workflow, step)
                self.assertNotIn("${{", script)

    def test_the_firmware_step_still_writes_the_file_it_is_read_from(self):
        workflow, step, *_ = rb.STEPS["fw-v"]
        self.assertIn(rb.FW_NOTES_PATH, rb.step_script(workflow, step))


class Composing(unittest.TestCase):
    def setUp(self):
        self.root = Root()
        self.addCleanup(self.root.close)

    def compose(self, tag, signed=True):
        return rb.compose(tag, signed=signed, repo="owner/name", root=self.root.path)

    def test_flasher_body_is_this_versions_notes_then_the_install_text(self):
        body = self.compose("flasher-v1.2.3")
        self.assertTrue(body.startswith("## What's changing in 1.2.3\n"), body[:80])
        self.assertIn("**A fixture line the Flasher says.**", body)
        self.assertIn("- One fixture bullet.", body)
        self.assertNotIn("older section", body)
        self.assertIn("\n---\n", body)

    def test_the_signing_answer_picks_the_macos_paragraph(self):
        for tag in ("flasher-v1.2.3", "app-v1.2.3"):
            with self.subTest(tag=tag):
                signed, unsigned = self.compose(tag, True), self.compose(tag, False)
                self.assertNotEqual(signed, unsigned)
                self.assertIn("unsigned", unsigned)
                self.assertNotIn("unsigned", signed)

    def test_lab_body_is_the_labs_notes(self):
        body = self.compose("app-v1.2.3")
        self.assertTrue(body.startswith("## What's changing in 1.2.3\n"), body[:80])
        self.assertIn("**A fixture line the Lab says.**", body)

    def test_firmware_body_is_the_changelog_section(self):
        body = self.compose("fw-v9.9.9")
        self.assertIn("> **A fixture notice.**", body)
        self.assertIn("### A fixture section", body)
        self.assertNotIn("older entry", body)

    def test_a_long_firmware_section_is_capped_with_a_link_to_the_tag(self):
        long_section = "".join(f"- Entry {i}: " + "x" * 200 + "\n" for i in range(800))
        root = Root(CHANGELOG.replace("- A fixture entry.\n", long_section))
        self.addCleanup(root.close)
        body = rb.compose("fw-v9.9.9", signed=False, repo="owner/name", root=root.path)
        self.assertLessEqual(len(body.encode("utf-8")), 125_000)
        self.assertIn("https://github.com/owner/name/blob/fw-v9.9.9/CHANGELOG.md", body)


class Refusals(unittest.TestCase):
    def setUp(self):
        self.root = Root()
        self.addCleanup(self.root.close)

    def test_an_unknown_tag(self):
        for tag in ("v1.0.0", "ios-v0.8.0", "flasher-v", "lab-latest"):
            with self.subTest(tag=tag), self.assertRaises(rb.StepError):
                rb.compose(tag, signed=True, repo="o/n", root=self.root.path)

    def _rewrite_step(self, prefix, old, new):
        workflow = rb.STEPS[prefix][0]
        path = self.root.path / ".github" / "workflows" / workflow
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def test_a_step_that_reads_an_expression_in_its_script(self):
        self._rewrite_step("flasher-v", 'echo "## What\'s changing in ${VER}"',
                           'echo "## What\'s changing in ${{ env.VER }}"')
        with self.assertRaises(rb.StepError):
            rb.compose("flasher-v1.2.3", signed=True, repo="o/n", root=self.root.path)

    def test_a_step_that_calls_gh_for_anything_else(self):
        line = '            echo "## What\'s changing in ${VER}"'
        self._rewrite_step("app-v", line, '            gh release view "$TAG"\n' + line)
        with self.assertRaises(rb.StepError):
            rb.compose("app-v1.2.3", signed=True, repo="o/n", root=self.root.path)

    def test_a_version_with_no_notes(self):
        with self.assertRaises(rb.StepError):
            rb.compose("flasher-v7.7.7", signed=True, repo="o/n", root=self.root.path)


if __name__ == "__main__":
    unittest.main()
