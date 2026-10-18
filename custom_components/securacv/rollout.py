"""Staged firmware rollout — one Canary at a time, abort on the first failure.

The firmware's signed pull-OTA engine gives Home Assistant an ``update``
entity per Canary (MQTT discovery, ``device_class: firmware``): installed
version, latest version, an Install button that publishes ``install`` to the
node's command topic. Pressing it on twenty nodes at once is how a bad
release reaches twenty nodes at once. This module is the hub's staged
driver (docs/OTA.md §5): take the fleet's update entities, install on ONE,
wait until that node reports the new version through its own entity, then
the next — and stop the moment one fails or times out, returning what was
installed, what was skipped and where it stopped.

Pure where it can be: :func:`plan` decides from entity states alone, and
:func:`async_run` takes the clock and the sleep as arguments so the tests
drive it without waiting ten minutes. Only :func:`candidate_entity_ids`
touches the registries, to find the update entities that belong to Canaries
(the firmware's discovery device block carries ``identifiers:
["securacv_<id>"]``, which Home Assistant's MQTT integration stores as the
device identifier ``("mqtt", "securacv_<id>")``).

Trust: this installs only what the node's own engine will accept — a signed
manifest from the node's trusted URL — so the hub cannot push an image the
node would refuse on its own. It mutates firmware, not trust
(docs/device_trust.md's rule is untouched).
"""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Awaitable, Callable, Iterable
from dataclasses import dataclass, field
from typing import Any

from homeassistant.core import HomeAssistant

_LOGGER = logging.getLogger(__name__)

UPDATE_DOMAIN = "update"
INSTALL_SERVICE = "install"
STATE_ON = "on"       # an update is available
STATE_OFF = "off"     # up to date
ATTR_INSTALLED = "installed_version"
ATTR_LATEST = "latest_version"
ATTR_IN_PROGRESS = "in_progress"
CANARY_IDENTIFIER_PREFIX = "securacv_"

DEFAULT_NODE_TIMEOUT_S = 600
MIN_NODE_TIMEOUT_S = 60
MAX_NODE_TIMEOUT_S = 3600
DEFAULT_POLL_S = 5.0


@dataclass
class RolloutResult:
    """What a rollout did, in the order it did it."""

    installed: list[str] = field(default_factory=list)
    skipped: list[str] = field(default_factory=list)
    failed: list[str] = field(default_factory=list)
    not_attempted: list[str] = field(default_factory=list)
    aborted: bool = False
    reason: str | None = None

    def as_dict(self) -> dict[str, Any]:
        return {
            "installed": list(self.installed),
            "skipped": list(self.skipped),
            "failed": list(self.failed),
            "not_attempted": list(self.not_attempted),
            "aborted": self.aborted,
            "reason": self.reason,
        }


def plan(
    states: Callable[[str], Any],
    entity_ids: Iterable[str],
) -> tuple[list[str], list[str]]:
    """Split ``entity_ids`` into (to_install, skipped) from their states.

    An entity installs when its state is ``on`` (an update is available) and
    it names a latest version to wait for. Anything else — up to date,
    unavailable, no latest version — is skipped, never pushed: a node that
    says nothing is not a node to update blind.
    """
    to_install: list[str] = []
    skipped: list[str] = []
    for entity_id in entity_ids:
        state = states(entity_id)
        if state is None or state.state != STATE_ON:
            skipped.append(entity_id)
            continue
        if not state.attributes.get(ATTR_LATEST):
            skipped.append(entity_id)
            continue
        to_install.append(entity_id)
    return to_install, skipped


def node_done(state: Any, latest: str) -> bool:
    """Has this node's entity reported the new version installed?"""
    if state is None:
        return False
    if state.attributes.get(ATTR_IN_PROGRESS):
        return False
    return state.attributes.get(ATTR_INSTALLED) == latest and state.state != STATE_ON


def candidate_entity_ids(hass: HomeAssistant) -> list[str]:
    """Every ``update`` entity that belongs to a Canary, by its device."""
    try:
        from homeassistant.helpers import device_registry as dr
        from homeassistant.helpers import entity_registry as er

        entity_registry = er.async_get(hass)
        device_registry = dr.async_get(hass)
        entries = list(entity_registry.entities.values())
    except (ImportError, AttributeError):
        # A core without the registries (the unit-test stubs) has no Canary
        # update entities to find; the action refuses with the honest reason.
        return []
    found: list[str] = []
    for entry in entries:
        if entry.domain != UPDATE_DOMAIN or not entry.device_id:
            continue
        device = device_registry.async_get(entry.device_id)
        if device is None:
            continue
        if any(str(ident[1]).startswith(CANARY_IDENTIFIER_PREFIX) for ident in device.identifiers):
            found.append(entry.entity_id)
    return sorted(found)


async def async_run(
    hass: HomeAssistant,
    entity_ids: Iterable[str],
    *,
    node_timeout_s: float = DEFAULT_NODE_TIMEOUT_S,
    poll_s: float = DEFAULT_POLL_S,
    clock: Callable[[], float] | None = None,
    sleep: Callable[[float], Awaitable[None]] | None = None,
) -> RolloutResult:
    """Install on each entity in turn; abort on the first failure or timeout."""
    import time

    now = clock or time.monotonic
    wait = sleep or asyncio.sleep
    ordered = list(entity_ids)
    to_install, skipped = plan(hass.states.get, ordered)
    result = RolloutResult(skipped=skipped)
    pending = list(to_install)

    while pending:
        entity_id = pending.pop(0)
        state = hass.states.get(entity_id)
        latest = str(state.attributes.get(ATTR_LATEST)) if state is not None else ""
        _LOGGER.info("Staged rollout: installing %s on %s", latest, entity_id)
        try:
            await hass.services.async_call(
                UPDATE_DOMAIN, INSTALL_SERVICE, {"entity_id": entity_id}, blocking=False
            )
        except Exception as exc:  # noqa: BLE001 — whatever stopped the install stops the rollout
            result.failed.append(entity_id)
            result.aborted = True
            result.reason = f"{entity_id}: the install could not be sent ({exc})"
            result.not_attempted = pending
            _LOGGER.warning("Staged rollout aborted: %s", result.reason)
            return result
        started = now()
        while True:
            await wait(poll_s)
            if node_done(hass.states.get(entity_id), latest):
                result.installed.append(entity_id)
                _LOGGER.info("Staged rollout: %s is on %s", entity_id, latest)
                break
            if now() - started >= node_timeout_s:
                result.failed.append(entity_id)
                result.aborted = True
                result.reason = f"{entity_id} did not report {latest} within {int(node_timeout_s)} s"
                result.not_attempted = pending
                _LOGGER.warning("Staged rollout aborted: %s", result.reason)
                return result
    return result
