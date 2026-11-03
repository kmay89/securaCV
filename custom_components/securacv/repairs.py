"""Repairs: the conditions a person can act on, and the one fix flow.

Only actionable or fixable conditions become issues (the repair-issues
rule); a fact — the chain is broken, a Canary is silent — is an event
entity or a PROBLEM sensor, never an issue. The issue ids follow the
``<key>_<entry_id>`` pattern; ``no_kernel_found`` is global because the
config flow raises it before any entry exists.

| key                   | fixable | when                                              |
|-----------------------|---------|---------------------------------------------------|
| no_kernel_found       | no      | Automatic found neither the app nor a sidecar     |
| kernel_unreachable    | no      | 15 min of consecutive backstop failures           |
| tofu_mismatch         | yes     | /api/identity serves a key other than the pin     |
| storage_replace       | no      | /status says replace the storage                  |
| entities_migrated     | no      | once, on the first claim (the pwk_* ids moved)    |
| mqtt_subscribe_failed | no      | the Canary subscriptions could not be made        |

The fix flow for ``tofu_mismatch`` is the rotate ceremony: it shows the
pinned and the received fingerprint, asks the person to confirm the new one
against the kernel's own printout (`pair`, the app's panel, `doctor`), and
only then moves the pin, keeping the previous key. Trust decisions stay a
person's (docs/device_trust.md); no service can do this.

What an issue holds is public once a diagnostics file is posted: Home
Assistant adds every issue it holds for the domain to the download, the
ones the owner ignored and the other entries' included, outside the
integration's redaction (ledger F399), and it stores a persistent one in
``.storage/repairs.issue_registry`` as it was raised. So the
``tofu_mismatch`` issue carries the two kernel key fingerprints and
nothing else: never the kernel's URL or host, never the key it served.
That key stays on the entry's kernel coordinator (``received_key``, set
by the pin check that raised the issue and cleared with it), and the fix
flow reads it there through ``runtime_for(entry)``. Until the coordinator
has seen the kernel serve that key again since Home Assistant started or
the entry reloaded, there is nothing to confirm and the flow aborts
(``no_received_key``).
Every entry-scoped issue id still ends in the entry id:
``async_create_fix_flow`` finds the entry by that suffix, and the
download's file name and ``setup_times`` carry the same ids whatever the
issues hold.
"""
from __future__ import annotations

import logging
import sys
from typing import TYPE_CHECKING, Any

from homeassistant.components.repairs import ConfirmRepairFlow, RepairsFlow
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import issue_registry as ir

if TYPE_CHECKING:
    if sys.version_info >= (3, 14):
        # Cores that run on Python 3.14 (2026.3 and later) type a repair
        # step's result as RepairsFlowResult; older cores as FlowResult.
        # Typing only — nothing here runs.
        from homeassistant.components.repairs import RepairsFlowResult
    else:
        from homeassistant.data_entry_flow import FlowResult as RepairsFlowResult

from .const import (
    ADDON_REPOSITORY_LINK,
    CONF_KERNEL_KEY_PINNED_BUCKET_START,
    CONF_KERNEL_PREVIOUS_KEYS,
    CONF_KERNEL_VERIFYING_KEY,
    DOCKER_QUICKSTART_LINK,
    DOMAIN,
    ENTITY_MIGRATION_DOCS_LINK,
    ISSUE_ENTITIES_MIGRATED,
    ISSUE_KERNEL_UNREACHABLE,
    ISSUE_MQTT_SUBSCRIBE_FAILED,
    ISSUE_NO_KERNEL_FOUND,
    ISSUE_STORAGE_REPLACE,
    ISSUE_TOFU_MISMATCH,
    KERNEL_UNREACHABLE_DOCS_LINK,
    STORAGE_REPLACE_DOCS_LINK,
    TIME_BUCKET_SECONDS,
)
from .data import runtime_for
from .kernel_trust import FINGERPRINT_LENGTH, kernel_key_fingerprint, normalize_kernel_key

_LOGGER = logging.getLogger(__name__)

