"""Online detection — one rule, two surfaces.

canary-wap only ever publishes JSON with an ``online`` boolean on its
status topic: ``{"online":true}`` on connect and inside its status body,
``{"online":false}`` as its last will
(firmware/projects/canary-wap/arduino/canary_wap/csi_mqtt.cpp), and its own
HA discovery template reads ``value_json.online``. The two regressions this
file pins down:

  - the Online binary sensor parsed no JSON at all — it matched the raw
    payload against a tuple of bare words — so a live, publishing WAP
    rendered permanently OFF;
  - ``voice._status_online`` parsed JSON but only ever read ``status`` /
    ``state``, so speech reported the same healthy WAP as "not reporting as
    online" and left it out of "watching tonight" — a false *not* watching
    answer, which this project treats as its worst failure.

They also disagreed with each other on ``{"status":"online"}`` (voice
accepted it, the sensor did not), contradicting voice.py's own claim that
the vocabulary is mirrored "so speech and the dashboard can never disagree
about what 'online' means". Both now go through ``voice.status_online`` /
``voice.json_status_online``, and every case below is asserted on BOTH
surfaces so they cannot drift apart again.
"""

from __future__ import annotations

import json
import types
from pathlib import Path

from . import conftest  # noqa: F401  (installs the base HA stubs)
from .conftest import make_entry

# Reuse the platform stubs the hardening tests install (idempotent).
from .test_mqtt_payload_hardening import _install_platform_stubs  # noqa: E402

_install_platform_stubs()

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from .. import MAX_MQTT_PAYLOAD_BYTES, _async_device_status_received  # noqa: E402
from .. import binary_sensor as bs_platform  # noqa: E402
from ..const import CONF_ENABLE_MQTT, CONF_MQTT_PREFIX, CONF_SETUP_MODE, DOMAIN, SETUP_MODE_MQTT  # noqa: E402
from ..voice import (  # noqa: E402
    fleet_brief,
    json_status_online,
    speak_device_check,
    status_online,
    status_verdict,
)

# The flagship's (firmware/canary) periodic status, as it publishes it.
FLAGSHIP_STATUS = json.dumps(
    json.loads(
        (Path(__file__).parent / "fixtures" / "canary_mqtt_payloads.json").read_text(encoding="utf-8")
    )["families"]["canary"]["status"]
)

ENTRY = types.SimpleNamespace(entry_id="e1")

ONLINE_PAYLOADS = [
    # The canary-wap dialect — the whole point of this file.
    '{"online": true}',
    '{"online":true,"csi_running":true,"heap_free":102400}',
    '{"online": "true"}',
    '{"online": "1"}',
    # The status/state dialect voice already accepted.
    '{"status": "online"}',
    '{"state": "connected"}',
    # The bare-word dialect the sensor already accepted.
    "online",
    "1",
    "true",
    "connected",
    "  ONLINE  ",
]

OFFLINE_PAYLOADS = [
    # The WAP's last will.
    '{"online": false}',
    '{"online": "false"}',
    # An explicit boolean is authoritative: never spoken as up on a
    # payload that says it is down, whatever else it carries.
    '{"online": false, "status": "online"}',
    '{"status": "offline"}',
    '{"state": "disconnected"}',
    "{}",
    "offline",
    "",
    "   ",
    "{not json",
    "5",
    '["online"]',
]


# A status that says neither: no ``online`` key and no word from either
# vocabulary. The flagship's is the one that matters (its ``state`` is its
# witness state); the rest is junk a broker can carry.
NO_VERDICT_PAYLOADS = [
    FLAGSHIP_STATUS,
    '{"state": "NOFIX"}',
    '{"state": "STAT", "uptime": 30}',
    '{"status": "MOVE"}',
    '{"online": "maybe"}',
    '{"online": null, "state": "LOST"}',
    "{}",
    "",
    "   ",
    "{not json",
    "5",
    '["online"]',
    "booting",
]

# What says offline in so many words.
EXPLICIT_OFFLINE_PAYLOADS = [
    '{"online": false}',
    '{"online": "false"}',
    '{"online": false, "status": "online"}',
    '{"online": "0"}',
    '{"status": "offline"}',
    '{"state": "disconnected"}',
    "offline",
    "0",
    "false",
    "disconnected",
    "  OFFLINE  ",
]


def _msg(payload) -> types.SimpleNamespace:
    return types.SimpleNamespace(payload=payload)


