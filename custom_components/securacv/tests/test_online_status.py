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

import types

from . import conftest  # noqa: F401  (installs the base HA stubs)

# Reuse the platform stubs the hardening tests install (idempotent).
from .test_mqtt_payload_hardening import _install_platform_stubs  # noqa: E402

_install_platform_stubs()

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from .. import MAX_MQTT_PAYLOAD_BYTES  # noqa: E402
from .. import binary_sensor as bs_platform  # noqa: E402
from ..const import DOMAIN  # noqa: E402
from ..voice import fleet_brief, speak_device_check, status_online  # noqa: E402

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
