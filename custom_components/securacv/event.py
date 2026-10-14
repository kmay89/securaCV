"""Event platform: witness events per zone, tamper per Canary, verification.

Three kinds of event entity (contract C10 as amended):

  - ``witness`` — one per zone the kernel has sealed an event for, on the
    kernel device, named after the zone ("Front door" →
    ``event.securacv_front_door``). Its ``event_types`` are the dictionary's
    event-type ids, DERIVED from ``const.WITNESS_EVENT_TYPES`` — this module
    spells no event type itself, and ``scripts/lint_dictionary_sync.py``
    fails the build if it ever does. Fires on the bridge's live
    ``<prefix>/events`` feed, through the coordinator, which hands over
    only non-retained, not-yet-seen events: a retained replay on restart
    feeds the last-event sensor and never re-fires a notification.
    Entities are created on the first event for a zone and restored from
    the entity registry on reload, so a zone's entity id is stable.
  - ``tamper_events`` — one per Canary device, ``event_types`` =
    ``ALL_TAMPER_TYPES``; fires on the device's non-retained
    ``securacv/<id>/tamper`` publishes.
  - ``verification_events`` — one per kernel: the integration's own chain
    walks (``chain_verified`` / ``chain_verification_failed``) and a key
    change (``witness_key_changed``).

No device class on any of them (DOORBELL without `ring` is a deprecation,
MOTION would misdescribe a tamper), and ``PARALLEL_UPDATES = 0``: nothing
here polls.
"""
from __future__ import annotations

import logging
import re
from typing import Any

from homeassistant.components import mqtt
from homeassistant.components.event import EventEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import (
    ALL_TAMPER_TYPES,
    CONF_ENABLE_MQTT,
    CONF_MQTT_PREFIX,
    DOMAIN,
    TOPIC_STATUS,
    TOPIC_TAMPER,
    VERIFICATION_EVENT_TYPES,
    WITNESS_EVENT_TYPES,
    normalize_event_type,
)
from .coordinator import (
    SecuraCVKernelCoordinator,
    verification_event_signal,
    witness_event_signal,
)
from .data import runtime_for
from .entity import SecuraCVKernelEntity, canary_device_info, canary_status_signal
from .kernel_verify import kernel_device_id_of, witness_event_attributes
from .mqtt_payload import parse_mqtt_json, valid_device_id

_LOGGER = logging.getLogger(__name__)

# Push-fed throughout (the parallel-updates rule).
PARALLEL_UPDATES = 0

WITNESS_UNIQUE_ID_INFIX = "_witness_"
TAMPER_EVENTS_KEY = "tamper_events"
VERIFICATION_EVENTS_KEY = "verification_events"


def zone_slug(zone_id: str) -> str:
    """The zone's slug: `zone:` dropped, lowercased, `[^a-z0-9]` → `_`.

    The Python counterpart of the bridge's `sanitize_for_id`, so the
    entity's unique id suffix agrees with the ids the bridge used for its
    own per-zone discovery entities.
    """
    zone = zone_id[len("zone:"):] if zone_id.startswith("zone:") else zone_id
    return re.sub(r"[^a-z0-9]", "_", zone.lower()) or "zone"


def zone_label(zone_id: str) -> str:
    """How the zone reads as an entity name: `front_door` → `Front door`."""
    zone = zone_id[len("zone:"):] if zone_id.startswith("zone:") else zone_id
    words = re.sub(r"[_\-]+", " ", zone).strip()
    return (words[:1].upper() + words[1:]) if words else "Zone"


