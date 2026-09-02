"""Unsigned-topic entities carry the shared trust slice (roadmap #53).

tamper / transport / mesh / chirp / health publishes are never signed by the
firmware, yet the entities they move used to carry no trust attribute at
all — a dashboard could tell one from a verified row only by noticing an
attribute was missing. Every such entity now stamps ``unsigned_trust_attrs``:
the same four keys sensor.py's ``_trust_attrs`` gives the signed entities,
with ``verified`` False and ``trust_reason`` "unsigned" (the verifiers' own
word for "no envelope"), plus the device's pinned fingerprint when one
exists. State logic is unchanged and the signed entities are untouched.
"""

from __future__ import annotations

from .test_mqtt_payload_hardening import ENTRY, HomeAssistant, _entity, _hass, _msg
from .conftest import run

from .. import binary_sensor as bs_platform
from .. import sensor as sensor_platform
from .. import unsigned_trust_attrs
from ..const import DOMAIN, TAMPER_POWER_LOSS, TAMPER_SD_REMOVE, TRANSPORT_WIFI_STA
from ..device_trust import TrustStore, fingerprint_from_pubkey_hex

KEY = "11" * 32
FP = fingerprint_from_pubkey_hex(KEY)


def _store_hass(pinned: bool) -> HomeAssistant:
    """An entry slice with a REAL trust store, optionally pinning canary01."""
    hass = HomeAssistant()
    store = TrustStore(hass, entry_id="e1")
    run(store.async_load())
    if pinned:
        run(store.async_pin("canary01", KEY))
    hass.data = {
        DOMAIN: {
            "e1": {"trust_store": store, "verify": {}, "replay": {}, "mismatch_notified": set()}
        }
    }
    return hass


def _assert_unsigned(attrs, pinned_fp=None) -> None:
    assert attrs["verified"] is False
    assert attrs["trust_reason"] == "unsigned"
    assert attrs["pinned_fingerprint"] == pinned_fp
    assert attrs["received_fingerprint"] is None


# ─── the helper ────────────────────────────────────────────────────────


def test_unsigned_slice_has_the_signed_slice_keys_and_never_says_verified() -> None:
    hass = _hass(
        {"verify": {"canary01": {"trusted": True, "reason": "ok",
                                 "pinned_fingerprint": FP, "received_fingerprint": FP}}}
    )
    signed = sensor_platform._trust_attrs(hass, ENTRY, "canary01")
    unsigned = unsigned_trust_attrs(hass, ENTRY, "canary01")
    assert set(signed) == set(unsigned), "same attribute, different value — not absence"
    assert signed["verified"] is True
    # A device whose chain verifies green does not launder that onto a topic
    # nobody signed: the slice describes THIS publish, not the device.
    _assert_unsigned(unsigned)

    # No entry slice / a placeholder store / a real store without a pin: no fingerprint.
    _assert_unsigned(unsigned_trust_attrs(_hass(), ENTRY, "canary01"))
    _assert_unsigned(unsigned_trust_attrs(_hass({"trust_store": object()}), ENTRY, "canary01"))
    _assert_unsigned(unsigned_trust_attrs(_store_hass(pinned=False), ENTRY, "canary01"))
    # A pinned device: the row still says which identity it is enrolled under.
    _assert_unsigned(unsigned_trust_attrs(_store_hass(pinned=True), ENTRY, "canary01"), FP)


# ─── binary_sensor: tamper / transport / mesh / chirp ──────────────────


def test_general_tamper_sensor_stamps_both_handlers_and_keeps_detail() -> None:
    inst = _entity(bs_platform.SecuraCVCanaryTamperSensor, hass=_store_hass(pinned=True))
    inst._handle_message(_msg('{"tamper": true}'))
    assert inst._attr_is_on is True
    _assert_unsigned(inst._attr_extra_state_attributes, FP)

    inst._handle_tamper_message(_msg('{"type": "enclosure", "detail": "lid"}'))
    attrs = inst._attr_extra_state_attributes
    assert attrs["tamper_type"] == "enclosure" and attrs["detail"] == "lid"
    _assert_unsigned(attrs, FP)

    # Junk on the tamper topic still means tamper (unchanged), is still annotated,
    # and does not erase the last structured detail.
    inst._handle_tamper_message(_msg("5"))
    assert inst._attr_is_on is True
    assert inst._attr_extra_state_attributes["tamper_type"] == "enclosure"
    _assert_unsigned(inst._attr_extra_state_attributes, FP)

    # A later health publish clearing the flag keeps the tamper detail too.
    inst._handle_message(_msg('{"tamper": false}'))
    assert inst._attr_is_on is False
    assert inst._attr_extra_state_attributes["tamper_type"] == "enclosure"
    _assert_unsigned(inst._attr_extra_state_attributes, FP)


def test_per_type_tamper_sensor_stamps_both_handlers() -> None:
    power = _entity(
        bs_platform.SecuraCVCanaryTamperTypeSensor,
        TAMPER_POWER_LOSS, "Power Loss", "mdi:power-plug-off",
        hass=_store_hass(pinned=False),
    )
    power._handle_tamper_message(_msg('{"type": "power_loss", "severity": "tamper"}'))
    assert power._attr_is_on is True
    assert power._attr_extra_state_attributes["severity"] == "tamper"
    _assert_unsigned(power._attr_extra_state_attributes)

    sd = _entity(
        bs_platform.SecuraCVCanaryTamperTypeSensor,
        TAMPER_SD_REMOVE, "SD Removed", "mdi:sd-off",
        hass=_store_hass(pinned=True),
    )
    sd._handle_health_message(_msg('{"sd_mounted": false}'))
    assert sd._attr_is_on is True
    assert sd._attr_extra_state_attributes["last_triggered"]
    _assert_unsigned(sd._attr_extra_state_attributes, FP)


