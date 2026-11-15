"""Who may reach the OTA release key, held to the workflow files.

The key signs every firmware image, flasher manifest and model manifest the
fleet accepts. Three things are checked here, against the real workflows:

  * every job that is handed the key runs in the `firmware-release`
    environment (where the secret belongs, and where the owner's required
    reviewers apply), so moving the secret there strands no signer;
  * firmware-release.yml signs a dispatch only from main, and refuses a
    commit that is not on main before any step reads the key. That check is
    run here as the workflow runs it, in scratch repositories with real git.
    A dispatch from any other ref is a red run with a sentence (its
    `main-only` job, run here too), never a run that ends with nothing said;
  * the "Update everything" button, which is deliberately outside that
    environment, still learns that the key exists, and does not dispatch
    firmware from a ref Firmware Release would refuse: its firmware gate is
    run here, on main, on a branch and on a tag.

What this cannot hold: that the environment has reviewers and that the secret
was moved. Those are repository settings (docs/RELEASE_PROCESS.md). Nor does
it hold the other two signers to main: Flasher Factory Images `channel: dev`
signs the dispatch ref's HEAD by design, and a Vision Model Asset dispatch
runs the dispatch ref's copy of that workflow. The environment is what they
are held to. And every check here is read from the workflow file, so it binds
the copy at the commit being released, not a commit that edits the file.
"""
from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github" / "workflows"
SECRET = "secrets.OTA_SIGNING_KEY_PEM"
ENVIRONMENT = "firmware-release"
# The one use that is not the key: a boolean that says whether it is set.
PRESENCE = re.compile(re.escape(SECRET) + r"\s*!=\s*''")

GIT_ENV = dict(
    os.environ,
    GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
    GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid",
    GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull,
)


def load(name: str) -> dict:
    return yaml.safe_load((WORKFLOWS / name).read_text(encoding="utf-8"))


def git(cwd: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=cwd, env=GIT_ENV, check=True,
                          capture_output=True, text=True).stdout.strip()


class EverySignerIsInTheEnvironment(unittest.TestCase):
    def test_a_job_handed_the_key_runs_in_the_environment(self):
        signers = []
        for path in sorted(WORKFLOWS.glob("*.yml")):
            doc = yaml.safe_load(path.read_text(encoding="utf-8"))
            for job_id, job in (doc.get("jobs") or {}).items():
                text = yaml.safe_dump(job)
                if SECRET not in PRESENCE.sub("", text):
                    continue
                signers.append(f"{path.name}:{job_id}")
                with self.subTest(job=signers[-1]):
                    self.assertEqual(
                        job.get("environment"), ENVIRONMENT,
                        f"{signers[-1]} reads {SECRET} outside the {ENVIRONMENT} environment: "
                        "once the secret lives there this job gets an empty key",
                    )
        self.assertEqual(sorted(signers), [
            "firmware-release.yml:build-sign-release",
            "flasher-release.yml:factory-images",
            "vision-model-release.yml:model-asset",
        ], "a new signer, or one that went away: update this list on purpose")

    def test_a_dispatch_signs_only_from_main(self):
        job = load("firmware-release.yml")["jobs"]["build-sign-release"]
        self.assertEqual(job["if"], "github.event_name == 'push' || github.ref == 'refs/heads/main'")
        # The only push this workflow listens for is an fw-v* tag.
        triggers = load("firmware-release.yml")[True]
        self.assertEqual(triggers["push"], {"tags": ["fw-v*"]})
        self.assertEqual(set(triggers), {"push", "workflow_dispatch"})

    def test_a_dispatch_from_anywhere_else_is_a_red_run_with_a_sentence(self):
        # The signing job's `if:` alone would skip it: a gray run, nothing
        # built, nothing said. With only the two triggers above, this job runs
        # exactly when that one does not.
        job = load("firmware-release.yml")["jobs"]["main-only"]
        self.assertEqual(job["if"], "github.event_name == 'workflow_dispatch' && github.ref != 'refs/heads/main'")
        # Outside the environment (a refusal must not wait for a reviewer),
        # with no token and no secret.
        self.assertIsNone(job.get("environment"))
        self.assertEqual(job["permissions"], {})
        self.assertNotIn("secrets.", yaml.safe_dump(job))
        self.assertIsNone(job.get("needs"))
        (step,) = job["steps"]
        self.assertNotIn("${{", step["run"])
        for ref in ("refs/heads/side", "refs/tags/fw-v9.9.9"):
            with self.subTest(ref=ref):
                proc = subprocess.run(["bash", "-c", step["run"]], env=dict(GIT_ENV, GITHUB_REF=ref),
                                      capture_output=True, text=True, timeout=30)
                self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
                self.assertIn(f"::error::Firmware Release signs only from main, and this dispatch is on {ref}",
                              proc.stdout)

    def test_a_release_that_is_not_firmware_never_waits_on_the_environment(self):
        job = load("vision-model-release.yml")["jobs"]["model-asset"]
        self.assertEqual(job["if"], "github.event_name != 'release' || startsWith(github.event.release.tag_name, 'fw-v')")

    def test_the_button_can_still_see_that_the_key_exists(self):
        update = load("release-everything.yml")["jobs"]["update"]
        self.assertIsNone(update.get("environment"), "the planner must not be able to read the key")
        step = next(s for s in update["steps"] if s.get("id") == "otakey")
        self.assertEqual(
            step["env"]["HAS_SECRET"],
            "${{ secrets.OTA_SIGNING_KEY_PEM != '' || vars.OTA_SIGNING_KEY_IN_ENVIRONMENT == 'true' }}",
        )


