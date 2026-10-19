#!/usr/bin/env python3
"""The canary-wap's MQTT discovery templates, rendered the way Home Assistant
renders them, on the bodies the real csi_mqtt.cpp publishes (sweep F34).

The canary-wap announces 27 entities and 6 device triggers over MQTT
discovery (csi_mqtt.cpp's publish_discovery); Home Assistant's MQTT
integration renders each config's value_template and availability_template
on every message on its topic and compares the result with plain equality
(homeassistant/components/mqtt: a binary sensor's rendered text against
payload_on "ON" / payload_off "OFF", an empty rendering ignored; a device
trigger fires when its rendering equals its payload; availability against
"online" / "offline"). custom_components/securacv/tests/test_wap_mqtt_contract.py
holds the configs to the bodies at key and word level, in a lane with no
jinja2. This test renders them, through _ha_jinja.environment() (Home
Assistant's sandbox, its LoggingUndefined and its round/int/float filters,
at its jinja2 pin), on the record of the canary-wap's MQTT harness
(firmware/projects/canary-wap/tests_host/test_wap_mqtt_bodies.cpp, built here
with make and run with --json: the real csi_mqtt.cpp over a fake esp_mqtt
client through a unit's life). It holds:

  * no template logs a warning on any body on its topic;
  * availability reads online exactly when the status says so, the last
    will included;
  * each sensor shows the field it is named for (a table here, not read off
    the templates), on every body that carries it;
  * Online and Channel Locked follow their booleans; Presence reads ON for
    every state the presence and breathing modules emit
    (core_presence.cpp's STATE_NAMES, core_breathing.cpp's state names)
    except empty, OFF for empty, and leaves the state alone on any other
    row (the tamper commit's); each acoustic sensor reads ON for exactly
    its word of securacv_audio.cpp's event_name() and OFF for the rest;
  * each presence device trigger fires on a live row of its state and on
    nothing else, the anomaly trigger on the tamper commit's anomaly row,
    and no trigger fires on the backfilled (replayed) row; every presence
    state has its trigger.

The vocabularies are read from the module sources; a reader that cannot find
one refuses by name. The two bodies the sketch builds (the update state and
the acoustic snapshot) are the harness's copies in the sketch's shape; their
keys are held to canary_wap.ino by test_wap_mqtt_contract.py.

What the record shows wrong is an expectedFailure (an unexpected success
fails the run): Signal Strength reads 0 dBm after every connect, because the
connect burst's status carries no rssi.

The harness needs make, a GCC C++ compiler and OpenSSL's headers. Without
them the test skips naming why, except under SECURACV_WAP_HARNESS=require
(lint.yml sets it), where it fails. jinja2 is imported unconditionally, as
in the other template tests here: a skipped render test reads as covered
while catching nothing.

Host-tested: rendered with jinja2 as Home Assistant configures it, not in a
running Home Assistant.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_ha_discovery_wap_render.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import subprocess
import unittest
from pathlib import Path
from typing import Any

import _ha_jinja

REPO = Path(__file__).resolve().parents[2]
WAP = REPO / "firmware" / "projects" / "canary-wap"
HOST = WAP / "tests_host"
SRC = WAP / "arduino" / "canary_wap"
HARNESS = HOST / "test_wap_mqtt_bodies"
REQUIRE = "SECURACV_WAP_HARNESS"

# What each sensor shows: (its state topic, the body field, a divisor).
SENSORS: dict[str, tuple[str, str, int]] = {
    "state": ("events", "state", 1),
    "confidence": ("events", "confidence", 1),
    "motion": ("events", "motion", 1),
    "breathing": ("events", "breathing", 1),
    "bpm": ("events", "bpm", 1),
    "witness_count": ("counts", "total", 1),
    "chain_length": ("chain", "length", 1),
    "uptime": ("health", "uptime", 1),
    "memory_free": ("health", "memory_free", 1),
    "rssi": ("status", "rssi", 1),
    "mesh_airtime_pct": ("mesh", "airtime_pct_x100", 100),
    "mesh_channel": ("mesh", "channel", 1),
    "chirp_state": ("chirp", "state", 1),
    "beacon_state": ("beacon", "state", 1),
    "beacon_airtime_pct": ("beacon", "beacon_airtime_pct_x100", 100),
    "beacon_active_template": ("beacon", "active_template", 1),
}
# Binary sensors that follow one boolean of their body.
BOOLEANS: dict[str, tuple[str, str]] = {
    "online": ("status", "online"),
    "mesh_channel_locked_to_sta": ("mesh", "locked_to_sta"),
}
# Each acoustic binary sensor and the event_name() word it is ON for.
ACOUSTIC: dict[str, str] = {
    "smoke_alarm": "smoke_alarm_t3",
    "co_alarm": "co_alarm_t4",
    "knock": "knock",
    "doorbell": "doorbell",
    "glass_break": "glass_break",
}
# event_name()'s words that name no event.
ACOUSTIC_QUIET = {"none", "unknown"}


def _cannot(why: str) -> None:
    if os.environ.get(REQUIRE) == "require":
        raise AssertionError(f"{why} ({REQUIRE}=require)")
    raise unittest.SkipTest(f"{why}; {REQUIRE}=require makes this a failure")


def build_record() -> dict[str, Any]:
    """The harness's record, built with make and run with --json."""
    cxx = shlex.split(os.environ.get("CXX") or "g++")[:1] or ["g++"]
    missing = [tool for tool in ("make", *cxx) if shutil.which(tool) is None]
    if missing:
        _cannot(f"cannot build the WAP MQTT harness: {', '.join(missing)} not on PATH")
    build = subprocess.run(["make", "-C", str(HOST), HARNESS.name], capture_output=True,
                           text=True, timeout=900, check=False)
    if build.returncode != 0:
        if os.environ.get(REQUIRE) == "require":
            raise AssertionError(f"the harness did not build:\n{build.stdout}\n{build.stderr}")
        first = next((ln for ln in build.stderr.splitlines() if "error" in ln.lower()),
                     build.stderr.strip()[:200])
        _cannot(f"the WAP MQTT harness does not build here ({first.strip()})")
    out = subprocess.run([str(HARNESS), "--json"], capture_output=True, text=True,
                         timeout=120, check=False)
    if out.returncode != 0:
        raise AssertionError(f"the harness's own checks failed:\n{out.stderr}")
    record = json.loads(out.stdout)
    if record.get("harness") != HARNESS.name:
        raise AssertionError(f"not the harness's record: {record.get('harness')!r}")
    return record