def test_transport_sensor_stamps_dict_and_bool_shapes() -> None:
    inst = _entity(
        bs_platform.SecuraCVCanaryTransportSensor,
        TRANSPORT_WIFI_STA, "WiFi Station", "mdi:wifi",
        hass=_store_hass(pinned=True),
    )
    inst._handle_message(_msg('{"wifi_sta": {"connected": true, "rssi": -60}}'))
    assert inst._attr_is_on is True
    assert inst._attr_extra_state_attributes["rssi"] == -60
    _assert_unsigned(inst._attr_extra_state_attributes, FP)

    inst._handle_message(_msg('{"wifi_sta": false}'))
    assert inst._attr_is_on is False
    assert inst._attr_extra_state_attributes["rssi"] == -60, "last link detail kept"
    _assert_unsigned(inst._attr_extra_state_attributes, FP)


def test_mesh_and_chirp_sensors_stamp() -> None:
    mesh = _entity(bs_platform.SecuraCVCanaryMeshConnectedSensor, hass=_store_hass(pinned=True))
    mesh._handle_message(_msg('{"peers": ["p1"], "sent": 2}'))
    assert mesh._attr_is_on is True
    assert mesh._attr_extra_state_attributes["peer_count"] == 1
    _assert_unsigned(mesh._attr_extra_state_attributes, FP)

    chirp = _entity(bs_platform.SecuraCVCanaryChirpActiveSensor, hass=_store_hass(pinned=False))
    chirp._handle_message(_msg('{"enabled": true, "ready": true, "session_id": "abc"}'))
    assert chirp._attr_is_on is True
    assert chirp._attr_extra_state_attributes["session_emoji"] == "abc"
    _assert_unsigned(chirp._attr_extra_state_attributes)


# ─── sensor: the health-topic family ───────────────────────────────────


def test_health_topic_sensors_stamp() -> None:
    hass = _store_hass(pinned=True)

    health = _entity(sensor_platform.SecuraCVCanaryHealthSensor, hass=hass)
    health._handle_message(
        _msg('{"battery": 100, "free_heap": 10000000, "public_key": "' + KEY + '"}')
    )
    assert health._attr_native_value == "healthy"
    attrs = health._attr_extra_state_attributes
    assert attrs["public_key"] == KEY
    _assert_unsigned(attrs, FP)
    # The pin on the row is the fingerprint of the key the device advertises.
    assert attrs["pinned_fingerprint"] == fingerprint_from_pubkey_hex(attrs["public_key"])

    gps = _entity(sensor_platform.SecuraCVCanaryGPSSensor, hass=hass)
    gps._handle_message(_msg('{"gps": {"fix_type": "3d", "satellites": 7}}'))
    assert gps._attr_native_value == "3d"
    assert gps._attr_extra_state_attributes["satellites"] == 7
    _assert_unsigned(gps._attr_extra_state_attributes, FP)
    gps._handle_message(_msg('{"gps": "no_fix"}'))
    assert gps._attr_native_value == "no_fix"
    _assert_unsigned(gps._attr_extra_state_attributes, FP)

    sd = _entity(sensor_platform.SecuraCVCanarySDWearSensor, hass=hass)
    sd._handle_message(_msg('{"free_heap": 1}'))
    assert sd.writes == [], "firmware without SD reporting still leaves the sensor alone"
    sd._handle_message(_msg('{"sd": {"wear_pct": 12.5, "mounted": true}}'))
    assert sd._attr_native_value is not None
    assert sd._attr_extra_state_attributes["mounted"] is True
    _assert_unsigned(sd._attr_extra_state_attributes, FP)

    radar = _entity(sensor_platform.SecuraCVCanaryRadarLinkSensor, hass=hass)
    radar._handle_message(_msg('{"radar": {"link_ok": true, "last_frame_age_ms": 120}}'))
    assert radar._attr_native_value == "ok"
    assert radar._attr_extra_state_attributes["last_frame_age_ms"] == 120
    _assert_unsigned(radar._attr_extra_state_attributes, FP)


# ─── the signed entities are untouched ─────────────────────────────────


def test_signed_entities_still_read_the_real_verdict() -> None:
    hass = _hass(
        {"verify": {"canary01": {"trusted": True, "reason": "ok",
                                 "pinned_fingerprint": FP, "received_fingerprint": FP}}}
    )
    chain_valid = _entity(bs_platform.SecuraCVCanaryChainValidSensor, hass=hass)
    chain_valid._handle_message(_msg('{"valid": true, "length": 3}'))
    assert chain_valid._attr_is_on is True
    assert chain_valid._attr_extra_state_attributes["verified"] is True
    assert chain_valid._attr_extra_state_attributes["trust_reason"] == "ok"

    # Same device, same moment: its unsigned tamper row says unsigned, not verified.
    tamper = _entity(bs_platform.SecuraCVCanaryTamperSensor, hass=hass)
    tamper._handle_message(_msg('{"tamper": true}'))
    assert tamper._attr_extra_state_attributes["verified"] is False
    assert tamper._attr_extra_state_attributes["trust_reason"] == "unsigned"
