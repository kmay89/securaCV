#!/usr/bin/env python3
"""The Vision dashboard's voxel card says what the voxel sensor means.

homeassistant/lovelace/securacv-vision-dashboard.yaml draws the Voxel
sensor ("r,c", read off the retained state row's voxel) on a 3x3 grid. Its
comment said the sensor reads "-1,-1" when nobody is present, and the card
said "No person in frame" only then. But the row's voxel is the voxel
tracker's settled cell (PresenceFSM::snapshot: s.voxel =
voxel_tracker_.stable()), which keeps the last cell after the person leaves
(sweep A39). So after the first visit the card painted the last cell
forever and never said nobody was there.

A39 made the card read whether anyone was in frame from the confidence
sensor, because the Presence binary sensor could not turn on: its discovery
template rendered "True"/"False" against payloads "true"/"false" (sweep
HA25). With that fixed (scripts/tests/test_ha_discovery_binary_sensors.py),
the card reads the Presence sensor, off the same state row: the cell is 🟧
while the device holds someone present and 🔲 once the visit has ended. The
confidence sensor is one frame's best box score, so it read 0, and the card
said nobody was there, on rows where the device still held a person
present: dwell_ended's row, the rows through the lost timeout, a heartbeat
taken on a frame that missed them. Each visit now starts its own tracker
(sweep F152), so while Presence is on the cell is this visit's.

Presence turns on and off only on firmware with the HA25 discovery fix; on
older firmware it stays unknown, and the dashboard YAML is copied
separately from firmware updates. So while Presence is neither on nor off
the card falls back to A39's confidence gate and says that it has, instead
of saying "Nobody present." about a device that has not said so.

This renders the card in Home Assistant's template environment
(_ha_jinja.environment(): its sandbox, its LoggingUndefined and its own
`int` filter; jinja2 itself is installed by lint.yml at HA's pin, so it is
imported unconditionally) and pins the firmware lines the card relies on.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_vision_dashboard_voxel.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import unittest
from pathlib import Path

import _ha_jinja
import yaml

REPO = Path(__file__).resolve().parents[2]
DASH = REPO / "homeassistant/lovelace/securacv-vision-dashboard.yaml"
FW = REPO / "firmware/projects/canary-vision"

VOXEL = "sensor.securacv_canary_vision_DEVICE_ID_voxel"
PRESENCE = "binary_sensor.securacv_canary_vision_DEVICE_ID_presence"
CONFIDENCE = "sensor.securacv_canary_vision_DEVICE_ID_confidence"


def card() -> dict:
    dash = yaml.safe_load(DASH.read_text(encoding="utf-8"))
    live = next(v for v in dash["views"] if v.get("path") == "canary-vision")
    return next(c for c in live["cards"] if c.get("title") == "Position (voxel grid)")


def render(voxel: str, presence: str, confidence: str = "0") -> list[str]:
    states = {VOXEL: voxel, PRESENCE: presence, CONFIDENCE: confidence}
    warnings: list[str] = []
    tmpl = _ha_jinja.environment(warnings).from_string(card()["content"])
    text = tmpl.render(states=lambda entity: states.get(entity, "unknown"))
    assert warnings == [], warnings  # HA would log each one
    return [line.strip() for line in text.splitlines() if line.strip()]


def cell(lines: list[str], r: int, c: int) -> str:
    # each grid row is three markers; the variation selector rides on ▫️
    row = lines[r].replace("️", "")
    assert len(row) == 3, row
    return row[c]


class TheFirmwareSaysWhatTheCardAssumes(unittest.TestCase):
    def test_the_state_row_publishes_the_settled_cell_and_presence(self):
        fsm = (FW / "src/state/presence_fsm.cpp").read_text(encoding="utf-8")
        self.assertIn("s.voxel = voxel_tracker_.stable();", fsm)
        self.assertIn("confidence_ = vs.person_now ? vs.bbox.score : 0;", fsm)
        # the tracker is reset in PresenceFSM::reset(), which main.cpp calls
        # once, at boot, and in open_visit, where a visit starts (sweep F152;
        # F186 made it the one place): between visits the cell stays where
        # the last visit settled
        self.assertEqual(fsm.count("voxel_tracker_.reset();"), 2)
        self.assertIn("EventMsg& out_event) {\n  voxel_tracker_.reset();\n"
                      "  voxel_tracker_.update(first_cell, seen_ms);\n", fsm)
        self.assertEqual(fsm.count('emit(out_event, "presence_started")'), 1)
        main = (FW / "src/main.cpp").read_text(encoding="utf-8")
        self.assertEqual(main.count("fsm.reset();"), 1)
        disc = (FW / "src/ha/ha_discovery.cpp").read_text(encoding="utf-8")
        self.assertIn('\\"value_template\\":\\"{{ value_json.voxel.r }},{{ value_json.voxel.c }}\\",', disc)
        # the Presence sensor the card reads renders its own payloads (HA25)
        self.assertIn('\\"value_template\\":\\"{{ \'true\' if value_json.presence | default(false) '
                      'else \'false\' }}\\",', disc)
        mqtt = (FW / "src/net/mqtt_mgr.cpp").read_text(encoding="utf-8")
        state_row = mqtt[mqtt.index("void publish_state_retained("):]
        self.assertLess(state_row.index('\\"presence\\":%s,'), state_row.index('\\"voxel\\":{'),
                        "presence and the voxel ride the same state row")

    def test_the_comment_no_longer_says_minus_one_means_nobody_present(self):
        text = DASH.read_text(encoding="utf-8")
        self.assertNotIn('("-1,-1" when nobody is present)', text)
        self.assertIn("it stays put", text)
        self.assertIn(PRESENCE, card()["content"])
        # the confidence is read only when Presence has said nothing
        self.assertIn("{% set present = ps == 'on' if known else states('" + CONFIDENCE + "') | int(0) > 0 %}",
                      card()["content"])
        self.assertIn("{% set known = ps in ['on', 'off'] %}", card()["content"])
        self.assertIn("sweep HA25 discovery fix", text, "the comment names the firmware Presence needs")


class TheCardRenders(unittest.TestCase):
    def test_nobody_seen_since_boot(self):
        lines = render("-1,-1", "off")
        self.assertEqual(lines[-1], "_Nobody seen since the device started._")
        self.assertNotIn("🟧", "".join(lines))
        self.assertNotIn("🔲", "".join(lines))

    def test_someone_present_marks_their_cell(self):
        lines = render("2,0", "on", "88")
        self.assertEqual(cell(lines, 2, 0), "🟧")
        self.assertEqual(sum(line.count("🟧") for line in lines), 1)
        self.assertFalse(any("Nobody" in line for line in lines))

    def test_present_on_a_frame_with_no_box_still_marks_their_cell(self):
        # dwell_ended's row, a row in the lost timeout, a heartbeat on a frame
        # that missed them: presence true, confidence 0. The confidence-gated
        # card said nobody was there; the device says someone is.
        lines = render("1,2", "on", "0")
        self.assertEqual(cell(lines, 1, 2), "🟧")
        self.assertFalse(any("Nobody" in line or "No person" in line for line in lines))

    def test_after_they_leave_the_card_says_so_and_keeps_the_cell(self):
        # the presence_ended row: the settled cell stays, presence is off
        lines = render("2,0", "off")
        self.assertEqual(cell(lines, 2, 0), "🔲")
        self.assertNotIn("🟧", "".join(lines))
        self.assertEqual(lines[-1], "_Nobody present. 🔲 is where they last settled._")

    def test_a_device_that_has_not_reported_is_not_nobody_seen(self):
        # offline (availability says so) or no state row yet: the card has
        # nothing to say about who is there, so it does not say nobody
        for state in ("unavailable", "unknown"):
            with self.subTest(state=state):
                lines = render(state, state)
                self.assertEqual(lines[-1], "_The Vision has not reported a position (offline, or no state row yet)._")
                self.assertNotIn("🟧", "".join(lines))
                self.assertNotIn("🔲", "".join(lines))

    def test_off_is_off_whatever_the_confidence(self):
        lines = render("2,0", "off", "91")
        self.assertEqual(cell(lines, 2, 0), "🔲")
        self.assertEqual(lines[-1], "_Nobody present. 🔲 is where they last settled._")

    def test_an_unknown_presence_falls_back_to_the_confidence_and_says_so(self):
        # firmware before HA25: Presence never matched its payloads, so it is
        # unknown; the card must not say nobody is present about it
        for presence in ("unknown", "unavailable"):
            with self.subTest(presence=presence):
                lines = render("2,0", presence, "91")
                self.assertEqual(cell(lines, 2, 0), "🟧")
                self.assertFalse(any("Nobody" in line for line in lines), lines)
                self.assertEqual(lines[-1], "_Presence is unknown, so this follows the confidence: someone in "
                                            "frame now. If Presence stays unknown, update the Vision's firmware._")
                lines = render("2,0", presence, "0")
                self.assertEqual(cell(lines, 2, 0), "🔲")
                self.assertFalse(any("Nobody" in line for line in lines), lines)
                self.assertEqual(lines[-1], "_Presence is unknown, so this follows the confidence: no person in "
                                            "frame, and 🔲 is where they last settled. If Presence stays unknown, "
                                            "update the Vision's firmware._")


if __name__ == "__main__":
    unittest.main()
