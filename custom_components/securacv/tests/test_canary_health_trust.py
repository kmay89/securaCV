"""The canary base's health publish carries the key Home Assistant pins.

Backlog F29 made the canary PIO tree sign its ``events`` bodies with the
witness Ed25519 key, but its MQTT health payload carried no ``public_key``.
HA's trust-on-first-use pin (``__init__.py`` ``_async_health_for_tofu``)
reads exactly that key from the health topic, so every canary events body
landed as ``no_pubkey`` / unverified until someone pinned the key by hand.
The firmware now sends it in the canary-wap's shape (64 lowercase hex).

These tests pin:

  - end to end with no manual pin: a canary-shaped health publish pins the
    key, and a canary-shaped signed events body then verifies;
  - in the monorepo, that ``mqtt_publish_health_update()`` still spells the
    key, and that it is the same identity ``csi_event_egress`` hands to the
    signer;
  - in the monorepo, that the largest health packet the canary can build
    still fits the MQTT client's buffer. PubSubClient refuses an oversize
    publish silently, and HA would then lose health, which is worse than
    losing the pin. A new health key fails this test until the worst case
    below learns its size.

The HACS mirror carries no firmware, so the monorepo-only tests skip there.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from types import SimpleNamespace

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .test_canary_events import DEVICE, TAMPER_ROW_GOLDEN, _sign_golden
from .test_tofu_health_hook import _setup

from ..device_trust import PIN_SOURCE_TOFU
from ..signature import verify_event

_FW = Path(__file__).resolve().parents[3] / "firmware"
_CANARY_MAIN = _FW / "canary" / "src" / "main.cpp"
_CANARY_EGRESS = _FW / "canary" / "src" / "csi_event_egress.cpp"
_CANARY_MQTT_H = _FW / "canary" / "lib" / "securacv_mqtt" / "src" / "securacv_mqtt.h"
_CANARY_MQTT_CPP = _FW / "canary" / "lib" / "securacv_mqtt" / "src" / "securacv_mqtt.cpp"

_monorepo_only = pytest.mark.skipif(
    not _CANARY_MAIN.is_file(),
    reason="monorepo-only: the HACS mirror carries no firmware source",
)


def _health_body() -> str:
    text = _CANARY_MAIN.read_text(encoding="utf-8")
    m = re.search(r"static void mqtt_publish_health_update\(\)\s*\{(.*?)\n\}\n", text, re.S)
    assert m, "mqtt_publish_health_update() moved; update this test"
    return m.group(1)


def test_canary_health_pins_the_key_its_events_verify_against() -> None:
    _hass, store, cb = _setup()
    priv = Ed25519PrivateKey.from_private_bytes(b"\x42" * 32)
    pub_hex = priv.public_key().public_bytes_raw().hex()
    # The canary's health shape (a subset of its fields), key included.
    health = {
        "uptime": 3600, "free_heap": 180000, "sd_healthy": True,
        "sd_mounted": True, "boot_count": 3, "firmware_version": "2.4.15",
        "public_key": pub_hex, "tamper_detected": False,
    }
    msg = SimpleNamespace(topic=f"securacv/{DEVICE}/health", payload=json.dumps(health))
    assert cb(msg) is None
    entry = store.get(DEVICE)
    assert entry is not None, "the canary's health publish did not pin its key"
    assert entry.pubkey_hex == pub_hex
    assert entry.pin_source == PIN_SOURCE_TOFU
    # No manual pin anywhere: the signed events body now verifies.
    payload = _sign_golden(TAMPER_ROW_GOLDEN, priv, store)
    verdict = verify_event(store, DEVICE, payload)
    assert verdict.trusted is True, verdict


def test_canary_health_without_a_key_pins_nothing() -> None:
    """The old canary shape, for contrast: nothing to pin, so the events
    body stays unverified (no_pubkey)."""
    _hass, store, cb = _setup()
    msg = SimpleNamespace(
        topic=f"securacv/{DEVICE}/health",
        payload=json.dumps({"uptime": 3600, "sd_healthy": True}),
    )
    cb(msg)
    assert store.get(DEVICE) is None
    # A signed-looking body; with no pin, verification stops at no_pubkey.
    fields = json.loads(TAMPER_ROW_GOLDEN.replace("@FP@", "0" * 16).replace("@SIG@", "x"))
    verdict = verify_event(store, DEVICE, fields)
    assert verdict.trusted is False
    assert verdict.reason == "no_pubkey"


@_monorepo_only
def test_canary_health_publisher_sends_the_witness_public_key() -> None:
    body = _health_body()
    assert 'doc["public_key"]' in body, (
        "the canary's MQTT health payload no longer carries public_key — "
        "Home Assistant cannot pin the key its signed events verify against"
    )
    # Built from the witness identity's 32-byte public key, lowercase hex.
    assert "device.pubkey[i]" in body
    assert '"0123456789abcdef"' in body
    assert "pk_hex[64]" in body
    # ...the same identity the events signer is initialized from.
    egress = _CANARY_EGRESS.read_text(encoding="utf-8")
    assert "device_signature::init(dev.privkey, dev.pubkey," in egress
    assert "witness_get_device()" in egress


# The largest value each health key can serialize to, as ArduinoJson writes
# it. Numbers are taken at their type's widest (u32 counters at 4294967295,
# the u64 lifetime at 20 digits, floats at 12 characters), strings at their
# longest spelling in the firmware. Deliberately pessimistic: this bounds
# the packet, it does not predict it.
_U32 = 4294967295
_FLOAT = -40.123456789
_WORST = {
    "uptime": _U32, "free_heap": _U32, "min_heap": _U32,
    "records_created": _U32, "records_verified": _U32, "verify_failures": _U32,
    "chain_persists": _U32, "chain_persist_failures": _U32,
    "gps_healthy": False, "crypto_healthy": False,
    "sd_healthy": False, "wifi_active": False, "http_requests": _U32,
    "sd_writes": _U32, "sd_errors": _U32, "sd_mounted": False,
    "boot_count": _U32, "firmware_version": "0" * 24,
    "public_key": "0" * 64, "tamper_detected": False, "enclosure_open": False,
    "power_loss_detected": False, "unexpected_reboot": False,
    "sd": {
        "mounted": False, "usage_pct": _U32, "writes": _U32, "errors": _U32,
        "lifetime_kb": 18446744073709551615, "wear_pct": _FLOAT,
        "replace_recommended": False,
    },
    "temp_c": _FLOAT, "battery_mv": _U32, "battery_soc": _U32,
    "battery_trend": -2147483648, "charge_cycles": _U32,
    "battery_present": False, "charge_state": "discharging",
    "battery_health_pct": _U32, "die_temp_c": -2147483648,
    "thermal_state": "throttled", "thermal_sensor_ok": False,
    "thermal_advisory": False, "thermal_throttled_min": _U32,
    "thermal_pause_events": _U32,
}


@_monorepo_only
def test_the_largest_canary_health_packet_fits_the_mqtt_buffer() -> None:
    body = _health_body()
    keys = set(re.findall(r'\bdoc\["([a-z0-9_]+)"\]', body))
    sd_keys = set(re.findall(r'\bsdo\["([a-z0-9_]+)"\]', body))
    assert keys == set(_WORST), (
        "the canary health payload's keys changed; give each new key its "
        f"worst-case value in _WORST (new: {sorted(keys - set(_WORST))}, "
        f"gone: {sorted(set(_WORST) - keys)})"
    )
    assert sd_keys == set(_WORST["sd"])
    payload = json.dumps(_WORST, separators=(",", ":"))
    # PubSubClient's packet: fixed header (5) + topic length (2) + topic +
    # payload, all in one buffer. Take the topic at its buffer's limit.
    topic_cap = int(
        re.search(r"static char s_topic_health\[(\d+)\];",
                  _CANARY_MQTT_CPP.read_text(encoding="utf-8")).group(1)
    )
    buf = int(
        re.search(r"#define MQTT_BUFFER_SIZE\s+(\d+)",
                  _CANARY_MQTT_H.read_text(encoding="utf-8")).group(1)
    )
    packet = 5 + 2 + (topic_cap - 1) + len(payload)
    assert packet <= buf, (
        f"worst-case health packet {packet} B overruns MQTT_BUFFER_SIZE {buf} B: "
        "PubSubClient would drop the publish silently"
    )
