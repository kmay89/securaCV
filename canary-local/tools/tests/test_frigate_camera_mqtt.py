#!/usr/bin/env python3
"""The hub plan's curated Frigate config keeps per-camera MQTT images off.

homeassistant/frigate/config.yaml is the config the hub plan's
write-frigate-config step copies onto a hub. Frigate's camera-level
`mqtt.enabled` is on by default and, whatever `snapshots.enabled` says,
publishes a cropped JPEG of every tracked object to
frigate/<camera>/<label>/snapshot as a retained message, which the broker
keeps. The top-level `mqtt:` is only the broker connection and must stay on,
or Frigate publishes no frigate/events for the witness kernel to seal.
There is no global switch for the camera key, so every camera in the file
carries it. Run:

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from _tooling import repo_root  # noqa: E402

CURATED = repo_root() / "homeassistant/frigate/config.yaml"


def _meaningful(text: str) -> list[tuple[int, str]]:
    """(indent, stripped line) for every line that is not blank or a comment."""
    out = []
    for raw in text.splitlines():
        stripped = raw.split(" #", 1)[0].rstrip() if not raw.lstrip().startswith("#") else ""
        if stripped.strip():
            out.append((len(raw) - len(raw.lstrip(" ")), stripped.strip()))
    return out


def camera_blocks(text: str) -> dict[str, list[tuple[int, str]]]:
    """Each camera under the top-level `cameras:` mapping, with the lines of
    its block. A structural reader so the pin holds where PyYAML is absent
    (this suite's CI step installs nothing)."""
    lines = _meaningful(text)
    cams: dict[str, list[tuple[int, str]]] = {}
    inside, cam_indent, current = False, None, None
    for indent, line in lines:
        if indent == 0:
            inside = line == "cameras:"
            current = None
            continue
        if not inside:
            continue
        if cam_indent is None:
            cam_indent = indent
        if indent == cam_indent and line.endswith(":"):
            current = line[:-1].strip("'\"")
            cams[current] = []
        elif current is not None and indent > cam_indent:
            cams[current].append((indent, line))
    return cams


def mqtt_images_off(block: list[tuple[int, str]]) -> bool:
    """True when the camera block holds `mqtt:` with `enabled: false` beneath
    it (or the flow form `mqtt: {enabled: false}`)."""
    if not block:
        return False
    key_indent = block[0][0]
    for i, (indent, line) in enumerate(block):
        if indent != key_indent:
            continue
        if line.replace(" ", "") == "mqtt:{enabled:false}":
            return True
        if line == "mqtt:":
            for child_indent, child in block[i + 1:]:
                if child_indent <= key_indent:
                    break
                if child.replace(" ", "") == "enabled:false":
                    return True
            return False
    return False


class CuratedFrigateConfig(unittest.TestCase):
    def setUp(self):
        self.text = CURATED.read_text(encoding="utf-8")

    def test_every_camera_turns_off_its_mqtt_images(self):
        cams = camera_blocks(self.text)
        self.assertTrue(cams, "the curated config ships at least one camera")
        for name, block in cams.items():
            with self.subTest(camera=name):
                self.assertTrue(
                    mqtt_images_off(block),
                    f"camera {name!r} has no `mqtt: enabled: false`: Frigate would publish "
                    "a retained JPEG of every object it tracks to the broker",
                )

    def test_the_broker_connection_stays_on(self):
        # The top-level block is the broker, not the images: off, there are
        # no frigate/events for the kernel to seal.
        self.assertIn("\nmqtt:\n  enabled: true\n", "\n" + self.text)

    def test_parsed(self):
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML not installed; the structural tests above hold the text")
        parsed = yaml.safe_load(self.text)
        self.assertIs(parsed["mqtt"]["enabled"], True)
        self.assertIs(parsed["snapshots"]["enabled"], False)
        self.assertIs(parsed["record"]["enabled"], False)
        self.assertTrue(parsed["cameras"])
        for name, conf in parsed["cameras"].items():
            with self.subTest(camera=name):
                self.assertEqual(conf.get("mqtt"), {"enabled": False})

    def test_the_structural_reader_catches_a_camera_without_it(self):
        # The reader the pin relies on is itself pinned: a camera with no
        # mqtt key, or one that leaves it on, is not taken for off.
        text = (
            "mqtt:\n  enabled: true\n"
            "cameras:\n"
            "  a:\n    detect:\n      fps: 5\n"
            "  b:\n    mqtt:\n      enabled: true\n"
            "  c:\n    mqtt: {enabled: false}\n"
            "  d:\n    mqtt:\n      height: 270\n      enabled: false  # off\n"
        )
        verdict = {n: mqtt_images_off(b) for n, b in camera_blocks(text).items()}
        self.assertEqual(verdict, {"a": False, "b": False, "c": True, "d": True})


if __name__ == "__main__":
    unittest.main()
