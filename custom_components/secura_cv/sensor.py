"""Sensors for SecuraCV."""
from __future__ import annotations

from typing import Any

from homeassistant.components.sensor import SensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from . import DOMAIN, SecuraCvCoordinator


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities,
) -> None:
    """Set up SecuraCV sensors based on a config entry."""
    coordinator: SecuraCvCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([SecuraCvLatestEventSensor(coordinator, entry)])


class SecuraCvLatestEventSensor(CoordinatorEntity[SecuraCvCoordinator], SensorEntity):
    """Represents the latest SecuraCV event."""

    _attr_name = "SecuraCV Last Event"
    _attr_icon = "mdi:shield-eye"

    def __init__(self, coordinator: SecuraCvCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = f"{entry.entry_id}_latest_event"

    @property
    def native_value(self) -> str | None:
        if event := self.coordinator.data.get("latest_event"):
            if (event_type := event.get("event_type")) is not None:
                return str(event_type)
        return None

    @property
    def extra_state_attributes(self) -> dict[str, Any] | None:
        event = self.coordinator.data.get("latest_event")
        if not event:
            return None
        attrs: dict[str, Any] = {}
        for key in ("zone_id", "time_bucket", "confidence"):
            if key in event:
                attrs[key] = event[key]
        return attrs or None
