"""Diagnostics on a real core: a dump a person may attach to a public issue
carries no identifier the owner did not mean to publish."""
from __future__ import annotations

import json
import re

import pytest
from cryptography.hazmat.primitives.asymmetric import ed25519
from homeassistant.components.diagnostics import async_redact_data
from homeassistant.core import HomeAssistant
from homeassistant.helpers import issue_registry as ir
from homeassistant.setup import async_setup_component
from pytest_homeassistant_custom_component.common import async_fire_mqtt_message
from pytest_homeassistant_custom_component.components.diagnostics import get_diagnostics_for_config_entry
from pytest_homeassistant_custom_component.test_util.aiohttp import AiohttpClientMocker
from pytest_homeassistant_custom_component.typing import ClientSessionGenerator, MqttMockHAClient

from conftest import (
    CLAIM_NONCE,
    KERNEL_DEVICE_ID,
    KERNEL_HOST,
    KERNEL_URL,
    PAIRED_TOKEN,
    PAIRED_TOKEN_ID,
    KernelMock,
    SigningKernel,
    bridge_event,
    canary_device,
    make_kernel_entry,
)
from custom_components.securacv import repairs
from custom_components.securacv.const import CONF_TOKEN_FILE, DOMAIN, ISSUE_TOFU_MISMATCH
from custom_components.securacv.diagnostics import TO_REDACT

CANARY = "canary-kitchen-01"
# Its fingerprint is 055f1b11f6ef323a: a hex letter in every run of four.
# The dump's bucket starts are the clock's decimal digits, so with a run of
# four digits in the fingerprint the scan below failed whenever the current
# bucket start spelled it (it did with "11" * 32, abd93943326cbd12).
CANARY_KEY = "77" * 32
CANARY_IP = "192.168.4.77"
CANARY_NAME = "Kitchen cam"
# The kernel's key, fixed for this module: the dump carries the kernel's own
# eight-character fingerprint, and a key drawn fresh for each test shares
# four characters with the Canary's fingerprint in about one run in a
# thousand, which the scan below would read as a leak.
KERNEL_PRIVATE_KEY = bytes([0x42]) * 32


@pytest.fixture
def kernel(aioclient_mock: AiohttpClientMocker) -> KernelMock:
    """The lane's kernel, signing with the module's fixed key."""
    return KernelMock(
        aioclient_mock, SigningKernel(private=ed25519.Ed25519PrivateKey.from_private_bytes(KERNEL_PRIVATE_KEY))
    )


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
    windows = [canary_fingerprint[start : start + 4] for start in range(len(canary_fingerprint) - 3)]
    # What lets a match mean a leak and nothing else: no run of four can be
    # spelled by a bucket start's digits, or by the kernel's fingerprint.
    assert all(re.search("[a-f]", window) for window in windows), canary_fingerprint
    assert not [window for window in windows if window in kernel.kernel.fingerprint], kernel.kernel.fingerprint
    for window in windows:
        assert window not in dump.lower(), window
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


async def test_a_canary_heard_only_on_health_has_a_row(
    hass: HomeAssistant,
    kernel: KernelMock,
    mqtt_mock: MqttMockHAClient,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
) -> None:
    """A health that pins the Canary's key and reports its version makes the
    device page before any status does; the dump gives that Canary a row
    with the page's version, and a trust row with a correlation id."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    async_fire_mqtt_message(
        hass, f"securacv/{CANARY}/health", json.dumps({"public_key": CANARY_KEY, "firmware_version": "2.4.16"}), qos=1, retain=True
    )
    await hass.async_block_till_done()

    diagnostics = await get_diagnostics_for_config_entry(hass, hass_client, entry)
    page = canary_device(hass, entry, CANARY)
    assert page is not None and page.sw_version == "2.4.16"
    assert diagnostics["canary_device_count"] == 1
    (shown,) = diagnostics["canary_devices"]
    assert diagnostics["canary_devices"][shown]["firmware"] == "2.4.16"
    assert diagnostics["trust"][shown]["pinned"] is True
    assert re.fullmatch(r"[g-hj-km-np-tv-z]{4}-[g-hj-km-np-tv-z]{4}", diagnostics["trust"][shown]["key_correlation_id"])


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


async def test_the_download_carries_every_issue_the_domain_holds(
    hass: HomeAssistant,
    kernel: KernelMock,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
) -> None:
    """What the security docs and the setup guide say of the downloaded
    file's `issues` section, held to the core (ledger F399): Home Assistant
    adds every issue its registry holds for the domain, outside this
    integration's sweep. One the owner ignored is still there, so is one
    another entry raised, and each carries the time it was raised."""
    entry = await setup_kernel_entry()
    other = make_kernel_entry(unique_id="kernel_pwk_other")
    other.add_to_hass(hass)
    for raised_by in (entry, other):
        repairs.async_report_tofu_mismatch(
            hass,
            raised_by,
            pinned_fingerprint=kernel.kernel.fingerprint,
            received_fingerprint="0badc0de",
            received_key="cc" * 32,
            url=KERNEL_URL,
        )
    ignored = repairs.issue_id(ISSUE_TOFU_MISMATCH, entry)
    ir.async_ignore_issue(hass, DOMAIN, ignored, True)
    await hass.async_block_till_done()

    assert await async_setup_component(hass, "diagnostics", {})
    client = await hass_client()
    response = await client.get(f"/api/diagnostics/config_entry/{entry.entry_id}")
    assert response.status == 200
    issues = {issue["issue_id"]: issue for issue in (await response.json())["issues"]}
    assert issues[ignored]["dismissed_version"] is not None
    assert repairs.issue_id(ISSUE_TOFU_MISMATCH, other) in issues
    assert all(issue["created"] for issue in issues.values())
