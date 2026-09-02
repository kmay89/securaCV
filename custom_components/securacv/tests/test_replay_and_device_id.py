"""Replay protection and the device-id gate on the wildcard MQTT subscriptions.

Two gaps the same audit found:

1. The signed publishes carried monotonic counters (event_id / seq / chain
   length / counts total) that the canonicals sign but nothing consumed, so an
   old, validly signed message replayed onto the broker verified green and
   moved entity state. `_replay_gate` now downgrades a verified publish whose
   counter goes BACKWARDS to `reason="replay"`, and the chain / counts / event
   handlers refuse to move state on it. Equal counters stay benign (the
   firmware republishes an unchanged head while idle; a broker re-delivers the
   retained last event on reconnect).

2. `device_id` came verbatim from the topic, so any publisher could mint
   unbounded devices and ~20 entities each. `valid_device_id` pins the segment
   to what a Canary can actually publish.
"""
from __future__ import annotations

from .test_mqtt_payload_hardening import _hass, _msg, ENTRY, HomeAssistant  # noqa: F401

from .. import valid_device_id  # noqa: E402
from .. import sensor as sensor_platform  # noqa: E402
from ..const import DOMAIN  # noqa: E402
from ..device_trust import TrustVerdict  # noqa: E402


def _entry_data() -> dict:
    return {
        "trust_store": object(),   # only its presence matters to _trust_store_for
        "verify": {},
        "replay": {},
        "mismatch_notified": set(),
    }


def _trusted(*_a, **_k) -> TrustVerdict:
    return TrustVerdict(trusted=True, reason="ok", pinned_fingerprint="ab" * 32,
                        received_fingerprint="ab" * 32)


def _named(fn, name):
    fn.__name__ = name
    return fn


# ─── the gate itself ───────────────────────────────────────────────────


def test_a_backwards_chain_length_is_a_replay_but_equal_is_not() -> None:
    hass = _hass(_entry_data())
    verify_chain = _named(lambda ts, d, p: _trusted(), "verify_chain")
    v = sensor_platform._verify_and_record(hass, ENTRY, "canary01", {"length": 5}, verify_chain)
    assert v.trusted and v.reason == "ok"
    # Idle device republishing the same head: benign.
    v = sensor_platform._verify_and_record(hass, ENTRY, "canary01", {"length": 5}, verify_chain)
    assert v.trusted
    # An older, validly signed head: replay.
    v = sensor_platform._verify_and_record(hass, ENTRY, "canary01", {"length": 4}, verify_chain)
    assert not v.trusted and v.reason == "replay"
    assert "length=4" in v.detail and "length=5" in v.detail
    # The stored verify view says so, and the mark did not move backwards.
    assert hass.data[DOMAIN]["e1"]["verify"]["canary01"]["reason"] == "replay"
    assert hass.data[DOMAIN]["e1"]["replay"]["canary01"]["length"] == 5
    # Progress is accepted again.
    v = sensor_platform._verify_and_record(hass, ENTRY, "canary01", {"length": 6}, verify_chain)
    assert v.trusted


def test_marks_are_per_device_and_per_kind() -> None:
    hass = _hass(_entry_data())
    verify_event = _named(lambda ts, d, p: _trusted(), "verify_event")
    verify_counts = _named(lambda ts, d, p: _trusted(), "verify_counts")
    assert sensor_platform._verify_and_record(hass, ENTRY, "a", {"event_id": 10}, verify_event).trusted
    # A different device with a lower id is not a replay of the first one.
    assert sensor_platform._verify_and_record(hass, ENTRY, "b", {"event_id": 2}, verify_event).trusted
    # A different KIND on the same device has its own mark.
    assert sensor_platform._verify_and_record(hass, ENTRY, "a", {"total": 1}, verify_counts).trusted
    assert sensor_platform._verify_and_record(hass, ENTRY, "a", {"event_id": 9}, verify_event).reason == "replay"


def test_untrusted_and_counterless_publishes_pass_through_unchanged() -> None:
    hass = _hass(_entry_data())
    unsigned = TrustVerdict(trusted=False, reason="unsigned")
    verify_event = _named(lambda ts, d, p: unsigned, "verify_event")
    v = sensor_platform._verify_and_record(hass, ENTRY, "a", {"event_id": 3}, verify_event)
    assert v is unsigned
    # No counter field / garbage counter: the verdict is left alone and no mark is set.
    verify_chain = _named(lambda ts, d, p: _trusted(), "verify_chain")
    assert sensor_platform._verify_and_record(hass, ENTRY, "a", {"length": "abc"}, verify_chain).trusted
    assert sensor_platform._verify_and_record(hass, ENTRY, "a", {}, verify_chain).trusted
    assert "length" not in hass.data[DOMAIN]["e1"]["replay"].get("a", {})


