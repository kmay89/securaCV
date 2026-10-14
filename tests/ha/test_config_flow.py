"""The config flow on a real core: user, Automatic, Supervisor, MQTT announce,
pairing, reauth, reconfigure and options."""
from __future__ import annotations

import asyncio
import json
import time
from unittest.mock import patch

import aiohttp
import pytest
from homeassistant import config_entries
from homeassistant.config_entries import ConfigEntryState
from homeassistant.const import CONF_TOKEN, CONF_URL
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers import issue_registry as ir
from homeassistant.helpers.service_info.hassio import HassioServiceInfo
from homeassistant.helpers.service_info.mqtt import MqttServiceInfo
from pytest_homeassistant_custom_component.common import MockConfigEntry
from pytest_homeassistant_custom_component.test_util.aiohttp import AiohttpClientMocker
from pytest_homeassistant_custom_component.typing import MqttMockHAClient

from conftest import (
    KERNEL_DEVICE_ID,
    KERNEL_URL,
    PAIRED_TOKEN,
    PAIRED_TOKEN_ID,
    KernelMock,
    SigningKernel,
    make_kernel_entry,
    make_mqtt_only_entry,
)
from custom_components.securacv.const import (
    AUTH_MODE_PAIRED,
    AUTH_MODE_STATIC,
    AUTH_MODE_TOKEN_FILE,
    CONF_AUTH_MODE,
    CONF_CLAIM_NONCE,
    CONF_ENABLE_MQTT,
    CONF_EVIDENCE_KEEP,
    CONF_KERNEL_DEVICE_ID,
    CONF_KERNEL_PREVIOUS_KEYS,
    CONF_KERNEL_TOKEN_ID,
    CONF_KERNEL_VERIFYING_KEY,
    CONF_MQTT_PREFIX,
    CONF_MQTT_TOPIC_PREFIX,
    CONF_PAIRING_CODE,
    CONF_SETUP_MODE,
    CONF_TOKEN_FILE,
    DEFAULT_KERNEL_URL,
    DEFAULT_TOKEN_FILE,
    DOMAIN,
    ISSUE_NO_KERNEL_FOUND,
    SETUP_MODE_AUTO,
    SETUP_MODE_BOTH,
    SETUP_MODE_KERNEL,
    SETUP_MODE_MQTT,
)

ADDON_HOST = "d0491a67-privacy-witness-kernel"
ADDON_URL = f"http://{ADDON_HOST}:8799"


def hassio_info(**config_overrides) -> HassioServiceInfo:
    """The Supervisor discovery payload the app posts (contract C6)."""
    config = {
        "host": ADDON_HOST,
        "port": 8799,
        "token_file": DEFAULT_TOKEN_FILE,
        "mqtt_topic_prefix": "witness",
        "device_id": KERNEL_DEVICE_ID,
    }
    config.update(config_overrides)
    return HassioServiceInfo(config=config, name="SecuraCV", slug="privacy_witness_kernel", uuid="1234abcd")


def mqtt_info(topic: str, payload: dict | str | bytes, *, retain: bool = True) -> MqttServiceInfo:
    """What the mqtt integration hands the flow for a matching message."""
    if isinstance(payload, dict):
        payload = json.dumps(payload)
    return MqttServiceInfo(
        topic=topic,
        payload=payload,
        qos=1,
        retain=retain,
        subscribed_topic="witness/+/announce" if topic.endswith("/announce") else "securacv/#",
        timestamp=time.time(),
    )


async def start_user_flow(hass: HomeAssistant, mode: str):
    """Open the user step and pick a mode."""
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    assert result["type"] is FlowResultType.FORM
    assert result["step_id"] == "user"
    return await hass.config_entries.flow.async_configure(result["flow_id"], {CONF_SETUP_MODE: mode})


# ── the manual forms ─────────────────────────────────────────────────────


async def test_kernel_mode_probes_identity_and_pins_the_key(hass: HomeAssistant, kernel: KernelMock) -> None:
    """Manual kernel mode: the identity read keys the entry and pins the key."""
    result = await start_user_flow(hass, SETUP_MODE_KERNEL)
    assert result["type"] is FlowResultType.FORM and result["step_id"] == "kernel"

    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_TOKEN: "ab" * 32, CONF_TOKEN_FILE: ""}
    )
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY
    entry = result["result"]
    assert entry.unique_id == f"kernel_{KERNEL_DEVICE_ID}"
    assert entry.data[CONF_AUTH_MODE] == AUTH_MODE_STATIC
    assert entry.data[CONF_KERNEL_DEVICE_ID] == KERNEL_DEVICE_ID
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == kernel.verifying_key
    assert entry.data[CONF_SETUP_MODE] == SETUP_MODE_KERNEL
    assert entry.data[CONF_ENABLE_MQTT] is False
    assert CONF_TOKEN_FILE not in entry.data
    assert entry.data[CONF_CLAIM_NONCE]
    assert entry.state is ConfigEntryState.LOADED


