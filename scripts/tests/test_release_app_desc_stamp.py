#!/usr/bin/env python3
"""Both release paths stamp each app's descriptor before anything hashes it.

firmware/scripts/stamp_app_desc.py writes the product's asset_stem and the
firmware's version into every app image's esp_app_desc_t (the Arduino core's
`arduino-lib-builder` up to fw-v2.4.17, so neither flasher could name a
board running SecuraCV). It edits bytes, so WHERE it runs is the whole
contract: after the last build step, and before the first step that copies,
measures, hashes, signs or merges an app. A stamp after the OTA signing step
would publish a manifest whose sha256 and Ed25519 signature cover bytes the
release no longer ships, and every device would refuse the update; a stamp
after the factory merge would ship unstamped factory images. This reads
firmware-release.yml and flasher-release.yml and holds both to that order,
to one version (the one the signing and merging steps use), and holds the
catalog to an identity the field can carry.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_release_app_desc_stamp.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests), unfiltered
"""

from __future__ import annotations

import json
import re
import sys
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "firmware" / "scripts"))

import stamp_app_desc as sad  # noqa: E402

WORKFLOWS = REPO / ".github" / "workflows"
JOBS = {"firmware-release.yml": "build-sign-release", "flasher-release.yml": "factory-images"}
STAMP = re.compile(r"python3? firmware/scripts/stamp_app_desc\.py release\b")
BUILD = re.compile(r"\bpio run\b|\barduino-cli compile\b")
# What a step does to an app that must see the stamped bytes: copy it (a
# copy taken first would be signed or shipped unstamped), run a tool on it,
# checksum it, grep it, attest it or publish it. An app is a PlatformIO
# firmware.bin, an arduino-cli <sketch>.ino.bin, or anything in the
# arduino-cli output dirs the release builds into.
APP = r"(?:firmware\.bin|\.ino\.bin|/tmp/wap-build|/tmp/display-)"
CONSUMER = re.compile(r"python3? firmware/scripts/(?:ota_release\.py manifest|build_flash_manifest\.py"
                      r"|make_factory\.py|check_slot_budget\.py)|\bsha256sum\b|\bstrings \""
                      r"|\bcp\b[^\n]*" + APP)
CONSUMING_ACTIONS = ("actions/attest-build-provenance", "softprops/action-gh-release")


def steps(workflow: str) -> list[dict]:
    wf = yaml.safe_load((WORKFLOWS / workflow).read_text(encoding="utf-8"))
    return wf["jobs"][JOBS[workflow]]["steps"]


def commands(step: dict) -> str:
    """A step's run block without its comment lines (prose names tools too)."""
    lines = (step.get("run") or "").splitlines()
    return "\n".join(line for line in lines if not line.lstrip().startswith("#"))


def order_problems(job: list[dict]) -> list[str]:
    stamps = [i for i, s in enumerate(job) if STAMP.search(commands(s))]
    if len(stamps) != 1:
        return [f"expected one stamp_app_desc.py release step, found {len(stamps)}"]
    (at,) = stamps
    problems = []
    for i, s in enumerate(job):
        name = s.get("name", f"step {i}")
        if BUILD.search(commands(s)) and i > at:
            problems.append(f"'{name}' builds an app after the stamp step: it would ship unstamped")
        consumes = CONSUMER.search(commands(s)) or str(s.get("uses", "")).startswith(CONSUMING_ACTIONS)
        if consumes and i < at:
            problems.append(f"'{name}' reads the apps before the stamp step: it would hash, sign or "
                            f"merge bytes the release then changes")
    return problems


class TheRule(unittest.TestCase):
    def test_a_stamp_after_signing_or_before_a_build_is_named(self):
        build = {"name": "Build", "run": "pio run -e release_ha"}
        stamp = {"name": "Stamp", "run": "python3 firmware/scripts/stamp_app_desc.py release --version 1"}
        sign = {"name": "Sign", "run": "python firmware/scripts/ota_release.py manifest x"}
        attest = {"name": "Attest", "uses": "actions/attest-build-provenance@v4"}
        self.assertEqual(order_problems([build, stamp, sign, attest]), [])
        self.assertEqual(len(order_problems([build, sign, stamp])), 1)
        self.assertEqual(len(order_problems([stamp, build, sign])), 1)
        self.assertEqual(len(order_problems([build, attest, stamp])), 1)
        # A copy taken before the stamp is the copy a later step signs.
        copy = {"name": "Copy", "run": 'cp /tmp/wap-build/canary_wap.ino.bin "/tmp/release/canary-wap-1.bin"'}
        self.assertEqual(len(order_problems([build, copy, stamp, sign])), 1)
        self.assertEqual(order_problems([build, stamp, copy, sign]), [])
        # Staging a whole build dir, as the display env loop does, is part of
        # building, not a read of the app.
        stage = {"name": "Stage", "run": 'cp -R ".pio/build/$E" "$STAGE/$E"\ncp lv_conf.h "${LIBDIR}/lv_conf.h"'}
        self.assertEqual(order_problems([build, stage, stamp, sign]), [])
        self.assertEqual(order_problems([build, sign]), ["expected one stamp_app_desc.py release step, found 0"])
        # A tool named in a comment is not a step that runs it.
        prose = {"name": "Prose", "run": "# make_factory.py and `strings` come later\necho hi"}
        self.assertEqual(order_problems([prose, build, stamp, sign]), [])


