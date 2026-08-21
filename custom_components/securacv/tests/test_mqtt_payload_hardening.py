"""MQTT callbacks vs hostile/malformed payloads.

Every MQTT handler now routes through ``parse_mqtt_json`` — the shared
gate that enforces the untrusted-broker size cap and only ever hands back
a dict. The regressions this file pins down, all reproduced before the
fix:

  - valid-JSON non-object payloads ("5", '["p1","p2"]', "true") raised
    AttributeError out of seven @callbacks and stalled the entities;
  - ``{"tamper": true}`` — a spelling the general tamper sensor supports —
    crashed the per-type tamper sensor's health handler;
  - the stated payload cap was enforced in only 2 of ~15 handlers;
  - the Chain Valid binary sensor said "on" (valid) before any chain
    publish had ever been checked;
  - the HomeKit-projected motion/occupancy sensors asserted with no trust
    annotation at all, unlike every other surface.

Platform stubs follow the convention in test_modality_and_radar.py.
"""

from __future__ import annotations

import sys
import types

from . import conftest  # noqa: F401  (installs the base HA stubs)


def _install_platform_stubs() -> None:
    """Sensor + binary_sensor platform surfaces on top of conftest's stubs."""
    enum_ns = types.SimpleNamespace

    sensor_mod = sys.modules.get("homeassistant.components.sensor") or types.ModuleType(
        "homeassistant.components.sensor"
    )
    sensor_mod.SensorEntity = getattr(
        sensor_mod, "SensorEntity", None
    ) or type("SensorEntity", (), {})
    sensor_mod.SensorDeviceClass = enum_ns(TEMPERATURE="temperature")
    sensor_mod.SensorStateClass = enum_ns(
        MEASUREMENT="measurement", TOTAL_INCREASING="total_increasing"
    )
    sys.modules["homeassistant.components.sensor"] = sensor_mod

    bs_mod = sys.modules.get(
        "homeassistant.components.binary_sensor"
    ) or types.ModuleType("homeassistant.components.binary_sensor")
    bs_mod.BinarySensorEntity = getattr(
        bs_mod, "BinarySensorEntity", None
    ) or type("BinarySensorEntity", (), {})
    bs_mod.BinarySensorDeviceClass = enum_ns(
        CONNECTIVITY="connectivity",
        PROBLEM="problem",
        TAMPER="tamper",
        MOTION="motion",
        OCCUPANCY="occupancy",
    )
    sys.modules["homeassistant.components.binary_sensor"] = bs_mod

    const_mod = sys.modules.get("homeassistant.const") or types.ModuleType(
        "homeassistant.const"
    )
    const_mod.CONF_URL = "url"
    const_mod.PERCENTAGE = "%"
    const_mod.EntityCategory = enum_ns(DIAGNOSTIC="diagnostic")
    const_mod.UnitOfTemperature = enum_ns(CELSIUS="°C")
    sys.modules["homeassistant.const"] = const_mod

    entity_mod = sys.modules.get("homeassistant.helpers.entity") or types.ModuleType(
        "homeassistant.helpers.entity"
    )
    entity_mod.DeviceInfo = dict
    sys.modules["homeassistant.helpers.entity"] = entity_mod

    plat_mod = sys.modules.get(
        "homeassistant.helpers.entity_platform"
    ) or types.ModuleType("homeassistant.helpers.entity_platform")
    plat_mod.AddEntitiesCallback = object
    sys.modules["homeassistant.helpers.entity_platform"] = plat_mod

    event_mod = sys.modules.get("homeassistant.helpers.event") or types.ModuleType(
        "homeassistant.helpers.event"
    )
    # async_call_later returns a cancel callable, which the projected
    # sensors keep to re-arm their hold window.
    event_mod.async_call_later = lambda hass, delay, action: (lambda: None)
    sys.modules["homeassistant.helpers.event"] = event_mod

    uc_mod = sys.modules["homeassistant.helpers.update_coordinator"]
    if uc_mod.CoordinatorEntity is object:
        uc_mod.CoordinatorEntity = type("CoordinatorEntity", (), {})


