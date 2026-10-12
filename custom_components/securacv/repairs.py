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
from .kernel_trust import kernel_key_fingerprint, normalize_kernel_key

_LOGGER = logging.getLogger(__name__)

# Issue data keys the tofu_mismatch fix flow reads back.
DATA_RECEIVED_KEY = "received_key"
DATA_PINNED_FINGERPRINT = "pinned_fingerprint"
DATA_RECEIVED_FINGERPRINT = "received_fingerprint"
DATA_URL = "url"


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
    pinned_fingerprint: str,
    received_fingerprint: str,
    received_key: str,
    url: str,
) -> None:
    """The kernel serves a key other than the pin: a person must decide."""
    ir.async_create_issue(
        hass,
        DOMAIN,
        issue_id(ISSUE_TOFU_MISMATCH, entry),
        is_fixable=True,
        is_persistent=True,
        severity=ir.IssueSeverity.CRITICAL,
        translation_key=ISSUE_TOFU_MISMATCH,
        translation_placeholders={
            DATA_PINNED_FINGERPRINT: pinned_fingerprint,
            DATA_RECEIVED_FINGERPRINT: received_fingerprint,
            DATA_URL: url,
        },
        data={
            DATA_RECEIVED_KEY: received_key,
            DATA_PINNED_FINGERPRINT: pinned_fingerprint,
            DATA_RECEIVED_FINGERPRINT: received_fingerprint,
            DATA_URL: url,
        },
    )


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
    """

    def __init__(self, entry: ConfigEntry, data: dict[str, Any]) -> None:
        """Hold the entry and the issue data the coordinator recorded."""
        super().__init__()
        self._entry = entry
        self._data = data

    async def async_step_confirm(
        self, user_input: dict[str, str] | None = None
    ) -> RepairsFlowResult:
        """Show both fingerprints; on submit rotate the pin and reload."""
        received = normalize_kernel_key(self._data.get(DATA_RECEIVED_KEY))
        if received is None:
            # The issue carried no usable key (hand-edited storage, an older
            # issue shape): nothing to rotate to.
            return self.async_abort(reason="no_received_key")
        if user_input is not None:
            previous = list(self._entry.data.get(CONF_KERNEL_PREVIOUS_KEYS) or [])
            pinned = self._entry.data.get(CONF_KERNEL_VERIFYING_KEY)
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
                self._data.get(DATA_PINNED_FINGERPRINT),
            )
            self.hass.config_entries.async_schedule_reload(self._entry.entry_id)
            return self.async_create_entry(data={})
        # No fields: a confirm-only form (the frontend renders just Submit).
        return self.async_show_form(
            step_id="confirm",
            description_placeholders={
                DATA_PINNED_FINGERPRINT: str(self._data.get(DATA_PINNED_FINGERPRINT, "")),
                DATA_RECEIVED_FINGERPRINT: str(self._data.get(DATA_RECEIVED_FINGERPRINT, "")),
                DATA_URL: str(self._data.get(DATA_URL, "")),
            },
        )


def _bucket_start_now() -> int:
    import time

    return int(time.time() // TIME_BUCKET_SECONDS) * TIME_BUCKET_SECONDS


async def async_create_fix_flow(
    hass: HomeAssistant, issue_id: str, data: dict[str, Any] | None
) -> RepairsFlow:
    """Home Assistant's entry point for a fixable issue."""
    prefix = f"{ISSUE_TOFU_MISMATCH}_"
    if issue_id.startswith(prefix):
        entry = hass.config_entries.async_get_entry(issue_id[len(prefix):])
        if entry is not None:
            return TofuMismatchRepairFlow(entry, dict(data or {}))
    # Any other fixable id would be a bug; the plain confirm flow lets the
    # person dismiss it rather than hitting "unknown error".
    return ConfirmRepairFlow()
