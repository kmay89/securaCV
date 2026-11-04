#!/usr/bin/env python3
"""The flasher catalog claims the Bluetooth setup door only for a pinned train
that carries it (sweep A71).

gen_flash.py derives each product's `setup_door` from what its build compiles
(has_setup_door), and the confirm card's provisioning_note and the done card's
hatch lead with the iPhone card on it. The catalog describes the release
images at fw-v<fw_train>, though, and the door shipped in 2.4.16: a train
pinned before it would have sent an owner to wait for a card the installed
image never shows. So the door is a CAPABILITY_TRAINS row (`ble_setup`, the
broker_tls pattern), and train_gated_setup_door() is the catalog's fact: the
build compiles the door AND the pinned train carries it. The `ap-door` choice
of both cards follows that fact, not the product's role.

Where the clone has the fw-v tags, corroborate_capability_trains() holds the
row's evidence files to every tag; a CI checkout without tags skips that part.

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import contextlib
import copy
import io
import json
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import gen_flash as gf  # noqa: E402  (definitions only; main() runs under __main__)

def product(pid: str) -> dict:
    return next(p for p in gf.PRODUCTS if p["id"] == pid)


def project_of(pid: str) -> str:
    flavors = {f["name"]: f for f in json.loads(gf.read(gf.REPO / "firmware/flavors.json"))}
    manifest = gf.by_flasher_product(gf.load_manifests(gf.REPO))[pid]
    return flavors[manifest["family"]]["dir"]


class SetupDoorTrain(unittest.TestCase):
    def test_the_row_names_its_first_train_and_evidence_that_exists(self):
        row = gf.CAPABILITY_TRAINS["ble_setup"]
        self.assertEqual(row["since"], "2.4.16")
        for f in (row["evidence"], *row.get("also", ())):
            self.assertTrue((gf.REPO / f).is_file(), f)

    def test_a_train_before_the_door_claims_no_door(self):
        # The WAP, the Sense and a Vision all compile the door in the tree.
        for pid in ("securacv-canary-wap", "securacv-canary-sense", "securacv-canary-vision"):
            with self.subTest(pid=pid):
                project, env = project_of(pid), product(pid)["env"]
                self.assertTrue(gf.has_setup_door(project, env), "the tree compiles the door")
                for train, door in (("2.4.15", False), ("2.4.16", True), ("2.4.19", True)):
                    self.assertIs(gf.train_gated_setup_door(project, env, train), door, train)

    def test_a_build_without_the_door_claims_none_on_any_train(self):
        project, env = project_of("securacv-canary"), product("securacv-canary")["env"]
        self.assertFalse(gf.train_gated_setup_door(project, env, "2.4.19"))

    def test_both_cards_lead_with_the_iphone_card_exactly_when_the_door_is_open(self):
        wap = product("securacv-canary-wap")
        self.assertEqual(wap["provisioning"], "ap")
        self.assertEqual(gf.hatch_kind(wap["id"], "ap", True), "ap-door")
        self.assertEqual(gf.hatch_kind(wap["id"], "ap", False), "ap")
        self.assertEqual(gf.provisioning_note("ap", True), gf.PROVISIONING["ap-door"])
        self.assertEqual(gf.provisioning_note("ap", False), gf.PROVISIONING["ap"])
        # The door is no reason to change a non-`ap` product's cards.
        self.assertEqual(gf.hatch_kind("securacv-canary-sense", "usb-secrets", True), "sense")
        self.assertEqual(gf.provisioning_note("usb-secrets", True), gf.PROVISIONING["usb-secrets"])


def tags() -> list[str]:
    try:
        out = subprocess.run(["git", "-C", str(gf.REPO), "tag", "--list", "fw-v*"],
                             capture_output=True, text=True, check=False)
    except OSError:
        return []
    return out.stdout.split() if out.returncode == 0 else []


class Corroborated(unittest.TestCase):
    def setUp(self):
        if "fw-v2.4.15" not in tags() or "fw-v2.4.17" not in tags():
            self.skipTest("this checkout has no fw-v2.4.15 / fw-v2.4.17 tags to read")

    def refused(self, row: dict) -> str:
        trains = copy.deepcopy(gf.CAPABILITY_TRAINS)
        trains["ble_setup"] = row
        err = io.StringIO()
        with mock.patch.object(gf, "CAPABILITY_TRAINS", trains), \
                contextlib.redirect_stderr(err), self.assertRaises(SystemExit):
            gf.corroborate_capability_trains()
        return err.getvalue()

    def test_the_row_as_written_stands_against_every_tag(self):
        gf.corroborate_capability_trains()

    def test_a_since_the_tags_contradict_is_refused(self):
        row = dict(gf.CAPABILITY_TRAINS["ble_setup"])
        self.assertIn("absent at fw-v2.4.15", self.refused({**row, "since": "2.4.15"}))
        self.assertIn("present at fw-v2.4.17", self.refused({**row, "since": "2.4.18"}))

    def test_every_also_file_is_held_to_the_tags(self):
        row = dict(gf.CAPABILITY_TRAINS["ble_setup"])
        said = self.refused({**row, "also": ["firmware/projects/canary-wap/arduino/canary_wap/gone.h"]})
        self.assertIn("gone.h is absent at fw-v2.4.17", said)


if __name__ == "__main__":
    unittest.main()
