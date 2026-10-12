"""Setup and unload on a real core: platforms, device, subscriptions, retry."""
from __future__ import annotations

import json

from homeassistant.config_entries import ConfigEntryState
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.typing import MqttMockHAClient

from conftest import (
    CLAIM_NONCE,
    KERNEL_DEVICE_ID,
    KERNEL_URL,
    KERNEL_VERSION,
    STATIC_TOKEN,
    KernelMock,
    kernel_device,
    make_kernel_entry,
    settle,
    subscribed_topics,
)
from custom_components.securacv.const import DOMAIN

KERNEL_ENTITY_KEYS = (
    "latest_event",
    "chain_length",
    "daily_digest",
    "kernel_fingerprint",
    "storage_health",
    "storage_free_pct",
    "storage_wear_pct",
    "storage_write_rate",
    "soc_temperature",
    "kernel_online",
    "chain_valid",
    "frigate_link",
    "storage_replace",
    "verification_events",
    "verify_now",
    "export_evidence",
)

KERNEL_PUSH_TOPICS = (
    "witness/last_event",
    "witness/events",
    "witness/digest",
    "witness/chain_problem",
    "witness/chain_problem/attrs",
    "witness/status",
    "witness/claim_state",
    "witness/frigate_link",
)
CANARY_TOPICS = ("securacv/+/status", "securacv/+/availability", "securacv/+/health")


async def test_kernel_only_entry_loads_every_platform(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """A kernel-only entry loads, creates the kernel device and its entities."""
    entry = await setup_kernel_entry()
    assert entry.state is ConfigEntryState.LOADED

    device = kernel_device(hass, entry)
    assert device is not None
    assert (DOMAIN, KERNEL_URL) in device.identifiers
    assert device.name == "SecuraCV"
    assert device.model == "Privacy Witness Kernel"
    assert device.serial_number == KERNEL_DEVICE_ID
    assert device.sw_version == KERNEL_VERSION
    assert device.configuration_url == KERNEL_URL

    entity_registry = er.async_get(hass)
    entries = er.async_entries_for_config_entry(entity_registry, entry.entry_id)
    assert {e.domain for e in entries} == {"sensor", "binary_sensor", "event", "button"}
    unique_ids = {e.unique_id for e in entries}
    assert unique_ids == {f"{DOMAIN}_{entry.entry_id}_{key}" for key in KERNEL_ENTITY_KEYS}
    # Every kernel entity hangs off the kernel device; none sets an entity id itself.
    assert {e.device_id for e in entries} == {device.id}
    assert all(e.entity_id.split(".", 1)[1].startswith("securacv_") for e in entries)

    # The backstop read the kernel over HTTP with the configured static token.
    assert kernel.calls("/health")
    identity_calls = kernel.calls("/api/identity")
    assert identity_calls
    assert kernel.auth_headers("/api/identity") == [f"Bearer {STATIC_TOKEN}"] * len(identity_calls)

    assert await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.state is ConfigEntryState.NOT_LOADED
    assert entry.runtime_data is None if hasattr(entry, "runtime_data") else True


async def test_both_mode_subscribes_claims_and_unloads_without_leaks(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """With MQTT: the push topics and the Canary wildcards are subscribed, the
    retained claim goes out (qos 1), and unload releases every subscription."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    assert entry.state is ConfigEntryState.LOADED

    topics = subscribed_topics(hass)
    for topic in KERNEL_PUSH_TOPICS + CANARY_TOPICS:
        assert topic in topics, topic

    claim_calls = [
        call for call in mqtt_mock.async_publish.call_args_list
        if call.args and call.args[0] == f"witness/claim/{KERNEL_DEVICE_ID}"
    ]
    assert len(claim_calls) == 1
    assert claim_calls[0].args[2:4] == (1, True), "retained, qos 1"
    payload = json.loads(claim_calls[-1].args[1])
    assert payload["v"] == 1 and payload["claimed"] is True
    assert payload["nonce"] == CLAIM_NONCE
    assert payload["claimed_bucket_start"] % 600 == 0
    assert "entry_id" not in json.dumps(payload) and entry.entry_id not in json.dumps(payload)

    assert await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.state is ConfigEntryState.NOT_LOADED
    # What is left under our prefixes is the MQTT integration's own discovery
    # subscription for this manifest's `mqtt` filter — never ours.
    leaked = {t for t in subscribed_topics(hass) if t.startswith(("witness/", "securacv/"))}
    assert leaked == {"securacv/#", "witness/+/announce"}
    # Unloading never withdraws the claim (only removal does).
    empty_claims = [
        call for call in mqtt_mock.async_publish.call_args_list
        if call.args and call.args[0] == f"witness/claim/{KERNEL_DEVICE_ID}" and call.args[1] in (b"", "")
    ]
    assert empty_claims == []


async def test_both_mode_without_mqtt_is_not_ready(hass: HomeAssistant, kernel: KernelMock) -> None:
    """test-before-setup: no MQTT entry at all → ConfigEntryNotReady, retried."""
    entry = make_kernel_entry(enable_mqtt=True, verifying_key=kernel.verifying_key)
    entry.add_to_hass(hass)
    assert not await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.state is ConfigEntryState.SETUP_RETRY
    assert "MQTT" in (entry.reason or "")
    # Nothing was asked of the kernel before the broker was there.
    assert kernel.calls("/api/identity") == []


async def test_kernel_down_in_kernel_only_mode_retries(hass: HomeAssistant, kernel: KernelMock) -> None:
    """Kernel-only mode keeps the strict first refresh: an unreachable kernel is SETUP_RETRY."""
    kernel.connection_down = True
    entry = make_kernel_entry(verifying_key=kernel.verifying_key)
    entry.add_to_hass(hass)
    assert not await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.state is ConfigEntryState.SETUP_RETRY


async def test_both_mode_tolerates_a_kernel_that_is_down(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """`both` mode loads with the kernel down so the Canaries keep reporting;
    the kernel entities read unavailable."""
    kernel.connection_down = True
    entry = await setup_kernel_entry(enable_mqtt=True)
    assert entry.state is ConfigEntryState.LOADED
    state = hass.states.get("binary_sensor.securacv_chain_valid")
    assert state is not None
    assert state.state == "unavailable"
    assert hass.states.get("sensor.securacv_securacv_last_event").state == "unavailable"


async def test_reload_keeps_one_set_of_entities(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """A reload re-creates the same entities (same unique ids), nothing doubled."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    registry = er.async_get(hass)
    before = sorted(e.unique_id for e in er.async_entries_for_config_entry(registry, entry.entry_id))
    assert await hass.config_entries.async_reload(entry.entry_id)
    await settle(hass)
    assert entry.state is ConfigEntryState.LOADED
    after = sorted(e.unique_id for e in er.async_entries_for_config_entry(registry, entry.entry_id))
    assert after == before
    topics = subscribed_topics(hass)
    assert all(t in topics for t in KERNEL_PUSH_TOPICS)
