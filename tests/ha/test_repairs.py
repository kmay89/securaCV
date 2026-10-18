"""Repairs on a real core: the issue table (C9) and the tofu_mismatch fix flow
driven through the repairs HTTP API."""
from __future__ import annotations

from datetime import timedelta
from http import HTTPStatus

from freezegun.api import FrozenDateTimeFactory
from homeassistant.config_entries import ConfigEntryState
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry as er, issue_registry as ir
from homeassistant.setup import async_setup_component
from pytest_homeassistant_custom_component.common import async_fire_time_changed
from pytest_homeassistant_custom_component.typing import ClientSessionGenerator, MqttMockHAClient

from conftest import (
    KERNEL_DEVICE_ID,
    KERNEL_URL,
    KernelMock,
    SigningKernel,
    fingerprint_of,
    make_kernel_entry,
    settle,
    status_document,
)
from custom_components.securacv.const import (
    CONF_KERNEL_PREVIOUS_KEYS,
    CONF_KERNEL_VERIFYING_KEY,
    CONF_MIGRATION_NOTICE_SHOWN,
    DOMAIN,
)


async def backstop(hass: HomeAssistant, freezer: FrozenDateTimeFactory, minutes: int = 5) -> None:
    """Advance past the HTTP backstop interval and let it run."""
    freezer.tick(timedelta(minutes=minutes, seconds=1))
    async_fire_time_changed(hass)
    await settle(hass)


async def test_tofu_mismatch_is_raised_and_fixed_through_the_repairs_api(
    hass: HomeAssistant,
    kernel: KernelMock,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
    freezer: FrozenDateTimeFactory,
) -> None:
    """A served key other than the pin → CRITICAL fixable issue; confirming
    rotates the pin, keeps the previous key, clears the issue, reloads."""
    assert await async_setup_component(hass, "repairs", {})
    entry = await setup_kernel_entry()
    pinned = kernel.verifying_key
    issue_id = f"tofu_mismatch_{entry.entry_id}"
    registry = ir.async_get(hass)
    assert registry.async_get_issue(DOMAIN, issue_id) is None

    replacement = SigningKernel()
    kernel.verifying_key = replacement.verifying_key
    kernel.kernel = replacement
    await backstop(hass, freezer)

    issue = registry.async_get_issue(DOMAIN, issue_id)
    assert issue is not None
    assert issue.severity is ir.IssueSeverity.CRITICAL
    assert issue.is_fixable is True
    assert issue.translation_key == "tofu_mismatch"
    assert issue.translation_placeholders == {
        "pinned_fingerprint": fingerprint_of(pinned),
        "received_fingerprint": replacement.fingerprint,
        "url": KERNEL_URL,
    }
    # The key change shows everywhere it should, and nowhere as "Failed".
    fingerprint = hass.states.get("sensor.securacv_kernel_key_fingerprint")
    assert fingerprint.attributes["key_mismatch"] is True
    verification = hass.states.get("event.securacv_chain_verification")
    assert verification.attributes["event_type"] == "witness_key_changed"
    chain_valid = hass.states.get("binary_sensor.securacv_chain_valid")
    # Unknown, never off: a key change is this Repairs issue, not a failed walk.
    assert chain_valid.state == "unknown"
    assert chain_valid.attributes["key_mismatch"] is True
    assert chain_valid.attributes["trust_reason"] == "kernel_key_mismatch"
    assert "Repairs" in chain_valid.attributes["error"]
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == pinned, "never an automatic re-pin"

    client = await hass_client()
    resp = await client.post("/api/repairs/issues/fix", json={"handler": DOMAIN, "issue_id": issue_id})
    assert resp.status == HTTPStatus.OK
    flow = await resp.json()
    assert flow["type"] == "form"
    assert flow["step_id"] == "confirm"
    assert flow["description_placeholders"]["received_fingerprint"] == replacement.fingerprint
    assert flow["description_placeholders"]["url"] == KERNEL_URL

    resp = await client.post(f"/api/repairs/issues/fix/{flow['flow_id']}", json={})
    assert resp.status == HTTPStatus.OK
    done = await resp.json()
    assert done["type"] == "create_entry"
    await settle(hass)

    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == replacement.verifying_key
    assert entry.data[CONF_KERNEL_PREVIOUS_KEYS] == [pinned]
    assert registry.async_get_issue(DOMAIN, issue_id) is None
    assert entry.state is ConfigEntryState.LOADED
    # Verified against the new pin after the reload.
    assert hass.states.get("binary_sensor.securacv_chain_valid").state == "on"
    assert hass.states.get("sensor.securacv_kernel_key_fingerprint").state == replacement.fingerprint


