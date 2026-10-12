"""The push coordinator on a real MQTT integration: retained state feeds the
entities, only live events fire the event entities, the backstop fills gaps.

A broker delivers a topic's retained message once, when the subscription is
made; every later publish arrives live (retain flag clear). Home Assistant's
MQTT client enforces the same rule per subscription, so these tests send the
first message on a topic retained and the updates live, as a broker would.
"""
from __future__ import annotations

import json
from datetime import timedelta

from freezegun.api import FrozenDateTimeFactory
from homeassistant.core import HomeAssistant
from pytest_homeassistant_custom_component.common import async_fire_mqtt_message, async_fire_time_changed
from pytest_homeassistant_custom_component.typing import MqttMockHAClient

from conftest import KernelMock, bridge_event, bucket_start, digest_document, settle

LAST_EVENT = "sensor.securacv_securacv_last_event"
DIGEST = "sensor.securacv_daily_digest"
CHAIN_VALID = "binary_sensor.securacv_chain_valid"
ONLINE = "binary_sensor.securacv_online"
FRIGATE_LINK = "binary_sensor.securacv_frigate_link"
WITNESS_FRONT_DOOR = "event.securacv_front_door"


def fire(hass: HomeAssistant, topic: str, payload, *, retain: bool = False) -> None:
    """One inbound MQTT message, JSON-encoded when a dict."""
    if isinstance(payload, dict):
        payload = json.dumps(payload)
    async_fire_mqtt_message(hass, topic, payload, qos=1, retain=retain)


async def backstop(hass: HomeAssistant, freezer: FrozenDateTimeFactory) -> None:
    """Advance past the 5-minute HTTP backstop and let it run."""
    freezer.tick(timedelta(minutes=5, seconds=1))
    async_fire_time_changed(hass)
    await settle(hass)


