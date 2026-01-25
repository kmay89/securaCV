"""Sensors for SecuraCV."""
from __future__ import annotations

from collections.abc import Callable
from typing import Any

from homeassistant.components.sensor import SensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from . import DOMAIN, SecuraCvCoordinator


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: "Callable[[list[SensorEntity]], None]",
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
        event = self.coordinator.data.get("latest_event")
        if event and (event_type := event.get("event_type")) is not None:
            return str(event_type)
        return None

    @property
    def extra_state_attributes(self) -> dict[str, Any] | None:
        if not (event := self.coordinator.data.get("latest_event")):
            return None
        keys = ("zone_id", "time_bucket", "confidence")
        attrs = {key: event[key] for key in keys if key in event}
        return attrs or None