def _hass() -> HomeAssistant:
    hass = HomeAssistant()
    hass.data = {DOMAIN: {"e1": {}}}
    return hass


def _sensor():
    inst = bs_platform.SecuraCVCanaryOnlineSensor("securacv", "canary01", ENTRY)
    inst.hass = _hass()
    inst.writes = []
    inst.async_write_ha_state = lambda: inst.writes.append(True)
    return inst


def _sensor_verdict(payload) -> bool:
    inst = _sensor()
    inst._handle_message(_msg(payload))
    return bool(inst._attr_is_on)


# ─── the two surfaces, on the same payloads ───────────────────────────


def test_both_surfaces_call_every_online_dialect_online() -> None:
    for payload in ONLINE_PAYLOADS:
        assert status_online(payload) is True, f"voice: {payload}"
        assert _sensor_verdict(payload) is True, f"binary_sensor: {payload}"


def test_both_surfaces_call_every_offline_dialect_offline() -> None:
    for payload in OFFLINE_PAYLOADS:
        assert status_online(payload) is False, f"voice: {payload}"
        assert _sensor_verdict(payload) is False, f"binary_sensor: {payload}"


def test_the_wap_lifecycle_moves_the_sensor_both_ways() -> None:
    """Connect, publish status, then the broker delivers the LWT."""
    inst = _sensor()
    inst._handle_message(_msg('{"online":true}'))
    assert inst._attr_is_on is True
    inst._handle_message(_msg(b'{"online":true,"csi_running":false}'))
    assert inst._attr_is_on is True
    inst._handle_message(_msg('{"online":false}'))
    assert inst._attr_is_on is False
    assert inst.writes == [True, True, True]


# ─── the hostile-payload guarantees the shared gate already made ──────


def test_junk_never_moves_the_sensor_state() -> None:
    """An over-cap or undecodable payload leaves the last known state
    alone rather than silently reporting the device down."""
    oversize = '{"online": true, "pad": "' + "x" * MAX_MQTT_PAYLOAD_BYTES + '"}'
    inst = _sensor()
    inst._handle_message(_msg('{"online": true}'))
    assert inst._attr_is_on is True
    for payload in (oversize, oversize.encode(), b"\xff\xfe{\x00"):
        inst._handle_message(_msg(payload))
        assert inst._attr_is_on is True, payload


# ─── and the spoken answer follows ────────────────────────────────────


def test_speech_reports_a_wap_dialect_device_as_online() -> None:
    """Before the fix this said "not reporting as online right now" about a
    device that was publishing perfectly well."""
    devices = {
        "cv-a1b2c3": {"status": '{"online": true}'},
        "cv-d4e5f6": {"status": '{"online": false}'},
    }
    brief = fleet_brief([{"devices": devices, "verify": {}, "kernel": None}], 1_000_000.0)

    assert brief["online"] == ["cv-a1b2c3"]
    assert speak_device_check(brief, "cv-a1b2c3").startswith("The cv a1b2c3 Canary is online")
    assert "not reporting as online" in speak_device_check(brief, "cv-d4e5f6")


# ─── a status that names no liveness moves no verdict ─────────────────


def test_the_verdict_is_three_valued() -> None:
    """Online word → True, offline word → False, neither → None. Speech
    keeps its contract: "don't know" is never spoken as online."""
    for payload in ONLINE_PAYLOADS:
        assert status_verdict(payload) is True, payload
    for payload in EXPLICIT_OFFLINE_PAYLOADS:
        assert status_verdict(payload) is False, payload
        assert status_online(payload) is False, payload
    for payload in NO_VERDICT_PAYLOADS:
        assert status_verdict(payload) is None, payload
        assert status_online(payload) is False, payload
    assert status_verdict(None) is None and status_verdict(7) is None
    assert json_status_online(json.loads(FLAGSHIP_STATUS)) is None
    assert json_status_online({"online": True, "state": "NOFIX"}) is True
    assert json_status_online({"online": False, "state": "NOFIX"}) is False
    # Nesting past the interpreter's limit is unreadable, not a traceback.
    assert status_verdict('{"a":' + "[" * 20000 + "]" * 20000 + "}") is None