def _label_from_slug(slug: str) -> str:
    return zone_label(slug)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up the event entities: the verification entity now, the witness
    entities as zones appear (and from the registry), tamper per Canary."""
    runtime = runtime_for(entry)
    if runtime is None:
        return
    coordinator = runtime.coordinator
    if coordinator is not None:
        async_add_entities([SecuraCVVerificationEventEntity(coordinator, entry)])
        manager = WitnessEventManager(hass, entry, coordinator, async_add_entities)
        manager.async_restore_from_registry()
        runtime.unsub_mqtt.append(
            async_dispatcher_connect(
                hass, witness_event_signal(entry.entry_id), manager.async_handle_event
            )
        )

    if entry.data.get(CONF_ENABLE_MQTT) and entry.data.get(CONF_MQTT_PREFIX):
        await _async_setup_tamper_events(
            hass, entry, str(entry.data[CONF_MQTT_PREFIX]), async_add_entities
        )


# =============================================================================
# Witness events — one entity per zone
# =============================================================================


class WitnessEventManager:
    """Creates one witness event entity per zone and routes events to them.

    The first event for a new zone creates the entity and is fired by it as
    soon as it is added (the entity carries it in as `initial_event`), so
    the first sealed event on a new camera is never lost between discovery
    and registration.
    """

    def __init__(
        self,
        hass: HomeAssistant,
        entry: ConfigEntry,
        coordinator: SecuraCVKernelCoordinator,
        async_add_entities: AddEntitiesCallback,
    ) -> None:
        """Bind to the entry; `entities` is keyed by zone slug."""
        self.hass = hass
        self.entry = entry
        self.coordinator = coordinator
        self._async_add_entities = async_add_entities
        self.entities: dict[str, SecuraCVWitnessEventEntity] = {}

    @callback
    def async_restore_from_registry(self) -> None:
        """Recreate the witness entities a previous load registered.

        An event entity that only exists after its first event would vanish
        on every reload until the next detection, taking its history and
        every automation target with it. The registry remembers the unique
        id (and the name it was given), so the entity comes back at once.
        """
        try:
            registry = er.async_get(self.hass)
            entries = er.async_entries_for_config_entry(registry, self.entry.entry_id)
        except Exception:  # noqa: BLE001 - a registry that cannot answer only delays restore
            _LOGGER.debug("entity registry unavailable; witness entities restore on first event")
            return
        prefix = f"{DOMAIN}_{self.entry.entry_id}{WITNESS_UNIQUE_ID_INFIX}"
        restored: list[SecuraCVWitnessEventEntity] = []
        for registered in entries:
            if getattr(registered, "domain", None) != "event":
                continue
            unique_id = getattr(registered, "unique_id", None)
            if not isinstance(unique_id, str) or not unique_id.startswith(prefix):
                continue
            slug = unique_id[len(prefix):]
            if not slug or slug in self.entities:
                continue
            label = getattr(registered, "original_name", None)
            entity = SecuraCVWitnessEventEntity(
                self.coordinator,
                self.entry,
                slug,
                label if isinstance(label, str) and label else _label_from_slug(slug),
            )
            self.entities[slug] = entity
            restored.append(entity)
        if restored:
            self._async_add_entities(restored)

    @callback
    def async_handle_event(self, event: dict[str, Any]) -> None:
        """Route one new event to its zone's entity, creating it if needed."""
        if not isinstance(event, dict):
            return
        zone_id = event.get("zone_id")
        if not isinstance(zone_id, str) or not zone_id:
            return
        if normalize_event_type(event.get("event_type")) not in WITNESS_EVENT_TYPES:
            _LOGGER.debug("ignoring witness event with unknown type %r", event.get("event_type"))
            return
        slug = zone_slug(zone_id)
        entity = self.entities.get(slug)
        if entity is None:
            entity = SecuraCVWitnessEventEntity(
                self.coordinator, self.entry, slug, zone_label(zone_id), initial_event=event
            )
            self.entities[slug] = entity
            self._async_add_entities([entity])
            return
        entity.async_fire(event)


class SecuraCVWitnessEventEntity(SecuraCVKernelEntity, EventEntity):
    """The sealed witness events of one zone (camera)."""

    _attr_translation_key = "witness"
    # Derived, never retyped: the dictionary's event-type ids via const.py.
    _attr_event_types = list(WITNESS_EVENT_TYPES)

    def __init__(
        self,
        coordinator: SecuraCVKernelCoordinator,
        entry: ConfigEntry,
        slug: str,
        label: str,
        *,
        initial_event: dict[str, Any] | None = None,
    ) -> None:
        """`slug` keys the unique id; `label` is the zone as it reads."""
        super().__init__(coordinator, entry, f"witness_{slug}")
        self.slug = slug
        self._attr_translation_placeholders = {"zone": label}
        self._initial_event = initial_event

    async def async_added_to_hass(self) -> None:
        """Fire the event that created this entity, if any."""
        await super().async_added_to_hass()
        if self._initial_event is not None:
            event, self._initial_event = self._initial_event, None
            self.async_fire(event)

    @callback
    def async_fire(self, event: dict[str, Any]) -> None:
        """Trigger the entity with the C5 attribute table."""
        event_type = normalize_event_type(event.get("event_type"))
        if event_type not in self.event_types:
            return
        runtime = runtime_for(self._entry)
        self._trigger_event(
            event_type,
            witness_event_attributes(
                event,
                runtime.verification if runtime is not None else None,
                kernel_device_id=kernel_device_id_of(self._entry),
                kernel_reported_chain_ok=self.coordinator.kernel_reported_chain_ok,
                kernel_verified_bucket_start=self.coordinator.kernel_reported_verified_bucket_start,
            ),
        )
        self.async_write_ha_state()


# =============================================================================
# Verification events — the integration's own walks and a key change
# =============================================================================


