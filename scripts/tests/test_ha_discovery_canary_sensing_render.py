#!/usr/bin/env python3
"""The canary's Sensing RSSI renders a reading or nothing, never 0 dBm (sweep F411).

firmware/canary announces Sensing RSSI over MQTT discovery itself
(lib/securacv_mqtt/src/securacv_mqtt.cpp, mqtt_send_ha_discovery): a sensor
on the retained sensing topic, dBm, device_class signal_strength. That
topic's one body is built by src/main.cpp's mqtt_publish_sensing_update().
Its rssi_dbm is a CSI window's mean, and only sensing_feed_csi() sets it, so
before the first window, and for good on a build without CSI (FEATURE_CSI
defaults to 0), the snapshot holds sensing_init()'s zero. The body carried
that zero, and the template's `default(0)` would have rendered 0 for a body
without the key too: either way Home Assistant showed 0 dBm, a
full-strength reading no radio measured.

The body now carries rssi_dbm only once a window has been seen
(windows_seen > 0), and the template renders a body without it as nothing,
which Home Assistant's MQTT sensor ignores on a sensor whose state must be
numeric (a unit makes it one): unknown before the first reading, the last
reading kept after it. tests/ha/test_canary_sensing_rssi.py drives that on a
real core; this test renders through _ha_jinja.environment() (Home
Assistant's sandbox, its LoggingUndefined and its round/int/float filters,
at its jinja2 pin). It holds:

  * the Sensing RSSI row is on the sensing topic, in dBm, signal_strength;
  * its template renders a body's rssi_dbm, and a body without the key as
    nothing, with no template warning either way;
  * main.cpp writes rssi_dbm in exactly one place, under the windows_seen
    test (a text pin: main.cpp is not host-compiled, firmware/canary's
    PlatformIO build is CI's);
  * every other entity announced on the sensing topic renders the same with
    and without rssi_dbm, so the key's absence moves nothing else; the body
    the test renders carries every key main.cpp writes, read from it.

A reader that cannot find what it reads refuses by name. jinja2 is imported
unconditionally, as in the other template tests here: a skipped render test
reads as covered while catching nothing.

Host-tested: rendered with jinja2 as Home Assistant configures it, not in a
running Home Assistant, and the firmware body is read from the source, not
built.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_ha_discovery_canary_sensing_render.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path
from typing import Any

import _ha_jinja

REPO = Path(__file__).resolve().parents[2]
CANARY = REPO / "firmware" / "canary"
MQTT_CPP = CANARY / "lib" / "securacv_mqtt" / "src" / "securacv_mqtt.cpp"
MAIN_CPP = CANARY / "src" / "main.cpp"

_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def c_string(text: str) -> str:
    """The adjacent C string literals in `text`, concatenated and decoded."""
    return "".join(re.sub(r"\\(.)", lambda m: m.group(1), lit) for lit in _LITERAL.findall(text))


def function_body(src: str, signature: str) -> str:
    """The braces-balanced body of the function whose definition starts with
    `signature` (the definition, not the forward declaration: the one whose
    signature is followed by an opening brace)."""
    m = re.search(re.escape(signature) + r"\s*\{", src)
    assert m, f"{signature} moved; re-read its file"
    depth, i = 1, m.end()
    while depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    return src[m.end():i - 1]


def sensing_entities() -> dict[str, dict[str, str]]:
    """Every entity mqtt_send_ha_discovery announces on the sensing topic, by
    object id: its component, name and value template, and the unit, device
    class and entity category it sets."""
    src = MQTT_CPP.read_text(encoding="utf-8")
    body = function_body(src, "bool mqtt_send_ha_discovery(const char* device_id, const char* firmware_version)")
    found: dict[str, dict[str, str]] = {}
    start = 0
    for call in re.finditer(r'publish_discovery\("(\w+)",\s*device_id,\s*([^,]+),', body):
        block, start = body[start:call.start()], call.end()
        if not re.search(r'doc\["stat_t"\]\s*=\s*s_topic_sensing;', block):
            continue
        component, object_id = call.group(1), call.group(2).strip()
        tpl = re.search(r'doc\["val_tpl"\]\s*=\s*([^;]+);', block)
        assert tpl, f"a sensing entity before {object_id} has no value template"
        if object_id == "diags[i].uid_slug":
            # The acoustic counters: one block over a table of rows.
            assert tpl.group(1).strip() == "diags[i].tpl", "the counters' template moved"
            table = re.search(r"static const DiagSensor diags\[\] = \{(.*?)\n\s*\};", block, re.S)
            assert table, "the acoustic counters' table moved"
            rows = re.findall(r'\{\s*"([^"]+)",\s*"((?:[^"\\]|\\.)*)",\s*"(\w+)",', table.group(1))
            assert rows, "the acoustic counters' table has no rows this reader can read"
            for name, template, slug in rows:
                found[slug] = {"component": component, "name": name, "val_tpl": c_string(f'"{template}"')}
            continue
        assert object_id.startswith('"'), f"a sensing entity's object id is not a literal: {object_id}"
        entity = {"component": component, "val_tpl": c_string(tpl.group(1))}
        for key in ("name", "unit_of_meas", "dev_cla", "ent_cat"):
            m = re.search(r'doc\["%s"\]\s*=\s*("(?:[^"\\]|\\.)*");' % key, block)
            if m:
                entity[key] = c_string(m.group(1))
        found[c_string(object_id)] = entity
    return found


def sensing_keys() -> tuple[set[str], set[str]]:
    """The keys main.cpp's sensing body writes, and acoustic_stats' keys."""
    body = function_body(MAIN_CPP.read_text(encoding="utf-8"), "static void mqtt_publish_sensing_update()")
    return set(re.findall(r'\bdoc\["(\w+)"\]', body)), set(re.findall(r'\bast\["(\w+)"\]', body))


def render(template: str, body: dict[str, Any]) -> tuple[str, list[str]]:
    """What Home Assistant's MQTT entity gets from the template on this body:
    the rendering, stripped, and every warning LoggingUndefined logged."""
    warnings: list[str] = []
    out = _ha_jinja.environment(warnings).from_string(template).render(
        value=json.dumps(body), value_json=body
    )
    return out.strip(), warnings


# Two sensing bodies with every key main.cpp writes: one quiet, one with each
# event field set (so the binary sensors render ON as well as OFF).
QUIET: dict[str, Any] = {
    "label": "quiet", "motion": 3, "breathing": 1, "rssi_dbm": -61,
    "frames_in_window": 48, "channel": 6, "time_bucket": 61,
    "acoustic_event": "none", "acoustic_conf": 0,
    "acoustic_stats": {"frames_processed": 9000, "on_transitions": 2, "off_transitions": 2,
                       "t3_detected": 0, "t4_detected": 0, "i2s_read_errors": 0,
                       "knock_detected": 1, "doorbell_detected": 0, "glass_break_detected": 0},
    "touch_event": "none", "touch_conf": 0, "touch_pad": 0,
    "ir_protocol": "none", "ir_bucket": 0, "ir_conf": 0,
    "temp_drift_active": False, "temp_drift_conf": 0,
    "wake_reason": "cold_boot", "power_mode": "normal",
}
BUSY: dict[str, Any] = {
    **QUIET, "label": "motion", "motion": 71, "rssi_dbm": -48,
    "acoustic_event": "smoke_alarm_t3", "acoustic_conf": 90,
    "touch_event": "enclosure_tamper", "touch_conf": 80, "touch_pad": 3,
    "ir_protocol": "nec", "ir_bucket": 7, "ir_conf": 60,
    "temp_drift_active": True, "temp_drift_conf": 70, "wake_reason": "touch",
}


def without_rssi(body: dict[str, Any]) -> dict[str, Any]:
    return {k: v for k, v in body.items() if k != "rssi_dbm"}


class SensingRssiRenderTest(unittest.TestCase):
    def rssi(self) -> dict[str, str]:
        entities = sensing_entities()
        self.assertIn("sensing_rssi", entities, "Sensing RSSI is not announced on the sensing topic")
        return entities["sensing_rssi"]

    def test_the_row_is_a_numeric_signal_strength_sensor(self):
        row = self.rssi()
        self.assertEqual(
            (row["component"], row["name"], row["unit_of_meas"], row["dev_cla"]),
            ("sensor", "Sensing RSSI", "dBm", "signal_strength"),
        )

    def test_a_reading_renders_and_no_reading_renders_nothing(self):
        """Sweep F411: a body with rssi_dbm renders it; a body without the key
        renders nothing, which Home Assistant ignores, keeping the last
        reading (unknown before the first). `default(0)` rendered 0 there."""
        template = self.rssi()["val_tpl"]
        for reading in (-61, -48, -90, -30):
            body = {**QUIET, "rssi_dbm": reading}
            self.assertEqual(render(template, body), (str(reading), []), reading)
        # Before the first CSI window, after a reboot until the next one, and
        # on a build without CSI: the body carries no rssi_dbm.
        for body in (without_rssi(QUIET), without_rssi(BUSY), {}):
            self.assertEqual(render(template, body), ("", []), body)

    def test_main_cpp_writes_rssi_dbm_only_once_a_window_was_seen(self):
        """The text pin: the body's one rssi_dbm write sits under the
        windows_seen test, so no body carries the snapshot's unmeasured zero."""
        body = function_body(MAIN_CPP.read_text(encoding="utf-8"), "static void mqtt_publish_sensing_update()")
        writes = [line.strip() for line in body.splitlines() if '"rssi_dbm"' in line]
        self.assertEqual(
            writes, ['if (s.windows_seen > 0) doc["rssi_dbm"] = (int)s.rssi_dbm;'],
            "main.cpp writes rssi_dbm other than once, under the windows_seen test",
        )

    def test_the_bodies_here_carry_every_key_main_cpp_writes(self):
        keys, stats = sensing_keys()
        self.assertIn("rssi_dbm", keys)
        for body in (QUIET, BUSY):
            self.assertEqual(set(body), keys)
            self.assertEqual(set(body["acoustic_stats"]), stats)

    def test_no_other_sensing_entity_moves_without_rssi_dbm(self):
        """Every other entity on the sensing topic renders the same text, and
        logs the same warnings, on a body with rssi_dbm and without it."""
        entities = sensing_entities()
        others = {oid: e for oid, e in entities.items() if oid != "sensing_rssi"}
        self.assertGreaterEqual(len(others), 15, f"the reader found too few entities: {sorted(entities)}")
        for body in (QUIET, BUSY):
            for oid, entity in others.items():
                self.assertNotIn("rssi_dbm", entity["val_tpl"], oid)
                with self.subTest(entity=oid, label=body["label"]):
                    self.assertEqual(
                        render(entity["val_tpl"], without_rssi(body)), render(entity["val_tpl"], body)
                    )


if __name__ == "__main__":
    unittest.main()
