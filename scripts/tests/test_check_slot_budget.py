#!/usr/bin/env python3
"""Pins firmware/scripts/check_slot_budget.py — the release workflow's
byte-accurate OTA slot check, run on the exact bytes it is about to sign.

What is pinned and why:
  • an env with no budget is a `::warning::` annotation and still exit 0. It
    used to print a plain line, and 9 of the 17 OTA products fw-v2.4.16
    signed went out unmeasured with nothing on the run's summary page to say
    so;
  • an image exactly at its budget passes, one byte over fails, and a
    missing image fails: the release loops skip a variant on exit 1, so a
    check that passed a missing file would sign nothing and say it measured;
  • every OTA product the release signs (the variant index and display loop
    check_ota_channels.py reads out of firmware-release.yml) has a budget in
    the committed firmware/flavors.json, except the ones declared in
    UNBUDGETED with a reason. A new published product without a budget
    fails here, not on the boards.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "firmware" / "scripts"


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


csb = _load("check_slot_budget")
sys.path.insert(0, str(SCRIPTS))  # check_ota_channels imports flavor_envs
coc = _load("check_ota_channels")

# The `--env` firmware-release.yml passes for the products whose manifest
# name is not their env name (its assemble step: the flagship's release_ha,
# the WAP's PR-CI env, and the default board/flavor of the vision and sense
# loops). Every other product's env IS its name.
ENV_FOR_PRODUCT = {
    "canary": "release_ha",
    "canary-wap": "canary-wap-default",
    "canary-vision": "canary-vision-default",
    "canary-sense": "canary-sense-default",
}

# Published, deliberately without a budget. Each needs a reason; the release
# prints a ::warning:: for each one every time it signs.
UNBUDGETED = {
    "canary-display-dash":
        "released from the arduino-cli build with PartitionScheme=huge_app "
        "(one 0x300000 app slot, no second slot), and its factory image lays "
        "that table down, while the PlatformIO env says default_16MB.csv "
        "(0x640000). No single budget is true until the two agree.",
    "canary-display-dash-modes":
        "same build and factory table as canary-display-dash.",
}


def guard(env: str, slot: int) -> dict:
    return {"bin": f".pio/build/{env}/firmware.bin", "slot_bytes": slot,
            "slot_name": "test slot"}


class CheckSlotBudget(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        flavors = self.dir / "flavors.json"
        flavors.write_text(json.dumps([{"name": "p", "size_guards": [
            guard("guarded", 100)]}]), encoding="utf-8")
        self._real = csb.FLAVORS
        csb.FLAVORS = flavors

    def tearDown(self) -> None:
        csb.FLAVORS = self._real
        self.tmp.cleanup()

    def image(self, size: int) -> str:
        path = self.dir / "fw.bin"
        path.write_bytes(b"\0" * size)
        return str(path)

    def run_main(self, *argv: str) -> str:
        out = io.StringIO()
        with redirect_stdout(out):
            csb.main(list(argv))
        return out.getvalue()

    def test_unbudgeted_env_is_a_warning_annotation_not_a_failure(self):
        out = self.run_main("--env", "unguarded", self.image(10_000))
        self.assertTrue(out.startswith("::warning::"), out)
        self.assertIn("'unguarded'", out)
        self.assertIn("NOT measured", out)

    def test_image_at_its_budget_passes(self):
        out = self.run_main("--env", "guarded", self.image(100))
        self.assertIn("Firmware size: 100 / 100 bytes", out)
        self.assertNotIn("::warning::", out)

    def test_one_byte_over_fails(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_main("--env", "guarded", self.image(101))
        self.assertIn("is 101 bytes but", str(cm.exception.code))

    def test_missing_image_fails(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_main("--env", "guarded", str(self.dir / "absent.bin"))
        self.assertIn("not found", str(cm.exception.code))


class EveryPublishedProductIsBudgeted(unittest.TestCase):
    def setUp(self) -> None:
        self.published = {}
        for name in coc.published_manifests():
            product = name[len("manifest-"):-len(".json")]
            self.published[product] = ENV_FOR_PRODUCT.get(product, product)
        self.budgets = csb.load_budgets()

    def test_the_published_set_is_read_not_empty(self):
        # a resolver that found nothing would make the next test vacuous
        self.assertGreaterEqual(len(self.published), 17, self.published)

    def test_every_published_env_has_a_budget_or_a_declared_reason(self):
        missing = sorted(env for product, env in self.published.items()
                         if env not in self.budgets and product not in UNBUDGETED)
        self.assertEqual(missing, [],
                         "these published OTA products have no size_guards "
                         "budget in firmware/flavors.json, so the release "
                         "signs them unmeasured — add one (the slot of the "
                         "table the fielded boards carry), or declare it in "
                         "UNBUDGETED with the reason")

    def test_declared_exceptions_are_still_published_and_still_unbudgeted(self):
        # keep the exception list honest: drop an entry once it has a budget
        # or stops shipping
        for product in UNBUDGETED:
            self.assertIn(product, self.published)
            self.assertNotIn(self.published[product], self.budgets,
                             f"{product} now has a budget — remove it from "
                             f"UNBUDGETED")


if __name__ == "__main__":
    unittest.main()
