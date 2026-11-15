"""Repairs on a real core: the issue table (C9) and the tofu_mismatch fix flow
driven through the repairs HTTP API."""
from __future__ import annotations

import json
import time
from datetime import timedelta
from http import HTTPStatus

from freezegun.api import FrozenDateTimeFactory
from homeassistant import config_entries
from homeassistant.config_entries import ConfigEntryState
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers import entity_registry as er, issue_registry as ir
from homeassistant.helpers.service_info.mqtt import MqttServiceInfo
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
    # The two fingerprints and nothing else: the kernel's URL and the key it
    # served stay out of the issue (a diagnostics download carries it, F399).
    fingerprints = {
        "pinned_fingerprint": fingerprint_of(pinned),
        "received_fingerprint": replacement.fingerprint,
    }
    assert issue.translation_placeholders == fingerprints
    assert issue.data == fingerprints
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
    assert flow["description_placeholders"] == fingerprints

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


async def test_the_fix_flow_pins_only_the_key_the_coordinator_holds(
    hass: HomeAssistant,
    kernel: KernelMock,
    mqtt_mock: MqttMockHAClient,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
    freezer: FrozenDateTimeFactory,
) -> None:
    """The fix flow through the repairs API across a reload (ledger F399).

    The issue holds fingerprints only, so the key the flow pins is the one
    the entry's coordinator saw the kernel serve. After a reload, before the
    reloaded coordinator has reached the kernel, it holds none: the flow
    aborts with `no_received_key` (it does not raise on the missing key), and
    a key an older integration left in the issue's data is never pinned. Once
    the backstop sees a key the form shows its fingerprint; a third key
    served before the submit is shown again rather than pinned unseen, and
    the next submit pins exactly the key the coordinator holds then."""
    assert await async_setup_component(hass, "repairs", {})
    entry = await setup_kernel_entry(enable_mqtt=True)
    pinned = kernel.verifying_key
    issue_id = f"tofu_mismatch_{entry.entry_id}"
    registry = ir.async_get(hass)
    client = await hass_client()

    second = SigningKernel()
    kernel.verifying_key = second.verifying_key
    kernel.kernel = second
    await backstop(hass, freezer)
    assert registry.async_get_issue(DOMAIN, issue_id) is not None

    # A restart's worth of state: the issue stays, the coordinator is new,
    # and the kernel does not answer its first refresh.
    kernel.connection_down = True
    assert await hass.config_entries.async_reload(entry.entry_id)
    await settle(hass)
    assert entry.state is ConfigEntryState.LOADED
    # The issue as an older integration raised it: the served key and the
    # URL in its data. The flow does not read them.
    stale = SigningKernel()
    ir.async_create_issue(
        hass, DOMAIN, issue_id, is_fixable=True, is_persistent=True,
        severity=ir.IssueSeverity.CRITICAL, translation_key="tofu_mismatch",
        translation_placeholders={
            "pinned_fingerprint": fingerprint_of(pinned), "received_fingerprint": stale.fingerprint, "url": KERNEL_URL,
        },
        data={
            "pinned_fingerprint": fingerprint_of(pinned), "received_fingerprint": stale.fingerprint,
            "received_key": stale.verifying_key, "url": KERNEL_URL,
        },
    )
    resp = await client.post("/api/repairs/issues/fix", json={"handler": DOMAIN, "issue_id": issue_id})
    assert resp.status == HTTPStatus.OK
    aborted = await resp.json()
    assert aborted["type"] == "abort" and aborted["reason"] == "no_received_key"
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == pinned
    assert entry.runtime_data.coordinator.received_key is None

    # The kernel answers again: the backstop holds the second key and
    # rewrites the issue to the fingerprints.
    kernel.connection_down = False
    await backstop(hass, freezer)
    issue = registry.async_get_issue(DOMAIN, issue_id)
    assert issue.data == issue.translation_placeholders == {
        "pinned_fingerprint": fingerprint_of(pinned), "received_fingerprint": second.fingerprint,
    }
    resp = await client.post("/api/repairs/issues/fix", json={"handler": DOMAIN, "issue_id": issue_id})
    flow = await resp.json()
    assert flow["type"] == "form"
    assert flow["description_placeholders"]["received_fingerprint"] == second.fingerprint
    assert not flow["errors"], "the first showing is not an error"

    # A third key before the person submits: shown, not pinned.
    third = SigningKernel()
    kernel.verifying_key = third.verifying_key
    kernel.kernel = third
    await backstop(hass, freezer)
    resp = await client.post(f"/api/repairs/issues/fix/{flow['flow_id']}", json={})
    again = await resp.json()
    assert again["type"] == "form" and again["step_id"] == "confirm"
    assert again["description_placeholders"]["received_fingerprint"] == third.fingerprint
    # Eight characters are all that differ from the form already compared:
    # the form says the key changed, so Submit is not pressed out of habit.
    assert again["errors"] == {"base": "key_changed"}
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == pinned

    resp = await client.post(f"/api/repairs/issues/fix/{flow['flow_id']}", json={})
    done = await resp.json()
    assert done["type"] == "create_entry"
    await settle(hass)
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == third.verifying_key
    assert entry.data[CONF_KERNEL_PREVIOUS_KEYS] == [pinned]
    assert stale.verifying_key not in (entry.data[CONF_KERNEL_VERIFYING_KEY], *entry.data[CONF_KERNEL_PREVIOUS_KEYS])
    assert registry.async_get_issue(DOMAIN, issue_id) is None