@pytest.mark.parametrize(
    ("identity_status", "error"),
    [(401, "invalid_auth"), (503, "cannot_connect"), (404, "kernel_identity_unavailable")],
)
async def test_kernel_mode_errors_keep_the_form(
    hass: HomeAssistant, kernel: KernelMock, identity_status: int, error: str
) -> None:
    """A refused, unreachable or identity-less kernel is a form error, not a crash."""
    kernel.identity_status = identity_status
    result = await start_user_flow(hass, SETUP_MODE_KERNEL)
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_TOKEN: "ab" * 32, CONF_TOKEN_FILE: ""}
    )
    assert result["type"] is FlowResultType.FORM
    assert result["errors"] == {"base": error}
    # The URL typed comes back as the default instead of being reset.
    schema_defaults = {str(key): key.default() for key in result["data_schema"].schema if hasattr(key, "default") and key.default is not None and callable(key.default)}
    assert schema_defaults.get(CONF_URL) == KERNEL_URL


async def test_kernel_mode_needs_a_credential(hass: HomeAssistant, kernel: KernelMock) -> None:
    """Neither a token nor a token file → token_required."""
    result = await start_user_flow(hass, SETUP_MODE_KERNEL)
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_TOKEN_FILE: ""}
    )
    assert result["type"] is FlowResultType.FORM
    assert result["errors"] == {"base": "token_required"}


async def test_both_mode_creates_a_both_entry(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient
) -> None:
    """Manual both mode keys on the identity and enables the Canary prefix."""
    result = await start_user_flow(hass, SETUP_MODE_BOTH)
    assert result["step_id"] == "both"
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"],
        {CONF_URL: KERNEL_URL, CONF_TOKEN: "ab" * 32, CONF_TOKEN_FILE: "", CONF_MQTT_PREFIX: "securacv", CONF_MQTT_TOPIC_PREFIX: "witness"},
    )
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY
    entry = result["result"]
    assert entry.data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert entry.data[CONF_ENABLE_MQTT] is True
    assert entry.data[CONF_MQTT_PREFIX] == "securacv"
    assert entry.state is ConfigEntryState.LOADED

    # The same kernel again is one entry.
    result = await start_user_flow(hass, SETUP_MODE_KERNEL)
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_TOKEN: "ab" * 32, CONF_TOKEN_FILE: ""}
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_configured"


async def test_mqtt_only_mode_and_duplicate_prefix(hass: HomeAssistant, mqtt_mock: MqttMockHAClient) -> None:
    """Canaries only: keyed on the prefix; the same prefix twice aborts."""
    result = await start_user_flow(hass, SETUP_MODE_MQTT)
    assert result["step_id"] == "mqtt_config"
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {CONF_MQTT_PREFIX: "securacv"})
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["result"].unique_id == "mqtt_securacv"
    assert result["result"].state is ConfigEntryState.LOADED

    result = await start_user_flow(hass, SETUP_MODE_MQTT)
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {CONF_MQTT_PREFIX: "securacv"})
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_configured"


# ── Automatic ────────────────────────────────────────────────────────────


