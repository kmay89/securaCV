"""Repairs (contract C9): the issue ids, placeholders, and the one fix flow.

Only actionable conditions are issues; `tofu_mismatch` is the only fixable
one and its flow is the rotate ceremony — show both fingerprints, let the
person confirm the received one against the kernel's own printout, then
move the pin and keep the previous key.
"""

from __future__ import annotations

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import issues, make_entry, make_runtime, run

from homeassistant.core import HomeAssistant  # noqa: E402
from homeassistant.helpers import issue_registry as ir  # noqa: E402

from .. import repairs  # noqa: E402
from ..coordinator import SecuraCVKernelCoordinator  # noqa: E402
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


# ─── tofu_mismatch: what the issue holds (ledger F399) ─────────────────

KERNEL_HOST = "kernel.lan"
KERNEL_URL = f"http://{KERNEL_HOST}:8799"
THIRD_KEY = "ef" * 32


class _Kernel:
    """A kernel API the backstop reads; `key` is what /api/identity serves."""

    base_url = KERNEL_URL
    token_file_unreadable = False

    def __init__(self, key: str) -> None:
        self.key = key

    async def async_get_health(self):
        return {"ok": True}

    async def async_get_identity(self):
        return {"device_id": "pwk_1a2b3c4d", "verifying_key": self.key}

    async def async_get_status(self):
        return None

    async def async_get_latest_event(self):
        return None

    async def async_get_digest(self):
        return None


def _kernel_entry(hass, entry_id="e1", *, serves=OLD_KEY):
    """A loaded kernel entry pinned to OLD_KEY whose kernel serves `serves`."""
    entry = make_entry(hass, entry_id=entry_id, data={CONF_KERNEL_VERIFYING_KEY: OLD_KEY, "url": KERNEL_URL})
    kernel = _Kernel(serves)
    coordinator = SecuraCVKernelCoordinator(
        hass, entry, kernel, auth_mode="token_file", topic_prefix="witness", pinned_key=OLD_KEY
    )
    entry.runtime_data.coordinator = coordinator
    entry.runtime_data.api = kernel
    return entry, kernel, coordinator


def _restart(hass, entry, kernel):
    """What a restart or a reload leaves: the entry and its issues, a new coordinator."""
    coordinator = SecuraCVKernelCoordinator(
        hass, entry, kernel, auth_mode="token_file", topic_prefix="witness",
        pinned_key=entry.data[CONF_KERNEL_VERIFYING_KEY],
    )
    entry.runtime_data = make_runtime(hass, entry.entry_id, coordinator=coordinator, api=kernel)
    return coordinator


def _forbidden(*keys: str) -> list[str]:
    """Every spelling of the kernel's address and of each key a leak could take."""
    import base64

    spelled = [KERNEL_HOST, KERNEL_URL, "8799"]
    for key in keys:
        raw = bytes.fromhex(key)
        spelled += [key, key.upper(), base64.b64encode(raw).decode(), base64.urlsafe_b64encode(raw).decode()]
    return spelled


def _assert_only_fingerprints(issue, *keys: str) -> None:
    fingerprints = {
        "pinned_fingerprint": kernel_key_fingerprint(OLD_KEY),
        "received_fingerprint": kernel_key_fingerprint(keys[-1]),
    }
    assert issue["data"] == fingerprints
    assert issue["translation_placeholders"] == fingerprints
    text = repr(issue)
    for spelled in _forbidden(OLD_KEY, *keys):
        assert spelled not in text, spelled


