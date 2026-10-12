"""The entity registry as a real core builds it: unique ids, devices,
via-device links, translated names, icons.json resolution, categories.

Explicit assertions rather than syrupy snapshots on purpose: the two cells
(the 2026.5 floor and the current release) serialize registry entries
differently, and one .ambr file cannot be right for both.
"""
from __future__ import annotations

import json

from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry as er
from pytest_homeassistant_custom_component.common import async_fire_mqtt_message
from pytest_homeassistant_custom_component.typing import MqttMockHAClient

from conftest import KERNEL_DEVICE_ID, KernelMock, bridge_event, bucket_start, canary_device, kernel_device, settle
from custom_components.securacv.const import ALL_TAMPER_TYPES, ALL_TRANSPORTS, DOMAIN

CANARY = "canary-kitchen-01"
CANARY_KEY = "11" * 32


def fire(hass: HomeAssistant, topic: str, payload, *, retain: bool = False) -> None:
    if isinstance(payload, dict):
        payload = json.dumps(payload)
    async_fire_mqtt_message(hass, topic, payload, qos=1, retain=retain)


STATUS = {"online": True, "firmware_version": "2.4.16", "hardware": "esp32s3", "device_name": "Kitchen cam", "ap_ip": "192.168.4.1", "device_type": "canary-sense"}
HEALTH = {"public_key": CANARY_KEY, "free_heap": 120000, "battery_pct": 80, "sd": {"wear_pct": 3.5}}


async def wake_a_canary(hass: HomeAssistant, device_id: str = CANARY) -> None:
    """Publish what a Canary publishes: status, health, chain, counts, events, transport, tamper.

    The retained status and health are sent twice: the first delivery reaches
    the discovery wildcards and creates the entities, the second is what a
    broker sends to the entities' own fresh subscriptions (retained messages
    are replayed per SUBSCRIBE; the mocked client does not do that by itself).
    """
    fire(hass, f"securacv/{device_id}/status", STATUS, retain=True)
    fire(hass, f"securacv/{device_id}/health", HEALTH, retain=True)
    fire(hass, f"securacv/{device_id}/chain", {"length": 10, "latest_hash": "ab" * 32}, retain=True)
    fire(hass, f"securacv/{device_id}/counts", {"total": 10}, retain=True)
    fire(hass, f"securacv/{device_id}/transport", {"wifi_sta": True}, retain=True)
    fire(hass, f"securacv/{device_id}/tamper", {"type": "enclosure"}, retain=False)
    await hass.async_block_till_done()
    fire(hass, f"securacv/{device_id}/status", STATUS, retain=True)
    fire(hass, f"securacv/{device_id}/health", HEALTH, retain=True)
    await hass.async_block_till_done()