async def test_automatic_finds_the_app_at_its_hostname(
    hass: HomeAssistant, aioclient_mock: AiohttpClientMocker, mqtt_mock: MqttMockHAClient
) -> None:
    """Automatic on Home Assistant OS: the app answers at its Supervisor hostname."""
    app = KernelMock(aioclient_mock, SigningKernel(), url=DEFAULT_KERNEL_URL)
    result = await start_user_flow(hass, SETUP_MODE_AUTO)
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY
    entry = result["result"]
    assert entry.unique_id == f"kernel_{KERNEL_DEVICE_ID}"
    assert entry.data[CONF_URL] == DEFAULT_KERNEL_URL
    assert entry.data[CONF_AUTH_MODE] == AUTH_MODE_TOKEN_FILE
    assert entry.data[CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == app.verifying_key
    assert entry.data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert entry.state is ConfigEntryState.LOADED


async def test_automatic_without_an_mqtt_integration_names_the_missing_broker(
    hass: HomeAssistant, aioclient_mock: AiohttpClientMocker, kernel: KernelMock
) -> None:
    """J2 with no MQTT integration at all: no announce can be heard, so the
    final screen says the MQTT integration and a shared broker come first,
    instead of promising to connect by itself."""
    assert hass.config_entries.async_entries("mqtt") == []
    aioclient_mock.get(f"{DEFAULT_KERNEL_URL}/api/identity", exc=aiohttp.ClientConnectionError("no app"))
    with patch("custom_components.securacv.config_flow.ANNOUNCE_LISTEN_SECONDS", 0.2):
        result = await start_user_flow(hass, SETUP_MODE_AUTO)
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["description"] == "no_kernel_found_no_mqtt"
    assert set(result["description_placeholders"]) == {"addon_repo_url", "docker_quickstart_url"}
    entry = result["result"]
    assert entry.data == {CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv", CONF_SETUP_MODE: SETUP_MODE_MQTT}
    assert ir.async_get(hass).async_get_issue(DOMAIN, ISSUE_NO_KERNEL_FOUND) is not None
    # The words themselves, through the translation Home Assistant renders.
    from homeassistant.helpers.translation import async_get_translations

    translations = await async_get_translations(hass, "en", "config", {DOMAIN})
    text = translations[f"component.{DOMAIN}.config.create_entry.no_kernel_found_no_mqtt"]
    assert "MQTT integration is not set up" in text and "share a broker" in text


async def test_automatic_with_nothing_running_creates_the_mqtt_entry_and_the_repair(
    hass: HomeAssistant, aioclient_mock: AiohttpClientMocker, mqtt_mock: MqttMockHAClient, kernel: KernelMock
) -> None:
    """J2: no app, no announce → MQTT-only entry, final text, Repairs no_kernel_found;
    the issue clears when a kernel entry loads later."""
    aioclient_mock.get(f"{DEFAULT_KERNEL_URL}/api/identity", exc=aiohttp.ClientConnectionError("no app"))
    with patch("custom_components.securacv.config_flow.ANNOUNCE_LISTEN_SECONDS", 0.2):
        result = await start_user_flow(hass, SETUP_MODE_AUTO)
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["description"] == "no_kernel_found"
    assert set(result["description_placeholders"]) == {"addon_repo_url", "docker_quickstart_url"}
    entry = result["result"]
    assert entry.unique_id == "mqtt_securacv"
    assert entry.data == {CONF_ENABLE_MQTT: True, CONF_MQTT_PREFIX: "securacv", CONF_SETUP_MODE: SETUP_MODE_MQTT}
    assert entry.state is ConfigEntryState.LOADED

    issue_registry = ir.async_get(hass)
    issue = issue_registry.async_get_issue(DOMAIN, ISSUE_NO_KERNEL_FOUND)
    assert issue is not None
    assert issue.severity is ir.IssueSeverity.WARNING
    assert issue.is_fixable is False
    assert issue.translation_placeholders and set(issue.translation_placeholders) == {"addon_repo_url", "docker_quickstart_url"}

    # A kernel arrives later (the announce path promotes the MQTT-only entry).
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_MQTT},
        data=mqtt_info(f"witness/{KERNEL_DEVICE_ID}/announce", kernel.kernel.announce()),
    )
    assert result["step_id"] == "announce_confirm"
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    assert result["step_id"] == "pair"
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_PAIRING_CODE: "483921"}
    )
    await hass.async_block_till_done()
    # Canaries first, kernel later: the MQTT-only entry is promoted, not doubled.
    assert result["type"] is FlowResultType.ABORT and result["reason"] == "already_configured"
    assert len(hass.config_entries.async_entries(DOMAIN)) == 1
    promoted = hass.config_entries.async_entries(DOMAIN)[0]
    assert promoted.unique_id == f"kernel_{KERNEL_DEVICE_ID}"
    assert promoted.data[CONF_AUTH_MODE] == AUTH_MODE_PAIRED
    assert promoted.data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert promoted.state is ConfigEntryState.LOADED
    assert issue_registry.async_get_issue(DOMAIN, ISSUE_NO_KERNEL_FOUND) is None


