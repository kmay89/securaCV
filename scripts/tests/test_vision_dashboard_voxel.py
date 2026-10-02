#!/usr/bin/env python3
"""The Vision dashboard's voxel card says what the voxel sensor means.

homeassistant/lovelace/securacv-vision-dashboard.yaml draws the Voxel
sensor ("r,c", read off the retained state row's voxel) on a 3x3 grid. Its
comment said the sensor reads "-1,-1" when nobody is present, and the card
said "No person in frame" only then. But the row's voxel is the voxel
tracker's settled cell (PresenceFSM::snapshot: s.voxel =
voxel_tracker_.stable()), which keeps the last cell after the person leaves
and is reset only by PresenceFSM::reset() at boot (sweep A39 and its review;
firmware/tests_host/test_vision_core_bindings.cpp pins the carry-over). So
after the first visit the card painted the last cell forever and never said
nobody was there.

The card now reads whether anyone is in frame from the confidence sensor,
off the same state row (the best person box's score, 0 when that frame had
none: PresenceFSM::tick's confidence_), marks the cell 🟧 while someone is
in frame and 🔲 once the frame is empty, and says "-1,-1" means nobody seen
since the device started. (It does not read the Presence binary sensor: see
sweep HA25, that sensor's discovery payload.)

This renders the card with jinja2 (Home Assistant's template engine) when it
is installed, and pins the firmware lines the card relies on either way.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_vision_dashboard_voxel.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
DASH = REPO / "homeassistant/lovelace/securacv-vision-dashboard.yaml"
FW = REPO / "firmware/projects/canary-vision"

VOXEL = "sensor.securacv_canary_vision_DEVICE_ID_voxel"
CONFIDENCE = "sensor.securacv_canary_vision_DEVICE_ID_confidence"


def card() -> dict:
    dash = yaml.safe_load(DASH.read_text(encoding="utf-8"))
    live = next(v for v in dash["views"] if v.get("path") == "canary-vision")
    return next(c for c in live["cards"] if c.get("title") == "Position (voxel grid)")


def render(voxel: str, confidence: str) -> list[str]:
    import jinja2

    states = {VOXEL: voxel, CONFIDENCE: confidence}
    tmpl = jinja2.Environment().from_string(card()["content"])
    text = tmpl.render(states=lambda entity: states.get(entity, "unknown"))
    return [line.strip() for line in text.splitlines() if line.strip()]


def cell(lines: list[str], r: int, c: int) -> str:
    # each grid row is three markers; the variation selector rides on ▫️
    row = lines[r].replace("️", "")
    assert len(row) == 3, row
    return row[c]


class TheFirmwareSaysWhatTheCardAssumes(unittest.TestCase):
    def test_the_state_row_publishes_the_settled_cell_and_the_frames_score(self):
        fsm = (FW / "src/state/presence_fsm.cpp").read_text(encoding="utf-8")
        self.assertIn("s.voxel = voxel_tracker_.stable();", fsm)
        self.assertIn("confidence_ = vs.person_now ? vs.bbox.score : 0;", fsm)
        # the tracker is reset in PresenceFSM::reset(), which main.cpp calls
        # once, at boot, and on the frame that starts a visit (sweep F152):
        # between visits the cell stays where the last visit settled
        self.assertEqual(fsm.count("voxel_tracker_.reset();"), 2)
        self.assertIn("    if (!presence_) voxel_tracker_.reset();\n"
                      "    voxel_tracker_.update(vs.voxel, now_ms);\n", fsm)
        main = (FW / "src/main.cpp").read_text(encoding="utf-8")
        self.assertEqual(main.count("fsm.reset();"), 1)
        disc = (FW / "src/ha/ha_discovery.cpp").read_text(encoding="utf-8")
        self.assertIn('\\"value_template\\":\\"{{ value_json.voxel.r }},{{ value_json.voxel.c }}\\",', disc)
        self.assertIn('\\"value_template\\":\\"{{ value_json.confidence }}\\",', disc)

    def test_the_comment_no_longer_says_minus_one_means_nobody_present(self):
        text = DASH.read_text(encoding="utf-8")
        self.assertNotIn('("-1,-1" when nobody is present)', text)
        self.assertIn("it stays put", text)
        self.assertIn(CONFIDENCE, card()["content"])


class TheCardRenders(unittest.TestCase):
    def setUp(self):
        try:
            import jinja2  # noqa: F401  (Home Assistant's template engine)
        except ImportError:
            self.skipTest("jinja2 not installed")

    def test_nobody_seen_since_boot(self):
        lines = render("-1,-1", "0")
        self.assertEqual(lines[-1], "_Nobody seen since the device started._")
        self.assertNotIn("🟧", "".join(lines))
        self.assertNotIn("🔲", "".join(lines))

    def test_someone_in_frame_marks_their_cell(self):
        lines = render("2,0", "88")
        self.assertEqual(cell(lines, 2, 0), "🟧")
        self.assertEqual(sum(line.count("🟧") for line in lines), 1)
        self.assertFalse(any("No person" in line or "Nobody" in line for line in lines))

    def test_after_they_leave_the_card_says_so_and_keeps_the_cell(self):
        # the presence_ended row: the settled cell stays, the frame is empty
        lines = render("2,0", "0")
        self.assertEqual(cell(lines, 2, 0), "🔲")
        self.assertNotIn("🟧", "".join(lines))
        self.assertEqual(lines[-1], "_No person in frame. 🔲 is where they last settled._")

    def test_an_unavailable_sensor_reads_as_nobody_seen(self):
        lines = render("unavailable", "unavailable")
        self.assertEqual(lines[-1], "_Nobody seen since the device started._")


if __name__ == "__main__":
    unittest.main()