def render(template: str, payload: str) -> tuple[str, list[str]]:
    """What Home Assistant's MQTT value template makes of `payload`: the
    rendering, stripped, and the warnings its LoggingUndefined would log."""
    warnings: list[str] = []
    variables: dict[str, Any] = {"value": payload}
    try:
        variables["value_json"] = json.loads(payload)
    except ValueError:
        pass
    out = _ha_jinja.environment(warnings).from_string(template).render(**variables).strip()
    return out, warnings


def _source(name: str) -> str:
    return (SRC / name).read_text(encoding="utf-8")


def presence_states() -> list[str]:
    """core_presence.cpp's STATE_NAMES, in order."""
    m = re.search(r"STATE_NAMES\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", _source("core_presence.cpp"))
    if not m:
        raise AssertionError("core_presence.cpp's STATE_NAMES moved; re-read its states")
    return re.findall(r'"(\w+)"', m.group(1))


def breathing_states() -> list[str]:
    """The states core_breathing.cpp's emitters write."""
    states = re.findall(r'strncpy\(v\.state_name,\s*"(\w+)"', _source("core_breathing.cpp"))
    if not states:
        raise AssertionError("core_breathing.cpp writes no state_name it can read; re-read it")
    return states


def acoustic_words() -> set[str]:
    """securacv_audio.cpp's event_name(): the words the sketch's acoustic
    snapshot carries (canary_wap.ino copies audio_event_name() into it)."""
    m = re.search(r"const char\* event_name\(uint8_t \w+\) \{(.*?)\n\}", _source("securacv_audio.cpp"),
                  re.S)
    if not m:
        raise AssertionError("securacv_audio.cpp's event_name() moved; re-read its words")
    return set(re.findall(r'return "(\w+)";', m.group(1)))