# The two fields a tofu_mismatch issue holds, as data and as translation
# placeholders: the pinned and the received key's eight-character
# fingerprints. Nothing else (ledger F399): an issue built before that held
# the kernel's URL and the served key too, and async_scrub_tofu_mismatch_issues
# rewrites such an issue to these two.
DATA_PINNED_FINGERPRINT = "pinned_fingerprint"
DATA_RECEIVED_FINGERPRINT = "received_fingerprint"
TOFU_MISMATCH_FIELDS = (DATA_PINNED_FINGERPRINT, DATA_RECEIVED_FINGERPRINT)
_HEX_DIGITS = frozenset("0123456789abcdef")


def issue_id(key: str, entry: ConfigEntry) -> str:
    """The entry-scoped issue id for a translation key."""
    return f"{key}_{entry.entry_id}"


@callback
def async_report_no_kernel_found(hass: HomeAssistant) -> None:
    """Automatic found no kernel: say where to get one (J2)."""
    ir.async_create_issue(
        hass,
        DOMAIN,
        ISSUE_NO_KERNEL_FOUND,
        is_fixable=False,
        is_persistent=True,
        severity=ir.IssueSeverity.WARNING,
        learn_more_url=DOCKER_QUICKSTART_LINK,
        translation_key=ISSUE_NO_KERNEL_FOUND,
        translation_placeholders={
            "addon_repo_url": ADDON_REPOSITORY_LINK,
            "docker_quickstart_url": DOCKER_QUICKSTART_LINK,
        },
    )


@callback
def async_clear_no_kernel_found(hass: HomeAssistant) -> None:
    """A kernel entry loaded: the pointer is no longer needed."""
    ir.async_delete_issue(hass, DOMAIN, ISSUE_NO_KERNEL_FOUND)


@callback
def async_report_kernel_unreachable(hass: HomeAssistant, entry: ConfigEntry, url: str) -> None:
    """The backstop has failed for KERNEL_UNREACHABLE_AFTER; not persistent."""
    ir.async_create_issue(
        hass,
        DOMAIN,
        issue_id(ISSUE_KERNEL_UNREACHABLE, entry),
        is_fixable=False,
        is_persistent=False,
        severity=ir.IssueSeverity.ERROR,
        learn_more_url=KERNEL_UNREACHABLE_DOCS_LINK,
        translation_key=ISSUE_KERNEL_UNREACHABLE,
        translation_placeholders={"url": url},
    )


