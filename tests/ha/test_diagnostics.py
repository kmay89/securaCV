"""Diagnostics on a real core: a dump a person may attach to a public issue
carries no identifier the owner did not mean to publish."""
from __future__ import annotations

import json
import re

from homeassistant.components.diagnostics import async_redact_data
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
    canary_device,
)
from custom_components.securacv.const import CONF_TOKEN_FILE
from custom_components.securacv.diagnostics import TO_REDACT

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
    # No digit of the Canary's key fingerprint: a per-dump correlation id in
    # letters that are not hex digits (device-details P19, ledger F392).
    assert "fingerprint" not in diagnostics["trust"][shown]
    assert re.fullmatch(r"[g-hj-km-np-tv-z]{4}-[g-hj-km-np-tv-z]{4}", diagnostics["trust"][shown]["key_correlation_id"])
    canary_fingerprint = entry.runtime_data.trust_store.get(CANARY).fingerprint_hex.lower()
    for start in range(len(canary_fingerprint) - 3):
        assert canary_fingerprint[start : start + 4] not in dump.lower()
    assert diagnostics["platforms_loaded"] == ["sensor", "binary_sensor", "event", "button"]
    assert diagnostics["chain_verification"]["ok"] is True
    assert "head_hash" not in diagnostics["chain_verification"]
    assert diagnostics["evidence_exports"] == {"count": 0}


async def test_the_dump_shows_the_firmware_the_device_page_shows(
    hass: HomeAssistant,
    kernel: KernelMock,
    mqtt_mock: MqttMockHAClient,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
) -> None:
    """The per-Canary firmware is the device page's, health's over the
    status's, and the board is bounded broker text, as on that page."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    async_fire_mqtt_message(
        hass,
        f"securacv/{CANARY}/status",
        json.dumps({"online": True, "firmware_version": "2.4.15", "hardware": "h" * 300, "device_type": "canary-sense"}),
        qos=1,
        retain=True,
    )
    async_fire_mqtt_message(
        hass, f"securacv/{CANARY}/health", json.dumps({"public_key": CANARY_KEY, "firmware_version": "2.4.16"}), qos=1, retain=True
    )
    await hass.async_block_till_done()

    diagnostics = await get_diagnostics_for_config_entry(hass, hass_client, entry)
    (row,) = diagnostics["canary_devices"].values()
    page = canary_device(hass, entry, CANARY)
    assert page is not None
    assert row["firmware"] == page.sw_version == "2.4.16"
    assert row["hardware"] == page.hw_version == "h" * 64
    assert row["device_type"] == "canary-sense"


def test_an_identifier_key_is_replaced_wherever_it_lands() -> None:
    """The second net on the core's own async_redact_data: a value under any
    of the identifier keys a Canary's answers use, at any depth, comes out
    redacted (device-details P18, ledger F391)."""
    seeded = {
        "hw_token": "b7a1c2d3e4f5a6b7",
        "mdns_host": "canary-kitchen.local",
        "serial_number": "7FA319C0AABBCCDD",
        "chip_id": "0x00a1b2c3d4e5",
        "module_id": "1073741824",
        "bluetooth": {
            "local_address": "34:85:18:aa:bb:cd",
            "connection": {"address": "5c:aa:bb:cc:dd:ee"},
            "paired": [{"peer_address": "7e:11:22:33:44:55"}],
        },
    }
    text = json.dumps(async_redact_data(seeded, TO_REDACT))
    for value in (
        "b7a1c2d3e4f5a6b7",
        "canary-kitchen.local",
        "7FA319C0AABBCCDD",
        "0x00a1b2c3d4e5",
        "1073741824",
        "34:85:18:aa:bb:cd",
        "5c:aa:bb:cc:dd:ee",
        "7e:11:22:33:44:55",
    ):
        assert value not in text, value