class _Record(unittest.TestCase):
    record: dict[str, Any]

    @classmethod
    def setUpClass(cls) -> None:
        cls.record = build_record()

    # ── the record ────────────────────────────────────────────────────

    def topic(self, suffix: str) -> str:
        return f"{self.record['prefix']}/{self.record['device_id']}/{suffix}"

    def publishes(self, *phases: str) -> list[dict[str, Any]]:
        names = phases or tuple(p["name"] for p in self.record["phases"])
        return [pub for p in self.record["phases"] if p["name"] in names for pub in p["publishes"]]

    def configs(self) -> dict[str, tuple[str, dict[str, Any]]]:
        """object id -> (component, config), from the connect burst."""
        out = {}
        for pub in self.publishes("connect"):
            parts = pub["topic"].split("/")
            if parts[-1] == "config":
                out[parts[-2]] = (parts[1], json.loads(pub["payload"]))
        return out

    def bodies(self, topic: str) -> list[str]:
        """Every non-empty body on `topic`, the last will included."""
        bodies = [p["payload"] for p in self.publishes() if p["topic"] == topic and p["payload"]]
        if self.record["last_will"]["topic"] == topic:
            bodies.append(self.record["last_will"]["payload"])
        return bodies

    def one(self, phase: str, suffix: str) -> dict[str, Any]:
        (body,) = [json.loads(p["payload"]) for p in self.publishes(phase)
                   if p["topic"] == self.topic(suffix)]
        return body