def test_the_online_sensor_ignores_a_status_that_names_no_liveness() -> None:
    """Written as off, the flagship's status turned this sensor off thirty
    seconds after every connect. No verdict writes nothing, either way."""
    inst = _sensor()
    inst._handle_message(_msg('{"online": true}'))
    assert inst._attr_is_on is True and inst.writes == [True]
    for payload in NO_VERDICT_PAYLOADS:
        inst._handle_message(_msg(payload))
        inst._handle_message(_msg(payload.encode()))
        assert inst._attr_is_on is True, payload
    assert inst.writes == [True]
    inst._handle_message(_msg('{"online": false}'))
    assert inst._attr_is_on is False
    for payload in NO_VERDICT_PAYLOADS:
        inst._handle_message(_msg(payload))
        assert inst._attr_is_on is False, payload
    assert inst.writes == [True, True]


def _flagship():
    """An entry's own status handler and the flagship's Online sensor."""
    hass = HomeAssistant()
    hass.data = {}
    entry = make_entry(hass, data={
        CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv", CONF_SETUP_MODE: SETUP_MODE_MQTT,
    })
    handler = _async_device_status_received(hass, entry)

    def publish(topic: str, payload: str) -> None:
        handler(types.SimpleNamespace(topic=f"securacv/flag01/{topic}", payload=payload))

    sensor = bs_platform.SecuraCVCanaryOnlineSensor("securacv", "flag01", entry)
    sensor.hass = hass
    sensor.writes = []
    sensor.async_write_ha_state = lambda: sensor.writes.append(True)
    return entry, publish, sensor


def _spoken_online(entry) -> list[str]:
    snapshot = [{"devices": entry.runtime_data.devices, "verify": {}, "kernel": None}]
    return fleet_brief(snapshot, 1_000_000.0)["online"]


def test_a_flagship_stays_online_through_its_periodic_status() -> None:
    """firmware/canary says it is up on its availability topic (retained
    ``online``, ``offline`` as its last will) and publishes its witness
    state on status every 30 s: ``{"state":"NOFIX",...}``, with no liveness
    word in it. That status was read as offline: thirty seconds after each
    connect the device, every entity on it and its Online sensor went down,
    and speech listed it as offline, until the next reconnect."""
    entry, publish, sensor = _flagship()
    publish("availability", "online")
    record = entry.runtime_data.devices["flag01"]
    assert record["online"] is True and sensor.is_on is True

    for _ in range(3):
        publish("status", FLAGSHIP_STATUS)               # the entry's wildcard
        sensor._handle_message(_msg(FLAGSHIP_STATUS))    # the sensor's own subscription
        assert record["online"] is True, "the status moved the device's liveness"
        assert sensor.is_on is True
        assert _spoken_online(entry) == ["flag01"]
    assert sensor.writes == [], "no verdict, no write"
    assert record["registered"] is True and record["status"] == FLAGSHIP_STATUS

    # Its last will still takes it down, and a status still in flight does
    # not bring it back.
    publish("availability", "offline")
    assert record["online"] is False and sensor.is_on is False
    assert _spoken_online(entry) == []
    publish("status", FLAGSHIP_STATUS)
    sensor._handle_message(_msg(FLAGSHIP_STATUS))
    assert record["online"] is False and sensor.is_on is False
    publish("availability", "online")
    assert record["online"] is True and sensor.is_on is True


def test_a_flagship_heard_on_status_first_is_not_called_offline() -> None:
    """Retained topics arrive in any order. Status first: the device is
    registered with no liveness verdict (its entities are available, the
    Online sensor has nothing to say on yet), and the availability word
    that follows is the verdict."""
    entry, publish, sensor = _flagship()
    publish("status", FLAGSHIP_STATUS)
    record = entry.runtime_data.devices["flag01"]
    assert record.get("online") is None, "no verdict is not offline"
    assert record["registered"] is True
    assert _spoken_online(entry) == [], "and it is never spoken as online on no word"
    publish("availability", "online")
    assert record["online"] is True and sensor.is_on is True
    assert _spoken_online(entry) == ["flag01"]


def test_the_status_dialects_still_move_the_entrys_verdict() -> None:
    """The families whose status does carry their liveness (the canary-wap's
    ``online`` boolean, the others' ``status`` word and its last will) are
    read as before."""
    entry, publish, sensor = _flagship()
    record = entry.runtime_data.devices.setdefault("flag01", {"status": None})
    for payload, expected in (
        ('{"online": true}', True),
        ('{"online": false}', False),
        ('{"status": "online", "device_type": "canary-sense"}', True),
        ('{"status": "offline"}', False),
        ("online", True),
        ("offline", False),
    ):
        publish("status", payload)
        assert record["online"] is expected, payload
        assert sensor.is_on is expected, payload
        assert _spoken_online(entry) == (["flag01"] if expected else []), payload
