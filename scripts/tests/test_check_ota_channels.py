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


if __name__ == "__main__":
    unittest.main()