class TheButtonDoesNotDispatchFirmwareOffMain(unittest.TestCase):
    """The button's firmware gate, run as the workflow runs it.

    "Update everything" dispatches every target on the ref it was pressed on.
    Firmware Release signs only from main, so a press anywhere else must
    close the firmware gate (nothing dispatched, the summary names the ref),
    not report a release and start a run that publishes nothing.
    """

    @classmethod
    def setUpClass(cls):
        update = load("release-everything.yml")["jobs"]["update"]
        cls.step = next(s for s in update["steps"] if s.get("id") == "otakey")
        cls.plan = next(s for s in update["steps"] if s.get("id") == "plan")
        cls._tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls._tmp.cleanup)
        # A stand-in for firmware/scripts/ota_key_state.py: the public half is
        # committed, so the cases below turn on the secret and the ref alone.
        scripts = Path(cls._tmp.name) / "firmware" / "scripts"
        scripts.mkdir(parents=True)
        (scripts / "ota_key_state.py").write_text(
            "import sys\nprint('{\"embedded\": true}' if '--json' in sys.argv else 'key: embedded')\n",
            encoding="utf-8")

    def press(self, ref: str, *, secret: str = "true", publish: str = "true") -> tuple[str, str]:
        """(the step's outputs, what it wrote to the run summary)."""
        out = Path(tempfile.mkstemp(dir=self._tmp.name)[1])
        summary = Path(tempfile.mkstemp(dir=self._tmp.name)[1])
        proc = subprocess.run(
            ["bash", "-c", self.step["run"]], cwd=self._tmp.name, capture_output=True, text=True, timeout=60,
            env=dict(GIT_ENV, GITHUB_REF=ref, HAS_SECRET=secret, PUBLISH=publish,
                     GITHUB_OUTPUT=str(out), GITHUB_STEP_SUMMARY=str(summary)))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return out.read_text(encoding="utf-8"), summary.read_text(encoding="utf-8")

    def test_the_step_is_what_the_plan_gates_firmware_on(self):
        self.assertNotIn("${{", self.step["run"])
        self.assertEqual(self.step["env"]["PUBLISH"], "${{ inputs.publish }}")
        self.assertEqual(self.plan["env"]["OTA_SIGNING_KEY_READY"], "${{ steps.otakey.outputs.ready }}")
        self.assertIn('"OTA_SIGNING_KEY_READY": os.environ.get("OTA_SIGNING_KEY_READY") == "true"',
                      self.plan["run"])

    def test_on_main_with_the_key_the_gate_is_open_and_nothing_is_said(self):
        outputs, summary = self.press("refs/heads/main")
        self.assertEqual(outputs, "ready=true\n")
        self.assertEqual(summary, "")

    def test_off_main_the_gate_is_closed_and_the_summary_names_the_ref(self):
        for ref in ("refs/heads/release-prep", "refs/tags/fw-v9.9.9"):
            with self.subTest(ref=ref):
                outputs, summary = self.press(ref)
                self.assertEqual(outputs, "ready=false\n")
                self.assertIn("### Firmware is not releasable from this press", summary)
                self.assertIn(
                    f"| pressed on `main` (Firmware Release signs only from there) | **no — this press is on `{ref}`** |",
                    summary)
                self.assertIn("| `OTA_SIGNING_KEY_PEM` secret | set ✓ |", summary)
                self.assertIn("The signing key is ready.", summary)
                self.assertIn("nothing is dispatched for firmware from this press", summary)
                self.assertNotIn("Firmware will not release", summary)

    def test_a_build_only_press_off_main_closes_the_gate_and_adds_no_section(self):
        # Firmware sits a build-only press out on any ref (release_plan.py),
        # so there is nothing the ref stopped and nothing to report.
        outputs, summary = self.press("refs/heads/release-prep", publish="false")
        self.assertEqual(outputs, "ready=false\n")
        self.assertEqual(summary, "")

    def test_a_missing_key_is_still_reported_on_every_press(self):
        for ref, publish in (("refs/heads/main", "true"), ("refs/heads/main", "false"),
                             ("refs/heads/release-prep", "false")):
            with self.subTest(ref=ref, publish=publish):
                outputs, summary = self.press(ref, secret="false", publish=publish)
                self.assertEqual(outputs, "ready=false\n")
                self.assertIn("| `OTA_SIGNING_KEY_PEM` secret | **not set**", summary)
                self.assertIn("Firmware will not release", summary)
                self.assertNotIn("The signing key is ready.", summary)