async def test_retained_state_topics_feed_the_sensors(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """last_event and digest arrive retained and land on the sensors with the C5 table."""
    await setup_kernel_entry(enable_mqtt=True)
    assert hass.states.get(LAST_EVENT).state == "unknown"

    event = bridge_event(zone="front_door", seq=7, verified_ok=True, verified_bucket_start=bucket_start() - 600)
    fire(hass, "witness/last_event", event, retain=True)
    await hass.async_block_till_done()
    state = hass.states.get(LAST_EVENT)
    assert state.state == "BoundaryCrossingObjectLarge"
    attrs = state.attributes
    assert attrs["zone_id"] == "zone:front_door"
    assert attrs["zone"] == "front_door"
    assert attrs["time_bucket"] == {"start_epoch_s": event["time_bucket_start"], "size_s": 600}
    assert attrs["confidence"] == 0.87
    assert attrs["attestation"] == "adapter"
    assert attrs["friendly_event"] == "Person or vehicle"
    assert attrs["kernel_device_id"] == "pwk_1a2b3c4d"
    assert attrs["signed"] is True
    # The kernel's own self-report rides along as an attribute, never as "verified".
    assert attrs["kernel_reported_chain_ok"] is True
    assert attrs["kernel_verified_bucket_start"] == bucket_start() - 600
    assert attrs["verification"] == "sealed"
    assert attrs["verified"] is False
    assert attrs["trust_reason"] == "kernel_sealed"
    assert attrs["seq"] == 7
    # The one dynamic icon in code: the event type's.
    assert attrs["icon"] == "mdi:car"

    fire(hass, "witness/digest", digest_document(total=5), retain=True)
    await hass.async_block_till_done()
    digest = hass.states.get(DIGEST)
    assert digest.state == "5"
    assert digest.attributes["per_zone"] == {"zone:front_door": 5}
    assert digest.attributes["retention_days"] == 30

    # A live update replaces the retained one.
    fire(hass, "witness/digest", digest_document(total=6))
    await hass.async_block_till_done()
    assert hass.states.get(DIGEST).state == "6"


async def test_a_row_older_than_the_live_window_shows_but_never_fires(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """A non-retained message whose bucket is older than two bucket sizes —
    a bridge that restarted or was told to replay its log, a backstop
    catching up after an outage — updates the last-event sensor and never
    fires an event entity (so no automation sees a camera seeing someone now)."""
    await setup_kernel_entry(enable_mqtt=True)
    assert hass.states.get(WITNESS_FRONT_DOOR) is None

    stale = bridge_event(zone="front_door", bucket=bucket_start() - 3 * 600, seq=1)
    fire(hass, "witness/events", stale, retain=False)
    await hass.async_block_till_done()
    assert hass.states.get(WITNESS_FRONT_DOOR) is None, "a stale row must not create or fire the entity"
    assert hass.states.get(LAST_EVENT).attributes["time_bucket"]["start_epoch_s"] == stale["time_bucket_start"]
    assert hass.states.get(LAST_EVENT).attributes["seq"] == 1

    # The backstop serving a newer, still-stale row: the same.
    kernel.latest_event = bridge_event(zone="front_door", bucket=bucket_start() - 2 * 600 - 60, seq=2)
    await backstop(hass, freezer)
    assert hass.states.get(WITNESS_FRONT_DOOR) is None
    assert hass.states.get(LAST_EVENT).attributes["seq"] == 2

    # A live row fires, and the entity shows that one.
    live = bridge_event(zone="front_door", seq=3)
    fire(hass, "witness/events", live, retain=False)
    await hass.async_block_till_done()
    state = hass.states.get(WITNESS_FRONT_DOOR)
    assert state is not None and state.state != "unknown"
    assert state.attributes["seq"] == 3
    assert hass.states.get(LAST_EVENT).attributes["seq"] == 3


async def test_only_live_events_fire_the_witness_entity_once(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """C10: a retained replay never fires; a live event fires exactly once per dedupe key."""
    await setup_kernel_entry(enable_mqtt=True)
    assert hass.states.get(WITNESS_FRONT_DOOR) is None

    event = bridge_event(zone="front_door", seq=1)
    fire(hass, "witness/events", event, retain=True)
    await hass.async_block_till_done()
    assert hass.states.get(WITNESS_FRONT_DOOR) is None, "a retained replay must not create or fire the entity"

    fire(hass, "witness/events", event)
    await hass.async_block_till_done()
    state = hass.states.get(WITNESS_FRONT_DOOR)
    assert state is not None
    assert state.state != "unknown"
    assert state.attributes["event_type"] == "boundary_crossing_object_large"
    assert state.attributes["zone"] == "front_door"
    assert state.attributes["friendly_name"] == "SecuraCV Front door"
    assert state.attributes["verification"] == "sealed"
    first_fired = state.state
    assert set(state.attributes["event_types"]) >= {"boundary_crossing_object_large", "tamper_detected"}

    # The same sealed event again (the bridge's zone topic, a reconnect): no second fire.
    freezer.tick(timedelta(seconds=2))
    fire(hass, "witness/events", event)
    await hass.async_block_till_done()
    assert hass.states.get(WITNESS_FRONT_DOOR).state == first_fired

    # A different bucket is a new event.
    freezer.tick(timedelta(seconds=2))
    fire(hass, "witness/events", bridge_event(zone="front_door", bucket=event["time_bucket_start"] + 600, seq=2))
    await hass.async_block_till_done()
    assert hass.states.get(WITNESS_FRONT_DOOR).state != first_fired
    # The last-event sensor followed the newest bucket.
    assert hass.states.get(LAST_EVENT).attributes["time_bucket"]["start_epoch_s"] == event["time_bucket_start"] + 600

    # A second camera gets its own entity.
    fire(hass, "witness/events", bridge_event(zone="garage", seq=3))
    await hass.async_block_till_done()
    assert hass.states.get("event.securacv_garage") is not None


async def test_the_backstop_fires_a_missed_event_exactly_once(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """/events/latest fills a push gap once; the next backstop does not re-fire it."""
    await setup_kernel_entry(enable_mqtt=True)
    kernel.latest_event = bridge_event(zone="porch", seq=9)

    await backstop(hass, freezer)
    state = hass.states.get("event.securacv_porch")
    assert state is not None and state.state != "unknown"
    fired = state.state
    assert hass.states.get(LAST_EVENT).attributes["zone"] == "porch"

    await backstop(hass, freezer)
    assert hass.states.get("event.securacv_porch").state == fired


async def test_a_rejittered_backstop_copy_of_a_pushed_event_never_refires(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """The kernel's export view re-draws every bucket start by ±0/60/120 s per
    fetch: the bridge published one draw, /events/latest serves another five
    minutes later. One sealed event is one fire and one latest-event slot,
    whatever the draws — the identity is the canonical (grid) bucket."""
    await setup_kernel_entry(enable_mqtt=True)
    true_bucket = bucket_start()
    pushed = bridge_event(zone="front_door", bucket=true_bucket - 120, seq=4)
    fire(hass, "witness/events", pushed)
    await hass.async_block_till_done()
    fired = hass.states.get(WITNESS_FRONT_DOOR).state
    assert fired != "unknown"

    for offset in (60, 120, -60, 0):
        kernel.latest_event = bridge_event(zone="front_door", bucket=true_bucket + offset, seq=4)
        await backstop(hass, freezer)
        assert hass.states.get(WITNESS_FRONT_DOOR).state == fired, offset
        # The push copy keeps the slot: the newer-than test compares canonical starts.
        assert hass.states.get(LAST_EVENT).attributes["time_bucket"]["start_epoch_s"] == true_bucket - 120

    # A genuinely newer bucket, drawn downwards, is still a new event. The
    # four backstops above moved the clock twenty minutes on, so "newer" is
    # the bucket the clock is in NOW: a fixed `true_bucket + 600` would fall
    # out of the live window (two buckets) whenever the test starts late in
    # its own bucket, and the rule under test would rightly refuse to fire.
    newer_bucket = bucket_start()
    assert newer_bucket > true_bucket
    kernel.latest_event = bridge_event(zone="front_door", bucket=newer_bucket - 120, seq=5)
    await backstop(hass, freezer)
    assert hass.states.get(WITNESS_FRONT_DOOR).state != fired
    assert hass.states.get(LAST_EVENT).attributes["time_bucket"]["start_epoch_s"] == newer_bucket - 120


async def test_availability_follows_the_lwt_and_the_backstop(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """Kernel entities are available while witness/status says online OR HTTP answers."""
    await setup_kernel_entry(enable_mqtt=True)
    assert hass.states.get(CHAIN_VALID).state != "unavailable"
    assert hass.states.get(ONLINE).state == "on"

    # The kernel goes down and the backstop notices: unavailable (the LWT has said nothing).
    kernel.connection_down = True
    await backstop(hass, freezer)
    assert hass.states.get(CHAIN_VALID).state == "unavailable"
    # The Online sensor itself stays available to say "off".
    assert hass.states.get(ONLINE).state == "off"

    # The bridge's retained LWT says online: available again.
    fire(hass, "witness/status", "online", retain=True)
    await hass.async_block_till_done()
    assert hass.states.get(CHAIN_VALID).state != "unavailable"
    assert hass.states.get(ONLINE).state == "on"

    # The bridge dies: its last will arrives live.
    fire(hass, "witness/status", "offline")
    await hass.async_block_till_done()
    assert hass.states.get(CHAIN_VALID).state == "unavailable"
    assert hass.states.get(ONLINE).state == "off"

    # HTTP answers again: available through the backstop even with the LWT offline.
    kernel.connection_down = False
    await backstop(hass, freezer)
    assert hass.states.get(CHAIN_VALID).state != "unavailable"
    assert hass.states.get(ONLINE).state == "on"


async def test_frigate_link_reads_connected_and_recent(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """D14: on iff connected AND Frigate was heard within the hour."""
    await setup_kernel_entry(enable_mqtt=True)
    assert hass.states.get(FRIGATE_LINK).state == "unknown"
    link = {
        "v": 1,
        "device_id": "pwk_1a2b3c4d",
        "connected": True,
        "topic_prefix": "frigate",
        "last_message_bucket_start": bucket_start(),
        "last_message_bucket_size": 600,
        "messages_since_start": 12,
        "mode": "events",
    }
    fire(hass, "witness/frigate_link", link, retain=True)
    await hass.async_block_till_done()
    state = hass.states.get(FRIGATE_LINK)
    assert state.state == "on"
    assert state.attributes["device_class"] == "connectivity"
    assert state.attributes["mode"] == "events"
    assert state.attributes["messages_since_start"] == 12
    assert "device_id" not in state.attributes

    fire(hass, "witness/frigate_link", {**link, "last_message_bucket_start": bucket_start() - 2 * 3600})
    await hass.async_block_till_done()
    assert hass.states.get(FRIGATE_LINK).state == "off"

    fire(hass, "witness/frigate_link", {**link, "connected": False})
    await hass.async_block_till_done()
    assert hass.states.get(FRIGATE_LINK).state == "off"

    fire(hass, "witness/frigate_link", {**link, "last_message_bucket_start": None})
    await hass.async_block_till_done()
    assert hass.states.get(FRIGATE_LINK).state == "off", "connected but never heard Frigate is not a link"


async def test_the_bridge_chain_report_is_an_attribute_never_the_verdict(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """witness/chain_problem is the kernel's self-report: kernel_reported_chain_ok, not is_on."""
    await setup_kernel_entry(enable_mqtt=True)
    verified = hass.states.get(CHAIN_VALID)
    assert verified.state == "on", "the integration's own walk of an empty log succeeds"

    fire(hass, "witness/chain_problem", "ON", retain=True)
    fire(hass, "witness/chain_problem/attrs", {"chain_valid": False, "verified_bucket_start": bucket_start()}, retain=True)
    await hass.async_block_till_done()
    state = hass.states.get(CHAIN_VALID)
    assert state.state == "on"
    assert state.attributes["kernel_reported_chain_ok"] is False
    assert state.attributes["kernel_verified_bucket_start"] == bucket_start()

    # A retained replay fired no verification event; a live transition does.
    verification = hass.states.get("event.securacv_chain_verification")
    assert verification.attributes["event_type"] == "chain_verified"
    fire(hass, "witness/chain_problem", "OFF")
    fire(hass, "witness/chain_problem", "ON")
    await hass.async_block_till_done()
    verification = hass.states.get("event.securacv_chain_verification")
    assert verification.attributes["event_type"] == "chain_verification_failed"
    assert verification.attributes["trigger"] == "bridge"
    assert verification.attributes["kernel_reported"] is True
    # The integration's own verdict did not move on the kernel's word.
    assert hass.states.get(CHAIN_VALID).state == "on"


async def test_hostile_payloads_are_dropped(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """Oversized, non-object or non-UTF-8 payloads never reach an entity."""
    await setup_kernel_entry(enable_mqtt=True)
    fire(hass, "witness/last_event", "5")
    fire(hass, "witness/last_event", "[1,2]")
    fire(hass, "witness/last_event", b"\xff\xfe")
    fire(hass, "witness/last_event", "x" * (64 * 1024 + 1))
    fire(hass, "witness/events", {"event_type": "BoundaryCrossingObjectLarge"})
    await hass.async_block_till_done()
    assert hass.states.get(LAST_EVENT).state == "unknown"
    assert hass.states.get(WITNESS_FRONT_DOOR) is None

    # An event type outside the dictionary never becomes an event entity (its
    # `event_types` are the dictionary's); the last-event sensor still shows
    # what the kernel sealed, honestly, by its wire name.
    fire(hass, "witness/events", {**bridge_event(zone="front_door"), "event_type": "NotInTheDictionary"})
    await hass.async_block_till_done()
    assert hass.states.get(WITNESS_FRONT_DOOR) is None
    assert hass.states.get(LAST_EVENT).state == "NotInTheDictionary"
    assert hass.states.get(LAST_EVENT).attributes["friendly_event"] == "NotInTheDictionary"
