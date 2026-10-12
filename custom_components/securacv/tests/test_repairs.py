"""Repairs (contract C9): the issue ids, placeholders, and the one fix flow.

Only actionable conditions are issues; `tofu_mismatch` is the only fixable
one and its flow is the rotate ceremony — show both fingerprints, let the
person confirm the received one against the kernel's own printout, then
move the pin and keep the previous key.
"""

from __future__ import annotations

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import issues, make_entry, run

from homeassistant.core import HomeAssistant  # noqa: E402

from .. import repairs  # noqa: E402
from ..const import (  # noqa: E402
    ADDON_REPOSITORY_LINK,
    CONF_KERNEL_KEY_PINNED_BUCKET_START,
    CONF_KERNEL_PREVIOUS_KEYS,
    CONF_KERNEL_VERIFYING_KEY,
    DOCKER_QUICKSTART_LINK,
    DOMAIN,
    ISSUE_ENTITIES_MIGRATED,
    ISSUE_KERNEL_UNREACHABLE,
    ISSUE_MQTT_SUBSCRIBE_FAILED,
    ISSUE_NO_KERNEL_FOUND,
    ISSUE_STORAGE_REPLACE,
    ISSUE_TOFU_MISMATCH,
)
from ..kernel_trust import kernel_key_fingerprint  # noqa: E402

OLD_KEY = "ab" * 32
NEW_KEY = "cd" * 32


def _hass() -> HomeAssistant:
    hass = HomeAssistant()
    hass.data = {}
    return hass


def test_no_kernel_found_is_global_dismissable_and_points_at_both_links():
    hass = _hass()
    repairs.async_report_no_kernel_found(hass)
    issue = issues(hass)[(DOMAIN, ISSUE_NO_KERNEL_FOUND)]
    assert issue["is_fixable"] is False and issue["is_persistent"] is True
    assert issue["severity"] == "warning"
    assert issue["translation_key"] == ISSUE_NO_KERNEL_FOUND
    assert issue["translation_placeholders"] == {
        "addon_repo_url": ADDON_REPOSITORY_LINK,
        "docker_quickstart_url": DOCKER_QUICKSTART_LINK,
    }
    repairs.async_clear_no_kernel_found(hass)
    assert issues(hass) == {}


def test_entry_scoped_issues_carry_the_entry_id():
    hass = _hass()
    entry = make_entry(entry_id="e1", data={})
    repairs.async_report_kernel_unreachable(hass, entry, "http://kernel:8799")
    repairs.async_report_storage_replace(hass, entry, 91)
    repairs.async_report_entities_migrated(hass, entry)
    repairs.async_report_mqtt_subscribe_failed(hass, entry, "securacv", RuntimeError("no broker"))
    assert set(issues(hass)) == {
        (DOMAIN, f"{ISSUE_KERNEL_UNREACHABLE}_e1"),
        (DOMAIN, f"{ISSUE_STORAGE_REPLACE}_e1"),
        (DOMAIN, f"{ISSUE_ENTITIES_MIGRATED}_e1"),
        (DOMAIN, f"{ISSUE_MQTT_SUBSCRIBE_FAILED}_e1"),
    }
    unreachable = issues(hass)[(DOMAIN, f"{ISSUE_KERNEL_UNREACHABLE}_e1")]
    assert unreachable["severity"] == "error" and unreachable["is_persistent"] is False
    assert unreachable["translation_placeholders"] == {"url": "http://kernel:8799"}
    storage = issues(hass)[(DOMAIN, f"{ISSUE_STORAGE_REPLACE}_e1")]
    assert storage["translation_placeholders"] == {"wear_pct": "91"}
    migrated = issues(hass)[(DOMAIN, f"{ISSUE_ENTITIES_MIGRATED}_e1")]
    assert migrated["is_persistent"] is True and migrated["learn_more_url"]
    mqtt = issues(hass)[(DOMAIN, f"{ISSUE_MQTT_SUBSCRIBE_FAILED}_e1")]
    assert mqtt["translation_placeholders"] == {"prefix": "securacv", "error": "no broker"}
    for issue in issues(hass).values():
        assert issue["is_fixable"] is False

    repairs.async_clear_kernel_unreachable(hass, entry)
    repairs.async_clear_storage_replace(hass, entry)
    repairs.async_clear_mqtt_subscribe_failed(hass, entry)
    assert set(issues(hass)) == {(DOMAIN, f"{ISSUE_ENTITIES_MIGRATED}_e1")}
    repairs.async_clear_entry_issues(hass, entry)
    assert issues(hass) == {}


