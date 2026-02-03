"""Sensor platform for SecuraCV integration."""
from __future__ import annotations

import json
import logging
from typing import Any

from homeassistant.components import mqtt
from homeassistant.components.sensor import (
    SensorEntity,
    SensorStateClass,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import (
    DOMAIN,
    CONF_MQTT_PREFIX,
    TOPIC_COUNTS,
    TOPIC_CHAIN,
    TOPIC_EVENTS,
    TOPIC_HEALTH,
    MANUFACTURER,
    MODEL_CANARY,
)

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up SecuraCV sensors from a config entry."""
    prefix = entry.data.get(CONF_MQTT_PREFIX, "securacv")
    entities_added: dict[str, set[str]] = {}

    @callback
    def _async_discover_sensors(msg: mqtt.ReceiveMessage) -> None:
        """Discover sensors from incoming MQTT messages."""
        parts = msg.topic.split("/")
        if len(parts) < 3:
            return

        device_id = parts[-2]
        topic_type = parts[-1]

        if device_id not in entities_added:
            entities_added[device_id] = set()

        new_entities: list[SensorEntity] = []

        if topic_type == TOPIC_COUNTS and "counts" not in entities_added[device_id]:
            entities_added[device_id].add("counts")
            new_entities.append(
                SecuraCVWitnessCountSensor(prefix, device_id, entry)
            )

        if topic_type == TOPIC_CHAIN and "chain" not in entities_added[device_id]:
            entities_added[device_id].add("chain")
            new_entities.append(
                SecuraCVChainLengthSensor(prefix, device_id, entry)
            )

        if topic_type == TOPIC_EVENTS and "events" not in entities_added[device_id]:
            entities_added[device_id].add("events")
            new_entities.append(
                SecuraCVLastEventSensor(prefix, device_id, entry)
            )

        if topic_type == TOPIC_HEALTH and "health" not in entities_added[device_id]:
            entities_added[device_id].add("health")
            new_entities.append(
                SecuraCVHealthSensor(prefix, device_id, entry)
            )
            new_entities.append(
                SecuraCVGPSSensor(prefix, device_id, entry)
            )

        if new_entities:
            async_add_entities(new_entities)

    # Subscribe to all device topics for sensor discovery
    for topic_suffix in [TOPIC_COUNTS, TOPIC_CHAIN, TOPIC_EVENTS, TOPIC_HEALTH]:
        await mqtt.async_subscribe(
            hass,
            f"{prefix}/+/{topic_suffix}",
            _async_discover_sensors,
        )


class SecuraCVSensorBase(SensorEntity):
    """Base class for SecuraCV sensors."""

    _attr_has_entity_name = True

    def __init__(
        self,
        prefix: str,
        device_id: str,
        entry: ConfigEntry,
        name_suffix: str,
        key: str,
    ) -> None:
        """Initialize the sensor."""
        self._prefix = prefix
        self._device_id = device_id
        self._attr_unique_id = f"{DOMAIN}_{device_id}_{key}"
        self._attr_name = name_suffix

    @property
    def device_info(self) -> DeviceInfo:
        """Return device info."""
        return DeviceInfo(
            identifiers={(DOMAIN, self._device_id)},
            manufacturer=MANUFACTURER,
            model=MODEL_CANARY,
            name=f"SecuraCV Canary {self._device_id}",
        )


class SecuraCVWitnessCountSensor(SecuraCVSensorBase):
    """Sensor for total witness record count."""

    _attr_icon = "mdi:counter"
    _attr_state_class = SensorStateClass.TOTAL_INCREASING
    _attr_native_unit_of_measurement = "records"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Witness Count", "witness_count")

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT when added."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_COUNTS}",
            self._handle_message,
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle count message."""
        try:
            data = json.loads(msg.payload)
            self._attr_native_value = data.get("total", data.get("count", 0))
        except (json.JSONDecodeError, TypeError):
            try:
                self._attr_native_value = int(msg.payload)
            except (ValueError, TypeError):
                return
        self.async_write_ha_state()


class SecuraCVChainLengthSensor(SecuraCVSensorBase):
    """Sensor for hash chain length."""

    _attr_icon = "mdi:link-variant"
    _attr_state_class = SensorStateClass.TOTAL_INCREASING
    _attr_native_unit_of_measurement = "blocks"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Chain Length", "chain_length")

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT when added."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_CHAIN}",
            self._handle_message,
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle chain message."""
        try:
            data = json.loads(msg.payload)
            self._attr_native_value = data.get("length", data.get("chain_length", 0))
            self._attr_extra_state_attributes = {
                "latest_hash": data.get("latest_hash", ""),
                "algorithm": data.get("algorithm", "ed25519"),
            }
        except (json.JSONDecodeError, TypeError):
            return
        self.async_write_ha_state()


class SecuraCVLastEventSensor(SecuraCVSensorBase):
    """Sensor for last witness event."""

    _attr_icon = "mdi:eye-outline"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Last Event", "last_event")

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT when added."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_EVENTS}",
            self._handle_message,
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle event message."""
        try:
            data = json.loads(msg.payload)
            self._attr_native_value = data.get("event_type", data.get("type", "unknown"))
            self._attr_extra_state_attributes = {
                "timestamp": data.get("timestamp", ""),
                "zone": data.get("zone", ""),
                "confidence": data.get("confidence", ""),
                "signed": data.get("signed", False),
            }
        except (json.JSONDecodeError, TypeError):
            self._attr_native_value = str(msg.payload)[:255]
        self.async_write_ha_state()


class SecuraCVHealthSensor(SecuraCVSensorBase):
    """Sensor for device health status."""

    _attr_icon = "mdi:heart-pulse"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Health", "health_status")

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT when added."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
            self._handle_message,
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle health message."""
        try:
            data = json.loads(msg.payload)
            # Overall status from key health indicators
            battery = data.get("battery", 100)
            memory_free = data.get("memory_free", 0)

            if battery < 10 or memory_free < 1024:
                self._attr_native_value = "critical"
            elif battery < 25:
                self._attr_native_value = "warning"
            else:
                self._attr_native_value = "healthy"

            self._attr_extra_state_attributes = {
                "battery_percent": battery,
                "memory_free_bytes": memory_free,
                "uptime_seconds": data.get("uptime", 0),
                "firmware_version": data.get("firmware_version", ""),
                "public_key": data.get("public_key", ""),
            }
        except (json.JSONDecodeError, TypeError):
            self._attr_native_value = "unknown"
        self.async_write_ha_state()


class SecuraCVGPSSensor(SecuraCVSensorBase):
    """Sensor for GPS fix status."""

    _attr_icon = "mdi:crosshairs-gps"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "GPS Fix", "gps_fix")

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT when added."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
            self._handle_message,
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle health message for GPS data."""
        try:
            data = json.loads(msg.payload)
            gps = data.get("gps", {})

            if isinstance(gps, dict):
                self._attr_native_value = gps.get("fix_type", "no_fix")
                self._attr_extra_state_attributes = {
                    "satellites": gps.get("satellites", 0),
                    "hdop": gps.get("hdop", 0),
                    "latitude": gps.get("latitude", ""),
                    "longitude": gps.get("longitude", ""),
                }
            else:
                self._attr_native_value = str(gps) if gps else "no_fix"
        except (json.JSONDecodeError, TypeError):
            return
        self.async_write_ha_state()
