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
from homeassistant.const import CONF_URL
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import (
    DOMAIN,
    CONF_MQTT_PREFIX,
    CONF_ENABLE_MQTT,
    TOPIC_COUNTS,
    TOPIC_CHAIN,
    TOPIC_EVENTS,
    TOPIC_HEALTH,
    MANUFACTURER,
    MODEL_KERNEL,
    MODEL_CANARY,
)

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up SecuraCV sensors from a config entry."""
    entry_data = hass.data[DOMAIN][entry.entry_id]
    coordinator = entry_data["coordinator"]

    # Always add the kernel sensor (HTTP-based)
    entities: list[SensorEntity] = [
        SecuraCVKernelLastEventSensor(coordinator, entry),
    ]
    async_add_entities(entities)

    # Optionally set up MQTT-based Canary sensors
    enable_mqtt = entry.data.get(CONF_ENABLE_MQTT, False)
    mqtt_prefix = entry.data.get(CONF_MQTT_PREFIX)

    if enable_mqtt and mqtt_prefix:
        await _setup_mqtt_sensors(hass, entry, mqtt_prefix, async_add_entities)


async def _setup_mqtt_sensors(
    hass: HomeAssistant,
    entry: ConfigEntry,
    prefix: str,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up MQTT-based sensors for Canary devices."""
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
                SecuraCVCanaryWitnessCountSensor(prefix, device_id, entry)
            )

        if topic_type == TOPIC_CHAIN and "chain" not in entities_added[device_id]:
            entities_added[device_id].add("chain")
            new_entities.append(
                SecuraCVCanaryChainLengthSensor(prefix, device_id, entry)
            )

        if topic_type == TOPIC_EVENTS and "events" not in entities_added[device_id]:
            entities_added[device_id].add("events")
            new_entities.append(
                SecuraCVCanaryLastEventSensor(prefix, device_id, entry)
            )

        if topic_type == TOPIC_HEALTH and "health" not in entities_added[device_id]:
            entities_added[device_id].add("health")
            new_entities.append(
                SecuraCVCanaryHealthSensor(prefix, device_id, entry)
            )
            new_entities.append(
                SecuraCVCanaryGPSSensor(prefix, device_id, entry)
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


# =============================================================================
# Kernel Sensors (HTTP API-based)
# =============================================================================

class SecuraCVKernelLastEventSensor(CoordinatorEntity, SensorEntity):
    """Sensor for latest event from the Privacy Witness Kernel (HTTP API)."""

    _attr_name = "SecuraCV Last Event"
    _attr_icon = "mdi:shield-eye"
    _attr_has_entity_name = True

    def __init__(self, coordinator, entry: ConfigEntry) -> None:
        """Initialize the sensor."""
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{DOMAIN}_{entry.entry_id}_latest_event"

    @property
    def device_info(self) -> DeviceInfo:
        """Return device info for the kernel."""
        return DeviceInfo(
            identifiers={(DOMAIN, self._entry.data[CONF_URL])},
            manufacturer=MANUFACTURER,
            model=MODEL_KERNEL,
            name="SecuraCV Privacy Witness Kernel",
            configuration_url=self._entry.data[CONF_URL],
        )

    @property
    def native_value(self) -> str | None:
        """Return the event type."""
        if event := self.coordinator.data.get("latest_event"):
            if (event_type := event.get("event_type")) is not None:
                return str(event_type)
        return None

    @property
    def extra_state_attributes(self) -> dict[str, Any] | None:
        """Return additional event attributes."""
        if not (event := self.coordinator.data.get("latest_event")):
            return None
        keys = ("zone_id", "time_bucket", "confidence", "kernel_version", "ruleset_id")
        attrs = {key: event[key] for key in keys if key in event}
        return attrs or None


# =============================================================================
# Canary Sensors (MQTT-based)
# =============================================================================

class SecuraCVCanarySensorBase(SensorEntity):
    """Base class for SecuraCV Canary sensors (MQTT-based)."""

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
        self._attr_unique_id = f"{DOMAIN}_canary_{device_id}_{key}"
        self._attr_name = name_suffix

    @property
    def device_info(self) -> DeviceInfo:
        """Return device info."""
        return DeviceInfo(
            identifiers={(DOMAIN, f"canary_{self._device_id}")},
            manufacturer=MANUFACTURER,
            model=MODEL_CANARY,
            name=f"SecuraCV Canary {self._device_id}",
        )


class SecuraCVCanaryWitnessCountSensor(SecuraCVCanarySensorBase):
    """Sensor for total witness record count from a Canary device."""

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


class SecuraCVCanaryChainLengthSensor(SecuraCVCanarySensorBase):
    """Sensor for hash chain length from a Canary device."""

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


class SecuraCVCanaryLastEventSensor(SecuraCVCanarySensorBase):
    """Sensor for last witness event from a Canary device."""

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


class SecuraCVCanaryHealthSensor(SecuraCVCanarySensorBase):
    """Sensor for device health status from a Canary device."""

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


class SecuraCVCanaryGPSSensor(SecuraCVCanarySensorBase):
    """Sensor for GPS fix status from a Canary device."""

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
