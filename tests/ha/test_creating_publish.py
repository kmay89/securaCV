"""The events publish that creates a Canary's entities, on a real core (sweep HA44).

Last Event, Motion and Occupancy are created by a Canary's first publish on
its events topic, and they subscribe only once they are added, after that
delivery. Events are not retained, so before HA44 that first event was never
shown: Last Event stayed unknown, and an event that asserts Motion or
Occupancy (the dictionary's Apple Home vocabulary, HOMEKIT_EVENT_SIGNALS) went
unseen. The discovery callbacks now hand the creating publish's body over, and
each entity applies it once, after its subscribe.

Here the real MQTT integration delivers the publishes (over a mocked paho
client): a live event creates the entities and is shown by them; a broker that
also retained it sends the entities' fresh subscriptions its copy, which the
core flags retained and which is that same publish, so nothing records or arms
twice; the next event takes the ordinary path. The canary-wap's presence words
map to no Apple Home signal (handed up, HA47), so the event here is the
dictionary's, from a Canary that speaks it.

Host-tested: a real Home Assistant core over a mocked paho client, not a unit
on a bench and a real broker.
"""
from __future__ import annotations

import json
from typing import Any

import pytest
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.common import async_fire_mqtt_message
from pytest_homeassistant_custom_component.typing import MqttMockHAClient

from conftest import make_mqtt_only_entry, settle
from custom_components.securacv import binary_sensor as bs_platform
from custom_components.securacv import sensor as sensor_platform
from custom_components.securacv.const import DOMAIN, HOMEKIT_MOTION_HOLD_SECONDS

PORCH = "porch01"
TOPIC = f"securacv/{PORCH}/events"
SIGNALS = {"motion": "BoundaryCrossingObjectLarge", "occupancy": "PresenceInRestrictedZone"}


def event(event_type: str, event_id: int = 41) -> str:
    return json.dumps(
        {
            "event_id": event_id,
            "event_type": event_type,
            "timestamp": 3600,
            "zone": "porch",
            "confidence": "likely",
            "signed": False,
        }
    )


class Deliveries:
    """Every voice-brief record Last Event makes and every hold window the
    projected sensors arm, passed through to the real calls."""

    def __init__(self, monkeypatch: pytest.MonkeyPatch) -> None:
        self.recorded: list[tuple[str, Any]] = []
        self.armed: list[float] = []
        record_last_event = sensor_platform._record_last_event
        call_later = bs_platform.async_call_later

        def _record(hass, entry, device_id, event_type) -> None:
            self.recorded.append((device_id, event_type))
            record_last_event(hass, entry, device_id, event_type)

        def _call_later(hass, delay, action):
            self.armed.append(delay)
            return call_later(hass, delay, action)

        monkeypatch.setattr(sensor_platform, "_record_last_event", _record)
        monkeypatch.setattr(bs_platform, "async_call_later", _call_later)


def entity_id(hass: HomeAssistant, platform: str, key: str) -> str:
    found = er.async_get(hass).async_get_entity_id(platform, DOMAIN, f"{DOMAIN}_canary_{PORCH}_{key}")
    assert found is not None, f"no {platform} {key} for {PORCH}"
    return found


async def publish(hass: HomeAssistant, body: str, *, retained: bool) -> None:
    async_fire_mqtt_message(hass, TOPIC, body, qos=0, retain=retained)
    await settle(hass)


@pytest.mark.parametrize("key", sorted(SIGNALS))
async def test_the_creating_event_is_shown_once_by_the_entities_it_creates(
    hass: HomeAssistant, mqtt_mock: MqttMockHAClient, monkeypatch: pytest.MonkeyPatch, key: str
) -> None:
    seen = Deliveries(monkeypatch)
    entry = make_mqtt_only_entry()
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await settle(hass)

    await publish(hass, event(SIGNALS[key]), retained=False)
    other = ({"motion", "occupancy"} - {key}).pop()
    last = hass.states.get(entity_id(hass, "sensor", "last_event"))
    assert last.state == SIGNALS[key]
    assert last.attributes["verified"] is False, "an unsigned event says so"
    asserted = hass.states.get(entity_id(hass, "binary_sensor", key))
    assert asserted.state == "on"
    assert asserted.attributes["verified"] is False
    assert hass.states.get(entity_id(hass, "binary_sensor", other)).state == "off"
    assert seen.recorded == [(PORCH, SIGNALS[key])]
    assert seen.armed == [HOMEKIT_MOTION_HOLD_SECONDS]

    # A broker that retained the publish too sends the entities' fresh
    # subscriptions its copy, flagged retained: the same publish, applied once.
    await publish(hass, event(SIGNALS[key]), retained=True)
    assert seen.recorded == [(PORCH, SIGNALS[key])]
    assert seen.armed == [HOMEKIT_MOTION_HOLD_SECONDS]

    # The next event takes the ordinary path.
    await publish(hass, event(SIGNALS[key], 42), retained=False)
    assert seen.recorded == [(PORCH, SIGNALS[key])] * 2
    assert seen.armed == [HOMEKIT_MOTION_HOLD_SECONDS] * 2
    assert hass.states.get(entity_id(hass, "binary_sensor", key)).state == "on"


async def test_a_live_event_before_the_retained_copy_does_not_reopen_the_creating_event(
    hass: HomeAssistant, mqtt_mock: MqttMockHAClient, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The core batches a new SUBSCRIBE (mqtt/client.py SUBSCRIBE_COOLDOWN), so
    a second event can reach the entities' fresh subscriptions live before the
    broker's retained copy of the creating publish does. The second is applied
    as it comes, and the late copy is still the creating publish's echo:
    nothing records or arms twice, and Last Event keeps the newer event."""
    seen = Deliveries(monkeypatch)
    entry = make_mqtt_only_entry()
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await settle(hass)

    first, second = event(SIGNALS["motion"], 41), event(SIGNALS["occupancy"], 42)
    await publish(hass, first, retained=False)
    await publish(hass, second, retained=False)
    both = [(PORCH, SIGNALS["motion"]), (PORCH, SIGNALS["occupancy"])]
    assert seen.recorded == both
    assert seen.armed == [HOMEKIT_MOTION_HOLD_SECONDS] * 2

    # The broker kept the first (the second was not retained) and sends it
    # to the fresh subscriptions now, flagged retained.
    await publish(hass, first, retained=True)
    assert seen.recorded == both
    assert seen.armed == [HOMEKIT_MOTION_HOLD_SECONDS] * 2, "each event arms one hold window"
    assert hass.states.get(entity_id(hass, "sensor", "last_event")).state == SIGNALS["occupancy"]
