"""A canary-wap health publish must not re-clear its own tamper sensors (F41).

HA's per-type Enclosure Open and SD Removed sensors are set by the tamper
topic (the WAP's per-kind bridge publishes ``{"type": "enclosure"}`` /
``{"type": "sd_remove"}`` when its ``system.integrity`` module commits one)
and then follow the health topic: every health publish sets them from
``enclosure_open`` / ``sd_mounted`` (binary_sensor.py), and an absent key
reads as "closed" / "mounted" — the canary base relies on that for a
card-less boot. canary-wap's MQTT health (``csi_mqtt::publish_health``)
carried heap, uptime and the battery only, so the next health publish after
a lid opening or a pulled card switched the sensor back off while the lid
was still open and the card still out. The WAP's health now carries both
fields, in the canary base's spelling and with its rules: ``sd_mounted`` only
once a card has mounted this boot, ``enclosure_open`` only on builds with the
contact (``FEATURE_TAMPER_GPIO``).

The behavior tests drive the sensor with a health body built from the keys
the WAP firmware's publish_health can actually emit, read from its source —
so they fail if the firmware stops spelling a field, not just if HA changes.
The HACS mirror carries no firmware source, so they skip there.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

from .test_mqtt_payload_hardening import _entity, _msg

from .. import binary_sensor as bs_platform
from ..const import TAMPER_ENCLOSURE, TAMPER_SD_REMOVE

_WAP_MQTT = (
    Path(__file__).resolve().parents[3]
    / "firmware" / "projects" / "canary-wap" / "arduino" / "canary_wap" / "csi_mqtt.cpp"
)
_WAP_INO = _WAP_MQTT.with_name("canary_wap.ino")

monorepo_only = pytest.mark.skipif(
    not _WAP_MQTT.is_file(),
    reason="monorepo-only: the HACS mirror carries no firmware source",
)


def _publish_health_body() -> str:
    text = _WAP_MQTT.read_text(encoding="utf-8")
    m = re.search(r"\nvoid publish_health\([^)]*\)\s*\{(.*?)\n\}\n", text, re.S)
    assert m, "csi_mqtt.cpp publish_health() moved; update this test"
    return m.group(1)


def _wap_health_keys() -> set[str]:
    """Every JSON key publish_health() can put in the health body."""
    return set(re.findall(r'\\"([a-z_]+)\\":', _publish_health_body()))


def _wap_health(*, lid_open: bool, card_in: bool) -> str:
    """A WAP health body carrying exactly the keys its firmware emits."""
    sample: dict[str, object] = {
        "battery": 100,
        "battery_present": False,
        "charge_state": "full",
        "battery_health_pct": 100,
        "battery_mv": 4100,
        "memory_free": 180_000,
        "uptime": 3600,
        "firmware_version": "2.0.0",
        "public_key": "ab" * 32,
        "sd_mounted": card_in,
        "enclosure_open": lid_open,
    }
    keys = _wap_health_keys()
    unknown = keys - sample.keys()
    assert not unknown, f"publish_health() emits keys this test has no sample for: {unknown}"
    return json.dumps({k: v for k, v in sample.items() if k in keys})


def _sensor(kind: str, icon: str):
    return _entity(bs_platform.SecuraCVCanaryTamperTypeSensor, kind, icon)


def _tamper(kind: str) -> str:
    # csi_event_wire::build_tamper_bridge_body — the WAP's exact tamper body.
    return json.dumps({"type": kind, "severity": "tamper"})


@monorepo_only
def test_wap_health_publisher_spells_the_tamper_levels() -> None:
    keys = _wap_health_keys()
    assert "enclosure_open" in keys, (
        "canary-wap's MQTT health does not carry enclosure_open — every health "
        "publish re-clears HA's Enclosure Open sensor while the lid is still open"
    )
    assert "sd_mounted" in keys, (
        "canary-wap's MQTT health does not carry sd_mounted — every health "
        "publish re-clears HA's SD Removed sensor while the card is still out"
    )


@monorepo_only
def test_open_lid_survives_the_next_wap_health_publish() -> None:
    sensor = _sensor(TAMPER_ENCLOSURE, "mdi:package-variant-closed-remove")
    sensor._handle_tamper_message(_msg(_tamper("enclosure")))
    assert sensor._attr_is_on is True
    # The lid is still off when the next health publish lands.
    sensor._handle_health_message(_msg(_wap_health(lid_open=True, card_in=True)))
    assert sensor._attr_is_on is True, "a WAP health publish re-cleared Enclosure Open"
    # Closing the lid is what clears it.
    sensor._handle_health_message(_msg(_wap_health(lid_open=False, card_in=True)))
    assert sensor._attr_is_on is False


@monorepo_only
def test_pulled_card_survives_the_next_wap_health_publish() -> None:
    sensor = _sensor(TAMPER_SD_REMOVE, "mdi:sd-off")
    sensor._handle_tamper_message(_msg(_tamper("sd_remove")))
    assert sensor._attr_is_on is True
    sensor._handle_health_message(_msg(_wap_health(lid_open=False, card_in=False)))
    assert sensor._attr_is_on is True, "a WAP health publish re-cleared SD Removed"
    sensor._handle_health_message(_msg(_wap_health(lid_open=False, card_in=True)))
    assert sensor._attr_is_on is False


@monorepo_only
def test_wap_sends_the_levels_under_the_canary_base_rules() -> None:
    """sd_mounted only once a card has mounted this boot (a card-less boot is
    a configuration, not a removal — the key is omitted and reads as mounted),
    from the three-state so a failing card is not "removed"; enclosure_open
    only on builds with the contact, from the debounced, adopted state."""
    ino = _WAP_INO.read_text(encoding="utf-8")
    m = re.search(r"csi_mqtt::MqttTamperLevels\s+\w+\s*=?[^;]*;(.*?)csi_mqtt::publish_health\(", ino, re.S)
    assert m, "canary_wap.ino no longer builds MqttTamperLevels for publish_health; update this test"
    feed = m.group(1)
    assert "g_sd_mounted_this_boot" in feed
    assert "g_hw.sd_state != SD_ABSENT" in feed
    assert "#if FEATURE_TAMPER_GPIO" in feed
    assert "g_tamper_contact.adopted && g_tamper_contact.open" in feed


def test_absent_level_keys_read_as_clear() -> None:
    """The HA contract the firmware is held to: a health body without the key
    clears the sensor (the canary base's card-less boot depends on it). That
    is why the WAP must carry the level, not merely the edge."""
    enclosure = _sensor(TAMPER_ENCLOSURE, "mdi:package-variant-closed-remove")
    enclosure._handle_tamper_message(_msg(_tamper("enclosure")))
    enclosure._handle_health_message(_msg('{"battery": 100, "memory_free": 180000}'))
    assert enclosure._attr_is_on is False
    removed = _sensor(TAMPER_SD_REMOVE, "mdi:sd-off")
    removed._handle_tamper_message(_msg(_tamper("sd_remove")))
    removed._handle_health_message(_msg('{"battery": 100, "memory_free": 180000}'))
    assert removed._attr_is_on is False
