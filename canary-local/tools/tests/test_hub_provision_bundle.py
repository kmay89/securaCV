#!/usr/bin/env python3
"""Host tests for canary-local/tools/gen_hub_provision_bundle.py.

Prove the bundle is (1) an honest pin of the current sources — its SHA-256s match
the real files — and (2) genuinely self-contained: a bundle built into a temp dir
runs its own `provision.sh --dry-run` with no repo and no Home Assistant. Run:

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import gen_hub_provision_bundle as gb  # noqa: E402


class Manifest(unittest.TestCase):
    def setUp(self):
        self.m = gb.build_manifest()

    def test_carries_plan_config_executor_runner(self):
        roles = {f["role"] for f in self.m["files"]}
        self.assertEqual(
            roles, {"plan", "frigate-config", "executor", "runner", "host-runner"}
        )

    def test_no_readme_in_bundle(self):
        # provision.sh is self-documenting; the bundle carries no separate README,
        # so the manifest must not declare one (else the on-card set would
        # contradict its own inventory).
        self.assertNotIn("readme", {f["role"] for f in self.m["files"]})
        self.assertTrue(all(f["bundle_path"] != "README.md" for f in self.m["files"]))

    def test_every_shipped_file_including_generated_is_pinned(self):
        # The manifest promises every carried file is pinned; the runner is
        # generated but still ships, so it must have a hash too.
        for f in self.m["files"]:
            self.assertTrue(f.get("sha256"), f"{f['role']} has no sha256 pin")

    def test_sha256_pins_match_the_real_files(self):
        # The whole point of the manifest: it can't silently carry stale code.
        for f in self.m["files"]:
            if f.get("generated"):
                continue
            real = hashlib.sha256((gb.REPO / f["source"]).read_bytes()).hexdigest()
            self.assertEqual(f["sha256"], real, f"{f['source']} pin is stale")

    def test_card_paths_are_namespaced_and_safe(self):
        for f in self.m["files"]:
            self.assertTrue(f["card_path"].startswith("CONFIG/securacv/"))
            self.assertNotIn("..", f["card_path"])

    def test_first_boot_is_honestly_marked_planned(self):
        self.assertEqual(self.m["first_boot"]["status"], "planned")
        self.assertTrue(self.m["first_boot"]["what_works_today"])

    def test_deterministic(self):
        self.assertEqual(gb.build_manifest(), gb.build_manifest())

    def test_committed_manifest_matches_a_fresh_build(self):
        committed = json.loads(gb.OUT_JSON.read_text())
        self.assertEqual(committed, self.m, "hub_provision_bundle.json is stale — regenerate it")


class Bundle(unittest.TestCase):
    def build(self, into: Path) -> Path:
        out = into / "securacv-bundle"
        gb.build_bundle(gb.build_manifest(), out)
        return out

    def test_bundle_is_complete(self):
        with tempfile.TemporaryDirectory() as tmp:
            b = self.build(Path(tmp))
            for name in ("hub_seed.json", "hub_seed_apply.py", "provision.sh",
                         "host_provision.sh", "MANIFEST.json",
                         "homeassistant/frigate/config.yaml"):
                self.assertTrue((b / name).exists(), f"bundle missing {name}")
            self.assertFalse((b / "README.md").exists(), "bundle should carry no README")

    def test_bundled_executor_is_byte_identical_to_repo(self):
        with tempfile.TemporaryDirectory() as tmp:
            b = self.build(Path(tmp))
            self.assertEqual(
                (b / "hub_seed_apply.py").read_bytes(),
                (gb.REPO / "canary-local/tools/hub_seed_apply.py").read_bytes(),
            )

    def test_runner_invokes_bundled_executor_with_local_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            b = self.build(Path(tmp))
            runner = (b / "provision.sh").read_text()
            self.assertIn("hub_seed_apply.py", runner)
            self.assertIn("--plan", runner)
            self.assertIn("--assets-root", runner)

    def test_host_runner_borrows_the_stack_and_passes_args_through(self):
        # The host runner's whole contract: run from the HAOS host shell (no
        # python3, no token), it must (1) take the token from the Core container
        # rather than print or invent one, (2) run the bundled executor with the
        # Core image's python3, (3) mount the add-on config tree at the absolute
        # path the plan's write step names, and (4) pass args through so
        # --dry-run previews. All checkable from the bytes, no docker needed.
        with tempfile.TemporaryDirectory() as tmp:
            b = self.build(Path(tmp))
            hr = (b / "host_provision.sh").read_text()
            self.assertTrue(hr.startswith("#!/bin/sh\n"))
            self.assertIn("SUPERVISOR_TOKEN=", hr)
            self.assertNotIn("echo \"$token\"", hr, "the token must never be printed")
            self.assertIn("--entrypoint python3", hr)
            self.assertIn("hub_seed_apply.py", hr)
            self.assertIn("/addon_configs", hr)
            self.assertIn('"$@"', hr)
            # It must run the *bundled* copies, not repo paths.
            self.assertIn("/securacv/hub_seed.json", hr)
            self.assertIn("--assets-root /securacv", hr)

    def test_host_runner_is_lf_only(self):
        # It lands on the card's FAT partition next to the Wi-Fi keyfile, and the
        # shell that runs it is busybox ash — a CR would become part of a token.
        src = (gb.REPO / "canary-local/tools/hub_host_provision.sh").read_bytes()
        self.assertNotIn(b"\r", src)

    def test_self_contained_dry_run(self):
        # The load-bearing test: build the bundle, run ITS OWN runner with no repo
        # context, and confirm the narrated plan comes out (securaCV's hashed slug,
        # the frigate-mode step). If the config didn't travel, --assets-root
        # wouldn't resolve and this would still dry-run — so we also assert the
        # config file is present and pinned above; here we prove the chain runs.
        with tempfile.TemporaryDirectory() as tmp:
            b = self.build(Path(tmp))
            r = subprocess.run(
                ["sh", str(b / "provision.sh"), "--dry-run"],
                capture_output=True, text=True,
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("d0491a67_privacy_witness_kernel", r.stdout)
            self.assertIn('{"mode":"frigate"}', r.stdout)

    def test_real_run_from_bundle_fails_closed_without_token(self):
        with tempfile.TemporaryDirectory() as tmp:
            b = self.build(Path(tmp))
            r = subprocess.run(
                ["sh", str(b / "provision.sh")],
                capture_output=True, text=True,
                env={"PATH": __import__("os").environ["PATH"]},  # no SUPERVISOR_TOKEN
            )
            self.assertNotEqual(r.returncode, 0)

    def test_shipped_bytes_match_every_manifest_pin(self):
        # End-to-end: the bytes actually written (including the generated runner
        # and README) hash to exactly what the manifest pins.
        with tempfile.TemporaryDirectory() as tmp:
            m = gb.build_manifest()
            b = self.build(Path(tmp))
            for f in m["files"]:
                got = hashlib.sha256((b / f["bundle_path"]).read_bytes()).hexdigest()
                self.assertEqual(got, f["sha256"], f"{f['bundle_path']} bytes != pin")


class SafeBuildDir(unittest.TestCase):
    """--build must never recursively delete a directory that isn't our bundle."""

    def test_refuses_non_empty_non_bundle_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "notabundle"
            target.mkdir()
            keep = target / "keepme.txt"
            keep.write_text("precious")
            with self.assertRaises(SystemExit):
                gb.build_bundle(gb.build_manifest(), target)
            self.assertTrue(keep.exists(), "refused build must not delete existing files")

    def test_allows_empty_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "empty"
            target.mkdir()
            gb.build_bundle(gb.build_manifest(), target)
            self.assertTrue((target / "provision.sh").exists())

    def test_rebuilds_into_a_prior_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "b"
            gb.build_bundle(gb.build_manifest(), target)  # first build
            gb.build_bundle(gb.build_manifest(), target)  # rebuild is allowed
            self.assertTrue((target / "MANIFEST.json").exists())