_install_platform_stubs()

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from .. import MAX_MQTT_PAYLOAD_BYTES, parse_mqtt_json  # noqa: E402
from .. import binary_sensor as bs_platform  # noqa: E402
from .. import sensor as sensor_platform  # noqa: E402
from ..const import DOMAIN, TAMPER_MOTION, TAMPER_SD_REMOVE  # noqa: E402

ENTRY = types.SimpleNamespace(entry_id="e1")

# Valid JSON that is not an object — every one of these AttributeError'd
# at least one callback before the shared gate existed — plus plain junk,
# undecodable bytes, and an over-cap payload.
NON_OBJECT_JSON = ["5", '["p1", "p2"]', "true", "null", '"just-a-string"']
JUNK = ["{not json", b"\xff\xfe{\x00"]
OVERSIZE = '{"padding": "' + "x" * MAX_MQTT_PAYLOAD_BYTES + '"}'


def _msg(payload) -> types.SimpleNamespace:
    return types.SimpleNamespace(payload=payload)


def _hass(entry_data: dict | None = None) -> HomeAssistant:
    hass = HomeAssistant()
    hass.data = {DOMAIN: {} if entry_data is None else {"e1": entry_data}}
    return hass


def _entity(cls, *args, hass: HomeAssistant | None = None):
    """Instantiate an entity for driving its @callback handlers directly."""
    inst = cls("securacv", "canary01", ENTRY, *args)
    inst.hass = hass or _hass()
    inst.writes = []
    inst.async_write_ha_state = lambda: inst.writes.append(True)
    inst.async_on_remove = lambda unsub: None
    return inst


# ─── the shared gate itself ───────────────────────────────────────────


def test_parse_mqtt_json_only_returns_objects() -> None:
    assert parse_mqtt_json('{"a": 1}') == {"a": 1}
    assert parse_mqtt_json(b'{"a": 1}') == {"a": 1}
    for payload in NON_OBJECT_JSON + JUNK:
        assert parse_mqtt_json(payload) is None, payload
    assert parse_mqtt_json(None) is None


def test_parse_mqtt_json_enforces_the_cap() -> None:
    """The untrusted-broker cap applies BEFORE any decode, in every path."""
    assert len(OVERSIZE) > MAX_MQTT_PAYLOAD_BYTES
    assert parse_mqtt_json(OVERSIZE) is None
    assert parse_mqtt_json(OVERSIZE.encode()) is None
    # At the cap is still fine.
    exact = '{"p": "' + "x" * (MAX_MQTT_PAYLOAD_BYTES - 9) + '"}'
    assert len(exact) == MAX_MQTT_PAYLOAD_BYTES
    assert parse_mqtt_json(exact) == {"p": "x" * (MAX_MQTT_PAYLOAD_BYTES - 9)}


# ─── sensor.py handlers ───────────────────────────────────────────────


def test_counts_handler_survives_and_keeps_the_int_fallback() -> None:
    inst = _entity(sensor_platform.SecuraCVCanaryWitnessCountSensor)
    # The designed bare-integer fallback was unreachable before: "5" is
    # valid JSON, so it hit data.get() and AttributeError'd instead.
    inst._handle_message(_msg("5"))
    assert inst._attr_native_value == 5
    for payload in ['["p1", "p2"]', "true", "{not json", OVERSIZE]:
        inst._handle_message(_msg(payload))  # must not raise
    assert inst._attr_native_value == 5  # junk never moved the state
    inst._handle_message(_msg('{"total": 42}'))
    assert inst._attr_native_value == 42


