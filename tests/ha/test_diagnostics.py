"""Diagnostics on a real core: a dump a person may attach to a public issue
carries no identifier the owner did not mean to publish."""
from __future__ import annotations

import base64
import json
import re
from datetime import timedelta

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519
from freezegun.api import FrozenDateTimeFactory
from homeassistant.components.diagnostics import async_redact_data
from homeassistant.config_entries import ConfigEntryDisabler, ConfigEntryState
from homeassistant.core import CoreState, HomeAssistant
from homeassistant.helpers import issue_registry as ir
from homeassistant.helpers.registry import SAVE_DELAY_LONG
from homeassistant.setup import async_setup_component
from pytest_homeassistant_custom_component.common import MockConfigEntry, async_fire_mqtt_message, async_fire_time_changed
from pytest_homeassistant_custom_component.components.diagnostics import get_diagnostics_for_config_entry
from pytest_homeassistant_custom_component.test_util.aiohttp import AiohttpClientMocker
from pytest_homeassistant_custom_component.typing import ClientSessionGenerator, MqttMockHAClient, WebSocketGenerator

from conftest import (
    CLAIM_NONCE,
    KERNEL_DEVICE_ID,
    KERNEL_HOST,
    KERNEL_PORT,
    KERNEL_URL,
    PAIRED_TOKEN,
    PAIRED_TOKEN_ID,
    KernelMock,
    SigningKernel,
    bridge_event,
    canary_device,
    fingerprint_of,
    kernel_entry_data,
    make_kernel_entry,
    settle,
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


async def test_the_records_are_numbered_before_the_other_pages(
    hass: HomeAssistant,
    kernel: KernelMock,
    mqtt_mock: MqttMockHAClient,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
) -> None:
    """Two canary-wap units share their eight characters, so each row gets an
    ordinal. The unit heard only on health has its page first, and the unit
    whose status the integration recorded is still #1: the records come
    first, then the other pages."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    async_fire_mqtt_message(
        hass, "securacv/canary-s3-xQ32/health", json.dumps({"public_key": CANARY_KEY, "firmware_version": "2.4.16"}), qos=1, retain=True
    )
    await hass.async_block_till_done()
    async_fire_mqtt_message(
        hass, "securacv/canary-s3-aB72/status", json.dumps({"online": True, "device_type": "canary-wap"}), qos=1, retain=True
    )
    await hass.async_block_till_done()

    diagnostics = await get_diagnostics_for_config_entry(hass, hass_client, entry)
    assert canary_device(hass, entry, "canary-s3-xQ32") is not None
    assert diagnostics["canary_device_count"] == 2
    assert diagnostics["canary_devices"]["canary-s… #1"]["device_type"] == "canary-wap"
    assert diagnostics["canary_devices"]["canary-s… #2"]["firmware"] == "2.4.16"
    assert diagnostics["trust"]["canary-s… #2"]["pinned"] is True


async def test_only_a_verdict_whose_page_is_gone_is_numbered_after_the_pages(
    hass: HomeAssistant,
    kernel: KernelMock,
    mqtt_mock: MqttMockHAClient,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
    hass_ws_client: WebSocketGenerator,
) -> None:
    """Verification runs in the handlers of a Canary's sensor entities, and
    those entities make its device page, so a chain publish verified before
    the Canary's status leaves a page and a device row, numbered with the
    other pages. An id only verification has seen is a verdict still held
    for a Canary whose page the owner has since deleted (the integration
    allows that for a Canary it has no record of and has not pinned): it is
    numbered after the pages and has no device row. A third canary-wap unit
    whose page a mesh publish made after the deletion, with no record, is
    the page the verdict is numbered after: `#2` to the verdict's `#3`."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    # The device registry's commands, the diagnostics view and the websocket
    # client, all before the first client freezes the router.
    assert await async_setup_component(hass, "config", {})
    assert await async_setup_component(hass, "diagnostics", {})
    client = await hass_ws_client(hass)
    async_fire_mqtt_message(
        hass, "securacv/canary-s3-aB72/status", json.dumps({"online": True, "device_type": "canary-wap"}), qos=1, retain=True
    )
    await hass.async_block_till_done()
    # The first chain publish adds the Canary's chain sensor; the second
    # stands in for the broker handing the retained message to that
    # sensor's own subscription, which verifies it.
    for _ in range(2):
        async_fire_mqtt_message(hass, "securacv/canary-s3-xQ32/chain", json.dumps({"length": 3}), qos=1, retain=True)
        await hass.async_block_till_done()
    assert list(entry.runtime_data.verify) == ["canary-s3-xQ32"]
    assert list(entry.runtime_data.devices) == ["canary-s3-aB72"]
    page = canary_device(hass, entry, "canary-s3-xQ32")
    assert page is not None

    diagnostics = await get_diagnostics_for_config_entry(hass, hass_client, entry)
    assert list(diagnostics["canary_devices"]) == ["canary-s… #1", "canary-s… #2"]
    assert diagnostics["canary_devices"]["canary-s… #1"]["device_type"] == "canary-wap"
    assert diagnostics["verification"] == {"canary-s… #2": {"trusted": False, "reason": "unsigned"}}

    # The owner deletes the page through the device registry's websocket
    # command: `remove` where the core has it (2026.10.1, which keeps
    # `remove_config_entry` as a deprecated alias), else `remove_config_entry`
    # (2026.5.0). Both ask the integration first, then remove the device.
    await client.send_json_auto_id({"type": "config/device_registry/remove", "device_id": page.id})
    response = await client.receive_json()
    if not response["success"] and response["error"]["code"] == "unknown_command":
        await client.send_json_auto_id(
            {"type": "config/device_registry/remove_config_entry", "config_entry_id": entry.entry_id, "device_id": page.id}
        )
        response = await client.receive_json()
    assert response["success"], response
    await hass.async_block_till_done()
    assert canary_device(hass, entry, "canary-s3-xQ32") is None
    assert list(entry.runtime_data.verify) == ["canary-s3-xQ32"]

    diagnostics = await get_diagnostics_for_config_entry(hass, hass_client, entry)
    assert list(diagnostics["canary_devices"]) == ["canary-s… #1"]
    assert diagnostics["verification"] == {"canary-s… #2": {"trusted": False, "reason": "unsigned"}}
    assert "xQ32" not in json.dumps(diagnostics)

    # A Canary that has only a page, sharing the eight characters: its row
    # comes after the record and before the verdict.
    async_fire_mqtt_message(hass, "securacv/canary-s3-Kx22/mesh", json.dumps({"connected": True}), qos=1, retain=True)
    await hass.async_block_till_done()
    assert canary_device(hass, entry, "canary-s3-Kx22") is not None
    assert list(entry.runtime_data.devices) == ["canary-s3-aB72"]
    assert list(entry.runtime_data.verify) == ["canary-s3-xQ32"]

    diagnostics = await get_diagnostics_for_config_entry(hass, hass_client, entry)
    assert list(diagnostics["canary_devices"]) == ["canary-s… #1", "canary-s… #2"]
    assert diagnostics["canary_devices"]["canary-s… #1"]["device_type"] == "canary-wap"
    assert diagnostics["canary_devices"]["canary-s… #2"]["device_type"] is None
    assert diagnostics["verification"] == {"canary-s… #3": {"trusted": False, "reason": "unsigned"}}


async def test_a_canary_row_is_keyed_by_eight_characters_of_its_id(
    hass: HomeAssistant,
    kernel: KernelMock,
    mqtt_mock: MqttMockHAClient,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
) -> None:
    """What the setup guide and the threat model say of a Canary's row key:
    the first eight characters of its id and an ellipsis, or the whole id
    when it has eight or fewer (`garage01` has eight, `garage` six)."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    for canary_id in (CANARY, "garage01", "garage"):
        async_fire_mqtt_message(hass, f"securacv/{canary_id}/mesh", json.dumps({"connected": True}), qos=1, retain=True)
    await hass.async_block_till_done()

    diagnostics = await get_diagnostics_for_config_entry(hass, hass_client, entry)
    assert set(diagnostics["canary_devices"]) == {"canary-k…", "garage01", "garage"}


# A first publish on each of these topics gives a Canary its device page
# through the entities the platforms add for it, and none of them writes a
# record of the status or the health handler: a health with no version (its
# key is pinned all the same), counts, chain, events, tamper, transport,
# mesh and chirp.
PAGE_ONLY_PUBLISHES = {
    "health": {"public_key": CANARY_KEY, "free_heap": 120000},
    "counts": {"total": 3},
    "chain": {"length": 3},
    "events": {"event_type": "BoundaryCrossingObjectLarge"},
    "tamper": {"type": "gpio", "active": True},
    "transport": {"wifi": {"connected": True}},
    "mesh": {"connected": True},
    "chirp": {"active": True},
}


@pytest.mark.parametrize("topic", list(PAGE_ONLY_PUBLISHES))
async def test_every_canary_with_a_device_page_has_a_row(
    topic: str,
    hass: HomeAssistant,
    kernel: KernelMock,
    mqtt_mock: MqttMockHAClient,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
) -> None:
    """Whichever topic gave a Canary its device page, the dump has a row for
    it, and a trust row when its key is pinned; the kernel's own page is not
    a Canary row. Before, a Canary whose page came from any of these topics
    alone had neither."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    async_fire_mqtt_message(
        hass, f"securacv/{CANARY}/{topic}", json.dumps(PAGE_ONLY_PUBLISHES[topic]), qos=1, retain=True
    )
    await hass.async_block_till_done()

    diagnostics = await get_diagnostics_for_config_entry(hass, hass_client, entry)
    assert canary_device(hass, entry, CANARY) is not None
    assert diagnostics["canary_device_count"] == 1
    (shown,) = diagnostics["canary_devices"]
    assert shown == CANARY[:8] + "…"
    assert diagnostics["canary_devices"][shown]["online"] is None
    assert diagnostics["canary_devices"][shown]["firmware"] is None
    if topic == "health":
        assert entry.runtime_data.trust_store.is_pinned(CANARY)
        assert diagnostics["trust"][shown]["pinned"] is True
        assert re.fullmatch(r"[g-hj-km-np-tv-z]{4}-[g-hj-km-np-tv-z]{4}", diagnostics["trust"][shown]["key_correlation_id"])
    else:
        assert diagnostics["trust"] == {}


