#!/usr/bin/env python3
"""The Vision lingering alert reads a clock its events carry.

homeassistant/automations/securacv_vision_presence.yaml pages on two
canary-vision events, dwell_started and interaction_likely, and its message
used to say "dwell <dwell_ms>s". Neither event ever carries a dwell: the FSM
starts the dwell on dwell_started's own frame, so its row says dwell_ms 0,
and interaction_likely follows presence_ended, when nobody is dwelling
(sweep F130; firmware/tests_host/test_vision_presence_fsm.cpp pins both).
So every alert said "dwell 0s". The message now reads presence_ms for
dwell_started (how long the person has been in view) and visit_ms for
interaction_likely (how long the visit lasted), the clocks those rows do
carry (firmware/projects/canary-vision/README.md).

This holds the recipe to that: the alert triggers on those two events, its
message never reads dwell_ms, and it reads each event's own clock. When
jinja2 is installed the message is also rendered for both rows.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_vision_automation_clocks.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
RECIPE = REPO / "homeassistant/automations/securacv_vision_presence.yaml"
FSM = REPO / "firmware/projects/canary-vision/src/state/presence_fsm.cpp"


def alert() -> dict:
    docs = yaml.safe_load(RECIPE.read_text(encoding="utf-8"))
    return next(a for a in docs if a.get("id") == "securacv_vision_dwell_alert")


class TheLingeringAlert(unittest.TestCase):
    def test_it_pages_on_dwell_started_and_interaction_likely(self):
        cond = alert()["condition"][0]["value_template"]
        self.assertIn("['dwell_started', 'interaction_likely']", cond)

    def test_the_firmware_still_starts_the_dwell_from_zero(self):
        fsm = FSM.read_text(encoding="utf-8")
        # dwell_started's frame sets dwell_start_ms_, so its snapshot says 0
        self.assertIn("      dwelling_ = true;\n      dwell_start_ms_ = now_ms;\n"
                      "      return emit(out_event, \"dwell_started\");", fsm)
        self.assertIn("s.visit_ms    = last_visit_ms_;", fsm)

    def test_the_message_reads_the_clocks_those_events_carry(self):
        msg = alert()["action"][0]["data"]["message"]
        self.assertNotIn("dwell_ms", msg, "dwell_ms is 0 on both events this alert pages on")
        self.assertRegex(msg, r"presence_ms[^}]*\bif trigger\.payload_json\.event == 'dwell_started'")
        self.assertRegex(msg, r"else 'stayed %ds' % \(\(\(trigger\.payload_json\.visit_ms")

    def test_the_message_renders(self):
        try:
            import jinja2  # Home Assistant's template engine; not installed in every CI job
        except ImportError:
            self.skipTest("jinja2 not installed")
        tmpl = jinja2.Environment().from_string(alert()["action"][0]["data"]["message"])

        class Trigger:
            def __init__(self, payload):
                self.payload_json = payload

        lingering = tmpl.render(trigger=Trigger({
            "device_id": "canary_vision_001", "event": "dwell_started", "confidence": 91,
            "presence_ms": 10000, "dwell_ms": 0, "visit_ms": 0}))
        self.assertEqual(re.sub(r"\s+", " ", lingering).strip(),
                         "canary_vision_001: dwell_started — confidence 91%, in view 10s so far")
        left = tmpl.render(trigger=Trigger({
            "device_id": "canary_vision_001", "event": "interaction_likely", "reason": "dwell_then_left",
            "confidence": 0, "presence_ms": 0, "dwell_ms": 0, "visit_ms": 16700}))
        self.assertEqual(re.sub(r"\s+", " ", left).strip(),
                         "canary_vision_001: interaction_likely (dwell_then_left) — confidence 0%, stayed 17s")


if __name__ == "__main__":
    unittest.main()
