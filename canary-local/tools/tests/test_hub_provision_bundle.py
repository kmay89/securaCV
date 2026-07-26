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

    def test_carries_plan_config_executor_runner_readme(self):
        roles = {f["role"] for f in self.m["files"]}
        self.assertEqual(roles, {"plan", "frigate-config", "executor", "runner", "readme"})

    def test_every_shipped_file_including_generated_is_pinned(self):
        # The manifest promises every carried file is pinned; runner + README are
        # generated but still ship, so they must have hashes too.
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
            for name in ("hub_seed.json", "hub_seed_apply.py", "provision.sh", "README.md",
                         "MANIFEST.json", "homeassistant/frigate/config.yaml"):
                self.assertTrue((b / name).exists(), f"bundle missing {name}")

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