def test_chain_length_handler_survives_non_objects() -> None:
    inst = _entity(sensor_platform.SecuraCVCanaryChainLengthSensor)
    for payload in NON_OBJECT_JSON + JUNK + [OVERSIZE]:
        inst._handle_message(_msg(payload))  # must not raise
    assert inst.writes == []
    inst._handle_message(_msg('{"length": 7}'))
    assert inst._attr_native_value == 7


def test_gps_handler_survives_non_objects() -> None:
    inst = _entity(sensor_platform.SecuraCVCanaryGPSSensor)
    for payload in NON_OBJECT_JSON + JUNK + [OVERSIZE]:
        inst._handle_message(_msg(payload))  # reproduced AttributeError on "5"
    assert inst.writes == []
    inst._handle_message(_msg('{"gps": {"fix_type": "3d", "satellites": 9}}'))
    assert inst._attr_native_value == "3d"


def test_health_handler_reports_unknown_on_non_objects() -> None:
    inst = _entity(sensor_platform.SecuraCVCanaryHealthSensor)
    inst._handle_message(_msg("[1, 2]"))
    assert inst._attr_native_value == "unknown"
    inst._handle_message(_msg('{"battery": 80, "memory_free": 50000}'))
    assert inst._attr_native_value == "healthy"


def test_sd_wear_handler_survives_non_objects() -> None:
    inst = _entity(sensor_platform.SecuraCVCanarySDWearSensor)
    for payload in NON_OBJECT_JSON + JUNK + [OVERSIZE]:
        inst._handle_message(_msg(payload))
    assert inst.writes == []


# ─── binary_sensor.py handlers ────────────────────────────────────────


def test_chain_valid_starts_unknown_not_on() -> None:
    """Before any chain publish is verified there is nothing honest to
    assert — HA renders None as unknown, never as a green "valid"."""
    inst = _entity(bs_platform.SecuraCVCanaryChainValidSensor)
    assert inst._attr_is_on is None


def test_chain_valid_handler_survives_and_requires_verification() -> None:
    inst = _entity(bs_platform.SecuraCVCanaryChainValidSensor)
    for payload in NON_OBJECT_JSON + JUNK + [OVERSIZE]:
        inst._handle_message(_msg(payload))
    assert inst._attr_is_on is None  # junk never flipped it out of unknown
    # A self-reported valid chain with no signature verification stays off.
    inst._handle_message(_msg('{"valid": true, "length": 3}'))
    assert inst._attr_is_on is False
    assert inst._attr_extra_state_attributes["verified"] is False


def test_tamper_type_health_handler_survives_boolean_tamper_field() -> None:
    """{"tamper": true} — the general sensor's supported spelling — used to
    AttributeError this handler (bool has no .get)."""
    inst = _entity(
        bs_platform.SecuraCVCanaryTamperTypeSensor,
        TAMPER_MOTION, "Unexpected Motion", "mdi:motion-sensor",
    )
    inst._handle_health_message(_msg('{"tamper": true}'))
    assert inst._attr_is_on is False  # a bare boolean names no specific type
    for payload in NON_OBJECT_JSON + JUNK + [OVERSIZE]:
        inst._handle_health_message(_msg(payload))
        inst._handle_tamper_message(_msg(payload))
    # The structured spellings still work.
    inst._handle_health_message(_msg('{"tamper": {"motion": true}}'))
    assert inst._attr_is_on is True


def test_tamper_type_health_handler_still_reads_flat_fields() -> None:
    inst = _entity(
        bs_platform.SecuraCVCanaryTamperTypeSensor,
        TAMPER_SD_REMOVE, "SD Removed", "mdi:sd-off",
    )
    inst._handle_health_message(_msg('{"sd_mounted": false}'))
    assert inst._attr_is_on is True
    inst._handle_health_message(_msg('{"sd_mounted": true, "tamper": true}'))
    assert inst._attr_is_on is False


