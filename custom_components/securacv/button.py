"""Button platform: Verify now and Export evidence, on the kernel device.

Both buttons run the same coroutines the ``securacv.verify`` and
``securacv.export_evidence`` actions run (they register nothing themselves),
so a press and an automation do exactly one thing. Both are available only
while the kernel is reachable — a press that could not reach it would be a
button that lies.

``PARALLEL_UPDATES = 1``: a press is an action against one kernel; two
presses at once are served one after the other.

Export evidence is admin-only, like the ``securacv.export_evidence`` action
(services.py, "Who may call what"): an export deletes this entry's oldest
packs past its keep limit. A press arrives through ``button.press``, an
entity action any signed-in user may call, so the button checks the press's
context itself, with the rule Home Assistant's admin-service helper applies
to the action (services.py registers the action through it; this file
registers nothing). Verify now only reads and stays open.
"""
from __future__ import annotations

import logging
from typing import TYPE_CHECKING

from homeassistant.components.button import ButtonEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import Unauthorized, UnknownUser
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import EXPORT_WINDOW_DEFAULT_HOURS, VERIFY_TRIGGER_BUTTON
from .coordinator import SecuraCVKernelCoordinator
from .data import runtime_for
from .entity import SecuraCVKernelEntity
from .services import async_export_for_entry, async_verify_for_entry

if TYPE_CHECKING:
    from homeassistant.core import Context

_LOGGER = logging.getLogger(__name__)

PARALLEL_UPDATES = 1


async def async_refuse_non_admin(hass: HomeAssistant, context: Context | None) -> None:
    """Raise unless the press came from an administrator or from no user.

    The rule Home Assistant's admin-service helper applies to a call
    (helpers/service.py, ``_async_admin_handler``): a context naming a user
    who no longer exists is ``UnknownUser``, a user who is not an
    administrator is ``Unauthorized``, and a context with no user (an
    automation's) passes.
    """
    user_id = context.user_id if context is not None else None
    if not user_id:
        return
    user = await hass.auth.async_get_user(user_id)
    if user is None:
        raise UnknownUser(context=context)
    if not user.is_admin:
        raise Unauthorized(context=context)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Two buttons per kernel; none without one."""
    runtime = runtime_for(entry)
    if runtime is None or runtime.coordinator is None:
        return
    async_add_entities(
        [
            SecuraCVVerifyNowButton(runtime.coordinator, entry),
            SecuraCVExportEvidenceButton(runtime.coordinator, entry),
        ]
    )


class SecuraCVVerifyNowButton(SecuraCVKernelEntity, ButtonEntity):
    """Walk the sealed log now, against the pinned key."""

    _attr_translation_key = "verify_now"

    def __init__(self, coordinator: SecuraCVKernelCoordinator, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(coordinator, entry, "verify_now")

    async def async_press(self) -> None:
        """The same path as the `securacv.verify` action."""
        response = await async_verify_for_entry(self.hass, self._entry, trigger=VERIFY_TRIGGER_BUTTON)
        _LOGGER.info("Verify now: %s", response.get("verdict"))


class SecuraCVExportEvidenceButton(SecuraCVKernelEntity, ButtonEntity):
    """Export the last 24 hours as a signed evidence pack."""

    _attr_translation_key = "export_evidence"

    def __init__(self, coordinator: SecuraCVKernelCoordinator, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(coordinator, entry, "export_evidence")

    async def async_press(self) -> None:
        """The same path as the `securacv.export_evidence` action, defaults,
        refused before the kernel is asked when the presser is not an admin."""
        await async_refuse_non_admin(self.hass, self._context)
        response = await async_export_for_entry(
            self.hass, self._entry, window_hours=EXPORT_WINDOW_DEFAULT_HOURS
        )
        _LOGGER.info("Export evidence: %s (%s events)", response.get("file"), response.get("events"))
