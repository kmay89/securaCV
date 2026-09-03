#!/usr/bin/env python3
"""Pins scripts/lint_device_manifests.py — the join between the seven device
schemas is only as good as the lint that checks it.

What is pinned and why:
  • the committed devices/ set is green — the lint must pass on the tree it
    ships in, and every device row reads its confidence from figures.json;
  • the lint actually FAILS on each mistake it exists for: an env the
    platformio.ini never defines, a build env no manifest claims, a flasher
    product on a different chip than the board, and a hand-typed ladder
    verdict (`status`) — a guard that reads as covered while catching nothing
    is worse than no guard;
  • the stdlib schema validator rejects what a full validator would.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import json
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "lint_device_manifests.py"
DEVICES = REPO / "devices"

spec = importlib.util.spec_from_file_location("lint_device_manifests", SCRIPT)
ldm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ldm)  # type: ignore[union-attr]


class _Mutated:
    """A scratch copy of devices/ the test can edit before linting."""

    def __enter__(self) -> Path:
        self._tmp = tempfile.TemporaryDirectory()
        dst = Path(self._tmp.name) / "devices"
        shutil.copytree(DEVICES, dst)
        return dst

    def __exit__(self, *exc) -> None:
        self._tmp.cleanup()


def edit(devices: Path, slug: str, fn) -> None:
    path = devices / slug / "device.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    fn(data)
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


class CommittedTreeIsGreen(unittest.TestCase):
    def test_lint_passes_and_covers_every_device_dir(self):
        rows, errors = ldm.lint()
        self.assertEqual(errors, [])
        dirs = sorted(p.name for p in DEVICES.iterdir() if p.is_dir())
        self.assertEqual(sorted(r["slug"] for r in rows), dirs)
        self.assertGreaterEqual(len(rows), 19)

    def test_confidence_is_read_from_figures_json_not_typed(self):
        figures = json.loads(
            (REPO / "canary-local/devices/figures.json").read_text(encoding="utf-8"))
        by_id = {f["id"]: f for f in figures["figures"]}
        rows, _ = ldm.lint()
        sense = next(r for r in rows if r["slug"] == "canary-sense")
        self.assertEqual(sense["confidence"], by_id["device.canary-sense"]["confidence"])
        for path in DEVICES.glob("*/device.json"):
            text = path.read_text(encoding="utf-8")
            for word in ("\"status\"", "\"confidence\"", "\"tier\""):
                self.assertNotIn(word, text, f"{path.name} hand-types a ladder verdict")

    def test_cli_exit_code(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(ldm.main([]), 0)


class LintCatchesRealMistakes(unittest.TestCase):
    def test_env_missing_from_platformio_ini_fails(self):
        with _Mutated() as devices:
            edit(devices, "canary-display-dash",
                 lambda d: d["board"]["envs"].append("canary-display-nope"))
            _, errors = ldm.lint(devices_dir=devices)
        self.assertTrue(any("canary-display-nope" in e and "[env:" in e for e in errors), errors)

    def test_unclaimed_build_env_fails(self):
        with _Mutated() as devices:
            edit(devices, "canary-display-dash",
                 lambda d: d["board"]["envs"].remove("canary-display-dash-ble5"))
            _, errors = ldm.lint(devices_dir=devices)
        self.assertTrue(any("canary-display-dash-ble5" in e and "claimed by no manifest" in e
                            for e in errors), errors)

    def test_unclaimed_json_entry_that_is_also_claimed_fails(self):
        with _Mutated() as devices:
            path = devices / "unclaimed.json"
            data = json.loads(path.read_text(encoding="utf-8"))
            data["envs"].append({"env": "canary-display-watch", "reason": "test"})
            path.write_text(json.dumps(data), encoding="utf-8")
            _, errors = ldm.lint(devices_dir=devices)
        self.assertTrue(any("unclaimed.json" in e and "canary-display-watch" in e
                            for e in errors), errors)

    def test_chip_mismatch_with_flasher_catalog_fails(self):
        with _Mutated() as devices:
            edit(devices, "canary-sense", lambda d: d["board"].__setitem__("mcu", "ESP32-S3"))
            _, errors = ldm.lint(devices_dir=devices)
        self.assertTrue(any("securacv-canary-sense" in e and "chip" in e for e in errors), errors)
        self.assertTrue(any("boards.json" in e and "xiao-esp32c6-mr60" in e for e in errors),
                        errors)

    def test_hand_typed_status_is_rejected_by_the_schema(self):
        with _Mutated() as devices:
            edit(devices, "canary-wap", lambda d: d.__setitem__("status", "shipping"))
            _, errors = ldm.lint(devices_dir=devices)
        hits = [e for e in errors if "'status'" in e and "additionalProperties" in e]
        self.assertEqual(len(hits), 1, errors)
        self.assertIn("figures.json", hits[0])

    def test_directory_name_must_equal_slug(self):
        with _Mutated() as devices:
            shutil.move(str(devices / "canary-wroom"), str(devices / "canary-wroom-renamed"))
            _, errors = ldm.lint(devices_dir=devices)
        self.assertTrue(any("canary-wroom-renamed" in e and "slug" in e for e in errors), errors)

    def test_peripheral_the_pins_header_does_not_prove_fails(self):
        with _Mutated() as devices:
            edit(devices, "canary-display-dash", lambda d: d["peripherals"].append("microphone"))
            _, errors = ldm.lint(devices_dir=devices)
        self.assertTrue(any("HAS_MICROPHONE" in e for e in errors), errors)

    def test_emulator_flavor_outside_build_sh_allowlist_fails(self):
        with _Mutated() as devices:
            edit(devices, "canary-display-dash7",
                 lambda d: d.__setitem__("emulator", {"flavor": "dash7"}))
            _, errors = ldm.lint(devices_dir=devices)
        self.assertTrue(any("'dash7'" in e and "allowlist" in e for e in errors), errors)


class SchemaValidatorSubset(unittest.TestCase):
    def test_keywords_the_schema_uses(self):
        schema = {
            "type": "object", "additionalProperties": False, "required": ["a"],
            "properties": {
                "a": {"type": "string", "pattern": "^x", "minLength": 2},
                "n": {"type": "integer", "minimum": 1},
                "e": {"type": "string", "enum": ["p", "q"]},
                "l": {"type": "array", "minItems": 1, "uniqueItems": True,
                      "items": {"type": "string"}},
            },
        }
        self.assertEqual(ldm.validate({"a": "xy", "n": 1, "e": "p", "l": ["s"]}, schema), [])
        bad = ldm.validate({"a": "y", "n": 0, "e": "z", "l": ["s", "s", 3], "zz": 1}, schema)
        joined = "\n".join(bad)
        for needle in ("does not match", "minLength", "minimum", "is not one of", "duplicate",
                       "expected string", "'zz'"):
            self.assertIn(needle, joined, joined)
        self.assertIn("missing required key 'a'", "\n".join(ldm.validate({}, schema)))
        self.assertIn("fewer than minItems", "\n".join(ldm.validate({"a": "xx", "l": []}, schema)))
        # booleans are not integers, as in JSON Schema
        self.assertTrue(ldm.validate({"a": "xx", "n": True}, schema))

    def test_chip_normalization(self):
        self.assertEqual(ldm.norm_chip("ESP32 (classic dual-core)"), "ESP32")
        self.assertEqual(ldm.norm_chip("ESP32-C3 host + Grove Vision AI V2"), "ESP32-C3")
        self.assertEqual(ldm.norm_chip("esp32s3"), "ESP32-S3")
        self.assertEqual(ldm.norm_chip("ESP32-C6 · MR60BHA2 radar"), "ESP32-C6")
        self.assertIsNone(ldm.norm_chip("RP2040"))


if __name__ == "__main__":
    sys.exit(unittest.main())