async def test_a_page_kept_from_before_a_reload_has_a_row(
    hass: HomeAssistant,
    kernel: KernelMock,
    mqtt_mock: MqttMockHAClient,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
) -> None:
    """A reload starts the integration's records afresh and the device
    registry keeps the page, as a restart does: the Canary has a row before
    it publishes again, with nothing it published before in it, and its
    trust row from the pin the trust store kept."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    async_fire_mqtt_message(
        hass, f"securacv/{CANARY}/status", json.dumps({"online": True, "device_type": "canary-sense"}), qos=1, retain=True
    )
    async_fire_mqtt_message(
        hass, f"securacv/{CANARY}/health", json.dumps({"public_key": CANARY_KEY, "firmware_version": "2.4.16"}), qos=1, retain=True
    )
    await hass.async_block_till_done()
    assert await hass.config_entries.async_reload(entry.entry_id)
    await hass.async_block_till_done()

    diagnostics = await get_diagnostics_for_config_entry(hass, hass_client, entry)
    page = canary_device(hass, entry, CANARY)
    assert page is not None and page.sw_version == "2.4.16"
    assert entry.runtime_data.devices == {}
    assert diagnostics["canary_device_count"] == 1
    (shown,) = diagnostics["canary_devices"]
    assert diagnostics["canary_devices"][shown] == {
        "online": None,
        "firmware": None,
        "hardware": None,
        "device_type": None,
        "last_event_type": None,
    }
    assert diagnostics["trust"][shown]["pinned"] is True


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


def test_a_health_counter_is_replaced_wherever_it_lands() -> None:
    """The second net behind the health projection on the core's own async_redact_data (device-details
    P18, ledger F391): a Canary's uptime, boot count, card writes, web
    requests and frame age, at any depth, come out redacted, numbers as
    much as strings."""
    seeded = {
        "uptime": 864123,
        "health": {
            "boot_count": 4127,
            "storage": {"sd_writes": 918273},
            "web": [{"http_requests": "55501"}],
            "camera": {"stats": [{"last_frame_age_ms": 31337}]},
        },
    }
    text = json.dumps(async_redact_data(seeded, TO_REDACT))
    for value in ("864123", "4127", "918273", "55501", "31337"):
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
            hass, raised_by, pinned_key=kernel.verifying_key, received_key="cc" * 32
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


# Every spelling of the kernel's port the scan below refuses: the digits
# standing alone, not inside a longer number or a timestamp's fraction.
_PORT = re.compile(rf"(?<![\d.]){KERNEL_PORT}(?!\d)")
# The fields Home Assistant writes for an issue that is not persistent.
_NOT_PERSISTENT_FIELDS = {"created", "dismissed_version", "domain", "is_persistent", "issue_id"}


def _key_spellings(key_hex: str) -> list[str]:
    """A verifying key as hex (either case), base64 (both alphabets) and each
    base64 line of its PEM."""
    raw = bytes.fromhex(key_hex)
    pem = (
        ed25519.Ed25519PublicKey.from_public_bytes(raw)
        .public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
        .decode()
    )
    lines = [line for line in pem.splitlines() if line and not line.startswith("-----")]
    return [key_hex, key_hex.upper(), base64.b64encode(raw).decode(), base64.urlsafe_b64encode(raw).decode(), *lines]


def _assert_no_issue_holds_a_host_or_a_key(
    issues: list[dict], text: str, keys: list[str], fingerprints: dict[str, dict[str, str]]
) -> None:
    """`text` (a whole downloaded file, or what the issue registry stored)
    spells no host and no key; every key-mismatch issue holds exactly its two
    fingerprints, and every issue that is not persistent only its id and
    times. `fingerprints` is the expected pair per key-mismatch issue id."""
    assert KERNEL_HOST not in text
    for key in keys:
        for spelled in _key_spellings(key):
            assert spelled not in text, spelled
    assert not _PORT.search(json.dumps(issues))
    seen = set()
    for issue in issues:
        if issue["domain"] != DOMAIN:
            continue
        if issue["issue_id"] in fingerprints:
            seen.add(issue["issue_id"])
            assert issue["data"] == fingerprints[issue["issue_id"]], issue["issue_id"]
            assert issue["translation_placeholders"] == fingerprints[issue["issue_id"]], issue["issue_id"]
            assert issue["learn_more_url"] is None
        elif not issue["is_persistent"]:
            assert set(issue) == _NOT_PERSISTENT_FIELDS, issue["issue_id"]
        # Every eight-character hex run in what an issue holds is one of the
        # two fingerprints it is allowed.
        for field in ("data", "translation_placeholders"):
            held = json.dumps(issue.get(field))
            allowed = set(fingerprints.get(issue["issue_id"], {}).values())
            assert set(re.findall(r"(?<![0-9a-fA-F])[0-9a-fA-F]{8}(?![0-9a-fA-F])", held)) <= allowed, held
    assert seen == set(fingerprints)


async def _download(client, entry) -> tuple[dict, str]:
    """The downloaded file: its body, and the body with its name in front."""
    response = await client.get(f"/api/diagnostics/config_entry/{entry.entry_id}")
    assert response.status == 200
    text = await response.text()
    return json.loads(text), response.headers["Content-Disposition"] + text


async def _restart_the_issue_registry(hass: HomeAssistant, freezer: FrozenDateTimeFactory, hass_storage) -> list[dict]:
    """Write the issue registry as Home Assistant does (its delayed save,
    to the mocked `.storage`), then load it back from what was written, as
    the next start does; returns what was stored."""
    freezer.tick(timedelta(seconds=SAVE_DELAY_LONG + 1))
    async_fire_time_changed(hass)
    await hass.async_block_till_done()
    stored = hass_storage[ir.STORAGE_KEY]["data"]["issues"]
    await ir.async_get(hass).async_load()
    return stored


async def test_no_issue_in_the_download_or_the_store_holds_a_host_or_a_key(
    hass: HomeAssistant,
    kernel: KernelMock,
    mqtt_mock: MqttMockHAClient,
    hass_client: ClientSessionGenerator,
    hass_storage,
    freezer: FrozenDateTimeFactory,
) -> None:
    """Ledger F399, through every state a key-mismatch issue passes: raised,
    ignored, kept over a restart, raised again for a third key, and one an
    older integration raised for an entry that never sets up again. Each
    download (its issues, the integration's data, the file name; the core
    times no setup once it is running, as it is here, so `setup_times` is
    empty, and the sibling test below fills it, with entry ids and times
    only) and what Home Assistant stores in `.storage/repairs.issue_registry`
    spells neither the kernel's host nor any key it served or was pinned to,
    in hex, base64 or PEM, and the port appears only in the kernel section's
    projection (scheme and port). A kernel_unreachable issue raised with the
    URL is in the file by its id and times only, the core's own rule for an
    issue that is not persistent."""
    pinned = kernel.verifying_key
    # An entry an older integration raised a key-mismatch issue for, with the
    # URL and the served key in it, stored and loaded back at this start.
    # The entry is disabled, so nothing raises that issue again.
    old = SigningKernel()
    disabled = MockConfigEntry(
        domain=DOMAIN, title="SecuraCV", data=kernel_entry_data(verifying_key=pinned),
        unique_id="kernel_pwk_old", version=3, disabled_by=ConfigEntryDisabler.USER,
    )
    disabled.add_to_hass(hass)
    old_issue = repairs.issue_id(ISSUE_TOFU_MISMATCH, disabled)
    old_fields = {"pinned_fingerprint": fingerprint_of(pinned), "received_fingerprint": old.fingerprint}
    hass_storage[ir.STORAGE_KEY] = {
        "version": ir.STORAGE_VERSION_MAJOR, "minor_version": ir.STORAGE_VERSION_MINOR, "key": ir.STORAGE_KEY,
        "data": {"issues": [{
            "created": "2026-10-01T12:00:00.000000+00:00", "dismissed_version": None, "domain": DOMAIN,
            "is_persistent": True, "issue_id": old_issue, "breaks_in_ha_version": None, "is_fixable": True,
            "issue_domain": None, "learn_more_url": None, "severity": "critical", "translation_key": "tofu_mismatch",
            "translation_placeholders": {**old_fields, "url": KERNEL_URL},
            "data": {**old_fields, "received_key": old.verifying_key, "url": KERNEL_URL},
        }]},
    }
    await ir.async_get(hass).async_load()
    assert ir.async_get(hass).async_get_issue(DOMAIN, old_issue).data["received_key"] == old.verifying_key

    entry = make_kernel_entry(verifying_key=pinned, enable_mqtt=True)
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await settle(hass)
    assert await async_setup_component(hass, "diagnostics", {})
    client = await hass_client()
    issue = repairs.issue_id(ISSUE_TOFU_MISMATCH, entry)

    # Raised: the kernel serves another key.
    second = SigningKernel()
    kernel.verifying_key = second.verifying_key
    kernel.kernel = second
    freezer.tick(timedelta(minutes=5, seconds=1))
    async_fire_time_changed(hass)
    await settle(hass)
    repairs.async_report_kernel_unreachable(hass, entry, KERNEL_URL)
    keys = [pinned, second.verifying_key, old.verifying_key]
    expected = {
        old_issue: old_fields,
        issue: {"pinned_fingerprint": fingerprint_of(pinned), "received_fingerprint": second.fingerprint},
    }
    body, text = await _download(client, entry)
    _assert_no_issue_holds_a_host_or_a_key(body["issues"], text, keys, expected)
    unreachable = repairs.issue_id("kernel_unreachable", entry)
    assert unreachable in {i["issue_id"] for i in body["issues"]}
    # The port is the kernel section's projection, and nowhere else.
    assert body["data"]["kernel"]["api"]["port"] == KERNEL_PORT
    rest = {**body, "data": {**body["data"], "kernel": {**body["data"]["kernel"], "api": None}}}
    assert not _PORT.search(json.dumps(rest))

    # Ignored: kept, and still only the fingerprints.
    ir.async_ignore_issue(hass, DOMAIN, issue, True)
    body, text = await _download(client, entry)
    assert {i["issue_id"]: i for i in body["issues"]}[issue]["dismissed_version"] is not None
    _assert_no_issue_holds_a_host_or_a_key(body["issues"], text, keys, expected)

    # A restart: the registry written and loaded back, the entry set up anew.
    stored = await _restart_the_issue_registry(hass, freezer, hass_storage)
    _assert_no_issue_holds_a_host_or_a_key(stored, json.dumps(stored), keys, expected)
    assert await hass.config_entries.async_reload(entry.entry_id)
    await settle(hass)
    body, text = await _download(client, entry)
    _assert_no_issue_holds_a_host_or_a_key(body["issues"], text, keys, expected)
    assert {i["issue_id"]: i for i in body["issues"]}[issue]["dismissed_version"] is not None

    # A third key while the issue is open.
    third = SigningKernel()
    kernel.verifying_key = third.verifying_key
    kernel.kernel = third
    freezer.tick(timedelta(minutes=5, seconds=1))
    async_fire_time_changed(hass)
    await settle(hass)
    keys.append(third.verifying_key)
    expected[issue] = {"pinned_fingerprint": fingerprint_of(pinned), "received_fingerprint": third.fingerprint}
    body, text = await _download(client, entry)
    _assert_no_issue_holds_a_host_or_a_key(body["issues"], text, keys, expected)
    stored = await _restart_the_issue_registry(hass, freezer, hass_storage)
    _assert_no_issue_holds_a_host_or_a_key(stored, json.dumps(stored), keys, expected)


async def test_the_download_carries_entry_ids_in_its_name_and_setup_times(
    hass: HomeAssistant,
    kernel: KernelMock,
    hass_client: ClientSessionGenerator,
) -> None:
    """What the setup guide says of the downloaded file outside the sweep,
    besides its issues, held to the core (ledger F399). Home Assistant names
    the file after the entry. It times the setup of every entry it sets up
    before the core is running, as it does at a start, under the entry's id,
    and adds those times to the download as `setup_times`: the download of
    one of two entries set up at a start names both there. With no issue
    raised, the `issues` and `setup_times` sections removed leave neither
    id in the body. The core's own `home_assistant` section carries the
    instance's time zone."""
    hass.set_state(CoreState.not_running)
    entry = make_kernel_entry(verifying_key=kernel.verifying_key)
    other = make_kernel_entry(verifying_key=kernel.verifying_key, unique_id="kernel_pwk_other")
    entry.add_to_hass(hass)
    other.add_to_hass(hass)
    # The component's own setup sets up every entry of the domain.
    assert await hass.config_entries.async_setup(entry.entry_id)
    await settle(hass)
    assert other.state is ConfigEntryState.LOADED
    hass.set_state(CoreState.running)

    assert await async_setup_component(hass, "diagnostics", {})
    client = await hass_client()
    response = await client.get(f"/api/diagnostics/config_entry/{entry.entry_id}")
    assert response.status == 200
    assert response.headers["Content-Disposition"] == f'attachment; filename="config_entry-{DOMAIN}-{entry.entry_id}.json"'
    body = await response.json()
    assert body["issues"] == []
    # "null" is the domain's own setup, timed under no entry.
    assert set(body["setup_times"]) == {"null", entry.entry_id, other.entry_id}
    assert body["home_assistant"]["timezone"] == str(hass.config.time_zone)
    rest = json.dumps({section: value for section, value in body.items() if section not in ("issues", "setup_times")})
    assert entry.entry_id not in rest
    assert other.entry_id not in rest