async def test_kernel_entities_names_icons_and_categories(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """Names come from translations, icons from icons.json, categories as declared."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    registry = er.async_get(hass)

    expectations = {
        # entity_id: (friendly name, translation key, entity_category)
        "sensor.securacv_securacv_last_event": ("SecuraCV SecuraCV Last Event", "kernel_last_event", None),
        "sensor.securacv_chain_length": ("SecuraCV Chain Length", "chain_length", None),
        "sensor.securacv_daily_digest": ("SecuraCV Daily digest", "daily_digest", None),
        "sensor.securacv_kernel_key_fingerprint": ("SecuraCV Kernel key fingerprint", "kernel_fingerprint", EntityCategory.DIAGNOSTIC),
        "sensor.securacv_storage_health": ("SecuraCV Storage Health", "storage_health", EntityCategory.DIAGNOSTIC),
        "binary_sensor.securacv_online": ("SecuraCV Online", "kernel_online", None),
        "binary_sensor.securacv_chain_valid": ("SecuraCV Chain Valid", "chain_valid", None),
        "binary_sensor.securacv_frigate_link": ("SecuraCV Frigate link", "frigate_link", EntityCategory.DIAGNOSTIC),
        "binary_sensor.securacv_storage_replacement_recommended": ("SecuraCV Storage Replacement Recommended", "storage_replace", EntityCategory.DIAGNOSTIC),
        "event.securacv_chain_verification": ("SecuraCV Chain verification", "verification_events", None),
        "button.securacv_verify_now": ("SecuraCV Verify now", "verify_now", None),
        "button.securacv_export_evidence": ("SecuraCV Export evidence", "export_evidence", None),
    }
    for entity_id, (name, translation_key, category) in expectations.items():
        state = hass.states.get(entity_id)
        assert state is not None, entity_id
        assert state.attributes["friendly_name"] == name, entity_id
        registered = registry.async_get(entity_id)
        assert registered is not None and registered.entity_category == category, entity_id
        assert registered.config_entry_id == entry.entry_id
        assert registered.translation_key == translation_key, entity_id
        # icons.json is the one source: no entity sets a static icon in code (the
        # frontend resolves the translation key, so the state carries none).
        assert registered.original_icon is None, f"{entity_id} sets a static icon in code"
        assert "icon" not in state.attributes, f"{entity_id} carries a static icon"

    kernel_state = hass.states.get("sensor.securacv_kernel_key_fingerprint")
    assert kernel_state.state == kernel.kernel.fingerprint
    assert kernel_state.attributes["verifying_key"] == kernel.verifying_key

    chain_length = hass.states.get("sensor.securacv_chain_length")
    assert chain_length.state == "0"
    assert chain_length.attributes["unit_of_measurement"] == "blocks"
    # No statistics class: the state switches from the kernel's row count to
    # the walk head seq, and retention prunes the count — either would read
    # as a meter reset under total_increasing.
    assert "state_class" not in chain_length.attributes
    assert chain_length.attributes["source"] in ("walk_head", "kernel_identity")
    assert chain_length.attributes["kernel_chain_length"] == 0
    assert chain_length.attributes["pinned"] is True
    assert chain_length.attributes["algorithm"] == "ed25519"


async def test_canary_entities_hang_off_a_device_linked_to_the_kernel(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """D9/C11: a Canary device, named by its owner, via-device → the kernel device."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    await wake_a_canary(hass)

    kernel_dev = kernel_device(hass, entry)
    canary = canary_device(hass, entry, CANARY)
    assert canary is not None
    assert canary.name == "Kitchen cam"
    assert canary.model == "SecuraCV Canary"
    assert canary.manufacturer == "ERRERlabs"
    assert canary.sw_version == "2.4.16"
    assert canary.hw_version == "esp32s3"
    assert canary.configuration_url == "http://192.168.4.1"
    assert canary.via_device_id == kernel_dev.id

    registry = er.async_get(hass)
    canary_entries = [e for e in er.async_entries_for_config_entry(registry, entry.entry_id) if e.device_id == canary.id]
    unique_ids = {e.unique_id for e in canary_entries}
    prefix = f"{DOMAIN}_canary_{CANARY}_"
    for key in ("online", "chain_valid", "tamper", "sd_replace", "witness_count", "chain_length", "health_status", "gps_fix", "sd_wear", "radar_link", "tamper_events"):
        assert f"{prefix}{key}" in unique_ids, key
    for tamper_type in ALL_TAMPER_TYPES:
        assert f"{prefix}tamper_{tamper_type}" in unique_ids
    for transport in ALL_TRANSPORTS:
        assert f"{prefix}transport_{transport}" in unique_ids
    assert all(e.unique_id.startswith(prefix) for e in canary_entries)

    # Names: "<device name> <entity name>", from translations.
    assert hass.states.get("binary_sensor.kitchen_cam_online").attributes["friendly_name"] == "Kitchen cam Online"
    assert hass.states.get("binary_sensor.kitchen_cam_online").state == "on"
    tamper = hass.states.get("binary_sensor.kitchen_cam_tamper")
    assert tamper is not None
    assert tamper.attributes["device_class"] == "tamper"

    # Transport sensors are diagnostic and disabled by default; tamper types stay enabled.
    for transport in ALL_TRANSPORTS:
        registered = registry.async_get_entity_id("binary_sensor", DOMAIN, f"{prefix}transport_{transport}")
        row = registry.async_get(registered)
        assert row.entity_category == EntityCategory.DIAGNOSTIC
        assert row.disabled_by is er.RegistryEntryDisabler.INTEGRATION
    power_loss = registry.async_get(registry.async_get_entity_id("binary_sensor", DOMAIN, f"{prefix}tamper_power_loss"))
    assert power_loss.disabled_by is None
    assert hass.states.get(power_loss.entity_id).attributes["friendly_name"] == "Kitchen cam Power Loss"

    # The tamper event entity fired on the live tamper publish.
    tamper_events = hass.states.get(registry.async_get_entity_id("event", DOMAIN, f"{prefix}tamper_events"))
    assert tamper_events.attributes["event_type"] == "enclosure"
    assert tamper_events.attributes["kind"] == "enclosure"
    assert tamper_events.attributes["device_id"] == CANARY
    assert set(tamper_events.attributes["event_types"]) == set(ALL_TAMPER_TYPES)
    assert tamper_events.attributes["friendly_name"] == "Kitchen cam Tamper events"

    # The health publish TOFU-pinned the Canary.
    assert entry.runtime_data.trust_store.is_pinned(CANARY)


async def test_canary_availability_follows_its_status_topic(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """A Canary that says offline takes its entities unavailable; the Online sensor says off."""
    await setup_kernel_entry(enable_mqtt=True)
    await wake_a_canary(hass)
    assert hass.states.get("sensor.kitchen_cam_health").state != "unavailable"

    # The canary base's last will, retained by the broker; the Online sensor
    # follows the merged liveness verdict, not only its own status topic.
    fire(hass, f"securacv/{CANARY}/availability", "offline", retain=True)
    await hass.async_block_till_done()
    assert hass.states.get("sensor.kitchen_cam_health").state == "unavailable"
    assert hass.states.get("binary_sensor.kitchen_cam_online").state == "off"

    # The device reconnects and says so (a live publish to existing subscribers).
    fire(hass, f"securacv/{CANARY}/availability", "online")
    await hass.async_block_till_done()
    assert hass.states.get("sensor.kitchen_cam_health").state != "unavailable"
    assert hass.states.get("binary_sensor.kitchen_cam_online").state == "on"


async def test_a_hostile_topic_segment_mints_no_device(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """A device id outside the firmware alphabet creates nothing."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    registry = er.async_get(hass)
    before = len(er.async_entries_for_config_entry(registry, entry.entry_id))
    fire(hass, "securacv/../status", {"online": True}, retain=True)
    fire(hass, "securacv/has space/status", {"online": True}, retain=True)
    fire(hass, f"securacv/{'x' * 70}/status", {"online": True}, retain=True)
    await hass.async_block_till_done()
    assert len(er.async_entries_for_config_entry(registry, entry.entry_id)) == before
    assert canary_device(hass, entry, "..") is None


async def test_witness_event_entities_are_restored_from_the_registry_on_reload(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """A zone's event entity survives a reload before its next event (stable entity id)."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    fire(hass, "witness/events", bridge_event(zone="front_door", seq=1), retain=False)
    await hass.async_block_till_done()
    assert hass.states.get("event.securacv_front_door") is not None

    before = hass.states.get("event.securacv_front_door")

    assert await hass.config_entries.async_reload(entry.entry_id)
    await settle(hass)
    restored = hass.states.get("event.securacv_front_door")
    assert restored is not None
    assert restored.attributes["friendly_name"] == "SecuraCV Front door"
    # Restored (the core's event entity restores its last event), not re-fired:
    # the state is the one from before the reload until the next live event.
    assert restored.state == before.state
    assert restored.attributes["event_type"] == before.attributes["event_type"]
    fire(hass, "witness/events", bridge_event(zone="front_door", bucket=bucket_start() + 600, seq=2), retain=False)
    await hass.async_block_till_done()
    assert hass.states.get("event.securacv_front_door").state != before.state


async def test_kernel_device_identifiers_merge_an_url_keyed_device(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """An older registry holds the kernel device by URL; the new id merges into it."""
    from homeassistant.helpers import device_registry as dr

    from conftest import KERNEL_URL, make_kernel_entry

    entry = make_kernel_entry(verifying_key=kernel.verifying_key)
    entry.add_to_hass(hass)
    registry = dr.async_get(hass)
    old = registry.async_get_or_create(
        config_entry_id=entry.entry_id, identifiers={(DOMAIN, KERNEL_URL)}, name="SecuraCV Privacy Witness Kernel"
    )
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    device = kernel_device(hass, entry)
    assert device.id == old.id
    assert device.identifiers == {(DOMAIN, KERNEL_URL), (DOMAIN, f"kernel_{KERNEL_DEVICE_ID}")}
    assert device.name == "SecuraCV"