async def test_automatic_hears_a_sidecar_announce(
    hass: HomeAssistant, aioclient_mock: AiohttpClientMocker, mqtt_mock: MqttMockHAClient, kernel: KernelMock
) -> None:
    """J3 through Automatic: the retained announce lands while the flow listens.

    On a real core the same announce also matches the manifest's
    ``witness/+/announce`` discovery filter, so the MQTT integration starts
    the discovered-card flow first; Automatic then yields to it
    (``already_in_progress``) rather than offering the same kernel twice.
    """
    from pytest_homeassistant_custom_component.common import async_fire_mqtt_message

    aioclient_mock.get(f"{DEFAULT_KERNEL_URL}/api/identity", exc=aiohttp.ClientConnectionError("no app"))
    with patch("custom_components.securacv.config_flow.ANNOUNCE_LISTEN_SECONDS", 3.0):
        task = hass.async_create_task(start_user_flow(hass, SETUP_MODE_AUTO))
        await asyncio.sleep(0.3)
        async_fire_mqtt_message(
            hass, f"witness/{KERNEL_DEVICE_ID}/announce", json.dumps(kernel.kernel.announce()), qos=1, retain=True
        )
        result = await task
        await hass.async_block_till_done()
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_in_progress"
    discovered = [
        flow for flow in hass.config_entries.flow.async_progress_by_handler(DOMAIN)
        if flow["context"]["source"] == config_entries.SOURCE_MQTT
    ]
    assert len(discovered) == 1
    assert discovered[0]["step_id"] == "announce_confirm"
    assert discovered[0]["context"]["unique_id"] == f"kernel_{KERNEL_DEVICE_ID}"
    assert discovered[0]["context"]["title_placeholders"] == {"name": f"sidecar {KERNEL_DEVICE_ID}"}


# ── the Supervisor (the app) ─────────────────────────────────────────────


async def test_hassio_discovery_creates_the_both_entry(
    hass: HomeAssistant, aioclient_mock: AiohttpClientMocker, mqtt_mock: MqttMockHAClient
) -> None:
    """C6: the five-key payload → a zero-field confirm → a token_file `both` entry
    keyed kernel_<device_id>, the key pinned from /api/identity."""
    app = KernelMock(aioclient_mock, SigningKernel(), url=ADDON_URL)
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_HASSIO}, data=hassio_info()
    )
    assert result["type"] is FlowResultType.FORM
    assert result["step_id"] == "hassio_confirm"
    assert result["description_placeholders"] == {"name": "SecuraCV", "url": ADDON_URL}
    # Confirm-only: no fields.
    assert result["data_schema"] is None or not result["data_schema"].schema

    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY
    entry = result["result"]
    assert entry.unique_id == f"kernel_{KERNEL_DEVICE_ID}"
    assert entry.data[CONF_URL] == ADDON_URL
    assert entry.data[CONF_TOKEN] == ""
    assert entry.data[CONF_TOKEN_FILE] == DEFAULT_TOKEN_FILE
    assert entry.data[CONF_AUTH_MODE] == AUTH_MODE_TOKEN_FILE
    assert entry.data[CONF_MQTT_TOPIC_PREFIX] == "witness"
    assert entry.data[CONF_KERNEL_DEVICE_ID] == KERNEL_DEVICE_ID
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == app.verifying_key
    assert entry.data[CONF_ENABLE_MQTT] is True
    assert entry.data[CONF_MQTT_PREFIX] == "securacv"
    assert entry.data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert entry.state is ConfigEntryState.LOADED


async def test_hassio_rediscovery_updates_the_url_and_reloads_once(
    hass: HomeAssistant, aioclient_mock: AiohttpClientMocker, mqtt_mock: MqttMockHAClient
) -> None:
    """discovery-update-info: a new host updates the entry and reloads it; the
    identical payload again changes nothing and reloads nothing."""
    app = KernelMock(aioclient_mock, SigningKernel(), url=ADDON_URL)
    new_url = "http://e1d2c3b4-privacy-witness-kernel:8799"
    KernelMock(aioclient_mock, app.kernel, url=new_url, verifying_key=app.verifying_key)
    entry = make_kernel_entry(
        auth_mode=AUTH_MODE_TOKEN_FILE, enable_mqtt=True, url=ADDON_URL, verifying_key=app.verifying_key
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()

    with patch.object(hass.config_entries, "async_schedule_reload", wraps=hass.config_entries.async_schedule_reload) as reload:
        result = await hass.config_entries.flow.async_init(
            DOMAIN,
            context={"source": config_entries.SOURCE_HASSIO},
            data=hassio_info(host="e1d2c3b4-privacy-witness-kernel"),
        )
        await hass.async_block_till_done()
        assert result["type"] is FlowResultType.ABORT
        assert result["reason"] == "already_configured"
        assert entry.data[CONF_URL] == new_url
        assert entry.data[CONF_KERNEL_VERIFYING_KEY] == app.verifying_key, "a rediscovery never moves the pin"
        assert reload.call_count == 1
        assert entry.state is ConfigEntryState.LOADED

        # The same payload again: a no-op.
        result = await hass.config_entries.flow.async_init(
            DOMAIN,
            context={"source": config_entries.SOURCE_HASSIO},
            data=hassio_info(host="e1d2c3b4-privacy-witness-kernel"),
        )
        await hass.async_block_till_done()
        assert result["type"] is FlowResultType.ABORT and result["reason"] == "already_configured"
        assert reload.call_count == 1
        assert entry.data[CONF_URL] == new_url


async def test_hassio_payload_without_connection_details_aborts(hass: HomeAssistant) -> None:
    """A payload missing host/port cannot be configured."""
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_HASSIO},
        data=HassioServiceInfo(config={"token_file": DEFAULT_TOKEN_FILE}, name="SecuraCV", slug="privacy_witness_kernel", uuid="x"),
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "invalid_discovery_info"