async def test_an_issue_the_kernel_api_contradicts_clears_at_the_next_check(
    hass: HomeAssistant,
    kernel: KernelMock,
    mqtt_mock: MqttMockHAClient,
    setup_kernel_entry,
    hass_client: ClientSessionGenerator,
    freezer: FrozenDateTimeFactory,
) -> None:
    """A key-mismatch issue says the kernel serves another key. One kept
    over a reload (the reloaded coordinator raised nothing itself), and one
    only an announce raised (no coordinator holds its key), are each a dead
    end: the fix flow aborts `no_received_key`. The next check that sees the
    kernel's API serve the pinned key clears them, and the pin never moves."""
    assert await async_setup_component(hass, "repairs", {})
    entry = await setup_kernel_entry(enable_mqtt=True)
    pinned = kernel.verifying_key
    original = kernel.kernel
    issue_id = f"tofu_mismatch_{entry.entry_id}"
    registry = ir.async_get(hass)
    client = await hass_client()

    # Kept over a reload: raised, the kernel goes back to its pin while the
    # entry reloads with the kernel down, then the kernel answers again.
    second = SigningKernel()
    kernel.verifying_key = second.verifying_key
    kernel.kernel = second
    await backstop(hass, freezer)
    assert registry.async_get_issue(DOMAIN, issue_id) is not None
    kernel.verifying_key = pinned
    kernel.kernel = original
    kernel.connection_down = True
    assert await hass.config_entries.async_reload(entry.entry_id)
    await settle(hass)
    assert registry.async_get_issue(DOMAIN, issue_id) is not None
    resp = await client.post("/api/repairs/issues/fix", json={"handler": DOMAIN, "issue_id": issue_id})
    aborted = await resp.json()
    assert aborted["type"] == "abort" and aborted["reason"] == "no_received_key"
    kernel.connection_down = False
    await backstop(hass, freezer)
    assert registry.async_get_issue(DOMAIN, issue_id) is None

    # Only an announce raised it: signed by the pin, naming another key.
    announced = SigningKernel()
    result = await hass.config_entries.flow.async_init(
        DOMAIN,
        context={"source": config_entries.SOURCE_MQTT},
        data=MqttServiceInfo(
            topic=f"witness/{KERNEL_DEVICE_ID}/announce",
            payload=json.dumps(original.announce(verifying_key=announced.verifying_key)),
            qos=1, retain=True, subscribed_topic="witness/+/announce", timestamp=time.time(),
        ),
    )
    assert result["type"] is FlowResultType.ABORT and result["reason"] == "announce_key_mismatch"
    assert registry.async_get_issue(DOMAIN, issue_id) is not None
    resp = await client.post("/api/repairs/issues/fix", json={"handler": DOMAIN, "issue_id": issue_id})
    aborted = await resp.json()
    assert aborted["type"] == "abort" and aborted["reason"] == "no_received_key"
    await backstop(hass, freezer)
    assert registry.async_get_issue(DOMAIN, issue_id) is None
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == pinned
    assert announced.verifying_key not in (entry.data.get(CONF_KERNEL_PREVIOUS_KEYS) or [])


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
