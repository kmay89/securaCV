"""Home Assistant reads the event-id space warning and the egress counters (HA24).

Since #1762 both firmware trees' MQTT health carries ``event_id_space_low``
(firmware sweep F82), and the canary base's carries ``csi_event_egress`` (what
its committed-event egress dropped and sent since boot, the backfill planner's
counters nested as ``planner``) and ``offline_queue`` (its MQTT offline
queue's drops) (sweep F109). The canary-wap sends the same egress object on a
retained topic of its own, ``egress`` (sweep F149): its health body has no
room for it. The integration's health sensor kept a fixed attribute set, so a
device nearing the wrap raised nothing and a device losing rows reached no
attribute.

Now:
  - a diagnostic problem binary sensor, Event ID Space Low, follows the flag,
    created for a device on the first health publish that carries it;
  - the health sensor carries ``csi_event_egress`` and ``offline_queue`` as
    attributes, the canary-wap's from its ``egress`` topic.

The monorepo-only tests hold const.py's counter names to the firmware's
structs and to the canary-wap's JSON builder, and drive the sensor with
bodies built from the firmware's own key lists, so a counter the firmware
adds fails here until Home Assistant names it. The HACS mirror carries no
firmware source, so they skip there.
"""

from __future__ import annotations

import json
import re
import types
from pathlib import Path

import pytest

from .test_mqtt_payload_hardening import _entity, _msg
from .conftest import run

from .. import binary_sensor as bs_platform
from .. import sensor as sensor_platform
from ..const import (
    DOMAIN,
    EGRESS_COUNTERS,
    EGRESS_PLANNER_COUNTERS,
    OFFLINE_QUEUE_COUNTERS,
    TOPIC_EGRESS,
    TOPIC_HEALTH,
)
from ..health_metrics import egress_counters, event_id_space_low, offline_queue_counters

_FW = Path(__file__).resolve().parents[3] / "firmware"
_WAP = _FW / "projects" / "canary-wap" / "arduino" / "canary_wap"
_WAP_EGRESS_H = _WAP / "csi_event_egress.h"
_WAP_MQTT = _WAP / "csi_mqtt.cpp"
_BACKFILL_H = _FW / "common" / "csi" / "src" / "csi_event_backfill.h"
_OFFLINE_QUEUE_H = _FW / "common" / "mqtt" / "mqtt_offline_queue.h"
_CANARY_MAIN = _FW / "canary" / "src" / "main.cpp"

monorepo_only = pytest.mark.skipif(
    not _WAP_EGRESS_H.is_file(),
    reason="monorepo-only: the HACS mirror carries no firmware source",
)

EGRESS = {
    "dropped": 1, "held_dropped": 2, "ambient_dropped": 3, "unsent_dropped": 4,
    "planner": {
        "live": 5, "held": 6, "queued": 7, "replayed": 8, "skipped": 9,
        "untrusted": 10, "unsendable": 11, "truncated_unsent": 12, "read_giveups": 13,
    },
}
QUEUE = {"dropped_overflow": 14, "dropped_oversize": 15, "dropped_flushed": 16}

# A canary base's health, trimmed to what these tests read.
CANARY_HEALTH = {
    "uptime": 3600, "free_heap": 180_000, "firmware_version": "2.4.16",
    "public_key": "ab" * 32, "event_id_space_low": False,
    "csi_event_egress": EGRESS, "offline_queue": QUEUE,
}
# A canary-wap's health: the flag, no counters (they ride `egress`).
WAP_HEALTH = {
    "battery": 100, "battery_present": False, "memory_free": 180_000,
    "uptime": 3600, "firmware_version": "2.4.16", "public_key": "ab" * 32,
    "event_id_space_low": False,
}


def _health_sensor():
    return _entity(sensor_platform.SecuraCVCanaryHealthSensor)


def _flag_sensor():
    return _entity(bs_platform.SecuraCVCanaryEventIdSpaceLowSensor)


# ─── the pure helpers ──────────────────────────────────────────────────