async def test_hassio_legacy_payload_without_device_id_reads_the_identity(
    hass: HomeAssistant, aioclient_mock: AiohttpClientMocker, mqtt_mock: MqttMockHAClient
) -> None:
    """An older app that omits device_id: the flow asks /api/identity before the unique_id."""
    app = KernelMock(aioclient_mock, SigningKernel(), url=ADDON_URL)
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_HASSIO}, data=hassio_info(device_id=None)
    )
    assert result["type"] is FlowResultType.FORM and result["step_id"] == "hassio_confirm"
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["result"].unique_id == f"kernel_{KERNEL_DEVICE_ID}"
    assert result["result"].data[CONF_KERNEL_VERIFYING_KEY] == app.verifying_key


# ── the MQTT announce and pairing (the sidecar) ──────────────────────────


async def test_announce_confirm_and_pair_create_the_paired_entry(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient
) -> None:
    """C1/C2: announce → announce_confirm (fingerprint shown) → pair (code) → paired entry."""
    announce = kernel.kernel.announce()
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_MQTT},
        data=mqtt_info(f"witness/{KERNEL_DEVICE_ID}/announce", announce),
    )
    assert result["type"] is FlowResultType.FORM
    assert result["step_id"] == "announce_confirm"
    placeholders = result["description_placeholders"]
    assert placeholders["fingerprint"] == kernel.kernel.fingerprint
    assert placeholders["device_id"] == KERNEL_DEVICE_ID
    assert placeholders["kernel_version"] == "2.4.16"
    # No api.host in the announce: the broker's host is the prefill.
    assert placeholders["url"] == "http://mock-broker:8799"

    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    assert result["type"] is FlowResultType.FORM
    assert result["step_id"] == "pair"
    assert result["description_placeholders"]["fingerprint"] == kernel.kernel.fingerprint

    # Wrong code: the kernel's constant 401 → the field error, the form stays.
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_PAIRING_CODE: "000000"}
    )
    assert result["type"] is FlowResultType.FORM
    assert result["errors"] == {"base": "invalid_pairing_code"}
    # Not six digits: refused before any request.
    pair_calls = len(kernel.calls("/api/pair", "POST"))
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_PAIRING_CODE: "12ab"}
    )
    assert result["errors"] == {CONF_PAIRING_CODE: "invalid_pairing_code"}
    assert len(kernel.calls("/api/pair", "POST")) == pair_calls
    # A URL without a scheme is refused too.
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: "192.168.1.20:8799", CONF_PAIRING_CODE: "483921"}
    )
    assert result["errors"] == {CONF_URL: "invalid_url"}

    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_PAIRING_CODE: "483921"}
    )
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY
    entry = result["result"]
    assert entry.unique_id == f"kernel_{KERNEL_DEVICE_ID}"
    assert entry.data[CONF_AUTH_MODE] == AUTH_MODE_PAIRED
    assert entry.data[CONF_TOKEN] == PAIRED_TOKEN
    assert entry.data[CONF_KERNEL_TOKEN_ID] == PAIRED_TOKEN_ID
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == kernel.verifying_key
    assert entry.data[CONF_MQTT_TOPIC_PREFIX] == "witness"
    assert entry.data[CONF_ENABLE_MQTT] is True and entry.data[CONF_SETUP_MODE] == SETUP_MODE_BOTH
    assert entry.state is ConfigEntryState.LOADED
    # The pair request carried the code and the label, nothing else.
    body = kernel.calls("/api/pair", "POST")[-1][2]
    assert body == {"code": "483921", "label": "home-assistant"}
    # Once paired, the kernel is read with the paired bearer.
    assert f"Bearer {PAIRED_TOKEN}" in kernel.auth_headers("/api/identity")


