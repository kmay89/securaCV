"""The event platform: witness per zone, tamper per Canary, verification.

What is pinned:
  - the coordinator hands the event entities only NON-retained, not-yet-seen
    events from `<prefix>/events` (a retained replay feeds the last-event
    sensor only; the same event on two topics fires once; the backstop fires
    a missed event exactly once);
  - the first event for a zone creates its entity and that entity fires it on
    being added; later events route to the existing entity; entities come
    back from the entity registry on reload under the same unique id;
  - the witness attributes are the C5 table (D10 verdict included);
  - the tamper entity fires on non-retained publishes naming an advertised
    type, never on retained or unknown ones, and not twice for one publish;
  - the verification entity fires whatever the verifier dispatches;
  - event.py spells no event type of its own.
"""

from __future__ import annotations

import json
import re
import types
from pathlib import Path

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import dispatched, make_entry, make_runtime, run

from homeassistant.core import HomeAssistant  # noqa: E402

from .. import event as event_platform  # noqa: E402
from ..const import (  # noqa: E402
    ALL_TAMPER_TYPES,
    CONF_ENABLE_MQTT,
    CONF_KERNEL_DEVICE_ID,
    CONF_MQTT_PREFIX,
    EVENT_TYPE_METADATA,
    VERIFICATION_EVENT_CHAIN_VERIFIED,
    WITNESS_EVENT_TYPES,
)
from ..coordinator import (  # noqa: E402
    SecuraCVKernelCoordinator,
    verification_event_signal,
    witness_event_signal,
)
from ..data import VerificationState  # noqa: E402

PACKAGE_DIR = Path(event_platform.__file__).resolve().parent
KEY = "ab" * 32
EVENT = {
    "event_type": "BoundaryCrossingObjectLarge",
    "zone_id": "zone:front_door",
    "time_bucket": {"start_epoch_s": 1700000400, "size_s": 600},
    "confidence": 0.87,
    "attestation": "adapter",
    "kernel_version": "2.4.16",
    "ruleset_id": "ruleset:frigate_v1",
    "device_id": "pwk_1a2b3c4d",
    "seq": 12,
}


class FakeApi:
    base_url = "http://kernel:8799"
    token_file_unreadable = False

    async def async_get_health(self):
        return {"ok": True}

    async def async_get_identity(self):
        return {"device_id": "pwk_1a2b3c4d", "verifying_key": KEY, "kernel_version": "2.4.16"}

    async def async_get_status(self):
        return None

    async def async_get_latest_event(self):
        return self.latest

    async def async_get_digest(self):
        return None

    latest = None