def test_egress_counters_keep_the_named_counts_only() -> None:
    assert egress_counters(EGRESS) == EGRESS
    assert egress_counters({}) == {}
    for junk in (None, 5, "x", [1, 2], True):
        assert egress_counters(junk) is None
    # A value that is not a non-negative integer is not a count; a key the
    # integration does not name is not shown.
    odd = {"dropped": -1, "held_dropped": True, "ambient_dropped": "3", "unsent_dropped": 2.0,
           "future_counter": 9, "planner": {"live": 5, "held": None, "extra": 1}}
    assert egress_counters(odd) == {"planner": {"live": 5}}
    assert egress_counters({"dropped": 0, "planner": "junk"}) == {"dropped": 0}


def test_offline_queue_counters() -> None:
    assert offline_queue_counters(QUEUE) == QUEUE
    assert offline_queue_counters({"dropped_overflow": 3, "other": 1}) == {"dropped_overflow": 3}
    assert offline_queue_counters(None) is None
    assert offline_queue_counters([3]) is None


def test_event_id_space_low_reads_only_a_boolean() -> None:
    assert event_id_space_low({"event_id_space_low": True}) is True
    assert event_id_space_low({"event_id_space_low": False}) is False
    for health in ({}, {"event_id_space_low": "true"}, {"event_id_space_low": 1},
                   {"event_id_space_low": None}, None, "junk"):
        assert event_id_space_low(health) is None, health


# ─── the health sensor's attributes ────────────────────────────────────


def test_canary_health_carries_its_egress_and_queue_counters() -> None:
    sensor = _health_sensor()
    sensor._handle_message(_msg(json.dumps(CANARY_HEALTH)))
    attrs = sensor._attr_extra_state_attributes
    assert attrs["csi_event_egress"] == EGRESS
    assert attrs["offline_queue"] == QUEUE
    assert sensor._attr_native_value == "healthy"
    # A losing canary is visible without the bench: the two counters HA24 names.
    losing = dict(CANARY_HEALTH, csi_event_egress=dict(EGRESS, unsent_dropped=40),
                  offline_queue=dict(QUEUE, dropped_overflow=7))
    sensor._handle_message(_msg(json.dumps(losing)))
    attrs = sensor._attr_extra_state_attributes
    assert attrs["csi_event_egress"]["unsent_dropped"] == 40
    assert attrs["offline_queue"]["dropped_overflow"] == 7


def test_health_without_counters_shows_none() -> None:
    """Firmware that predates the counters: no attribute, rather than zeros
    that would claim nothing was lost."""
    sensor = _health_sensor()
    sensor._handle_message(_msg(json.dumps({"uptime": 5, "free_heap": 180_000})))
    attrs = sensor._attr_extra_state_attributes
    assert "csi_event_egress" not in attrs and "offline_queue" not in attrs


def test_wap_egress_topic_reaches_the_health_attributes_in_either_order() -> None:
    # The retained egress publish first (a hub that starts after the WAP).
    sensor = _health_sensor()
    sensor._handle_egress_message(_msg(json.dumps(EGRESS)))
    assert sensor._attr_extra_state_attributes["csi_event_egress"] == EGRESS
    assert sensor._attr_extra_state_attributes["trust_reason"] == "unsigned"
    sensor._handle_message(_msg(json.dumps(WAP_HEALTH)))
    attrs = sensor._attr_extra_state_attributes
    assert attrs["csi_event_egress"] == EGRESS, "a health rebuild dropped the WAP's counters"
    assert "offline_queue" not in attrs   # the WAP has no MQTT offline queue
    assert attrs["uptime_seconds"] == 3600
    # Health first, then a newer egress publish: merged, health kept.
    sensor = _health_sensor()
    sensor._handle_message(_msg(json.dumps(WAP_HEALTH)))
    newer = dict(EGRESS, held_dropped=9)
    sensor._handle_egress_message(_msg(json.dumps(newer)))
    attrs = sensor._attr_extra_state_attributes
    assert attrs["csi_event_egress"]["held_dropped"] == 9
    assert attrs["uptime_seconds"] == 3600 and attrs["firmware_version"] == "2.4.16"
    # Junk on the egress topic keeps the last counters.
    writes = len(sensor.writes)
    for junk in ("{not json", "[1,2]", "5"):
        sensor._handle_egress_message(_msg(junk))
    assert len(sensor.writes) == writes
    assert sensor._attr_extra_state_attributes["csi_event_egress"]["held_dropped"] == 9