class ManifestOutputPath(unittest.TestCase):
    def test_out_of_repo_manifest_path_does_not_crash(self):
        # relative_to(REPO) must not blow up the success message for an absolute
        # --manifest outside the checkout.
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "m.json"  # tmp is outside the repo
            rc = gb.main(["--manifest", str(out)])
            self.assertEqual(rc, 0)
            self.assertTrue(out.exists())


if __name__ == "__main__":
    unittest.main()


FAKE_DOCKER = """#!/bin/sh
# Stands in for the HAOS host's docker: answers the runner's three inspects
# and, for `run`, prints the argument list one per line instead of running.
case "$1" in
  inspect)
    case "$*" in
      *Config.Env*)   echo "SUPERVISOR_TOKEN=fake-token" ;;
      *Config.Image*) echo "ghcr.io/home-assistant/fake-homeassistant:0" ;;
      *IPAddress*)    echo "172.30.32.2" ;;
      *) exit 0 ;;
    esac ;;
  run) shift; printf '%s\\n' "$@" ;;
  *) exit 1 ;;
esac
"""


class HostRunnerSslMount(unittest.TestCase):
    """The host runner mounts the hub's ssl tree read-only ONLY when the
    broker_tls feature is asked for and the tree exists — never an
    unconditional -v, which would make docker create an empty root-owned
    directory on the host. Driven through the real script with a fake docker
    on PATH, so the branch logic is exercised, not grepped."""

    def run_runner(self, args: list[str], ssl_dir: str | None,
                   cwd: str | None = None) -> subprocess.CompletedProcess:
        import os
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            b = tmp / "securacv-bundle"
            gb.build_bundle(gb.build_manifest(), b)
            bindir = tmp / "bin"
            bindir.mkdir()
            (bindir / "docker").write_text(FAKE_DOCKER)
            (bindir / "docker").chmod(0o755)
            env = {"PATH": f"{bindir}:{os.environ['PATH']}"}
            if ssl_dir is not None:
                env["SECURACV_HOST_SSL_DIR"] = ssl_dir
            return subprocess.run(
                ["sh", str(b / "host_provision.sh"), *args],
                capture_output=True, text=True, env=env, cwd=cwd,
            )

    @staticmethod
    def mount_of(stdout: str) -> str | None:
        argv = stdout.splitlines()
        for i, a in enumerate(argv):
            if a.endswith(":/ssl:ro") and i and argv[i - 1] == "-v":
                return a
        return None

    def test_asked_for_and_present_mounts_read_only_before_the_image(self):
        with tempfile.TemporaryDirectory() as ssl:
            r = self.run_runner(["--with", "broker_tls", "--dry-run"], ssl)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(self.mount_of(r.stdout), f"{ssl}:/ssl:ro")
            argv = r.stdout.splitlines()
            self.assertLess(argv.index(f"{ssl}:/ssl:ro"), argv.index("--entrypoint"))
            # Args still pass through, after the executor's own plan flags.
            self.assertEqual(argv[-3:], ["--with", "broker_tls", "--dry-run"])
            # The --with=value spelling argparse accepts is honored too.
            r2 = self.run_runner(["--with=broker_tls"], ssl)
            self.assertEqual(self.mount_of(r2.stdout), f"{ssl}:/ssl:ro")

    def test_not_asked_for_means_no_mount_even_when_present(self):
        with tempfile.TemporaryDirectory() as ssl:
            r = self.run_runner(["--with", "pihole", "--dry-run"], ssl)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIsNone(self.mount_of(r.stdout))
            self.assertNotIn("/ssl", r.stdout)

    def test_asked_for_but_absent_skips_the_mount_and_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            missing = str(Path(tmp) / "no-such-ssl")
            r = self.run_runner(["--with", "broker_tls", "--dry-run"], missing)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIsNone(self.mount_of(r.stdout))
            self.assertIn(missing, r.stderr)
            self.assertIn("broker_tls", r.stderr)
            # The note names the remedy, not just the symptom: the executor's
            # later "cannot see /ssl" refusal is generic, so the ONE place the
            # override is spelled for the console path is here.
            self.assertIn("SECURACV_HOST_SSL_DIR=", r.stderr)

    def test_the_default_host_path_is_beside_addon_configs(self):
        # The path is inferred from the Supervisor's data layout (the same
        # tree addon_configs is mounted from), not yet proven on a hub — so
        # it is one string, overridable, next to the mount it was inferred from.
        hr = (gb.REPO / "canary-local/tools/hub_host_provision.sh").read_text()
        self.assertIn("/mnt/data/supervisor/addon_configs:/addon_configs", hr)
        self.assertIn('${SECURACV_HOST_SSL_DIR:-/mnt/data/supervisor/ssl}', hr)
        self.assertNotIn("-v /mnt/data/supervisor/ssl", hr, "the ssl mount must be conditional")

    def assert_refused_with_the_rule(self, r: subprocess.CompletedProcess) -> None:
        # The same branch as a missing folder: no mount, a note on stderr, and
        # the run goes on, so the executor's own "cannot see /ssl" refusal
        # stops the broker_tls step while the core plan still completes.
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIsNone(self.mount_of(r.stdout))
        self.assertNotIn(":/ssl:ro", r.stdout)
        # The rule and the override share ONE line, so whoever reads the hint
        # learns what a value it will accept looks like.
        hint = [ln for ln in r.stderr.splitlines() if "SECURACV_HOST_SSL_DIR=" in ln]
        self.assertEqual(len(hint), 1, r.stderr)
        self.assertIn("absolute", hint[0])
        self.assertIn("no ':'", hint[0])

    def test_a_value_docker_would_split_is_refused_not_mounted(self):
        # A real directory whose name holds ':' passes `-d`; spliced into
        # `-v <src>:/ssl:ro` it would become a different mount spec.
        with tempfile.TemporaryDirectory() as tmp:
            odd = Path(tmp) / "a:b"
            odd.mkdir()
            r = self.run_runner(["--with", "broker_tls", "--dry-run"], str(odd))
            self.assert_refused_with_the_rule(r)
            self.assertIn(str(odd), r.stderr)

    def test_a_relative_value_is_refused_not_mounted(self):
        # docker reads a `-v` source with no leading '/' as a NAMED VOLUME: an
        # empty one would be mounted at /ssl, and the executor would report a
        # missing certificate for what is really a wrong setting.
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "ssl").mkdir()
            r = self.run_runner(["--with", "broker_tls", "--dry-run"], "ssl", cwd=tmp)
            self.assert_refused_with_the_rule(r)
