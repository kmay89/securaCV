"""The Health, SD Wear and GPS sensors write only what the core can carry.

Home Assistant's JSON writer refuses an integer past 64 bits in a state
attribute (TypeError "Integer exceeds 64-bit range" from
``State.as_dict_json``), and a state that cannot be serialized fails the
recorder and every websocket subscription on the entity. Sweep HA57 held
the Health sensor's battery and memory keys to that; every other number
the three sensors show was copied from the broker as sent: ``uptime``, the
battery detail, the whole ``sd`` object, ``temp_c``, the egress and
offline-queue counters, and the GPS object. Any client with publish rights
on the broker could put one such integer in a retained health.

The stub lane has no core to serialize with, so these tests hold every
attribute to the writer's range themselves; ``tests/ha/test_entities.py``
serializes the real states on the 2026.5 floor and the current release.
"""

from __future__ import annotations

import json
import types
from typing import Any

import pytest

from . import conftest  # noqa: F401  (installs the base HA stubs)

# Reuse the platform stubs the hardening tests install (idempotent).
from .test_mqtt_payload_hardening import _install_platform_stubs  # noqa: E402

_install_platform_stubs()

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from .. import sensor as sensor_platform  # noqa: E402
from ..const import DOMAIN  # noqa: E402
from ..health_metrics import HA_ATTRIBUTE_INT_MAX  # noqa: E402

ENTRY = types.SimpleNamespace(entry_id="e1")

# What the core's JSON writer takes for an integer: a signed 64-bit one's
# range below zero, an unsigned 64-bit one's above.
WRITER_INT_MIN = -(2**63)
WRITER_INT_MAX = 2**64 - 1

# Integers past 64 bits, either side of zero, and one past a double's range.
WIDE = (2**64, 2**70, -(2**63) - 1, -(2**64), 10**400)


def _entity(cls):
    inst = cls("securacv", "canary01", ENTRY)
    hass = HomeAssistant()
    hass.data = {DOMAIN: {}}
    inst.hass = hass
    inst.writes = []
    inst.async_write_ha_state = lambda: inst.writes.append(True)
    inst.async_on_remove = lambda unsub: None
    return inst


def _msg(body: dict[str, Any]) -> types.SimpleNamespace:
    return types.SimpleNamespace(payload=json.dumps(body))


def _assert_carriable(value: Any, path: str = "attributes") -> None:
    """Every integer in `value`, at any depth, is one the writer takes."""
    if isinstance(value, dict):
        for key, item in value.items():
            _assert_carriable(item, f"{path}.{key}")
    elif isinstance(value, (list, tuple)):
        for index, item in enumerate(value):
            _assert_carriable(item, f"{path}[{index}]")
    elif isinstance(value, int) and not isinstance(value, bool):
        assert WRITER_INT_MIN <= value <= WRITER_INT_MAX, f"{path} = {value}"


def _health(number: Any) -> dict[str, Any]:
    """A health with `number` in every key the three sensors show."""
    return {
        "free_heap": 120000,
        "uptime": number,
        "firmware_version": number,
        "public_key": number,
        "battery_present": number,
        "charge_state": number,
        "battery_health_pct": number,
        "battery_mv": number,
        "temp_c": number,
        "sd": {
            "mounted": number, "usage_pct": number, "writes": number, "errors": number,
            "lifetime_kb": number, "wear_pct": number, "replace_recommended": number,
            "unknown_key": number, "nested": {"deeper": number},
        },
        "gps": {
            "fix_type": "3d", "satellites": number, "hdop": number,
            "latitude": number, "longitude": number,
        },
        "csi_event_egress": {
            "dropped": number, "held_dropped": 1,
            "planner": {"live": number, "held": 2},
        },
        "offline_queue": {"dropped_overflow": number, "dropped_oversize": 3},
    }


@pytest.mark.parametrize("wide", WIDE)
def test_the_health_sensor_carries_no_integer_past_64_bits(wide: int) -> None:
    inst = _entity(sensor_platform.SecuraCVCanaryHealthSensor)
    inst._handle_message(_msg(_health(wide)))
    attrs = inst._attr_extra_state_attributes
    _assert_carriable(attrs)
    assert inst._attr_native_value == "healthy" and inst.writes == [True]
    assert attrs["uptime_seconds"] is None
    assert attrs["firmware_version"] is None and attrs["public_key"] is None
    for key in ("battery_present", "charge_state", "battery_health_pct", "battery_mv", "temp_c"):
        assert key not in attrs, key
    # Only the keys the firmware writes, each a flag or a bounded number.
    assert attrs["sd"] == {}
    assert attrs["csi_event_egress"] == {"held_dropped": 1, "planner": {"held": 2}}
    assert attrs["offline_queue"] == {"dropped_oversize": 3}