def test_health_sensor_subscribes_to_the_egress_topic(monkeypatch) -> None:
    topics: list[str] = []

    async def subscribe(hass, topic, callback):
        topics.append(topic)
        return lambda: None

    monkeypatch.setattr(sensor_platform.mqtt, "async_subscribe", subscribe)
    sensor = _health_sensor()
    run(sensor.async_added_to_hass())
    assert topics == [f"securacv/canary01/{TOPIC_HEALTH}", f"securacv/canary01/{TOPIC_EGRESS}"]


# ─── the Event ID Space Low binary sensor ──────────────────────────────


def test_event_id_space_low_sensor_follows_the_flag() -> None:
    sensor = _flag_sensor()
    assert sensor._attr_is_on is None
    sensor._handle_message(_msg(json.dumps(dict(WAP_HEALTH, event_id_space_low=False))))
    assert sensor._attr_is_on is False
    sensor._handle_message(_msg(json.dumps(dict(WAP_HEALTH, event_id_space_low=True))))
    assert sensor._attr_is_on is True
    assert sensor._attr_extra_state_attributes["verified"] is False
    assert sensor._attr_extra_state_attributes["trust_reason"] == "unsigned"
    # A health publish without the flag says nothing about the space.
    sensor._handle_message(_msg(json.dumps({"uptime": 1})))
    assert sensor._attr_is_on is None
    # Junk is ignored.
    writes = len(sensor.writes)
    sensor._handle_message(_msg("{not json"))
    assert len(sensor.writes) == writes and sensor._attr_is_on is None


def test_event_id_space_low_sensor_is_a_diagnostic_problem() -> None:
    sensor = _flag_sensor()
    assert sensor._attr_device_class == bs_platform.BinarySensorDeviceClass.PROBLEM
    assert sensor._attr_entity_category == bs_platform.EntityCategory.DIAGNOSTIC
    assert sensor._attr_unique_id == f"{DOMAIN}_canary_canary01_event_id_space_low"
    assert sensor._attr_translation_key == "event_id_space_low"


def _discovered(monkeypatch, payloads: list[tuple[str, str]]) -> list:
    """Drive the binary-sensor discovery callback with (topic, payload)s and
    return the entities it created."""
    callbacks = {}

    async def subscribe(hass, topic, callback):
        callbacks[topic] = callback
        return lambda: None

    monkeypatch.setattr(bs_platform.mqtt, "async_subscribe", subscribe)
    created: list = []
    hass = types.SimpleNamespace(data={DOMAIN: {"e1": {"unsub_mqtt": []}}})
    run(bs_platform._setup_mqtt_binary_sensors(
        hass, types.SimpleNamespace(entry_id="e1"), "securacv", created.extend))
    for topic, payload in payloads:
        suffix = topic.rsplit("/", 1)[1]
        callbacks[f"securacv/+/{suffix}"](types.SimpleNamespace(topic=topic, payload=payload))
    return created


def _flag_sensors(entities: list) -> list:
    return [e for e in entities if isinstance(e, bs_platform.SecuraCVCanaryEventIdSpaceLowSensor)]


def test_discovery_creates_the_flag_sensor_once_for_a_device_that_sends_it(monkeypatch) -> None:
    created = _discovered(monkeypatch, [
        ("securacv/canary01/health", json.dumps(WAP_HEALTH)),
        ("securacv/canary01/health", json.dumps(WAP_HEALTH)),
    ])
    assert len(_flag_sensors(created)) == 1


def test_discovery_creates_no_flag_sensor_for_a_device_without_the_flag(monkeypatch) -> None:
    """A device whose health never carries the flag (canary-sense, older
    firmware) does not grow a sensor that would stay unknown; one that starts
    sending it gets one then."""
    created = _discovered(monkeypatch, [
        ("securacv/sense01/health", json.dumps({"uptime": 5})),
        ("securacv/sense01/health", "{not json"),
    ])
    assert not _flag_sensors(created)
    created = _discovered(monkeypatch, [
        ("securacv/canary02/health", json.dumps({"uptime": 5})),
        ("securacv/canary02/health", json.dumps(dict(WAP_HEALTH, event_id_space_low=True))),
    ])
    assert len(_flag_sensors(created)) == 1


# ─── held to the firmware (monorepo only) ──────────────────────────────