class WapDiscoveryRenders(_Record):
    def test_the_record_has_the_rows_the_triggers_gate_on(self):
        events = [json.loads(b) for b in self.bodies(self.topic("events"))]
        self.assertEqual(sum(e["replay"] for e in events), 1, "one replayed row")
        self.assertEqual([e["category"] for e in events if e["category"] != "event"], ["anomaly"])
        self.assertEqual(len(self.configs()), 33, "27 entity configs and 6 device triggers")

    def test_every_template_renders_every_body_on_its_topic_without_a_warning(self):
        rendered = 0
        for oid, (_, cfg) in self.configs().items():
            for topic, key in ((cfg.get("stat_t") or cfg.get("topic"), "val_tpl"),
                               (cfg.get("avty_t"), "avty_tpl")):
                if key not in cfg:
                    continue
                bodies = self.bodies(topic)
                self.assertTrue(bodies, (oid, topic))
                for body in bodies:
                    with self.subTest(entity=oid, template=key, body=body):
                        _, warnings = render(cfg[key], body)
                        rendered += 1
                        self.assertEqual(warnings, [])
        self.assertGreater(rendered, 100)

    def test_availability_reads_online_exactly_when_the_status_says_so(self):
        status = self.bodies(self.topic("status"))
        self.assertIn(self.record["last_will"]["payload"], status)
        for oid, (_, cfg) in self.configs().items():
            if "avty_t" not in cfg:
                continue
            with self.subTest(entity=oid):
                self.assertEqual(cfg["avty_t"], self.topic("status"))
                # Home Assistant's defaults: "online" / "offline".
                self.assertFalse({"pl_avail", "pl_not_avail"} & set(cfg))
                for body in status:
                    want = "online" if json.loads(body)["online"] is True else "offline"
                    self.assertEqual(render(cfg["avty_tpl"], body)[0], want, body)

    def test_each_sensor_shows_the_field_it_is_named_for(self):
        sensors = {oid: cfg for oid, (comp, cfg) in self.configs().items() if comp == "sensor"}
        self.assertEqual(set(sensors), set(SENSORS))
        for oid, (suffix, field, divisor) in SENSORS.items():
            cfg = sensors[oid]
            self.assertEqual(cfg["stat_t"], self.topic(suffix), oid)
            shown = 0
            for body in self.bodies(cfg["stat_t"]):
                data = json.loads(body)
                if field not in data:
                    continue   # DEFAULTED_GAPS in test_wap_mqtt_contract.py; rssi below
                want = data[field] / divisor if divisor != 1 else data[field]
                with self.subTest(sensor=oid, body=body):
                    self.assertEqual(render(cfg["val_tpl"], body)[0], str(want))
                    shown += 1
            self.assertGreater(shown, 0, oid)

    def test_online_and_channel_locked_follow_their_booleans(self):
        configs = self.configs()
        for oid, (suffix, field) in BOOLEANS.items():
            _, cfg = configs[oid]
            self.assertEqual(cfg["stat_t"], self.topic(suffix), oid)
            seen = set()
            for body in self.bodies(cfg["stat_t"]):
                value = json.loads(body)[field]
                seen.add(value)
                with self.subTest(sensor=oid, body=body):
                    self.assertEqual(render(cfg["val_tpl"], body)[0], "ON" if value else "OFF")
            self.assertEqual(seen, {True, False}, f"{oid}: the record holds both readings")

    def test_presence_reads_every_state_the_modules_emit(self):
        _, cfg = self.configs()["presence"]
        live = self.one("running", "events")
        states = presence_states() + breathing_states()
        self.assertIn("empty", states)
        for state in states:
            body = json.dumps({**live, "state": state, "event_type": state})
            with self.subTest(state=state):
                self.assertEqual(render(cfg["val_tpl"], body)[0],
                                 "OFF" if state == "empty" else "ON")
        # Any other row on events leaves Presence as it was: an empty
        # rendering, which Home Assistant ignores.
        anomaly = self.one("tamper", "events")
        self.assertNotIn(anomaly["state"], states)
        self.assertEqual(render(cfg["val_tpl"], json.dumps(anomaly))[0], "")

    def test_each_acoustic_sensor_reads_on_for_its_word_alone(self):
        words = acoustic_words()
        self.assertEqual(words, set(ACOUSTIC.values()) | ACOUSTIC_QUIET,
                         "securacv_audio.cpp's event_name() words moved")
        cap = re.search(r"g_audio_mqtt_event\[(\d+)\]", _source("canary_wap.ino"))
        self.assertIsNotNone(cap, "canary_wap.ino's g_audio_mqtt_event moved")
        self.assertTrue(all(len(w) < int(cap.group(1)) for w in words),
                        "a word the sketch's buffer cuts")
        configs = self.configs()
        sensing = self.one("running", "sensing")
        for word in sorted(words):
            body = json.dumps({**sensing, "acoustic_event": word})
            on = {oid for oid in ACOUSTIC
                  if render(configs[oid][1]["val_tpl"], body)[0] == "ON"}
            off = {oid for oid in ACOUSTIC
                   if render(configs[oid][1]["val_tpl"], body)[0] == "OFF"}
            with self.subTest(word=word):
                want = {oid for oid, w in ACOUSTIC.items() if w == word}
                self.assertEqual(on, want)
                self.assertEqual(off, set(ACOUSTIC) - want)

    def test_every_binary_sensor_is_rendered_here(self):
        binary = {oid for oid, (comp, _) in self.configs().items() if comp == "binary_sensor"}
        self.assertEqual(binary, set(BOOLEANS) | set(ACOUSTIC) | {"presence"})

    def test_device_triggers_fire_on_live_rows_only(self):
        triggers = {oid: cfg for oid, (comp, cfg) in self.configs().items()
                    if comp == "device_automation"}
        presence = {cfg["pl"]: cfg for cfg in triggers.values()
                    if cfg["type"] == "presence_changed"}
        self.assertEqual(set(presence), set(presence_states()),
                         "each presence state has its trigger, and only those")
        live = self.one("running", "events")
        for state, cfg in presence.items():
            with self.subTest(trigger=state):
                row = {**live, "state": state, "event_type": state}
                self.assertEqual(render(cfg["val_tpl"], json.dumps(row))[0], cfg["pl"])
                replayed = json.dumps({**row, "replay": True})
                self.assertEqual(render(cfg["val_tpl"], replayed)[0], "")
        (anomaly,) = [cfg for cfg in triggers.values() if cfg["type"] == "anomaly"]
        self.assertEqual(len(triggers), len(presence) + 1)
        # On the record itself: each events body fires exactly the triggers it should.
        for body in self.bodies(self.topic("events")):
            data = json.loads(body)
            fired = {oid for oid, cfg in triggers.items()
                     if render(cfg["val_tpl"], body)[0] == cfg["pl"]}
            if data["replay"]:
                want: set[str] = set()
            elif data["category"] == "anomaly":
                want = {oid for oid, cfg in triggers.items() if cfg is anomaly}
            else:
                want = {oid for oid, cfg in triggers.items() if cfg.get("pl") == data["state"]
                        and cfg["type"] == "presence_changed"}
            with self.subTest(body=body):
                self.assertEqual(fired, want)
        self.assertEqual(render(anomaly["val_tpl"], json.dumps(self.one("tamper", "events")))[0],
                         anomaly["pl"])

    @unittest.expectedFailure
    def test_signal_strength_keeps_its_reading_across_a_connect(self):
        """Handed up (#1836): the connect burst's status carries no rssi, so
        Signal Strength renders 0 dBm until the loop's next status (30 s or
        longer under power gating). A status without rssi must render
        nothing (Home Assistant then keeps the last reading), or carry it."""
        _, cfg = self.configs()["rssi"]
        for body in self.bodies(self.topic("status")):
            data = json.loads(body)
            if data["online"] is True:
                want = str(data["rssi"]) if "rssi" in data else ""
                self.assertEqual(render(cfg["val_tpl"], body)[0], want, body)


if __name__ == "__main__":
    unittest.main()