def _kernel(hass=None):
    hass = hass or HomeAssistant()
    hass.data = {}
    entry = make_entry(hass, data={CONF_KERNEL_DEVICE_ID: "pwk_1a2b3c4d", CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv"})
    api = FakeApi()
    coordinator = SecuraCVKernelCoordinator(
        hass, entry, api, auth_mode="paired", topic_prefix="witness", pinned_key=KEY
    )
    entry.runtime_data = make_runtime(hass, coordinator=coordinator, api=api)
    return hass, entry, api, coordinator


def _msg(payload, *, retain=False, topic="witness/events"):
    """A ReceiveMessage: the wire carries text, so a dict is serialized."""
    if isinstance(payload, dict):
        payload = json.dumps(payload)
    return types.SimpleNamespace(topic=topic, payload=payload, retain=retain)


def _added(entity, hass):
    """Add an entity the way the platform does: hass bound, lifecycle run."""
    entity.hass = hass
    entity.writes = 0

    def _write():
        entity.writes += 1

    entity.async_write_ha_state = _write
    on_remove: list = []
    entity.async_on_remove = on_remove.append
    run(entity.async_added_to_hass())
    return on_remove


def _clock_at(monkeypatch, stamp: float) -> None:
    """Freeze the clock beside the fixture's 2023 buckets: a pushed row fires
    only while it is live (coordinator.event_is_live, two buckets back)."""
    import time as _time

    monkeypatch.setattr(_time, "time", lambda: stamp)


# ─── the coordinator's live feed ────────────────────────────────────────


def test_a_retained_replay_feeds_the_last_event_sensor_only():
    hass, entry, _api, coordinator = _kernel()
    seen: list = []
    from homeassistant.helpers.dispatcher import async_dispatcher_connect

    async_dispatcher_connect(hass, witness_event_signal(entry.entry_id), seen.append)
    coordinator._handle_last_event(_msg(EVENT, retain=True, topic="witness/last_event"))
    coordinator._handle_event(_msg(EVENT, retain=True))
    assert coordinator.data["latest_event"] == EVENT
    assert seen == [], "a retained replay never fires an event entity"


def test_a_live_event_fires_once_whatever_carried_it(monkeypatch):
    _clock_at(monkeypatch, 1700000700.0)
    hass, entry, _api, coordinator = _kernel()
    seen: list = []
    from homeassistant.helpers.dispatcher import async_dispatcher_connect

    async_dispatcher_connect(hass, witness_event_signal(entry.entry_id), seen.append)
    coordinator._handle_event(_msg(EVENT))
    coordinator._handle_event(_msg(EVENT))  # the same event again (a reconnect)
    assert seen == [EVENT]
    assert coordinator.data["latest_event"] == EVENT
    # A different bucket is a different event.
    later = dict(EVENT, time_bucket={"start_epoch_s": 1700001000, "size_s": 600})
    coordinator._handle_event(_msg(later))
    assert seen == [EVENT, later]
    # The backstop finding the same latest event does not fire it again.
    coordinator.api.latest = later
    run(coordinator.async_refresh())
    assert seen == [EVENT, later]


def test_the_backstop_fires_a_missed_event_exactly_once(monkeypatch):
    _clock_at(monkeypatch, 1700000700.0)
    hass, entry, api, coordinator = _kernel()
    seen: list = []
    from homeassistant.helpers.dispatcher import async_dispatcher_connect

    async_dispatcher_connect(hass, witness_event_signal(entry.entry_id), seen.append)
    api.latest = EVENT
    run(coordinator.async_refresh())
    run(coordinator.async_refresh())
    assert seen == [EVENT]
    coordinator._handle_event(_msg(EVENT))
    assert seen == [EVENT], "the push copy of a backstopped event is a duplicate"


def test_a_stale_row_updates_the_last_event_sensor_but_never_fires(monkeypatch):
    """A non-retained row whose bucket is older than two bucket sizes (a
    bridge that restarted or replayed its log, a backstop catching up after
    an outage) is history: it takes the last-event slot and fires nothing."""
    hass, entry, api, coordinator = _kernel()
    seen: list = []
    from homeassistant.helpers.dispatcher import async_dispatcher_connect

    async_dispatcher_connect(hass, witness_event_signal(entry.entry_id), seen.append)
    # 1900 s after the row's bucket started: past the 1200 s live window.
    _clock_at(monkeypatch, 1700000400 + 1900.0)
    coordinator._handle_event(_msg(EVENT))
    assert seen == [], "a row older than the live window never fires"
    assert coordinator.data["latest_event"] == EVENT, "but the last-event sensor shows it"
    # Its key is remembered: a second copy is not a new event either.
    coordinator._handle_event(_msg(EVENT))
    assert seen == []
    # The backstop serving an old row behaves the same.
    older = dict(EVENT, zone_id="zone:yard", time_bucket={"start_epoch_s": 1700000400 - 600, "size_s": 600})
    api.latest = dict(EVENT, time_bucket={"start_epoch_s": 1700000400 + 600, "size_s": 600})
    run(coordinator.async_refresh())
    assert seen == []
    assert coordinator.data["latest_event"]["time_bucket"]["start_epoch_s"] == 1700000400 + 600
    coordinator._handle_event(_msg(older))
    assert seen == []
    # A row in the live window fires: the current bucket, and one that
    # started 1100 s ago (inside two bucket sizes).
    live = dict(EVENT, time_bucket={"start_epoch_s": 1700000400 + 1800, "size_s": 600})
    coordinator._handle_event(_msg(live))
    edge = dict(EVENT, zone_id="zone:side", time_bucket={"start_epoch_s": 1700000400 + 1200, "size_s": 600})
    coordinator._handle_event(_msg(edge))
    assert seen == [live, edge]


def test_junk_on_the_events_topic_is_ignored():
    hass, entry, _api, coordinator = _kernel()
    for payload in ("not json", '{"event_type": 5}', '{"zone_id": "zone:a"}', "[1,2]"):
        coordinator._handle_event(_msg(payload))
    assert coordinator.data["latest_event"] is None


# ─── witness entities per zone ──────────────────────────────────────────


def test_the_first_event_for_a_zone_creates_its_entity_which_fires_it():
    hass, entry, _api, coordinator = _kernel()
    added: list = []
    manager = event_platform.WitnessEventManager(hass, entry, coordinator, added.extend)
    manager.async_handle_event(EVENT)
    (entity,) = added
    assert isinstance(entity, event_platform.SecuraCVWitnessEventEntity)
    assert entity.unique_id == f"securacv_{entry.entry_id}_witness_front_door"
    assert entity._attr_translation_placeholders == {"zone": "Front door"}
    assert entity.event_types == list(WITNESS_EVENT_TYPES)
    assert not hasattr(entity, "fired"), "nothing fires before the entity is in hass"

    _added(entity, hass)
    (event_type, attrs) = entity.fired[0]
    assert event_type == "boundary_crossing_object_large"
    assert attrs["zone"] == "front_door" and attrs["zone_id"] == "zone:front_door"
    assert attrs["verification"] == "sealed" and attrs["verified"] is False
    assert attrs["trust_reason"] == "kernel_sealed" and attrs["signed"] is True
    assert attrs["friendly_event"] == "Person or vehicle"
    assert attrs["kernel_device_id"] == "pwk_1a2b3c4d" and attrs["seq"] == 12
    assert attrs["time_bucket"] == {"start_epoch_s": 1700000400, "size_s": 600}
    assert entity.writes == 1

    # The next event for the zone routes to the same entity; a new zone
    # makes a new entity; an unknown type goes nowhere.
    second = dict(EVENT, time_bucket={"start_epoch_s": 1700001000, "size_s": 600})
    manager.async_handle_event(second)
    assert len(added) == 1 and len(entity.fired) == 2
    manager.async_handle_event(dict(EVENT, zone_id="zone:Drive-Way"))
    assert len(added) == 2 and added[1].slug == "drive_way"
    assert added[1]._attr_translation_placeholders == {"zone": "Drive Way"}
    manager.async_handle_event(dict(EVENT, event_type="FaceRecognized"))
    assert len(added) == 2 and len(entity.fired) == 2


def test_a_covered_event_fires_verified(monkeypatch):
    _clock_at(monkeypatch, 1700000700.0)
    hass, entry, _api, coordinator = _kernel()
    entry.runtime_data.verification = VerificationState(
        ok=True, verified_bucket_start=1700001200,
        covered={("boundary_crossing_object_large", "zone:front_door", 1700000400)},
        covered_seqs={12: ("boundary_crossing_object_large", "zone:front_door", 1700000400)},
    )
    entity = event_platform.SecuraCVWitnessEventEntity(coordinator, entry, "front_door", "Front door")
    _added(entity, hass)
    entity.async_fire(EVENT)
    (_type, attrs) = entity.fired[0]
    assert attrs["verification"] == "verified" and attrs["verified"] is True
    assert attrs["trust_reason"] == "ha_verified_chain"
    assert attrs["verified_bucket_start"] == 1700001200


def test_witness_entities_are_restored_from_the_registry_on_reload():
    hass, entry, _api, coordinator = _kernel()
    hass.data["_securacv_test_entity_registry"] = [
        types.SimpleNamespace(domain="event", unique_id=f"securacv_{entry.entry_id}_witness_front_door",
                              config_entry_id=entry.entry_id, original_name="Front door"),
        types.SimpleNamespace(domain="event", unique_id=f"securacv_{entry.entry_id}_witness_garage",
                              config_entry_id=entry.entry_id, original_name=None),
        types.SimpleNamespace(domain="event", unique_id=f"securacv_{entry.entry_id}_verification_events",
                              config_entry_id=entry.entry_id, original_name="Chain verification"),
        types.SimpleNamespace(domain="sensor", unique_id=f"securacv_{entry.entry_id}_witness_x",
                              config_entry_id=entry.entry_id, original_name="x"),
        types.SimpleNamespace(domain="event", unique_id="securacv_other_witness_yard",
                              config_entry_id="other", original_name="Yard"),
    ]
    added: list = []
    manager = event_platform.WitnessEventManager(hass, entry, coordinator, added.extend)
    manager.async_restore_from_registry()
    assert sorted(e.slug for e in added) == ["front_door", "garage"]
    by_slug = {e.slug: e for e in added}
    assert by_slug["front_door"]._attr_translation_placeholders == {"zone": "Front door"}
    assert by_slug["garage"]._attr_translation_placeholders == {"zone": "Garage"}
    # A live event for a restored zone fires on the restored entity.
    _added(by_slug["front_door"], hass)
    manager.async_handle_event(EVENT)
    assert len(added) == 2 and len(by_slug["front_door"].fired) == 1


def test_setup_entry_wires_the_signal_and_the_verification_entity(monkeypatch):
    _clock_at(monkeypatch, 1700000700.0)
    hass, entry, _api, coordinator = _kernel()
    added: list = []

    async def _nothing(hass, topic, callback):
        return lambda: None

    import sys
    mqtt_stub = sys.modules["homeassistant.components.mqtt"]
    original = mqtt_stub.async_subscribe
    mqtt_stub.async_subscribe = _nothing
    try:
        run(event_platform.async_setup_entry(hass, entry, added.extend))
    finally:
        mqtt_stub.async_subscribe = original
    assert [type(e).__name__ for e in added] == ["SecuraCVVerificationEventEntity"]
    assert dispatched(hass, witness_event_signal(entry.entry_id)), "the manager listens"
    assert len(entry.runtime_data.unsub_mqtt) == 3, "the listener and the two tamper topics"
    # A live event through the coordinator creates the zone's entity.
    coordinator._handle_event(_msg(EVENT))
    assert [type(e).__name__ for e in added][1:] == ["SecuraCVWitnessEventEntity"]


def test_zone_slug_and_label_rules():
    assert event_platform.zone_slug("zone:front_door") == "front_door"
    assert event_platform.zone_slug("zone:Front Door") == "front_door"
    assert event_platform.zone_slug("front-door") == "front_door"
    assert event_platform.zone_slug("zone:Café!") == "caf__"
    assert event_platform.zone_slug("zone:") == "zone"
    assert event_platform.zone_label("zone:front_door") == "Front door"
    assert event_platform.zone_label("back-yard") == "Back yard"
    assert event_platform.zone_label("zone:") == "Zone"


# ─── the verification entity ───────────────────────────────────────────


def test_the_verification_entity_fires_what_the_verifier_dispatches():
    hass, entry, _api, coordinator = _kernel()
    entity = event_platform.SecuraCVVerificationEventEntity(coordinator, entry)
    assert entity.unique_id == f"securacv_{entry.entry_id}_verification_events"
    on_remove = _added(entity, hass)
    from homeassistant.helpers.dispatcher import async_dispatcher_send

    async_dispatcher_send(hass, verification_event_signal(entry.entry_id),
                          VERIFICATION_EVENT_CHAIN_VERIFIED, {"chain_valid": True, "trigger": "idle"})
    assert entity.fired == [(VERIFICATION_EVENT_CHAIN_VERIFIED, {"chain_valid": True, "trigger": "idle"})]
    async_dispatcher_send(hass, verification_event_signal(entry.entry_id), "something_else", {})
    assert len(entity.fired) == 1
    for unsub in on_remove:
        unsub()
    assert dispatched(hass, verification_event_signal(entry.entry_id)) == []


# ─── tamper events per Canary ──────────────────────────────────────────


def test_the_tamper_entity_fires_on_live_publishes_naming_an_advertised_type():
    hass = HomeAssistant()
    hass.data = {}
    entry = make_entry(hass)
    entry.runtime_data.devices["canary01"] = {"status": None, "online": True}
    entity = event_platform.SecuraCVCanaryTamperEventEntity("securacv", "canary01", entry)
    assert entity.unique_id == "securacv_canary_canary01_tamper_events"
    assert entity.event_types == list(ALL_TAMPER_TYPES)
    assert entity.available is True
    _added(entity, hass)
    (topic, callback) = hass.data["_securacv_test_mqtt_subscriptions"][0]
    assert topic == "securacv/canary01/tamper"

    callback(_msg('{"type": "sd_remove", "timestamp": "t1"}', topic=topic))
    callback(_msg('{"type": "sd_remove", "timestamp": "t1"}', topic=topic))  # the same publish twice
    callback(_msg('{"type": "sd_remove", "timestamp": "t2"}', topic=topic))
    callback(_msg('{"type": "enclosure"}', retain=True, topic=topic))  # retained: a replay
    callback(_msg('{"type": "audio_anomaly"}', topic=topic))  # a FUTURE_* type: not advertised
    callback(_msg('{"detail": "no type"}', topic=topic))
    callback(_msg("junk", topic=topic))
    assert entity.fired == [
        ("sd_remove", {"kind": "sd_remove", "device_id": "canary01"}),
        ("sd_remove", {"kind": "sd_remove", "device_id": "canary01"}),
    ]
    entry.runtime_data.devices["canary01"]["online"] = False
    assert entity.available is False


def test_a_tamper_publish_that_discovers_the_device_is_not_lost():
    hass = HomeAssistant()
    hass.data = {}
    entry = make_entry(hass, data={CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv"})
    added: list = []
    run(event_platform._async_setup_tamper_events(hass, entry, "securacv", added.extend))
    topics = {t: cb for t, cb in hass.data["_securacv_test_mqtt_subscriptions"]}
    assert set(topics) == {"securacv/+/status", "securacv/+/tamper"}
    topics["securacv/+/tamper"](_msg('{"type": "motion"}', topic="securacv/canary02/tamper"))
    topics["securacv/+/status"](_msg('{"status": "online"}', topic="securacv/canary02/status"))
    topics["securacv/+/status"](_msg('{"status": "online"}', topic="securacv/canary03/status"))
    topics["securacv/+/status"](_msg('{"status": "online"}', topic="securacv/bad id/status"))
    assert [e._device_id for e in added] == ["canary02", "canary03"]
    _added(added[0], hass)
    assert added[0].fired == [("motion", {"kind": "motion", "device_id": "canary02"})]
    _added(added[1], hass)
    assert not hasattr(added[1], "fired")


def test_event_py_spells_no_event_type_itself():
    """The dictionary linter's rule, held beside the code too: every id the
    witness entities can fire is read from const.WITNESS_EVENT_TYPES."""
    source = (PACKAGE_DIR / "event.py").read_text(encoding="utf-8")
    assert "WITNESS_EVENT_TYPES" in source
    for event_id in EVENT_TYPE_METADATA:
        assert not re.search(rf"""["']{re.escape(event_id)}["']""", source), event_id