class OnlyACommitOnMainIsSigned(unittest.TestCase):
    """The first lines of 'Derive version, tag, and channel', run for real."""

    @classmethod
    def setUpClass(cls):
        steps = load("firmware-release.yml")["jobs"]["build-sign-release"]["steps"]
        names = [s.get("name") for s in steps]
        cls.derive_at = names.index("Derive version, tag, and channel")
        script = steps[cls.derive_at]["run"]
        head, sep, _ = script.partition('if [ "${{ github.event_name }}" = "workflow_dispatch" ]; then')
        assert sep, "the step no longer branches on the event where this test cuts it"
        cls.head = head
        cls.first_key_read = min(i for i, s in enumerate(steps) if SECRET in yaml.safe_dump(s))

        cls._tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls._tmp.cleanup)
        root = Path(cls._tmp.name)
        cls.origin = root / "origin.git"
        git(root, "init", "-q", "--bare", "-b", "main", str(cls.origin))
        seed = root / "seed"
        git(root, "init", "-q", "-b", "main", str(seed))
        git(seed, "commit", "-q", "--allow-empty", "-m", "c1")
        git(seed, "tag", "old-on-main")
        git(seed, "commit", "-q", "--allow-empty", "-m", "c2")
        git(seed, "tag", "recent-on-main")
        git(seed, "checkout", "-q", "-b", "side")
        git(seed, "commit", "-q", "--allow-empty", "-m", "never merged")
        git(seed, "tag", "fw-v9.9.9")
        git(seed, "checkout", "-q", "main")
        # 201 more commits: `old-on-main` and `recent-on-main` end up more
        # than 200 behind the tip; `near-tip` stays inside the window.
        tree = git(seed, "rev-parse", "HEAD^{tree}")
        parent = git(seed, "rev-parse", "HEAD")
        for i in range(201):
            parent = git(seed, "commit-tree", tree, "-p", parent, "-m", f"m{i}")
            if i == 100:
                git(seed, "tag", "near-tip", parent)
        git(seed, "update-ref", "refs/heads/main", parent)
        git(seed, "push", "-q", str(cls.origin), "main", "side", "--tags")

    def checkout(self, ref: str, origin: Path | None = None) -> tuple[Path, str]:
        """A depth-1 checkout of `ref`, the way actions/checkout leaves it."""
        work = Path(tempfile.mkdtemp(dir=self._tmp.name))
        git(work, "init", "-q")
        git(work, "remote", "add", "origin", f"file://{origin or self.origin}")
        git(work, "fetch", "-q", "--no-tags", "--depth=1", "origin", ref)
        git(work, "checkout", "-q", "--detach", "FETCH_HEAD")
        return work, git(work, "rev-parse", "HEAD")

    def run_head(self, work: Path, sha: str) -> subprocess.CompletedProcess:
        return subprocess.run(["bash", "-c", self.head], cwd=work, env=dict(GIT_ENV, GITHUB_SHA=sha),
                              capture_output=True, text=True, timeout=60)

    def test_the_check_comes_before_the_key_and_reads_no_expression(self):
        self.assertLess(self.derive_at, self.first_key_read)
        self.assertIn('git merge-base --is-ancestor "$GITHUB_SHA" FETCH_HEAD', self.head)
        self.assertIn("git fetch --no-tags --depth=200 origin main", self.head)
        self.assertNotIn("${{", self.head)

    def test_main_is_signed(self):
        for ref in ("refs/heads/main", "refs/tags/near-tip"):
            with self.subTest(ref=ref):
                proc = self.run_head(*self.checkout(ref))
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertNotIn("::error", proc.stdout)

    def test_a_tag_on_a_commit_that_never_reached_main_is_refused(self):
        work, sha = self.checkout("refs/tags/fw-v9.9.9")
        proc = self.run_head(work, sha)
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("::error::", proc.stdout)
        self.assertIn(f"release commit {sha} is not on main", proc.stdout)

    def test_a_commit_far_behind_main_is_refused_too(self):
        proc = self.run_head(*self.checkout("refs/tags/old-on-main"))
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("is not on main (within its last 200 commits)", proc.stdout)

    def test_no_answer_from_the_remote_is_a_refusal(self):
        work, sha = self.checkout("refs/heads/main")
        git(work, "remote", "set-url", "origin", f"file://{self._tmp.name}/gone.git")
        proc = self.run_head(work, sha)
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("::error::", proc.stdout)


if __name__ == "__main__":
    unittest.main()
