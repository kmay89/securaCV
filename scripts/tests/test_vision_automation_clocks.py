#!/usr/bin/env python3
"""The Vision alerts read values their events carry.

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

The same message printed "confidence <confidence>%" on both events, and the
litter-box recipe (homeassistant/automations/securacv_litterbox.yaml)
printed it on every visit-completed alert. The confidence is the best person
box's score on the frame the event was sent from (PresenceFSM::tick:
confidence_ = vs.person_now ? vs.bbox.score : 0), and interaction_likely is
only ever sent from an empty frame after presence_ended, so it said
"confidence 0%" every time (sweep HA26). Both recipes now print the
confidence for dwell_started only (sent from a frame with the person in it)
and never on interaction_likely.

This holds the recipes to that: the alert triggers on those two events, its
message never reads dwell_ms, it reads each event's own clock, and both
messages are rendered with jinja2 (Home Assistant's template engine; lint.yml
installs it at HA's pin, so it is imported unconditionally) for the rows the
device sends.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_vision_automation_clocks.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import yaml
from jinja2.sandbox import ImmutableSandboxedEnvironment

REPO = Path(__file__).resolve().parents[2]
RECIPE = REPO / "homeassistant/automations/securacv_vision_presence.yaml"
LITTER = REPO / "homeassistant/automations/securacv_litterbox.yaml"
FSM = REPO / "firmware/projects/canary-vision/src/state/presence_fsm.cpp"


def alert() -> dict:
    docs = yaml.safe_load(RECIPE.read_text(encoding="utf-8"))
    return next(a for a in docs if a.get("id") == "securacv_vision_dwell_alert")


def litter_visit() -> dict:
    docs = yaml.safe_load(LITTER.read_text(encoding="utf-8"))
    return next(a for a in docs if a.get("id") == "securacv_litterbox_visit_logged")


class Trigger:
    def __init__(self, payload):
        self.payload_json = payload


def render(automation: dict, payload: dict) -> str:
    # Home Assistant renders templates in a sandboxed environment
    tmpl = ImmutableSandboxedEnvironment().from_string(automation["action"][0]["data"]["message"])
    return re.sub(r"\s+", " ", tmpl.render(trigger=Trigger(payload))).strip()


# interaction_likely as the device sends it (firmware/tests_host/
# test_vision_presence_fsm.cpp holds confidence 0 on it)
LEFT = {"device_id": "canary_vision_001", "event": "interaction_likely", "reason": "dwell_then_left",
        "confidence": 0, "presence_ms": 0, "dwell_ms": 0, "visit_ms": 16700}


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
        # the confidence is the frame's best box score, 0 on an empty frame,
        # and the person branch returns before the leave-side events, so
        # interaction_likely (sent from an empty frame) always carries 0
        self.assertIn("confidence_ = vs.person_now ? vs.bbox.score : 0;", fsm)
        person_branch = fsm.index("  if (vs.person_now) {")
        self.assertLess(fsm.index("    return false;\n  }\n", person_branch),
                        fsm.index('return emit(out_event, "interaction_likely", reason);'))

    def test_the_message_reads_the_clocks_those_events_carry(self):
        msg = alert()["action"][0]["data"]["message"]
        self.assertNotIn("dwell_ms", msg, "dwell_ms is 0 on both events this alert pages on")
        self.assertRegex(msg, r"presence_ms[^}]*\bif trigger\.payload_json\.event == 'dwell_started'")
        self.assertRegex(msg, r"else 'stayed %ds' % \(\(\(trigger\.payload_json\.visit_ms")

    def test_the_message_renders(self):
        lingering = render(alert(), {
            "device_id": "canary_vision_001", "event": "dwell_started", "confidence": 91,
            "presence_ms": 10000, "dwell_ms": 0, "visit_ms": 0})
        self.assertEqual(lingering, "canary_vision_001: dwell_started — confidence 91%, in view 10s so far")
        left = render(alert(), LEFT)
        self.assertEqual(left, "canary_vision_001: interaction_likely (dwell_then_left) — stayed 17s")

    def test_interaction_likely_reports_no_confidence(self):
        # HA26: it is sent from an empty frame, so its confidence is always 0
        self.assertNotIn("confidence", render(alert(), LEFT))
        self.assertNotIn("confidence", render(alert(), {**LEFT, "reason": "zone_interaction_then_left"}))


class TheLitterBoxVisitAlert(unittest.TestCase):
    def test_it_pages_on_interaction_likely(self):
        cond = litter_visit()["condition"][0]["value_template"]
        self.assertIn("trigger.payload_json.get('event') == 'interaction_likely'", cond)

    def test_the_message_says_how_long_and_no_confidence(self):
        msg = litter_visit()["action"][0]["data"]["message"]
        self.assertNotIn("confidence", msg, "interaction_likely always carries confidence 0")
        left = render(litter_visit(), {**LEFT, "profile": "litter_box"})
        self.assertEqual(left, "Visit completed (dwell_then_left) — in the box about 17s.")


if __name__ == "__main__":
    unittest.main()