def _struct_fields(text: str, opener: str) -> list[str]:
    m = re.search(re.escape(opener) + r"\s*\{(.*?)\n\};", text, re.S)
    assert m, f"{opener} moved; update this test"
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    return re.findall(r"^\s*[\w:]+\s+(\w+)\s*;", body, re.M)


def _wap_stats_json_keys() -> tuple[list[str], list[str]]:
    """The keys csi_event_egress::stats_json() spells, top level and planner."""
    text = _WAP_EGRESS_H.read_text(encoding="utf-8")
    m = re.search(r"inline size_t stats_json\(.*?snprintf\(out, cap,(.*?)\);\n", text, re.S)
    assert m, "csi_event_egress.h stats_json() moved; update this test"
    fmt = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1).split("(unsigned long)")[0]))
    keys = re.findall(r'\\"(\w+)\\":', fmt)
    split = keys.index("planner")
    return keys[:split], keys[split + 1:]


@monorepo_only
def test_counter_names_are_the_firmware_structs() -> None:
    wap = _struct_fields(_WAP_EGRESS_H.read_text(encoding="utf-8"), "struct Stats")
    assert tuple(f for f in wap if f != "planner") == EGRESS_COUNTERS
    planner = _struct_fields(_BACKFILL_H.read_text(encoding="utf-8"), "struct Stats")
    assert tuple(planner) == EGRESS_PLANNER_COUNTERS
    queue = _struct_fields(_OFFLINE_QUEUE_H.read_text(encoding="utf-8"), "struct Stats")
    assert tuple(f for f in queue if f.startswith("dropped_")) == OFFLINE_QUEUE_COUNTERS


@monorepo_only
def test_wap_egress_topic_body_is_read_whole() -> None:
    """A body with every key the canary-wap's stats_json() spells reaches the
    attribute whole: no counter the WAP sends is dropped."""
    top, planner = _wap_stats_json_keys()
    assert tuple(top) == EGRESS_COUNTERS and tuple(planner) == EGRESS_PLANNER_COUNTERS
    body = {k: i for i, k in enumerate(top, start=100)}
    body["planner"] = {k: i for i, k in enumerate(planner, start=200)}
    sensor = _health_sensor()
    sensor._handle_egress_message(_msg(json.dumps(body)))
    assert sensor._attr_extra_state_attributes["csi_event_egress"] == body
    mqtt = _WAP_MQTT.read_text(encoding="utf-8")
    pub = re.search(r"\nvoid publish_egress\(\)\s*\{(.*?)\n\}\n", mqtt, re.S)
    assert pub, "csi_mqtt.cpp publish_egress() moved; update this test"
    assert f'build_topic(topic, sizeof(topic), "{TOPIC_EGRESS}");' in pub.group(1)
    assert "csi_event_egress::stats_json(csi_event_egress::stats()" in pub.group(1)


@monorepo_only
def test_canary_health_objects_are_read_whole() -> None:
    """The canary base's health objects, keyed as main.cpp fills them, reach
    the attributes whole."""
    main = _CANARY_MAIN.read_text(encoding="utf-8")
    ego = re.findall(r'\bego\["([a-z0-9_]+)"\]\s*=\s*st\.', main)
    plo = re.findall(r'\bplo\["([a-z0-9_]+)"\]\s*=', main)
    oqo = re.findall(r'\boqo\["([a-z0-9_]+)"\]\s*=', main)
    assert ego and plo and oqo, "main.cpp's health objects moved; update this test"
    egress = {k: 1 for k in ego}
    egress["planner"] = {k: 2 for k in plo}
    queue = {k: 3 for k in oqo}
    health = dict(CANARY_HEALTH, csi_event_egress=egress, offline_queue=queue)
    sensor = _health_sensor()
    sensor._handle_message(_msg(json.dumps(health)))
    assert sensor._attr_extra_state_attributes["csi_event_egress"] == egress
    assert sensor._attr_extra_state_attributes["offline_queue"] == queue


@monorepo_only
def test_both_devices_send_the_flag_the_sensor_reads() -> None:
    main = _CANARY_MAIN.read_text(encoding="utf-8")
    assert 'doc["event_id_space_low"] = csi_event_egress_id_space_low();' in main
    mqtt = _WAP_MQTT.read_text(encoding="utf-8")
    assert '",\\"event_id_space_low\\":%s"' in mqtt
