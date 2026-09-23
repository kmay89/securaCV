#!/usr/bin/env python3
"""check_ota_channels.py reads the display signing loop in BOTH of its shapes.

The regression this pins: when firmware-release.yml stopped typing the Canary
Display release envs and started expanding them from `flavor_envs.py` inside
the signing loop's header (`for D in "watch:watch" … $(jq … "$DISPLAY_ENVS")`),
the checker's literal-only regex stopped matching the header at all, every
display flavor read as unpublished, and Regression Guards went red on a list
that was in fact complete. So: literals are read, the derived list is read
through the same resolver the workflow runs, and a header the checker cannot
find is an error, never an empty set.

Run:  python3 -m unittest scripts.tests.test_check_ota_channels
"""

from __future__ import annotations

import importlib.util
import re
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "firmware" / "scripts" / "check_ota_channels.py"

spec = importlib.util.spec_from_file_location("check_ota_channels", SCRIPT)
coc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coc)  # type: ignore[union-attr]

INDEX_LINES = (
    '  INDEX="securacv-canary=${DL}/manifest-canary.json"\n'
    '  INDEX="${INDEX} securacv-canary-wap=${DL}/manifest-canary-wap.json"\n'
)
LITERAL_LOOP = (
    '  for D in "watch:watch" "dash:dash" "dash-modes:modes" '
    '"dash7:dash7" "amoled241:amoled241"; do\n'
)
DERIVED_LOOP = (
    '  for D in "watch:watch" "dash:dash" "dash-modes:modes" '
    """$(jq -r '.[] | "\\(.short):\\(.short)"' <<<"$DISPLAY_ENVS"); do\n"""
)


def workflow_with(loop: str | None) -> Path:
    text = "steps:\n" + INDEX_LINES + (loop or "") + "  echo done\n"
    fd, name = tempfile.mkstemp(suffix=".yml")
    Path(name).write_text(text, encoding="utf-8")
    return Path(name)


class PublishedManifests(unittest.TestCase):
    def test_literal_header_names_each_flavor(self):
        got = coc.published_manifests(workflow_with(LITERAL_LOOP))
        self.assertEqual(got, {
            "manifest-canary.json", "manifest-canary-wap.json",
            "manifest-canary-display-watch.json",
            "manifest-canary-display-dash.json",
            "manifest-canary-display-dash-modes.json",
            "manifest-canary-display-dash7.json",
            "manifest-canary-display-amoled241.json",
        })

    def test_derived_header_adds_every_release_env_from_flavors_json(self):
        got = coc.published_manifests(workflow_with(DERIVED_LOOP))
        # the literals survive alongside the expansion
        for lit in ("watch", "dash", "dash-modes"):
            self.assertIn(f"manifest-canary-display-{lit}.json", got)
        # and the derived list is the resolver's, not a guess
        shorts = coc.derived_display_release_shorts()
        self.assertTrue(shorts, "flavors.json release_envs resolved to nothing")
        for short in shorts:
            self.assertIn(f"manifest-canary-display-{short}.json", got)
        self.assertNotIn("manifest-canary-display-.json", got)

    def test_derived_list_matches_the_real_workflow(self):
        # The committed workflow must publish exactly the literals plus the
        # resolver's list — the shape this checker was rewritten for.
        real = coc.published_manifests()
        for short in coc.derived_display_release_shorts():
            self.assertIn(f"manifest-canary-display-{short}.json", real)

    def test_missing_loop_is_an_error_not_an_empty_set(self):
        with self.assertRaises(SystemExit) as ctx:
            coc.published_manifests(workflow_with(None))
        self.assertIn("for D in", str(ctx.exception))

    def test_two_loops_are_an_error(self):
        with self.assertRaises(SystemExit):
            coc.published_manifests(workflow_with(LITERAL_LOOP + DERIVED_LOOP))


SENTINEL_INI = REPO / "firmware" / "envs" / "platformio" / "canary-sentinel.ini"
ENV_SECTION = re.compile(r"^\[env:(canary-sentinel-[a-z0-9-]+)\]$", re.M)
PRODUCT_FLAG = re.compile(r"""-DSECURACV_OTA_PRODUCT=\"([^"]+)\"""")
MANIFEST_FLAG = re.compile(r"-DSECURACV_OTA_MANIFEST_URL=\"[^\"]*/(manifest-[a-z0-9-]+\.json)\"")


def sentinel_env_channels(text: str) -> dict[str, tuple[list[str], list[str]]]:
    """env -> (products, manifests) named in that env's OWN section."""
    heads = list(ENV_SECTION.finditer(text))
    out = {}
    for i, head in enumerate(heads):
        end = heads[i + 1].start() if i + 1 < len(heads) else len(text)
        body = text[head.end():end]
        # A section runs to the next header of any kind, env or not.
        nxt = re.search(r"^\[", body, re.M)
        body = body[: nxt.start()] if nxt else body
        out[head.group(1)] = (PRODUCT_FLAG.findall(body), MANIFEST_FLAG.findall(body))
    return out


class SentinelPresetChannels(unittest.TestCase):
    """A canary-sentinel preset is compile-time data, so its OTA product is
    the only thing standing between a published door image and a window,
    hallway or Heavy unit: the engine refuses an install only on product
    mismatch. The review found the four C6 presets sharing one product and one
    manifest — the first manifest ever signed would have put the door preset
    on all of them. Every env names its own, in its own section."""

    def test_every_sentinel_env_names_a_distinct_product_and_manifest(self):
        channels = sentinel_env_channels(SENTINEL_INI.read_text(encoding="utf-8"))
        self.assertGreaterEqual(len(channels), 5, "the preset envs moved — update this test")
        products, manifests = [], []
        for env, (prods, mans) in channels.items():
            self.assertEqual(len(prods), 1, f"{env}: expected exactly one SECURACV_OTA_PRODUCT flag")
            self.assertEqual(len(mans), 1, f"{env}: expected exactly one SECURACV_OTA_MANIFEST_URL flag")
            products += prods
            manifests += mans
        self.assertEqual(len(set(products)), len(products), f"shared OTA product: {products}")
        self.assertEqual(len(set(manifests)), len(manifests), f"shared manifest: {manifests}")

    def test_a_preset_env_without_its_own_product_is_caught(self):
        text = (
            "[env:canary-sentinel-door]\n"
            "build_flags =\n"
            "    '-DSECURACV_OTA_PRODUCT=\"securacv-canary-sentinel-door\"'\n"
            "    '-DSECURACV_OTA_MANIFEST_URL=\"https://x/manifest-canary-sentinel-door.json\"'\n"
            "\n[env:canary-sentinel-window]\n"
            "extends = env:canary-sentinel-door\n"
            "build_flags =\n    -DSENTINEL_PRESET_WINDOW\n"
        )
        channels = sentinel_env_channels(text)
        self.assertEqual(channels["canary-sentinel-door"],
                         (["securacv-canary-sentinel-door"], ["manifest-canary-sentinel-door.json"]))
        self.assertEqual(channels["canary-sentinel-window"], ([], []))

    def test_every_sentinel_manifest_is_declared(self):
        # check_ota_channels.py itself fails an undeclared polled manifest;
        # this names the sentinel's so a rename cannot drop one silently.
        channels = sentinel_env_channels(SENTINEL_INI.read_text(encoding="utf-8"))
        for _env, (_prods, mans) in channels.items():
            for name in mans:
                self.assertIn(name, coc.UNPUBLISHED)


if __name__ == "__main__":
    unittest.main()