def test_general_tamper_handlers_survive_non_objects() -> None:
    inst = _entity(bs_platform.SecuraCVCanaryTamperSensor)
    for payload in NON_OBJECT_JSON + JUNK + [OVERSIZE]:
        inst._handle_message(_msg(payload))
    assert inst._attr_is_on is False
    # Any publish on the dedicated tamper topic means tamper, even junk.
    inst._handle_tamper_message(_msg("5"))
    assert inst._attr_is_on is True


def test_transport_handler_survives_non_objects() -> None:
    inst = _entity(
        bs_platform.SecuraCVCanaryTransportSensor,
        "wifi_sta", "WiFi Station", "mdi:wifi",
    )
    for payload in NON_OBJECT_JSON + JUNK + [OVERSIZE]:
        inst._handle_message(_msg(payload))
    assert inst.writes == []
    inst._handle_message(_msg('{"wifi_sta": {"connected": true, "rssi": -60}}'))
    assert inst._attr_is_on is True


def test_mesh_handler_survives_non_objects_and_odd_values() -> None:
    inst = _entity(bs_platform.SecuraCVCanaryMeshConnectedSensor)
    # The reproduced case: a bare JSON array on the mesh topic.
    inst._handle_message(_msg('["p1", "p2"]'))
    for payload in NON_OBJECT_JSON + JUNK + [OVERSIZE, '{"peers": 5}']:
        inst._handle_message(_msg(payload))
    assert inst.writes == []
    inst._handle_message(_msg('{"peers": ["p1"], "sent": 2}'))
    assert inst._attr_is_on is True


def test_chirp_handler_survives_non_objects() -> None:
    inst = _entity(bs_platform.SecuraCVCanaryChirpActiveSensor)
    for payload in NON_OBJECT_JSON + JUNK + [OVERSIZE]:
        inst._handle_message(_msg(payload))
    assert inst.writes == []
    inst._handle_message(_msg('{"enabled": true, "ready": true}'))
    assert inst._attr_is_on is True


def test_occupancy_state_handler_survives_non_objects() -> None:
    inst = _entity(bs_platform.SecuraCVCanaryOccupancySensor)
    for payload in NON_OBJECT_JSON + JUNK + [OVERSIZE]:
        inst._handle_state(_msg(payload))
    assert inst.writes == []
    inst._handle_state(_msg('{"presence": true}'))
    assert inst._attr_is_on is True


# ─── projected sensors carry the trust verdict (Apple Home honesty) ───


def test_projected_motion_sensor_stamps_trust_attributes() -> None:
    """State follows warn-loudly-accept, but the verdict must ride along —
    an asserted signal with no annotation launders an unverified publish."""
    inst = _entity(bs_platform.SecuraCVCanaryMotionSensor)
    inst._handle_event(_msg('{"event_type": "BoundaryCrossingObjectLarge"}'))
    assert inst._attr_is_on is True
    attrs = inst._attr_extra_state_attributes
    assert attrs["verified"] is False
    assert attrs["trust_reason"] == "no_pubkey"

    # With a verify verdict stamped (by sensor.py's events handler), the
    # annotation reflects it.
    verified_hass = _hass(
        {"verify": {"canary01": {"trusted": True, "reason": "verified",
                                 "pinned_fingerprint": "aa", "received_fingerprint": "aa"}}}
    )
    inst2 = _entity(bs_platform.SecuraCVCanaryMotionSensor, hass=verified_hass)
    inst2._handle_event(_msg('{"event_type": "BoundaryCrossingObjectLarge"}'))
    assert inst2._attr_extra_state_attributes["verified"] is True
    assert inst2._attr_extra_state_attributes["trust_reason"] == "verified"


def test_projected_sensor_ignores_non_objects_and_unmapped_events() -> None:
    inst = _entity(bs_platform.SecuraCVCanaryMotionSensor)
    for payload in NON_OBJECT_JSON + JUNK + [OVERSIZE]:
        inst._handle_event(_msg(payload))
    inst._handle_event(_msg('{"event_type": "AcousticImpulseInZone"}'))
    assert inst._attr_is_on is False
    assert inst.writes == []
