"""The retained claim (C3): published on load and hourly, withdrawn only on removal."""
from __future__ import annotations

import json
from datetime import timedelta

from freezegun.api import FrozenDateTimeFactory
from homeassistant.core import HomeAssistant
from pytest_homeassistant_custom_component.common import async_fire_time_changed
from pytest_homeassistant_custom_component.typing import MqttMockHAClient

from conftest import CLAIM_NONCE, KERNEL_DEVICE_ID, PAIRED_TOKEN_ID, KernelMock

CLAIM_TOPIC = f"witness/claim/{KERNEL_DEVICE_ID}"


def claim_publishes(mqtt_mock: MqttMockHAClient) -> list[tuple]:
    """Every publish to the claim topic as (payload, qos, retain)."""
    return [
        (call.args[1], call.args[2], call.args[3])
        for call in mqtt_mock.async_publish.call_args_list
        if call.args and call.args[0] == CLAIM_TOPIC
    ]


async def test_claim_is_published_on_load_and_hourly(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """Retained, qos 1, the nonce and a bucket start — never the entry id."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    publishes = claim_publishes(mqtt_mock)
    assert len(publishes) == 1
    payload, qos, retain = publishes[0]
    assert (qos, retain) == (1, True)
    document = json.loads(payload)
    assert document == {"v": 1, "claimed": True, "nonce": CLAIM_NONCE, "claimed_bucket_start": document["claimed_bucket_start"]}
    assert entry.entry_id not in payload
    assert entry.runtime_data.claim_published is True

    freezer.tick(timedelta(hours=1, seconds=1))
    async_fire_time_changed(hass)
    await hass.async_block_till_done()
    publishes = claim_publishes(mqtt_mock)
    assert len(publishes) == 2
    assert json.loads(publishes[1][0])["nonce"] == CLAIM_NONCE


async def test_unload_keeps_the_claim_but_removal_withdraws_it_and_revokes_the_token(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """A reload must not flip the bridge's entities; deleting the entry clears everything."""
    entry = await setup_kernel_entry(auth_mode="paired", enable_mqtt=True)
    assert len(claim_publishes(mqtt_mock)) == 1

    assert await hass.config_entries.async_reload(entry.entry_id)
    await hass.async_block_till_done()
    publishes = claim_publishes(mqtt_mock)
    assert len(publishes) == 2 and all(p[0] not in (b"", "") for p in publishes)

    assert await hass.config_entries.async_remove(entry.entry_id)
    await hass.async_block_till_done()
    publishes = claim_publishes(mqtt_mock)
    assert publishes[-1] == (b"", 1, True)
    assert kernel.revoked == [PAIRED_TOKEN_ID]
    assert hass.config_entries.async_entries("securacv") == []


async def test_no_claim_without_mqtt(hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry) -> None:
    """A kernel-only entry has no bridge to tell; nothing is published and removal is quiet."""
    entry = await setup_kernel_entry()
    assert entry.runtime_data.claim_published is False
    assert await hass.config_entries.async_remove(entry.entry_id)
    await hass.async_block_till_done()
    assert kernel.revoked == []