def test_the_mismatch_issue_holds_the_two_fingerprints_and_nothing_else():
    """Raised by the backstop's pin check: the kernel's URL, its host and the
    key it served stay out of the issue (Home Assistant puts every issue of
    the domain in the diagnostics download); the coordinator holds the key."""
    hass = _hass()
    entry, kernel, coordinator = _kernel_entry(hass, serves=NEW_KEY)
    run(coordinator.async_refresh())
    issue = issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")]
    assert issue["is_fixable"] is True and issue["severity"] == "critical"
    assert issue["is_persistent"] is True
    _assert_only_fingerprints(issue, NEW_KEY)
    assert coordinator.received_key == NEW_KEY

    # A second key while the issue is open replaces both fingerprints, and
    # still nothing else.
    kernel.key = THIRD_KEY
    run(coordinator.async_refresh())
    _assert_only_fingerprints(issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")], NEW_KEY, THIRD_KEY)
    assert coordinator.received_key == THIRD_KEY


def _fix_flow(hass, entry, data):
    flow = run(repairs.async_create_fix_flow(hass, f"{ISSUE_TOFU_MISMATCH}_{entry.entry_id}", data))
    flow.hass = hass
    flow.handler = DOMAIN
    flow.issue_id = f"{ISSUE_TOFU_MISMATCH}_{entry.entry_id}"
    return flow


def test_the_fix_flow_shows_both_fingerprints_then_rotates_keeping_the_previous_key():
    hass = _hass()
    entry, _kernel, coordinator = _kernel_entry(hass, serves=NEW_KEY)
    run(coordinator.async_refresh())
    issue_data = issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")]["data"]
    flow = _fix_flow(hass, entry, issue_data)
    assert isinstance(flow, repairs.TofuMismatchRepairFlow)

    result = run(flow.async_step_init())
    assert result["type"] == "form" and result["step_id"] == "confirm"
    assert result["description_placeholders"] == {
        "pinned_fingerprint": kernel_key_fingerprint(OLD_KEY),
        "received_fingerprint": kernel_key_fingerprint(NEW_KEY),
    }
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == OLD_KEY, "showing the form changes nothing"

    result = run(flow.async_step_confirm({}))
    assert result["type"] == "create_entry"
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == NEW_KEY
    assert entry.data[CONF_KERNEL_PREVIOUS_KEYS] == [OLD_KEY]
    assert entry.data[CONF_KERNEL_KEY_PINNED_BUCKET_START] % 600 == 0
    assert (DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1") not in issues(hass)
    assert hass.config_entries.reloads == ["e1"]

    # A second rotation (the reloaded entry's coordinator saw a third key)
    # keeps the whole history, without duplicates.
    coordinator = _restart(hass, entry, _Kernel(THIRD_KEY))
    run(coordinator.async_refresh())
    flow = _fix_flow(hass, entry, issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")]["data"])
    run(flow.async_step_init())
    run(flow.async_step_confirm({}))
    assert entry.data[CONF_KERNEL_PREVIOUS_KEYS] == [OLD_KEY, NEW_KEY]
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == THIRD_KEY


def _old_format_issue(hass, entry, key=NEW_KEY):
    """The issue as the integration raised it before F399: the URL in its
    placeholders, the URL and the whole served key in its data."""
    fields = {
        "pinned_fingerprint": kernel_key_fingerprint(OLD_KEY),
        "received_fingerprint": kernel_key_fingerprint(key),
    }
    ir.async_create_issue(
        hass, DOMAIN, f"{ISSUE_TOFU_MISMATCH}_{entry.entry_id}",
        is_fixable=True, is_persistent=True, severity=ir.IssueSeverity.CRITICAL,
        translation_key=ISSUE_TOFU_MISMATCH,
        translation_placeholders={**fields, "url": KERNEL_URL},
        data={**fields, "received_key": key, "url": KERNEL_URL},
    )
    return issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_{entry.entry_id}")]["data"]


def test_after_a_restart_the_flow_has_nothing_to_pin_until_the_kernel_serves_the_key_again():
    """Home Assistant keeps the persistent issue; the coordinator a restart
    makes holds no key until its backstop sees one. Opening the issue then
    aborts (no raise on None), and a key an older issue held is never pinned."""
    hass = _hass()
    entry, kernel, _coordinator = _kernel_entry(hass, serves=NEW_KEY)
    issue_data = _old_format_issue(hass, entry)
    coordinator = _restart(hass, entry, kernel)

    flow = _fix_flow(hass, entry, issue_data)
    result = run(flow.async_step_init())
    assert result["type"] == "abort" and result["reason"] == "no_received_key"
    result = run(flow.async_step_confirm({}))
    assert result["type"] == "abort" and result["reason"] == "no_received_key"
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == OLD_KEY
    assert hass.config_entries.reloads == []
    assert coordinator.received_key is None

    # The first backstop sees the kernel serve a key, which need not be the
    # one the old issue held: the flow pins the coordinator's, and the
    # issue is rewritten to the fingerprints.
    kernel.key = THIRD_KEY
    run(coordinator.async_refresh())
    _assert_only_fingerprints(issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")], NEW_KEY, THIRD_KEY)
    flow = _fix_flow(hass, entry, issue_data)
    result = run(flow.async_step_init())
    assert result["description_placeholders"]["received_fingerprint"] == kernel_key_fingerprint(THIRD_KEY)
    run(flow.async_step_confirm({}))
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == THIRD_KEY
    assert entry.data[CONF_KERNEL_PREVIOUS_KEYS] == [OLD_KEY]


def test_the_flow_pins_only_a_key_whose_fingerprint_it_showed():
    """The coordinator sees a new key, the form shows it, then the kernel
    serves a third key before the person submits: the submit pins nothing
    and the form is shown again with the third key's fingerprint; the next
    submit pins exactly the key the coordinator holds then."""
    hass = _hass()
    entry, kernel, coordinator = _kernel_entry(hass, serves=NEW_KEY)
    run(coordinator.async_refresh())
    flow = _fix_flow(hass, entry, issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")]["data"])
    result = run(flow.async_step_init())
    assert result["description_placeholders"]["received_fingerprint"] == kernel_key_fingerprint(NEW_KEY)

    assert result["errors"] == {}, "the first showing is not an error"

    kernel.key = THIRD_KEY
    run(coordinator.async_refresh())
    result = run(flow.async_step_confirm({}))
    assert result["type"] == "form" and result["step_id"] == "confirm"
    assert result["description_placeholders"]["received_fingerprint"] == kernel_key_fingerprint(THIRD_KEY)
    # Only eight characters differ from the form the person already
    # compared, so the form says the key changed: a habit Submit must not
    # pin a key nobody compared.
    assert result["errors"] == {"base": "key_changed"}
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == OLD_KEY, "a key whose fingerprint was not shown is never pinned"

    result = run(flow.async_step_confirm({}))
    assert result["type"] == "create_entry"
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == THIRD_KEY
    assert entry.data[CONF_KERNEL_PREVIOUS_KEYS] == [OLD_KEY]


def test_the_flow_aborts_when_the_kernel_serves_its_pinned_key_again():
    hass = _hass()
    entry, kernel, coordinator = _kernel_entry(hass, serves=NEW_KEY)
    run(coordinator.async_refresh())
    flow = _fix_flow(hass, entry, issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")]["data"])
    run(flow.async_step_init())
    kernel.key = OLD_KEY
    run(coordinator.async_refresh())
    assert (DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1") not in issues(hass)
    result = run(flow.async_step_confirm({}))
    assert result["type"] == "abort" and result["reason"] == "no_received_key"
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == OLD_KEY


def test_an_issue_kept_over_a_restart_clears_when_the_kernel_serves_its_pin():
    """The issue outlives a restart or a reload; the new coordinator starts
    with no mismatch of its own. When its backstop sees the kernel serve the
    pinned key, the issue is cleared all the same: it says the kernel serves
    another key, which the authenticated API has just contradicted, and its
    fix flow could only abort (no key is held)."""
    hass = _hass()
    entry, kernel, coordinator = _kernel_entry(hass, serves=NEW_KEY)
    run(coordinator.async_refresh())
    assert (DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1") in issues(hass)

    kernel.key = OLD_KEY
    coordinator = _restart(hass, entry, kernel)
    assert coordinator.key_mismatch is False
    run(coordinator.async_refresh())
    assert (DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1") not in issues(hass)
    assert coordinator.received_key is None
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == OLD_KEY


def test_an_issue_only_an_announce_raised_clears_when_the_kernel_serves_its_pin():
    """An announce naming another key raises the issue without handing the
    key to the coordinator, so the fix flow has nothing to pin. The next
    backstop that sees the kernel serve its pinned key clears it; one that
    sees another key raises it again with that key held."""
    hass = _hass()
    entry, kernel, coordinator = _kernel_entry(hass, serves=OLD_KEY)
    repairs.async_report_tofu_mismatch(hass, entry, pinned_key=OLD_KEY, received_key=NEW_KEY)
    assert coordinator.key_mismatch is False and coordinator.received_key is None

    run(coordinator.async_refresh())
    assert (DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1") not in issues(hass)

    kernel.key = NEW_KEY
    run(coordinator.async_refresh())
    _assert_only_fingerprints(issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")], NEW_KEY)
    assert coordinator.received_key == NEW_KEY


def test_the_fix_flow_refuses_an_entry_with_no_kernel_coordinator():
    """Not loaded (setup failed, or the entry unloaded), or loaded with no
    kernel: nothing holds a key, whatever the issue's data says."""
    hass = _hass()
    unloaded = make_entry(entry_id="e1", data={CONF_KERNEL_VERIFYING_KEY: OLD_KEY})
    hass.config_entries.add(unloaded)
    no_kernel = make_entry(hass, entry_id="e2", data={CONF_KERNEL_VERIFYING_KEY: OLD_KEY})
    for entry in (unloaded, no_kernel):
        flow = _fix_flow(hass, entry, {"received_key": NEW_KEY, "url": KERNEL_URL})
        result = run(flow.async_step_confirm({}))
        assert result["type"] == "abort" and result["reason"] == "no_received_key"
        assert entry.data[CONF_KERNEL_VERIFYING_KEY] == OLD_KEY


def test_a_start_rewrites_an_older_issue_to_its_two_fingerprints():
    """The integration's setup, before any entry: an issue an older version
    raised (the URL and the served key in it) is rewritten to the two
    fingerprints, whichever entry raised it and whether or not that entry
    ever sets up again. Other issues are left alone."""
    from .. import async_setup

    hass = _hass()
    entry, _kernel, _coordinator = _kernel_entry(hass, serves=NEW_KEY)
    other, _other_kernel, _other_coordinator = _kernel_entry(hass, entry_id="e2", serves=THIRD_KEY)
    _old_format_issue(hass, entry)
    _old_format_issue(hass, other, THIRD_KEY)
    repairs.async_report_kernel_unreachable(hass, entry, KERNEL_URL)
    ir.async_create_issue(
        hass, "other_domain", f"{ISSUE_TOFU_MISMATCH}_x", is_fixable=False,
        severity=ir.IssueSeverity.WARNING, translation_key="t", data={"url": KERNEL_URL},
    )

    assert run(async_setup(hass, {})) is True
    _assert_only_fingerprints(issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")], NEW_KEY)
    _assert_only_fingerprints(issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e2")], THIRD_KEY)
    assert issues(hass)[(DOMAIN, f"{ISSUE_KERNEL_UNREACHABLE}_e1")]["translation_placeholders"] == {"url": KERNEL_URL}
    assert issues(hass)[("other_domain", f"{ISSUE_TOFU_MISMATCH}_x")]["data"] == {"url": KERNEL_URL}

    # A rewritten issue is left as it is on the next start.
    before = dict(issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")])
    repairs.async_scrub_tofu_mismatch_issues(hass)
    assert issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")] == before


def test_a_rewrite_keeps_no_value_that_is_not_a_fingerprint():
    """Whatever an older or hand-edited issue holds under the fingerprint
    names, only an eight-character lowercase hex value is carried over."""
    hass = _hass()
    ir.async_create_issue(
        hass, DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1", is_fixable=True, is_persistent=True,
        severity=ir.IssueSeverity.CRITICAL, translation_key=ISSUE_TOFU_MISMATCH,
        translation_placeholders={"pinned_fingerprint": KERNEL_URL, "received_fingerprint": NEW_KEY},
        data={"received_key": NEW_KEY},
    )
    repairs.async_scrub_tofu_mismatch_issues(hass)
    issue = issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_e1")]
    assert issue["data"] == issue["translation_placeholders"] == {
        "pinned_fingerprint": "", "received_fingerprint": "",
    }


def test_an_unknown_issue_gets_the_plain_confirm_flow():
    hass = _hass()
    flow = run(repairs.async_create_fix_flow(hass, "something_else_e1", None))
    assert not isinstance(flow, repairs.TofuMismatchRepairFlow)
    # And a tofu id for an entry that no longer exists is not a crash either.
    flow = run(repairs.async_create_fix_flow(hass, f"{ISSUE_TOFU_MISMATCH}_gone", {}))
    assert not isinstance(flow, repairs.TofuMismatchRepairFlow)