async def test_a_full_paired_set_keeps_the_code_and_names_the_remedy(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient
) -> None:
    """The kernel's 403 `paired_token_limit` (its paired set is full; the
    right code was presented and NOT consumed) is its own form error, so the
    person revokes a token on the Docker host and submits the same code."""
    announce = kernel.kernel.announce()
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_MQTT},
        data=mqtt_info(f"witness/{KERNEL_DEVICE_ID}/announce", announce),
    )
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    assert result["step_id"] == "pair"

    kernel.pair_status = 403
    kernel.pair_error = "paired_token_limit"
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_PAIRING_CODE: "483921"}
    )
    assert result["type"] is FlowResultType.FORM
    assert result["step_id"] == "pair"
    assert result["errors"] == {"base": "paired_token_limit"}
    # Any other 403 stays the generic connection error (C2).
    kernel.pair_error = "forbidden"
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_PAIRING_CODE: "483921"}
    )
    assert result["errors"] == {"base": "cannot_connect"}

    # A token revoked on the host: the same code pairs.
    kernel.pair_status = 200
    kernel.pair_error = None
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_PAIRING_CODE: "483921"}
    )
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["result"].data[CONF_AUTH_MODE] == AUTH_MODE_PAIRED


async def test_announce_signature_is_checked_against_the_pin(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """A pinned kernel's announce must be signed by the pinned key."""
    entry = await setup_kernel_entry(auth_mode=AUTH_MODE_PAIRED, enable_mqtt=True)
    impostor = SigningKernel()
    forged = impostor.announce(verifying_key=kernel.verifying_key, api={"scheme": "http", "bind": "all", "port": 8799, "host": "10.0.0.66"})
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_MQTT},
        data=mqtt_info(f"witness/{KERNEL_DEVICE_ID}/announce", forged),
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "announce_signature_invalid"
    assert entry.data[CONF_URL] == KERNEL_URL, "an unsigned host never moves the entry"

    # Signed by the pin but naming another key: a key change, Repairs, abort.
    disagreeing = kernel.kernel.announce(verifying_key=impostor.verifying_key)
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_MQTT},
        data=mqtt_info(f"witness/{KERNEL_DEVICE_ID}/announce", disagreeing),
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "announce_key_mismatch"
    assert result["description_placeholders"] == {
        "pinned_fingerprint": kernel.kernel.fingerprint,
        "received_fingerprint": impostor.fingerprint,
    }
    assert ir.async_get(hass).async_get_issue(DOMAIN, f"tofu_mismatch_{entry.entry_id}") is not None

    # A genuine, signed announce with an explicit host moves the URL and reloads.
    moved = kernel.kernel.announce(api={"scheme": "http", "bind": "all", "port": 8799, "host": "192.168.1.21"})
    KernelMock(hass.data.get("_unused", None) or kernel.mock, kernel.kernel, url="http://192.168.1.21:8799")
    with patch.object(hass.config_entries, "async_schedule_reload", wraps=hass.config_entries.async_schedule_reload) as reload:
        result = await hass.config_entries.flow.async_init(
            DOMAIN,
            context={"source": config_entries.SOURCE_MQTT},
            data=mqtt_info(f"witness/{KERNEL_DEVICE_ID}/announce", moved),
        )
        await hass.async_block_till_done()
    assert result["type"] is FlowResultType.ABORT and result["reason"] == "already_configured"
    assert entry.data[CONF_URL] == "http://192.168.1.21:8799"
    assert reload.call_count == 1


async def test_announce_with_a_cleared_or_malformed_payload_aborts(hass: HomeAssistant) -> None:
    """The empty retained clear of a sidecar that shut down is nothing to offer."""
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_MQTT},
        data=mqtt_info(f"witness/{KERNEL_DEVICE_ID}/announce", b""),
    )
    assert result["type"] is FlowResultType.ABORT and result["reason"] == "invalid_discovery_info"
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_MQTT},
        data=mqtt_info(f"witness/{KERNEL_DEVICE_ID}/announce", {"v": 2, "device_id": KERNEL_DEVICE_ID}),
    )
    assert result["type"] is FlowResultType.ABORT and result["reason"] == "invalid_discovery_info"


async def test_pair_key_disagreeing_with_the_announce_revokes_and_aborts(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient
) -> None:
    """The /api/pair answer must name the announced key; otherwise the minted token is revoked."""
    other = SigningKernel()
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_MQTT},
        data=mqtt_info(f"witness/{KERNEL_DEVICE_ID}/announce", other.announce()),
    )
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_PAIRING_CODE: "483921"}
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "announce_key_mismatch"
    assert kernel.revoked == [PAIRED_TOKEN_ID]
    assert hass.config_entries.async_entries(DOMAIN) == []


