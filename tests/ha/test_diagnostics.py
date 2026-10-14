"""Diagnostics on a real core: a dump a person may attach to a public issue
carries no identifier the owner did not mean to publish."""
from __future__ import annotations

import json

from homeassistant.core import HomeAssistant
from pytest_homeassistant_custom_component.common import async_fire_mqtt_message
from pytest_homeassistant_custom_component.components.diagnostics import get_diagnostics_for_config_entry
from pytest_homeassistant_custom_component.typing import ClientSessionGenerator, MqttMockHAClient

from conftest import (
    CLAIM_NONCE,
    KERNEL_DEVICE_ID,
    KERNEL_HOST,
    PAIRED_TOKEN,
    PAIRED_TOKEN_ID,
    KernelMock,
    bridge_event,
)
from custom_components.securacv.const import CONF_TOKEN_FILE

CANARY = "canary-kitchen-01"
CANARY_KEY = "11" * 32
CANARY_IP = "192.168.4.77"
CANARY_NAME = "Kitchen cam"


async def test_no_seeded_secret_survives_the_dump(
    hass: HomeAssistant,
    kernel: KernelMock,
    mqtt_mock: MqttMockHAClient,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
) -> None:
    """Token, token file path, URL host, device ids, keys, addresses and names stay out."""
    token_file = "/config/secret-token-path/api_token"
    entry = await setup_kernel_entry(auth_mode="paired", enable_mqtt=True, **{CONF_TOKEN_FILE: token_file})
    async_fire_mqtt_message(
        hass,
        f"securacv/{CANARY}/status",
        json.dumps({"online": True, "firmware_version": "2.4.16", "hardware": "esp32s3", "device_name": CANARY_NAME, "ap_ip": CANARY_IP, "ssid": "HomeWifi", "mac": "aa:bb:cc:dd:ee:ff"}),
        qos=1,
        retain=True,
    )
    async_fire_mqtt_message(
        hass, f"securacv/{CANARY}/health", json.dumps({"public_key": CANARY_KEY, "free_heap": 120000}), qos=1, retain=True
    )
    async_fire_mqtt_message(hass, "witness/last_event", json.dumps(bridge_event(zone="front_door", seq=3)), qos=1, retain=True)
    async_fire_mqtt_message(
        hass,
        "witness/frigate_link",
        json.dumps({"v": 1, "device_id": KERNEL_DEVICE_ID, "connected": True, "topic_prefix": "frigate", "last_message_bucket_start": 1700000400, "last_message_bucket_size": 600, "messages_since_start": 4, "mode": "events"}),
        qos=1,
        retain=True,
    )
    await hass.async_block_till_done()

    diagnostics = await get_diagnostics_for_config_entry(hass, hass_client, entry)
    dump = json.dumps(diagnostics)

    secrets = {
        "paired token": PAIRED_TOKEN,
        "token id": PAIRED_TOKEN_ID,
        "token file path": token_file,
        "kernel host": KERNEL_HOST,
        "kernel device id": KERNEL_DEVICE_ID,
        "kernel verifying key": kernel.verifying_key,
        "kernel latest hash": kernel.kernel.head_hash if kernel.kernel.entries else "no-hash",
        "canary id": CANARY,
        "canary key": CANARY_KEY,
        "canary ip": CANARY_IP,
        "canary name": CANARY_NAME,
        "ssid": "HomeWifi",
        "mac": "aa:bb:cc:dd:ee:ff",
        "entry id": entry.entry_id,
        "claim nonce": CLAIM_NONCE,
        "zone label": "front_door",
    }
    for what, value in secrets.items():
        assert value not in dump, f"{what} ({value}) reached the diagnostics dump"

    # What a maintainer needs is still there: shapes and counts.
    kernel_info = diagnostics["kernel"]
    assert kernel_info["api"] == {"configured": True, "scheme": "http", "port": 8799}
    assert kernel_info["auth_mode"] == "paired"
    assert kernel_info["key_pinned"] is True
    assert kernel_info["pinned_fingerprint"] == kernel.kernel.fingerprint
    assert kernel_info["kernel_device_id_prefix"] == "pwk_1a2b…"
    assert kernel_info["latest_event"] == {
        "event_type": "BoundaryCrossingObjectLarge",
        "time_bucket": {"start_epoch_s": kernel_info["latest_event"]["time_bucket"]["start_epoch_s"], "size_s": 600},
        "attestation": "adapter",
    }
    assert kernel_info["frigate_link"]["connected"] is True
    assert kernel_info["identity"]["served_key_matches_pin"] is True
    assert kernel_info["storage_status"]["status"] == "good"
    assert "source_device" not in json.dumps(kernel_info["storage_status"])
    assert diagnostics["canary_device_count"] == 1
    (shown,) = diagnostics["canary_devices"]
    assert len(shown) == 9 and shown.startswith(CANARY[:8])
    assert diagnostics["canary_devices"][shown]["online"] is True
    assert diagnostics["canary_devices"][shown]["firmware"] == "2.4.16"
    assert diagnostics["trust"][shown]["pinned"] is True
    assert len(diagnostics["trust"][shown]["fingerprint"]) == 9
    assert diagnostics["platforms_loaded"] == ["sensor", "binary_sensor", "event", "button"]
    assert diagnostics["chain_verification"]["ok"] is True
    assert "head_hash" not in diagnostics["chain_verification"]
    assert diagnostics["evidence_exports"] == {"count": 0}