@callback
def async_clear_kernel_unreachable(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """The backstop reached the kernel again."""
    ir.async_delete_issue(hass, DOMAIN, issue_id(ISSUE_KERNEL_UNREACHABLE, entry))


@callback
def async_report_tofu_mismatch(
    hass: HomeAssistant,
    entry: ConfigEntry,
    *,
    pinned_key: str,
    received_key: str,
) -> None:
    """The kernel serves a key other than the pin: a person must decide.

    Takes the two keys and keeps only their fingerprints, so nothing but a
    fingerprint can reach the issue (module docstring). The caller keeps
    the received key where the fix flow reads it, the coordinator.
    """
    _async_raise_tofu_mismatch(
        hass,
        issue_id(ISSUE_TOFU_MISMATCH, entry),
        {
            DATA_PINNED_FINGERPRINT: kernel_key_fingerprint(pinned_key),
            DATA_RECEIVED_FINGERPRINT: kernel_key_fingerprint(received_key),
        },
    )


@callback
def _async_raise_tofu_mismatch(
    hass: HomeAssistant, tofu_issue_id: str, fields: dict[str, str]
) -> None:
    """Create the issue, or replace an existing one's data and placeholders.

    Home Assistant keeps an existing issue's `created` and, when the owner
    ignored it, its `dismissed_version`; everything passed here replaces
    what the issue held.
    """
    ir.async_create_issue(
        hass,
        DOMAIN,
        tofu_issue_id,
        is_fixable=True,
        is_persistent=True,
        severity=ir.IssueSeverity.CRITICAL,
        translation_key=ISSUE_TOFU_MISMATCH,
        translation_placeholders=dict(fields),
        data=dict(fields),
    )


def _fingerprint_or_blank(value: Any) -> str:
    """An eight-character lowercase hex fingerprint as it is, anything else ''."""
    if (
        isinstance(value, str)
        and len(value) == FINGERPRINT_LENGTH
        and set(value) <= _HEX_DIGITS
    ):
        return value
    return ""


@callback
def async_scrub_tofu_mismatch_issues(hass: HomeAssistant) -> None:
    """Rewrite every key-mismatch issue of the domain to its two fingerprints.

    An issue raised before ledger F399 holds the kernel's URL and the whole
    served key in its data, and the URL in its placeholders. Home Assistant
    stores a persistent issue as it was raised and loads it back at every
    start, so without this an issue that is never raised again (the kernel
    went back to its pinned key, the entry is disabled or fails to set up)
    would keep both in every download of the domain for good. Run once per
    start, before any entry sets up; the key in it is dropped, never read:
    the fix flow pins only a key the coordinator has seen this run.
    """
    prefix = f"{ISSUE_TOFU_MISMATCH}_"
    for (domain, tofu_issue_id), issue in list(ir.async_get(hass).issues.items()):
        if domain != DOMAIN or not tofu_issue_id.startswith(prefix):
            continue
        placeholders = issue.translation_placeholders or {}
        data = issue.data or {}
        fields = {
            name: _fingerprint_or_blank(placeholders.get(name) or data.get(name))
            for name in TOFU_MISMATCH_FIELDS
        }
        if issue.data == fields and issue.translation_placeholders == fields:
            continue
        _async_raise_tofu_mismatch(hass, tofu_issue_id, fields)


@callback
def async_clear_tofu_mismatch(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """The served key matches the pin again (or the pin was rotated)."""
    ir.async_delete_issue(hass, DOMAIN, issue_id(ISSUE_TOFU_MISMATCH, entry))


@callback
def async_report_storage_replace(
    hass: HomeAssistant, entry: ConfigEntry, wear_pct: Any
) -> None:
    """/status recommends replacing the kernel's storage."""
    ir.async_create_issue(
        hass,
        DOMAIN,
        issue_id(ISSUE_STORAGE_REPLACE, entry),
        is_fixable=False,
        is_persistent=False,
        severity=ir.IssueSeverity.WARNING,
        learn_more_url=STORAGE_REPLACE_DOCS_LINK,
        translation_key=ISSUE_STORAGE_REPLACE,
        translation_placeholders={
            "wear_pct": "unknown" if wear_pct is None else str(wear_pct),
        },
    )


@callback
def async_clear_storage_replace(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """The storage report no longer recommends a replacement."""
    ir.async_delete_issue(hass, DOMAIN, issue_id(ISSUE_STORAGE_REPLACE, entry))


@callback
def async_report_entities_migrated(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Once, on the first claim: the app's pwk_* entities moved to these ids."""
    ir.async_create_issue(
        hass,
        DOMAIN,
        issue_id(ISSUE_ENTITIES_MIGRATED, entry),
        is_fixable=False,
        is_persistent=True,
        severity=ir.IssueSeverity.WARNING,
        learn_more_url=ENTITY_MIGRATION_DOCS_LINK,
        translation_key=ISSUE_ENTITIES_MIGRATED,
    )


@callback
def async_report_mqtt_subscribe_failed(
    hass: HomeAssistant, entry: ConfigEntry, prefix: Any, err: Exception
) -> None:
    """The Canary subscriptions could not be made: say what to fix."""
    ir.async_create_issue(
        hass,
        DOMAIN,
        issue_id(ISSUE_MQTT_SUBSCRIBE_FAILED, entry),
        is_fixable=False,
        severity=ir.IssueSeverity.WARNING,
        learn_more_url="https://www.home-assistant.io/integrations/mqtt/",
        translation_key=ISSUE_MQTT_SUBSCRIBE_FAILED,
        translation_placeholders={"prefix": str(prefix), "error": str(err)},
    )


@callback
def async_clear_mqtt_subscribe_failed(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """The subscriptions succeeded."""
    ir.async_delete_issue(hass, DOMAIN, issue_id(ISSUE_MQTT_SUBSCRIBE_FAILED, entry))


@callback
def async_clear_entry_issues(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Drop every entry-scoped issue (the entry is being removed)."""
    for key in (
        ISSUE_KERNEL_UNREACHABLE,
        ISSUE_TOFU_MISMATCH,
        ISSUE_STORAGE_REPLACE,
        ISSUE_ENTITIES_MIGRATED,
        ISSUE_MQTT_SUBSCRIBE_FAILED,
    ):
        ir.async_delete_issue(hass, DOMAIN, issue_id(key, entry))


class TofuMismatchRepairFlow(ConfirmRepairFlow):
    """The rotate ceremony for the kernel's verifying key.

    The person sees both fingerprints and confirms the received one against
    the kernel's own printout. Only then does the pin move; the previous
    key is kept so a sealed log signed under it stays attributable.

    The key it pins is the one the entry's coordinator holds when the
    person submits (``received_key``), never one read from the issue: an
    issue holds fingerprints only, and one an older integration wrote,
    which held a key, is not trusted for it. The form shows the
    fingerprint of the key it would pin; if the coordinator has seen
    another key by the time the person submits, the form is shown again
    with that key's fingerprint and a ``key_changed`` error, and nothing
    is pinned, so the pin only ever moves to a key whose fingerprint the
    person was shown.
    """

    def __init__(self, entry: ConfigEntry) -> None:
        """Hold the entry whose pin the ceremony moves."""
        super().__init__()
        self._entry = entry
        # The key whose fingerprint the form last showed.
        self._shown_key: str | None = None

    def _received_key(self) -> str | None:
        """The key the coordinator holds as served and not pinned, or None.

        None when the entry is not loaded, has no kernel coordinator, or
        the coordinator has not seen a key other than the pin since it
        started (Home Assistant restarted, the entry reloaded, or the
        kernel serves its pinned key again).
        """
        runtime = runtime_for(self._entry)
        if runtime is None or runtime.coordinator is None:
            return None
        return normalize_kernel_key(runtime.coordinator.received_key)

    async def async_step_confirm(
        self, user_input: dict[str, str] | None = None
    ) -> RepairsFlowResult:
        """Show both fingerprints; on submit rotate the pin and reload."""
        received = self._received_key()
        if received is None:
            return self.async_abort(reason="no_received_key")
        pinned = normalize_kernel_key(self._entry.data.get(CONF_KERNEL_VERIFYING_KEY))
        pinned_fingerprint = kernel_key_fingerprint(pinned) if pinned else ""
        if user_input is not None and received == self._shown_key:
            previous = list(self._entry.data.get(CONF_KERNEL_PREVIOUS_KEYS) or [])
            if pinned and pinned != received and pinned not in previous:
                previous.append(pinned)
            self.hass.config_entries.async_update_entry(
                self._entry,
                data={
                    **self._entry.data,
                    CONF_KERNEL_VERIFYING_KEY: received,
                    CONF_KERNEL_PREVIOUS_KEYS: previous,
                    CONF_KERNEL_KEY_PINNED_BUCKET_START: _bucket_start_now(),
                },
            )
            async_clear_tofu_mismatch(self.hass, self._entry)
            _LOGGER.warning(
                "Witness kernel key rotated by the owner to fingerprint %s (previous %s)",
                kernel_key_fingerprint(received),
                pinned_fingerprint or None,
            )
            self.hass.config_entries.async_schedule_reload(self._entry.entry_id)
            return self.async_create_entry(data={})
        # No fields: a confirm-only form (the frontend renders just Submit).
        # Shown first, and again when the key changed since it was shown,
        # then with an error that says so: the fingerprint is all that
        # differs, and a person who already compared the first one must
        # not submit the second out of habit.
        changed = user_input is not None and self._shown_key is not None
        errors = {"base": "key_changed"} if changed else None
        self._shown_key = received
        return self.async_show_form(
            step_id="confirm",
            errors=errors,
            description_placeholders={
                DATA_PINNED_FINGERPRINT: pinned_fingerprint,
                DATA_RECEIVED_FINGERPRINT: kernel_key_fingerprint(received),
            },
        )


def _bucket_start_now() -> int:
    import time

    return int(time.time() // TIME_BUCKET_SECONDS) * TIME_BUCKET_SECONDS


async def async_create_fix_flow(
    hass: HomeAssistant, issue_id: str, data: dict[str, Any] | None
) -> RepairsFlow:
    """Home Assistant's entry point for a fixable issue."""
    # The issue's data is not read: it holds fingerprints only, and the
    # key the flow pins comes from the coordinator.
    prefix = f"{ISSUE_TOFU_MISMATCH}_"
    if issue_id.startswith(prefix):
        entry = hass.config_entries.async_get_entry(issue_id[len(prefix):])
        if entry is not None:
            return TofuMismatchRepairFlow(entry)
    # Any other fixable id would be a bug; the plain confirm flow lets the
    # person dismiss it rather than hitting "unknown error".
    return ConfirmRepairFlow()