async def test_canary_mqtt_discovery_confirms_and_the_claim_topic_is_not_a_device(
    hass: HomeAssistant, mqtt_mock: MqttMockHAClient
) -> None:
    """Anything under securacv/# is a Canary (confirm, zero fields); the claim is not."""
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_MQTT},
        data=mqtt_info("securacv/claim/pwk_1a2b3c4d", {"v": 1}, retain=True),
    )
    assert result["type"] is FlowResultType.ABORT and result["reason"] == "not_securacv_device"

    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_MQTT},
        data=mqtt_info("securacv/canary01/status", {"online": True}, retain=True),
    )
    assert result["type"] is FlowResultType.FORM and result["step_id"] == "confirm"
    assert result["description_placeholders"] == {"prefix": "securacv"}
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["result"].unique_id == "mqtt_securacv"


# ── reauth and reconfigure ───────────────────────────────────────────────


async def test_reauth_of_a_paired_entry_takes_a_new_code(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """The coordinator's ConfigEntryAuthFailed → reauth_confirm → a fresh pairing."""
    entry = await setup_kernel_entry(auth_mode=AUTH_MODE_PAIRED, enable_mqtt=True)
    kernel.paired.append({"id": PAIRED_TOKEN_ID, "label": "home-assistant"})
    result = await entry.start_reauth_flow(hass)
    assert result["type"] is FlowResultType.FORM
    assert result["step_id"] == "reauth_confirm"
    assert result["description_placeholders"]["fingerprint"] == kernel.kernel.fingerprint
    assert result["description_placeholders"]["device_id"] == KERNEL_DEVICE_ID
    fields = {str(key) for key in result["data_schema"].schema}
    assert fields == {CONF_URL, CONF_PAIRING_CODE}

    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_PAIRING_CODE: "483921"}
    )
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "reauth_successful"
    assert entry.data[CONF_KERNEL_TOKEN_ID] == "00000002"
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == kernel.verifying_key
    assert entry.state is ConfigEntryState.LOADED


async def test_reauth_refuses_a_different_kernel(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """Re-pairing with another kernel is a new entry, never a move."""
    entry = await setup_kernel_entry(auth_mode=AUTH_MODE_PAIRED, enable_mqtt=True)
    other = KernelMock(kernel.mock, SigningKernel(device_id="pwk_feedbeef"), url="http://192.168.1.99:8799")
    result = await entry.start_reauth_flow(hass)
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: other.url, CONF_PAIRING_CODE: "483921"}
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "wrong_kernel"
    assert entry.data[CONF_URL] == KERNEL_URL


async def test_reauth_of_a_token_file_entry_takes_a_path(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry, tmp_path
) -> None:
    """The app's entry re-authenticates with a readable token file."""
    token_file = tmp_path / "api_token"
    token_file.write_text("ef" * 32, encoding="utf-8")
    entry = await setup_kernel_entry(auth_mode=AUTH_MODE_TOKEN_FILE, enable_mqtt=True, token_file=str(token_file))
    result = await entry.start_reauth_flow(hass)
    assert result["step_id"] == "reauth_confirm"
    fields = {str(key) for key in result["data_schema"].schema}
    assert CONF_TOKEN_FILE in fields and CONF_PAIRING_CODE not in fields
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: KERNEL_URL, CONF_TOKEN_FILE: str(token_file)}
    )
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.ABORT and result["reason"] == "reauth_successful"
    assert entry.data[CONF_TOKEN_FILE] == str(token_file)
    assert f"Bearer {'ef' * 32}" in kernel.auth_headers("/api/identity")


async def test_reconfigure_moves_the_url_of_the_same_kernel(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """Reconfigure: a new URL for the same kernel is accepted and the entry reloads."""
    entry = await setup_kernel_entry(auth_mode=AUTH_MODE_STATIC, enable_mqtt=True)
    moved = KernelMock(kernel.mock, kernel.kernel, url="http://192.168.1.30:8799", verifying_key=kernel.verifying_key)
    result = await entry.start_reconfigure_flow(hass)
    assert result["type"] is FlowResultType.FORM and result["step_id"] == "reconfigure"
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {CONF_URL: moved.url, CONF_TOKEN: "ab" * 32, CONF_TOKEN_FILE: "", CONF_MQTT_TOPIC_PREFIX: "witness2"}
    )
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.ABORT and result["reason"] == "reconfigure_successful"
    assert entry.data[CONF_URL] == moved.url
    assert entry.data[CONF_MQTT_TOPIC_PREFIX] == "witness2"
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == kernel.verifying_key
    assert entry.state is ConfigEntryState.LOADED
    assert moved.calls("/api/identity")


# ── options ──────────────────────────────────────────────────────────────


