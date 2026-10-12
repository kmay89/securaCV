"""The integration's own chain walk (D10) through the real coordinator: the
shared sealed-log vector, a tampered log, an incremental walk, persistence."""
from __future__ import annotations

import copy
import json
from datetime import datetime, timezone, timedelta

from freezegun.api import FrozenDateTimeFactory
from homeassistant.core import HomeAssistant
from pytest_homeassistant_custom_component.common import async_fire_mqtt_message, async_fire_time_changed
from pytest_homeassistant_custom_component.typing import MqttMockHAClient

from conftest import SEALED_LOG_VECTOR, VECTOR_KEY, VECTOR_NOW, KernelMock, bridge_event, bucket_start, settle
from custom_components.securacv.const import DOMAIN

CHAIN_VALID = "binary_sensor.securacv_chain_valid"
CHAIN_LENGTH = "sensor.securacv_chain_length"
VERIFICATION = "event.securacv_chain_verification"
LAST_EVENT = "sensor.securacv_securacv_last_event"


def push(hass: HomeAssistant, topic: str, payload: dict, *, retain: bool = False) -> None:
    async_fire_mqtt_message(hass, topic, json.dumps(payload), qos=1, retain=retain)


async def test_the_shared_vector_verifies_and_marks_its_events_verified(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """tests/fixtures/envelope/sealed_log_document_vector.json walks clean in
    Python on a live core, and an event it covers renders Verified ✓."""
    freezer.move_to(datetime.fromtimestamp(VECTOR_NOW, tz=timezone.utc))
    kernel.verifying_key = VECTOR_KEY
    kernel.sealed_log = copy.deepcopy(SEALED_LOG_VECTOR)
    await setup_kernel_entry(enable_mqtt=True, verifying_key=VECTOR_KEY)

    state = hass.states.get(CHAIN_VALID)
    assert state.state == "on"
    assert state.attributes["verified_seq"] == 3
    assert state.attributes["verified_entries"] == 3
    assert state.attributes["head_seq"] == 3
    assert state.attributes["failed_seq"] is None
    assert state.attributes["key_mismatch"] is False
    assert state.attributes["verifying"] is False
    assert state.attributes["last_trigger"] == "scheduled"
    assert state.attributes["error"] is None
    assert hass.states.get(CHAIN_LENGTH).state == "3"
    assert hass.states.get(CHAIN_LENGTH).attributes["latest_hash"] == SEALED_LOG_VECTOR["head"]["entry_hash"]
    verification = hass.states.get(VERIFICATION)
    assert verification.attributes["event_type"] == "chain_verified"
    assert verification.attributes["events_verified"] == 3
    assert verification.attributes["trigger"] == "scheduled"

    # Entry 1 of the vector: zone:a at bucket 1700000400.
    covered = bridge_event(zone="a", bucket=1700000400, seq=1)
    push(hass, "witness/events", covered)
    await hass.async_block_till_done()
    row = hass.states.get("event.securacv_a")
    assert row.attributes["verification"] == "verified"
    assert row.attributes["verified"] is True
    assert row.attributes["trust_reason"] == "ha_verified_chain"
    assert row.attributes["verified_bucket_start"] == int(VECTOR_NOW // 600) * 600

    # An event the walk never saw stays Sealed ✓.
    push(hass, "witness/events", bridge_event(zone="z", bucket=1700000400, seq=99))
    await hass.async_block_till_done()
    row = hass.states.get("event.securacv_z")
    assert row.attributes["verification"] == "sealed"
    assert row.attributes["verified"] is False
    assert row.attributes["trust_reason"] == "kernel_sealed"
    assert row.attributes["verified_bucket_start"] is None

    # Entry 2 (zone:b, true bucket 1700001000) as the bridge publishes it: the
    # kernel's export view re-draws the bucket start by -120/-60/0/+60/+120 s
    # on every fetch, so the wire never has to carry the grid start for the
    # row to be covered — and every further draw is the SAME event (no second
    # fire), the row keeping the window of the copy that fired it.
    push(hass, "witness/events", bridge_event(zone="b", bucket=1700001000 - 120, seq=2))
    await hass.async_block_till_done()
    row = hass.states.get("event.securacv_b")
    assert row.attributes["verification"] == "verified"
    assert row.attributes["verified"] is True
    assert row.attributes["time_bucket"]["start_epoch_s"] == 1700001000 - 120
    fired = row.state
    for offset in (-60, 0, 60, 120):
        push(hass, "witness/events", bridge_event(zone="b", bucket=1700001000 + offset, seq=2))
        await hass.async_block_till_done()
        row = hass.states.get("event.securacv_b")
        assert row.state == fired, f"a re-drawn copy ({offset:+d} s) is not a new event"
        assert row.attributes["verification"] == "verified"
        assert row.attributes["time_bucket"]["start_epoch_s"] == 1700001000 - 120


async def test_a_tampered_entry_fails_the_walk_and_every_row(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """One altered payload: the walk stops there, the sensor says off, rows say Failed ⚠."""
    kernel.kernel.seal_event(zone="a")
    kernel.kernel.seal_event(zone="b")
    kernel.kernel.seal_event(zone="c")
    kernel.kernel.entries[1]["payload"] = kernel.kernel.entries[1]["payload"].replace("zone:b", "zone:x")
    await setup_kernel_entry(enable_mqtt=True)

    state = hass.states.get(CHAIN_VALID)
    assert state.state == "off"
    assert state.attributes["failed_seq"] == 2
    assert "entry_hash mismatch" in state.attributes["error"]
    verification = hass.states.get(VERIFICATION)
    assert verification.attributes["event_type"] == "chain_verification_failed"
    assert verification.attributes["failed_seq"] == 2
    assert verification.attributes["error"] == "entry_hash_mismatch"

    push(hass, "witness/last_event", bridge_event(zone="a", seq=1), retain=True)
    await hass.async_block_till_done()
    row = hass.states.get(LAST_EVENT)
    assert row.attributes["verification"] == "failed"
    assert row.attributes["trust_reason"] == "ha_verify_failed"
    assert row.attributes["verified"] is False


async def test_a_served_key_other_than_the_pin_is_a_key_change_not_a_failure(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """The sealed log signed by another key → Repairs tofu_mismatch, trust_reason kernel_key_mismatch."""
    from homeassistant.helpers import issue_registry as ir

    from conftest import SigningKernel

    pinned = SigningKernel()
    kernel.kernel.seal_event(zone="a")
    entry = await setup_kernel_entry(enable_mqtt=True, verifying_key=pinned.verifying_key)
    # /api/identity serves the pinned key; the document is signed by the kernel's real key.
    kernel.verifying_key = pinned.verifying_key
    await hass.services.async_call(DOMAIN, "verify", {}, blocking=True, return_response=True)
    await settle(hass)

    state = hass.states.get(CHAIN_VALID)
    # Never `off`: every consumer renders `off` as "Verification failed", and
    # a key change is a Repairs matter. Unknown, with the words that say so.
    assert state.state == "unknown"
    assert state.attributes["key_mismatch"] is True
    assert state.attributes["trust_reason"] == "kernel_key_mismatch"
    assert state.attributes["error"] == "the witness key changed; confirm it in Settings → Repairs"
    assert state.attributes["failed_seq"] is None
    assert ir.async_get(hass).async_get_issue(DOMAIN, f"tofu_mismatch_{entry.entry_id}") is not None
    push(hass, "witness/last_event", bridge_event(zone="a", seq=1), retain=True)
    await hass.async_block_till_done()
    assert hass.states.get(LAST_EVENT).attributes["trust_reason"] == "kernel_key_mismatch"
    assert hass.states.get(LAST_EVENT).attributes["verification"] == "sealed", "a key change is never Failed (D10)"
    assert hass.states.get(LAST_EVENT).attributes["verified"] is False


async def test_two_entries_sharing_one_key_are_verified_one_by_one(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """A dog and a cat in one zone in one bucket are two sealed entries with
    one (event_type, zone_id, bucket) key. Walking the dog's entry must not
    render the cat's row Verified before the cat's own signature is walked:
    the row's `seq` names its entry (C5), and the key alone proves nothing."""
    bucket = bucket_start()
    kernel.kernel.seal_event(event_type="BoundaryCrossingObjectSmall", zone="yard", bucket=bucket)
    await setup_kernel_entry(enable_mqtt=True)
    assert hass.states.get(CHAIN_VALID).attributes["verified_seq"] == 1

    dog = bridge_event(event_type="BoundaryCrossingObjectSmall", zone="yard", bucket=bucket, seq=1)
    push(hass, "witness/events", dog)
    await hass.async_block_till_done()
    row = hass.states.get("event.securacv_yard")
    assert row.attributes["verification"] == "verified" and row.attributes["seq"] == 1

    # The cat: sealed after the walk, same key, seq 2. The bridge's retained
    # last_event shows it (the live feed dedupes it onto the dog's key, as
    # C5 states) — and it is Sealed, not Verified on the dog's signature.
    kernel.kernel.seal_event(event_type="BoundaryCrossingObjectSmall", zone="yard", bucket=bucket)
    cat = bridge_event(event_type="BoundaryCrossingObjectSmall", zone="yard", bucket=bucket, seq=2)
    push(hass, "witness/last_event", cat, retain=True)
    await hass.async_block_till_done()
    last = hass.states.get(LAST_EVENT)
    assert last.attributes["seq"] == 2
    assert last.attributes["verification"] == "sealed"
    assert last.attributes["verified"] is False
    assert last.attributes["trust_reason"] == "kernel_sealed"

    # The next walk (the 15-minute backstop) verifies the cat's entry; the
    # sensor then reads Verified for seq 2 on its own proof.
    freezer.tick(timedelta(minutes=15, seconds=1))
    async_fire_time_changed(hass)
    await settle(hass)
    assert hass.states.get(CHAIN_VALID).attributes["verified_seq"] == 2
    last = hass.states.get(LAST_EVENT)
    assert last.attributes["seq"] == 2
    assert last.attributes["verification"] == "verified"
    assert last.attributes["verified"] is True


async def test_the_walk_is_incremental_idle_triggered_and_persisted(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """New entries arm a walk 60 s after they stop; the cursor survives a reload."""
    kernel.kernel.seal_event(zone="a")
    entry = await setup_kernel_entry(enable_mqtt=True)
    assert hass.states.get(CHAIN_VALID).attributes["verified_seq"] == 1
    walks_before = len(kernel.calls("/api/sealed-log"))

    kernel.kernel.seal_event(zone="b")
    push(hass, "witness/events", bridge_event(zone="b", seq=2))
    await hass.async_block_till_done()
    assert len(kernel.calls("/api/sealed-log")) == walks_before, "a new entry walks after the idle delay, not at once"
    freezer.tick(timedelta(seconds=61))
    async_fire_time_changed(hass)
    await settle(hass)
    calls = kernel.calls("/api/sealed-log")
    assert len(calls) == walks_before + 1
    assert calls[-1][1].query.get("from_seq") == "1"
    state = hass.states.get(CHAIN_VALID)
    assert state.attributes["verified_seq"] == 2
    assert state.attributes["last_trigger"] == "idle"
    assert hass.states.get("event.securacv_b").attributes["verification"] == "sealed", "the row rendered before the walk"

    # The 15-minute backstop walks too.
    freezer.tick(timedelta(minutes=15, seconds=1))
    async_fire_time_changed(hass)
    await settle(hass)
    assert hass.states.get(CHAIN_VALID).attributes["last_trigger"] == "scheduled"

    # Reload: the first walk continues from the persisted cursor.
    assert await hass.config_entries.async_reload(entry.entry_id)
    await settle(hass)
    assert kernel.calls("/api/sealed-log")[-1][1].query.get("from_seq") == "2"
    assert hass.states.get(CHAIN_VALID).attributes["verified_entries"] == 2


async def test_a_replaced_log_is_reported_until_a_person_re_anchors(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """The head moving backwards is a failure; only Verify now re-anchors."""
    for zone in ("a", "b", "c"):
        kernel.kernel.seal_event(zone=zone)
    await setup_kernel_entry(enable_mqtt=True)
    assert hass.states.get(CHAIN_VALID).attributes["verified_seq"] == 3

    from conftest import SigningKernel

    fresh = SigningKernel()
    fresh.seal_event(zone="a")
    kernel.kernel = fresh
    kernel.verifying_key = fresh.verifying_key
    # The pin still says the old key: that is a key change first.
    freezer.tick(timedelta(minutes=15, seconds=1))
    async_fire_time_changed(hass)
    await settle(hass)
    assert hass.states.get(CHAIN_VALID).attributes["key_mismatch"] is True
