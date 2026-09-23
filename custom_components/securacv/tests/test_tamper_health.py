"""The canary base's SD story reaches Home Assistant (backlog F25).

HA's SD Removed sensor reads ``sd_mounted`` from the health payload
(binary_sensor.py's per-type tamper sensor). The canary PIO tree's MQTT
health publish never carried that key — only ``sd_healthy`` / ``sd_errors``
— so the sensor could not fire for a canary base whatever happened to its
card. The firmware now sends ``sd_mounted`` once a card has mounted this
boot (a card-less boot is a configuration, not a removal, and an absent key
reads as mounted here).

These tests pin the field name and the three shapes the canary publishes.
In the monorepo, where the firmware source sits next to the integration,
they also prove the canary's health publisher still spells the key; the
HACS mirror carries no firmware, so that one test skips there.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from .test_mqtt_payload_hardening import _entity, _msg

from .. import binary_sensor as bs_platform
from ..const import TAMPER_SD_ERROR, TAMPER_SD_REMOVE

_CANARY_MAIN = Path(__file__).resolve().parents[3] / "firmware" / "canary" / "src" / "main.cpp"


def _sd_removed():
    return _entity(
        bs_platform.SecuraCVCanaryTamperTypeSensor,
        TAMPER_SD_REMOVE, "SD Removed", "mdi:sd-off",
    )


def _sd_error():
    return _entity(
        bs_platform.SecuraCVCanaryTamperTypeSensor,
        TAMPER_SD_ERROR, "SD Error", "mdi:alert-circle",
    )


def test_canary_health_with_a_pulled_card_lights_sd_removed() -> None:
    """The canary's health shape after its card left: sd_mounted false."""
    removed, error = _sd_removed(), _sd_error()
    payload = (
        '{"sd_healthy": false, "sd_writes": 42, "sd_errors": 0, '
        '"sd_mounted": false, "boot_count": 3}'
    )
    removed._handle_health_message(_msg(payload))
    error._handle_health_message(_msg(payload))
    assert removed._attr_is_on is True
    assert error._attr_is_on is False  # removal is not a write failure


def test_canary_health_with_the_card_back_clears_sd_removed() -> None:
    removed = _sd_removed()
    removed._handle_health_message(_msg('{"sd_healthy": false, "sd_mounted": false}'))
    assert removed._attr_is_on is True
    removed._handle_health_message(_msg('{"sd_healthy": true, "sd_mounted": true}'))
    assert removed._attr_is_on is False


def test_card_less_canary_boot_omits_the_key_and_stays_quiet() -> None:
    """No card has mounted this boot: the firmware sends no sd_mounted, and
    the sensor must not call a canary without a card 'removed'."""
    removed = _sd_removed()
    removed._handle_health_message(_msg('{"sd_healthy": false, "sd_writes": 0, "sd_errors": 0}'))
    assert removed._attr_is_on is False


@pytest.mark.skipif(
    not _CANARY_MAIN.is_file(),
    reason="monorepo-only: the HACS mirror carries no firmware source",
)
def test_canary_health_publisher_spells_the_key_the_sensor_reads() -> None:
    text = _CANARY_MAIN.read_text(encoding="utf-8")
    m = re.search(r"static void mqtt_publish_health_update\(\)\s*\{(.*?)\n\}\n", text, re.S)
    assert m, "mqtt_publish_health_update() moved; update this test"
    body = m.group(1)
    assert 'doc["sd_mounted"]' in body, (
        "the canary's MQTT health payload no longer carries sd_mounted — "
        "HA's SD Removed sensor cannot fire for a canary base without it"
    )
    # Sent only once a card has mounted this boot (adopt-silently rule).
    assert "storage_mount_generation() > 0" in body