async def test_options_flow_sets_evidence_keep(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry
) -> None:
    """The settings step bounds evidence_keep to 1–200."""
    entry = await setup_kernel_entry()
    result = await hass.config_entries.options.async_init(entry.entry_id)
    assert result["type"] is FlowResultType.MENU
    assert set(result["menu_options"]) == {"pin", "rotate", "unpin", "settings"}
    result = await hass.config_entries.options.async_configure(result["flow_id"], {"next_step_id": "settings"})
    assert result["type"] is FlowResultType.FORM and result["step_id"] == "settings"

    result = await hass.config_entries.options.async_configure(result["flow_id"], {CONF_EVIDENCE_KEEP: 0})
    assert result["type"] is FlowResultType.FORM
    assert result["errors"] == {CONF_EVIDENCE_KEEP: "invalid_evidence_keep"}

    result = await hass.config_entries.options.async_configure(result["flow_id"], {CONF_EVIDENCE_KEEP: 5})
    await hass.async_block_till_done()
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert entry.options == {CONF_EVIDENCE_KEEP: 5}


async def test_options_pin_rotate_unpin_a_canary_key(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry
) -> None:
    """The trust-store menu drives TrustStore on a real core."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    key_a = "aa" * 32
    key_b = "bb" * 32

    result = await hass.config_entries.options.async_init(entry.entry_id)
    result = await hass.config_entries.options.async_configure(result["flow_id"], {"next_step_id": "pin"})
    result = await hass.config_entries.options.async_configure(
        result["flow_id"], {"device_id": "canary01", "pubkey_hex": key_a.upper()}
    )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    trust = entry.runtime_data.trust_store
    assert trust.is_pinned("canary01")

    result = await hass.config_entries.options.async_init(entry.entry_id)
    result = await hass.config_entries.options.async_configure(result["flow_id"], {"next_step_id": "rotate"})
    result = await hass.config_entries.options.async_configure(
        result["flow_id"], {"device_id": "canary02", "pubkey_hex": key_b}
    )
    assert result["type"] is FlowResultType.FORM and result["errors"] == {"device_id": "device_not_pinned"}
    result = await hass.config_entries.options.async_configure(
        result["flow_id"], {"device_id": "canary01", "pubkey_hex": key_b}
    )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert trust.get("canary01").pubkey_hex == key_b

    result = await hass.config_entries.options.async_init(entry.entry_id)
    result = await hass.config_entries.options.async_configure(result["flow_id"], {"next_step_id": "unpin"})
    result = await hass.config_entries.options.async_configure(result["flow_id"], {"device_id": "canary01"})
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert not trust.is_pinned("canary01")


# ── migration ────────────────────────────────────────────────────────────


async def test_a_version_2_entry_migrates_to_the_kernel_unique_id(
    hass: HomeAssistant, kernel: KernelMock
) -> None:
    """VERSION 2 → 3: the identity keys the entry and pins the key."""
    entry = MockConfigEntry(
        domain=DOMAIN,
        title="SecuraCV",
        version=2,
        unique_id=KERNEL_URL,
        data={CONF_URL: KERNEL_URL, CONF_TOKEN: "ab" * 32, CONF_SETUP_MODE: SETUP_MODE_KERNEL, CONF_ENABLE_MQTT: False},
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.version == 3
    assert entry.unique_id == f"kernel_{KERNEL_DEVICE_ID}"
    assert entry.data[CONF_AUTH_MODE] == AUTH_MODE_STATIC
    assert entry.data[CONF_KERNEL_DEVICE_ID] == KERNEL_DEVICE_ID
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == kernel.verifying_key
    assert entry.data[CONF_MQTT_TOPIC_PREFIX] == "witness"
    assert entry.data[CONF_CLAIM_NONCE]
    assert CONF_KERNEL_PREVIOUS_KEYS not in entry.data
    assert entry.state is ConfigEntryState.LOADED


async def test_an_entry_from_the_future_is_refused(hass: HomeAssistant, kernel: KernelMock) -> None:
    """A downgrade never guesses at a newer schema."""
    entry = make_kernel_entry(verifying_key=kernel.verifying_key)
    object.__setattr__(entry, "version", 4)
    entry.add_to_hass(hass)
    assert not await hass.config_entries.async_setup(entry.entry_id)
    assert entry.state is ConfigEntryState.MIGRATION_ERROR


async def test_mqtt_only_entry_loads_without_a_kernel(hass: HomeAssistant, mqtt_mock: MqttMockHAClient) -> None:
    """The J2 entry: Canary subscriptions only, no kernel entities."""
    entry = make_mqtt_only_entry()
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    assert entry.state is ConfigEntryState.LOADED
    assert hass.states.get("binary_sensor.securacv_chain_valid") is None
    assert hass.states.get("button.securacv_verify_now") is None