def test_tofu_mismatch_is_fixable_critical_and_carries_the_received_key():
    hass = _hass()
    entry = make_entry(entry_id="e1", data={CONF_KERNEL_VERIFYING_KEY: OLD_KEY})
    repairs.async_report_tofu_mismatch(
        hass, entry,
        pinned_fingerprint=kernel_key_fingerprint(OLD_KEY),
        received_fingerprint=kernel_key_fingerprint(NEW_KEY),
        received_key=NEW_KEY,
        url="http://kernel:8799",
    )
    issue = issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")]
    assert issue["is_fixable"] is True and issue["severity"] == "critical"
    assert issue["translation_placeholders"] == {
        "pinned_fingerprint": kernel_key_fingerprint(OLD_KEY),
        "received_fingerprint": kernel_key_fingerprint(NEW_KEY),
        "url": "http://kernel:8799",
    }
    assert issue["data"]["received_key"] == NEW_KEY


def _fix_flow(hass, entry, data):
    flow = run(repairs.async_create_fix_flow(hass, f"{ISSUE_TOFU_MISMATCH}_{entry.entry_id}", data))
    flow.hass = hass
    flow.handler = DOMAIN
    flow.issue_id = f"{ISSUE_TOFU_MISMATCH}_{entry.entry_id}"
    return flow


def test_the_fix_flow_shows_both_fingerprints_then_rotates_keeping_the_previous_key():
    hass = _hass()
    entry = make_entry(hass, entry_id="e1", data={CONF_KERNEL_VERIFYING_KEY: OLD_KEY})
    repairs.async_report_tofu_mismatch(
        hass, entry,
        pinned_fingerprint=kernel_key_fingerprint(OLD_KEY),
        received_fingerprint=kernel_key_fingerprint(NEW_KEY),
        received_key=NEW_KEY,
        url="http://kernel:8799",
    )
    issue_data = issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")]["data"]
    flow = _fix_flow(hass, entry, issue_data)
    assert isinstance(flow, repairs.TofuMismatchRepairFlow)

    result = run(flow.async_step_init())
    assert result["type"] == "form" and result["step_id"] == "confirm"
    assert result["description_placeholders"] == {
        "pinned_fingerprint": kernel_key_fingerprint(OLD_KEY),
        "received_fingerprint": kernel_key_fingerprint(NEW_KEY),
        "url": "http://kernel:8799",
    }
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == OLD_KEY, "showing the form changes nothing"

    result = run(flow.async_step_confirm({}))
    assert result["type"] == "create_entry"
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == NEW_KEY
    assert entry.data[CONF_KERNEL_PREVIOUS_KEYS] == [OLD_KEY]
    assert entry.data[CONF_KERNEL_KEY_PINNED_BUCKET_START] % 600 == 0
    assert (DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1") not in issues(hass)
    assert hass.config_entries.reloads == ["e1"]

    # A second rotation keeps the whole history, without duplicates.
    third = "ef" * 32
    flow = _fix_flow(hass, entry, {**issue_data, "received_key": third})
    run(flow.async_step_confirm({}))
    assert entry.data[CONF_KERNEL_PREVIOUS_KEYS] == [OLD_KEY, NEW_KEY]
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == third


def test_the_fix_flow_refuses_an_issue_without_a_key():
    hass = _hass()
    entry = make_entry(hass, entry_id="e1", data={CONF_KERNEL_VERIFYING_KEY: OLD_KEY})
    flow = _fix_flow(hass, entry, {"pinned_fingerprint": "x"})
    result = run(flow.async_step_confirm({}))
    assert result["type"] == "abort" and result["reason"] == "no_received_key"
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == OLD_KEY


def test_an_unknown_issue_gets_the_plain_confirm_flow():
    hass = _hass()
    flow = run(repairs.async_create_fix_flow(hass, "something_else_e1", None))
    assert not isinstance(flow, repairs.TofuMismatchRepairFlow)
    # And a tofu id for an entry that no longer exists is not a crash either.
    flow = run(repairs.async_create_fix_flow(hass, f"{ISSUE_TOFU_MISMATCH}_gone", {}))
    assert not isinstance(flow, repairs.TofuMismatchRepairFlow)