async def test_kernel_unreachable_after_fifteen_minutes_and_cleared_on_recovery(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """Three failed backstops in a row → ERROR issue (not persistent); one success clears it."""
    entry = await setup_kernel_entry(enable_mqtt=True)
    issue_id = f"kernel_unreachable_{entry.entry_id}"
    registry = ir.async_get(hass)

    kernel.connection_down = True
    await backstop(hass, freezer)
    await backstop(hass, freezer)
    assert registry.async_get_issue(DOMAIN, issue_id) is None, "a short outage is not an issue"
    await backstop(hass, freezer)
    await backstop(hass, freezer)
    issue = registry.async_get_issue(DOMAIN, issue_id)
    assert issue is not None
    assert issue.severity is ir.IssueSeverity.ERROR
    assert issue.is_fixable is False
    assert issue.is_persistent is False
    assert issue.translation_placeholders == {"url": KERNEL_URL}

    kernel.connection_down = False
    await backstop(hass, freezer)
    assert registry.async_get_issue(DOMAIN, issue_id) is None


async def test_storage_replace_follows_the_status_report(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """/status recommending a replacement → WARNING issue with the wear; back to good clears it."""
    kernel.status = status_document("replacement_recommended", wear_pct=91.0)
    entry = await setup_kernel_entry()
    issue_id = f"storage_replace_{entry.entry_id}"
    registry = ir.async_get(hass)
    issue = registry.async_get_issue(DOMAIN, issue_id)
    assert issue is not None
    assert issue.translation_placeholders == {"wear_pct": "91.0"}
    assert hass.states.get("binary_sensor.securacv_storage_replacement_recommended").state == "on"

    kernel.status = status_document("good")
    await backstop(hass, freezer)
    assert registry.async_get_issue(DOMAIN, issue_id) is None
    assert hass.states.get("binary_sensor.securacv_storage_replacement_recommended").state == "off"


async def test_entities_migrated_only_when_the_bridge_entities_existed(
    hass: HomeAssistant, kernel: KernelMock, mqtt_mock: MqttMockHAClient
) -> None:
    """D9: the notice is for people whose pwk_* entities are being replaced."""
    registry = er.async_get(hass)
    issues = ir.async_get(hass)

    # A fresh install: nothing to migrate, no notice, but the flag is set.
    fresh = make_kernel_entry(enable_mqtt=True, verifying_key=kernel.verifying_key)
    fresh.add_to_hass(hass)
    assert await hass.config_entries.async_setup(fresh.entry_id)
    await settle(hass)
    assert issues.async_get_issue(DOMAIN, f"entities_migrated_{fresh.entry_id}") is None
    assert fresh.data[CONF_MIGRATION_NOTICE_SHOWN] is True
    assert await hass.config_entries.async_remove(fresh.entry_id)
    await hass.async_block_till_done()

    # An upgrade: MQTT discovery had created the bridge's entities.
    registry.async_get_or_create("sensor", "mqtt", f"{KERNEL_DEVICE_ID}_last_event", suggested_object_id="pwk_last_event")
    upgraded = make_kernel_entry(enable_mqtt=True, verifying_key=kernel.verifying_key)
    upgraded.add_to_hass(hass)
    assert await hass.config_entries.async_setup(upgraded.entry_id)
    await settle(hass)
    issue = issues.async_get_issue(DOMAIN, f"entities_migrated_{upgraded.entry_id}")
    assert issue is not None
    assert issue.severity is ir.IssueSeverity.WARNING
    assert issue.is_fixable is False
    assert issue.learn_more_url
    assert upgraded.data[CONF_MIGRATION_NOTICE_SHOWN] is True

    # Raised once: a reload does not raise it again after it was dismissed.
    issues.async_delete(DOMAIN, f"entities_migrated_{upgraded.entry_id}")
    assert await hass.config_entries.async_reload(upgraded.entry_id)
    await settle(hass)
    assert issues.async_get_issue(DOMAIN, f"entities_migrated_{upgraded.entry_id}") is None


async def test_removing_the_entry_clears_its_issues(
    hass: HomeAssistant, kernel: KernelMock, setup_kernel_entry, freezer: FrozenDateTimeFactory
) -> None:
    """Every entry-scoped issue goes with the entry."""
    kernel.status = status_document("critical", wear_pct=99.0)
    entry = await setup_kernel_entry()
    registry = ir.async_get(hass)
    assert registry.async_get_issue(DOMAIN, f"storage_replace_{entry.entry_id}") is not None
    assert await hass.config_entries.async_remove(entry.entry_id)
    await hass.async_block_till_done()
    assert registry.async_get_issue(DOMAIN, f"storage_replace_{entry.entry_id}") is None
