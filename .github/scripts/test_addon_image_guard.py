"""addon-image.yml's forgotten-bump guard, run as the workflow runs it.

The step's own `run:` script is read from the real workflow file and executed
with a stub `curl` (and `sleep`) on PATH and a fixture CHANGELOG in a scratch
root, so nothing here touches GHCR and a CHANGELOG edit never runs (or
breaks) this suite. What it holds: a push refuses to move a :<version> that
is already on the registry while the app's CHANGELOG has entries under
"## Unreleased", and lets every other case through.
"""
from __future__ import annotations

import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "addon-image.yml"
STEP = "Refuse to re-point a published version that has unreleased changes"

UNRELEASED = """<!-- Newest first. -->
## Unreleased

- **A fixture fix.** It is not out
  yet.
- Another.

## 9.9.9

- An old entry.
"""

RELEASED = """<!-- Newest first. -->
## 9.9.9

- **A fixture fix.** It is out.

## 9.9.8

- An old entry.
"""

EMPTY_UNRELEASED = """<!-- Newest first. -->
## Unreleased

## 9.9.9

- An old entry.
"""

# curl stub: the token exchange answers a token; the manifest request prints
# the status the test chose (the step asks curl for `%{http_code}` only).
CURL = """#!/bin/sh
echo "$*" >> "$CURL_LOG"
case "$*" in
  *ghcr.io/token*) printf '{"token": "anon"}' ;;
  *) printf '%s' "$MANIFEST_STATUS" ;;
esac
"""


def build_steps() -> list[dict]:
    doc = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    return doc["jobs"]["build"]["steps"]


def run_guard(changelog: str, status: str) -> tuple[int, str, list[str]]:
    """(exit code, output, the stub curl's calls) for one fixture."""
    (step,) = [s for s in build_steps() if s.get("name") == STEP]
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "privacy_witness_kernel").mkdir()
        (root / "privacy_witness_kernel" / "CHANGELOG.md").write_text(changelog, encoding="utf-8")
        bin_dir = root / "bin"
        bin_dir.mkdir()
        for name, body in (("curl", CURL), ("sleep", "#!/bin/sh\nexit 0\n")):
            exe = bin_dir / name
            exe.write_text(body, encoding="utf-8")
            exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
        log = root / "curl.log"
        log.write_text("", encoding="utf-8")
        env = {
            "PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}",
            "IMAGE": "ghcr.io/example/amd64-addon-privacy_witness_kernel",
            "VERSION": "9.9.9",
            "MANIFEST_STATUS": status,
            "CURL_LOG": str(log),
        }
        # `bash -e` is what Actions runs a `run:` block under.
        proc = subprocess.run(
            [shutil.which("bash") or "bash", "-e", "-c", step["run"]],
            cwd=root, env=env, capture_output=True, text=True, timeout=60,
        )
        return proc.returncode, proc.stdout + proc.stderr, log.read_text(encoding="utf-8").splitlines()


class TheStep(unittest.TestCase):
    def test_it_runs_on_pushes_only_and_before_anything_is_built(self):
        steps = build_steps()
        names = [s.get("name") for s in steps]
        self.assertEqual(names.count(STEP), 1, "the guard step is gone or duplicated")
        guard = steps[names.index(STEP)]
        self.assertIn("github.event_name != 'pull_request'", guard["if"])
        self.assertIn("steps.gate.outputs.run == 'true'", guard["if"])
        builds = [i for i, s in enumerate(steps) if "docker/build-push-action" in str(s.get("uses", ""))]
        self.assertEqual(len(builds), 1)
        self.assertLess(names.index(STEP), builds[0], "the guard must run before the image is built and pushed")
        # It probes the same image:tag the build step pushes.
        self.assertEqual(guard["env"], {
            "IMAGE": "${{ steps.meta.outputs.image }}",
            "VERSION": "${{ steps.meta.outputs.version }}",
        })
        self.assertIn("${{ steps.meta.outputs.image }}:${{ steps.meta.outputs.version }}", steps[builds[0]]["with"]["tags"])
        self.assertNotIn("${{", guard["run"], "the script takes its inputs from env, never from an expression")


class TheDecision(unittest.TestCase):
    def test_a_published_version_with_unreleased_entries_is_refused(self):
        code, out, calls = run_guard(UNRELEASED, "200")
        self.assertEqual(code, 1, out)
        self.assertIn("::error", out)
        self.assertIn("scripts/bump_version.sh", out)
        self.assertIn("ghcr.io/example/amd64-addon-privacy_witness_kernel:9.9.9", out)
        manifest = [c for c in calls if "/manifests/" in c]
        self.assertEqual(len(manifest), 1, calls)
        self.assertIn("https://ghcr.io/v2/example/amd64-addon-privacy_witness_kernel/manifests/9.9.9", manifest[0])
        self.assertIn("--head", manifest[0])

    def test_the_first_build_of_a_new_version_goes_ahead(self):
        code, out, _ = run_guard(UNRELEASED, "404")
        self.assertEqual(code, 0, out)
        self.assertNotIn("::error", out)

    def test_a_released_changelog_is_never_probed(self):
        for changelog in (RELEASED, EMPTY_UNRELEASED):
            code, out, calls = run_guard(changelog, "200")
            self.assertEqual(code, 0, out)
            self.assertEqual(calls, [], "no entries under Unreleased: the registry is not asked")

    def test_the_bracketed_heading_counts_too(self):
        code, out, _ = run_guard(UNRELEASED.replace("## Unreleased", "## [Unreleased]"), "200")
        self.assertEqual(code, 1, out)

    def test_an_answer_it_cannot_read_refuses_rather_than_pushes(self):
        for status in ("503", "401", "000"):
            code, out, calls = run_guard(UNRELEASED, status)
            self.assertEqual(code, 1, f"{status}: {out}")
            self.assertIn("could not tell", out)
            self.assertNotIn("is already published and", out)
            if status != "401":
                # A transient status is retried before the step gives up.
                self.assertEqual(len([c for c in calls if "/manifests/" in c]), 5, calls)


if __name__ == "__main__":
    unittest.main()