@pytest.mark.parametrize("wide", WIDE)
def test_the_sd_wear_sensor_carries_no_integer_past_64_bits(wide: int) -> None:
    inst = _entity(sensor_platform.SecuraCVCanarySDWearSensor)
    inst._handle_message(_msg(_health(wide)))
    attrs = inst._attr_extra_state_attributes
    _assert_carriable(attrs)
    assert inst.writes == [True]
    for key in ("mounted", "usage_pct", "writes", "errors", "lifetime_kb", "replace_recommended"):
        assert attrs[key] is None, key
    # The state is a float, or no reading for a figure too wide for one.
    assert inst._attr_native_value is None or isinstance(inst._attr_native_value, float)


@pytest.mark.parametrize("wide", WIDE)
def test_the_gps_sensor_carries_no_integer_past_64_bits(wide: int) -> None:
    inst = _entity(sensor_platform.SecuraCVCanaryGPSSensor)
    inst._handle_message(_msg(_health(wide)))
    attrs = inst._attr_extra_state_attributes
    _assert_carriable(attrs)
    assert inst._attr_native_value == "3d" and inst.writes == [True]
    for key in ("satellites", "hdop", "latitude", "longitude"):
        assert attrs[key] is None, key


def test_what_a_unit_sends_still_reads_as_sent() -> None:
    """The bound takes nothing a unit publishes: its counts are uint32 at
    most, its card's lifetime a uint64, and the largest integer the core
    carries still reads as sent."""
    sent = {
        "free_heap": 120000, "uptime": 3600, "firmware_version": "2.4.19",
        "public_key": "AB" * 32, "battery_present": True, "charge_state": "discharging",
        "battery_health_pct": 97, "battery_mv": 4100, "battery_soc": 80, "temp_c": -40.5,
        "sd": {"mounted": True, "usage_pct": 12, "writes": 42000, "errors": 0,
               "lifetime_kb": HA_ATTRIBUTE_INT_MAX, "wear_pct": 2.8, "replace_recommended": False},
        "gps": {"fix_type": "3d", "satellites": 9, "hdop": 0.9},
        "csi_event_egress": {"dropped": 4294967295, "planner": {"live": 7}},
    }
    health = _entity(sensor_platform.SecuraCVCanaryHealthSensor)
    health._handle_message(_msg(sent))
    attrs = health._attr_extra_state_attributes
    _assert_carriable(attrs)
    assert attrs["uptime_seconds"] == 3600
    assert attrs["firmware_version"] == "2.4.19"
    assert attrs["public_key"] == "ab" * 32
    assert attrs["battery_present"] is True and attrs["charge_state"] == "discharging"
    assert attrs["battery_health_pct"] == 97 and attrs["battery_mv"] == 4100
    assert attrs["temp_c"] == -40.5
    assert attrs["sd"] == sent["sd"]
    assert attrs["csi_event_egress"] == sent["csi_event_egress"]

    wear = _entity(sensor_platform.SecuraCVCanarySDWearSensor)
    wear._handle_message(_msg(sent))
    assert wear._attr_native_value == 2.8
    shown = wear._attr_extra_state_attributes
    assert {key: shown[key] for key in sent["sd"] if key != "wear_pct"} == {
        key: value for key, value in sent["sd"].items() if key != "wear_pct"
    }

    gps = _entity(sensor_platform.SecuraCVCanaryGPSSensor)
    gps._handle_message(_msg(sent))
    shown = gps._attr_extra_state_attributes
    assert (shown["satellites"], shown["hdop"]) == (9, 0.9)
    assert (shown["latitude"], shown["longitude"]) == ("", "")


def test_no_reading_is_none_not_a_zero_nobody_sent() -> None:
    """A health with no uptime, and a GPS object with no satellite count or
    dilution, show None for them, as memory_free_bytes does (HA53)."""
    health = _entity(sensor_platform.SecuraCVCanaryHealthSensor)
    health._handle_message(_msg({"free_heap": 120000}))
    assert health._attr_extra_state_attributes["uptime_seconds"] is None
    gps = _entity(sensor_platform.SecuraCVCanaryGPSSensor)
    gps._handle_message(_msg({"gps": {"fix_type": "no_fix"}}))
    assert gps._attr_extra_state_attributes["satellites"] is None
    assert gps._attr_extra_state_attributes["hdop"] is None