def test_a_fresh_tofu_pin_resets_the_marks() -> None:
    from .. import async_record_verify

    hass = _hass(_entry_data())
    verify_chain = _named(lambda ts, d, p: _trusted(), "verify_chain")
    assert sensor_platform._verify_and_record(hass, ENTRY, "a", {"length": 40}, verify_chain).trusted
    # Factory reset: the device pins fresh and its chain starts over.
    async_record_verify(hass, ENTRY, "a", TrustVerdict(trusted=True, reason="tofu_pin"))
    assert sensor_platform._verify_and_record(hass, ENTRY, "a", {"length": 1}, verify_chain).trusted


# ─── the handlers refuse to move state on a replay ─────────────────────


def _entity(cls, hass):
    inst = cls("securacv", "canary01", ENTRY)
    inst.hass = hass
    inst.writes = []
    inst.async_write_ha_state = lambda: inst.writes.append(True)
    inst.async_on_remove = lambda unsub: None
    return inst


def test_chain_length_sensor_holds_its_state_on_replay(monkeypatch) -> None:
    hass = _hass(_entry_data())
    monkeypatch.setattr(sensor_platform, "verify_chain",
                        _named(lambda ts, d, p: _trusted(), "verify_chain"))
    ent = _entity(sensor_platform.SecuraCVCanaryChainLengthSensor, hass)
    ent._handle_message(_msg('{"length": 7, "latest_hash": "aa"}'))
    assert ent._attr_native_value == 7
    ent._handle_message(_msg('{"length": 3, "latest_hash": "bb"}'))
    assert ent._attr_native_value == 7, "an older signed head must not move the length"
    assert ent._attr_extra_state_attributes["latest_hash"] == "aa"
    assert ent._attr_extra_state_attributes["trust_reason"] == "replay"
    assert ent._attr_extra_state_attributes["verified"] is False
    assert len(ent.writes) == 2, "the replay is still written, as an annotation"


def test_counts_sensor_holds_its_state_on_replay(monkeypatch) -> None:
    hass = _hass(_entry_data())
    monkeypatch.setattr(sensor_platform, "verify_counts",
                        _named(lambda ts, d, p: _trusted(), "verify_counts"))
    ent = _entity(sensor_platform.SecuraCVCanaryWitnessCountSensor, hass)
    ent._handle_message(_msg('{"total": 12}'))
    assert ent._attr_native_value == 12
    ent._handle_message(_msg('{"total": 5}'))
    assert ent._attr_native_value == 12
    assert ent._attr_extra_state_attributes["trust_reason"] == "replay"


def test_last_event_sensor_keeps_the_newer_event_on_replay(monkeypatch) -> None:
    hass = _hass(_entry_data())
    monkeypatch.setattr(sensor_platform, "verify_event",
                        _named(lambda ts, d, p: _trusted(), "verify_event"))
    ent = _entity(sensor_platform.SecuraCVCanaryLastEventSensor, hass)
    ent._handle_message(_msg('{"event_id": 20, "event_type": "presence_changed"}'))
    assert ent._attr_native_value == "presence_changed"
    ent._handle_message(_msg('{"event_id": 19, "event_type": "unusual_motion"}'))
    assert ent._attr_native_value == "presence_changed", "the older event must not replace the newer one"
    assert ent._attr_extra_state_attributes["trust_reason"] == "replay"


# ─── the device-id gate ────────────────────────────────────────────────


def test_valid_device_id_accepts_what_a_canary_publishes() -> None:
    for ok in ("canary01", "canary-wap-a3f7", "canary_nightstand7_001", "A", "x" * 64):
        assert valid_device_id(ok), ok


def test_valid_device_id_rejects_hostile_segments() -> None:
    for bad in ("", "../etc", "a b", "canary/one", "canary.local", "-lead", "_lead",
                "x" * 65, "naïve", "canary\n", None, 7):
        assert not valid_device_id(bad), repr(bad)