class SecuraCVVerificationEventEntity(SecuraCVKernelEntity, EventEntity):
    """Every verdict the integration's chain walk reaches, as it reaches it."""

    _attr_translation_key = VERIFICATION_EVENTS_KEY
    _attr_event_types = list(VERIFICATION_EVENT_TYPES)

    def __init__(self, coordinator: SecuraCVKernelCoordinator, entry: ConfigEntry) -> None:
        """One per kernel."""
        super().__init__(coordinator, entry, VERIFICATION_EVENTS_KEY)

    async def async_added_to_hass(self) -> None:
        """Listen for the verifier's verdicts; released with the entity."""
        await super().async_added_to_hass()
        self.async_on_remove(
            async_dispatcher_connect(
                self.hass, verification_event_signal(self._entry.entry_id), self._async_verdict
            )
        )

    @callback
    def _async_verdict(self, event_type: str, attributes: dict[str, Any]) -> None:
        if event_type not in self.event_types:
            return
        self._trigger_event(event_type, dict(attributes))
        self.async_write_ha_state()


# =============================================================================
# Tamper events — one entity per Canary
# =============================================================================


async def _async_setup_tamper_events(
    hass: HomeAssistant,
    entry: ConfigEntry,
    prefix: str,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Create a tamper event entity for each Canary as it is first heard."""
    known: set[str] = set()

    @callback
    def _async_discover(msg: mqtt.ReceiveMessage) -> None:
        parts = msg.topic.split("/")
        if len(parts) < 3:
            return
        device_id = parts[-2]
        if not valid_device_id(device_id) or device_id in known:
            return
        known.add(device_id)
        initial = None
        if parts[-1] == TOPIC_TAMPER and not getattr(msg, "retain", False):
            initial = parse_mqtt_json(msg.payload)
        async_add_entities(
            [SecuraCVCanaryTamperEventEntity(prefix, device_id, entry, initial_payload=initial)]
        )

    runtime = runtime_for(entry)
    if runtime is None:
        return
    for suffix in (TOPIC_STATUS, TOPIC_TAMPER):
        runtime.unsub_mqtt.append(
            await mqtt.async_subscribe(hass, f"{prefix}/+/{suffix}", _async_discover)
        )


def tamper_kind(payload: dict[str, Any] | None) -> str | None:
    """The advertised tamper type a tamper publish names, or None."""
    if not isinstance(payload, dict):
        return None
    for key in ("type", "kind", "tamper_type"):
        kind = payload.get(key)
        if isinstance(kind, str):
            kind = kind.strip().lower()
            if kind in ALL_TAMPER_TYPES:
                return kind
    return None


class SecuraCVCanaryTamperEventEntity(EventEntity):
    """A Canary's tamper publishes, as events (`kind`, `device_id`)."""

    _attr_has_entity_name = True
    _attr_translation_key = TAMPER_EVENTS_KEY
    # Derived from the advertised list; a FUTURE_* type never appears.
    _attr_event_types = list(ALL_TAMPER_TYPES)

    def __init__(
        self,
        prefix: str,
        device_id: str,
        entry: ConfigEntry,
        *,
        initial_payload: dict[str, Any] | None = None,
    ) -> None:
        """On the Canary device; unique id `securacv_canary_<id>_tamper_events`."""
        self._prefix = prefix
        self._device_id = device_id
        self._entry = entry
        self._attr_unique_id = f"{DOMAIN}_canary_{device_id}_{TAMPER_EVENTS_KEY}"
        self._initial_payload = initial_payload
        self._last_key: tuple[Any, ...] | None = None

    @property
    def device_info(self) -> DeviceInfo:
        """The Canary device, linked to the kernel when the entry has one."""
        return canary_device_info(self.hass, self._entry, self._device_id)

    @property
    def available(self) -> bool:
        """Follows the device's status / availability topic."""
        runtime = runtime_for(self._entry)
        if runtime is None:
            return False
        device = runtime.devices.get(self._device_id)
        online = device.get("online") if isinstance(device, dict) else None
        return online is not False

    async def async_added_to_hass(self) -> None:
        """Subscribe to the device's tamper topic; release on removal."""
        await super().async_added_to_hass()
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass, f"{self._prefix}/{self._device_id}/{TOPIC_TAMPER}", self._handle_tamper
            )
        )
        self.async_on_remove(
            async_dispatcher_connect(
                self.hass,
                canary_status_signal(self._entry.entry_id, self._device_id),
                self.async_write_ha_state,
            )
        )
        if self._initial_payload is not None:
            payload, self._initial_payload = self._initial_payload, None
            self._fire(payload)

    @callback
    def _handle_tamper(self, msg: mqtt.ReceiveMessage) -> None:
        if getattr(msg, "retain", False):
            return
        payload = parse_mqtt_json(msg.payload)
        if payload is None:
            return
        self._fire(payload)

    @callback
    def _fire(self, payload: dict[str, Any]) -> None:
        kind = tamper_kind(payload)
        if kind is None:
            _LOGGER.debug("tamper publish from %s names no advertised type", self._device_id)
            return
        key = (kind, payload.get("timestamp"), payload.get("detail"))
        if key == self._last_key:
            return
        self._last_key = key
        self._trigger_event(kind, {"kind": kind, "device_id": self._device_id})
        self.async_write_ha_state()