class TheWorkflows(unittest.TestCase):
    def stamp_step(self, job: list[dict]) -> dict:
        return next(s for s in job if STAMP.search(commands(s)))

    def test_both_release_paths_stamp_between_the_builds_and_the_first_reader(self):
        for wf in JOBS:
            with self.subTest(wf):
                job = steps(wf)
                self.assertTrue(any(BUILD.search(commands(s)) for s in job))
                self.assertEqual(order_problems(job), [])

    def test_the_stamp_is_the_version_the_release_signs_and_merges(self):
        for wf, reader in (("firmware-release.yml", "firmware/scripts/ota_release.py manifest"),
                           ("flasher-release.yml", "python3 firmware/scripts/build_flash_manifest.py")):
            with self.subTest(wf):
                job = steps(wf)
                stamp = self.stamp_step(job)
                signer = next(s for s in job if reader in commands(s))
                self.assertIn('--version "$VERSION"', stamp["run"])
                self.assertEqual(stamp["env"]["VERSION"], signer["env"]["VERSION"])
                self.assertEqual(stamp["env"]["VERSION"], "${{ steps.ver.outputs.version }}")
                # It runs whenever the job does, and a failure is a failed release.
                self.assertNotIn("if", stamp)
                self.assertNotIn("continue-on-error", stamp)

    def test_only_the_gated_release_drops_a_refused_app(self):
        # firmware-release.yml's assemble step and completeness gate weigh a
        # product the stamper drops; flasher-release.yml uploads its catalog
        # over the tag's with neither, so there a refusal fails the run.
        self.assertIn("--fail-on-refused", self.stamp_step(steps("flasher-release.yml"))["run"])
        self.assertNotIn("--fail-on-refused", self.stamp_step(steps("firmware-release.yml"))["run"])

    def test_the_tag_rebuild_overlays_the_stamper_with_the_rest_of_the_tooling(self):
        # The release channel checks out an old tag, which has no stamper (or
        # an older one): it must come from the dispatch ref, before it runs.
        job = steps("flasher-release.yml")
        overlay = next(i for i, s in enumerate(job) if "_tooling/firmware/scripts/make_factory.py" in (s.get("run") or ""))
        self.assertIn("cp _tooling/firmware/scripts/stamp_app_desc.py firmware/scripts/stamp_app_desc.py",
                      job[overlay]["run"])
        self.assertLess(overlay, job.index(self.stamp_step(job)))

    def test_the_version_guards_still_test_the_firmware(self):
        # The guards after the stamp grep the image for the version literal;
        # the stamper writes a version only over one the image already
        # carries outside the descriptor (stamp_app_desc.carries, pinned by
        # firmware/scripts/test_stamp_app_desc.py), so they still fail a
        # forgotten bump. The product guard greps the full product id, which
        # no asset_stem can equal.
        text = (WORKFLOWS / "firmware-release.yml").read_text(encoding="utf-8")
        self.assertIn('grep -qFx "${VERSION}"', text)
        self.assertIn('grep -qFx "securacv-canary-display-${NAME}"', text)


class TheCatalog(unittest.TestCase):
    def test_every_asset_stem_is_a_project_name_the_field_can_hold(self):
        catalog = json.loads((REPO / "canary-local/devices/flash.json").read_text(encoding="utf-8"))
        stems = [p["asset_stem"] for p in catalog["products"]]
        ids = {p["id"] for p in catalog["products"]}
        self.assertEqual(len(stems), len(set(stems)), "two products would stamp the same name")
        for stem in stems:
            with self.subTest(stem):
                sad.check_value("project_name", stem)    # raises on one that cannot fit
                self.assertNotIn(stem, ids)
                self.assertFalse(stem.startswith("securacv-"), "a stamp must never read as a product id")


if __name__ == "__main__":
    unittest.main()
