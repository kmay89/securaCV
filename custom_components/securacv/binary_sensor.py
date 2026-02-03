"""Binary sensor platform for SecuraCV integration."""
from __future__ import annotations

import json
import logging

from homeassistant.components import mqtt
from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import (
    DOMAIN,
    CONF_MQTT_PREFIX,
    TOPIC_STATUS,
    TOPIC_CHAIN,
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
    """Set up SecuraCV binary sensors from a config entry."""
    prefix = entry.data.get(CONF_MQTT_PREFIX, "securacv")
    entities_added: dict[str, set[str]] = {}

    @callback
    def _async_discover_binary_sensors(msg: mqtt.ReceiveMessage) -> None:
        """Discover binary sensors from incoming MQTT messages."""
        parts = msg.topic.split("/")
        if len(parts) < 3:
            return

        device_id = parts[-2]
        topic_type = parts[-1]

        if device_id not in entities_added:
            entities_added[device_id] = set()

        new_entities: list[BinarySensorEntity] = []

        if topic_type == TOPIC_STATUS and "online" not in entities_added[device_id]:
            entities_added[device_id].add("online")
            new_entities.append(
                SecuraCVOnlineSensor(prefix, device_id, entry)
            )

        if topic_type == TOPIC_CHAIN and "chain_valid" not in entities_added[device_id]:
            entities_added[device_id].add("chain_valid")
            new_entities.append(
                SecuraCVChainValidSensor(prefix, device_id, entry)
            )

        if topic_type == TOPIC_HEALTH and "tamper" not in entities_added[device_id]:
            entities_added[device_id].add("tamper")
            new_entities.append(
                SecuraCVTamperSensor(prefix, device_id, entry)
            )

        if new_entities:
            async_add_entities(new_entities)

    # Subscribe for discovery
    for topic_suffix in [TOPIC_STATUS, TOPIC_CHAIN, TOPIC_HEALTH]:
        await mqtt.async_subscribe(
            hass,
            f"{prefix}/+/{topic_suffix}",
            _async_discover_binary_sensors,
        )


class SecuraCVBinarySensorBase(BinarySensorEntity):
    """Base class for SecuraCV binary sensors."""

    _attr_has_entity_name = True

    def __init__(
        self,
        prefix: str,
        device_id: str,
        entry: ConfigEntry,
        name_suffix: str,
        key: str,
    ) -> None:
        """Initialize."""
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


class SecuraCVOnlineSensor(SecuraCVBinarySensorBase):
    """Binary sensor for device online/offline status."""

    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY
    _attr_icon = "mdi:access-point-network"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Online", "online")
        self._attr_is_on = False

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT status topic."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_STATUS}",
            self._handle_message,
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle status message."""
        payload = str(msg.payload).lower().strip()
        self._attr_is_on = payload in ("online", "1", "true", "connected")
        self.async_write_ha_state()


class SecuraCVChainValidSensor(SecuraCVBinarySensorBase):
    """Binary sensor for hash chain integrity."""

    _attr_icon = "mdi:shield-check"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Chain Valid", "chain_valid")
        self._attr_is_on = True  # Assume valid until proven otherwise

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT chain topic."""
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
            # Chain is valid if explicitly stated, or if no error is present
            valid = data.get("valid", data.get("integrity", True))
            self._attr_is_on = bool(valid)
            self._attr_extra_state_attributes = {
                "chain_length": data.get("length", 0),
                "latest_hash": data.get("latest_hash", ""),
                "verification_error": data.get("error", None),
            }
        except (json.JSONDecodeError, TypeError):
            return
        self.async_write_ha_state()


class SecuraCVTamperSensor(SecuraCVBinarySensorBase):
    """Binary sensor for tamper detection."""

    _attr_device_class = BinarySensorDeviceClass.TAMPER
    _attr_icon = "mdi:shield-alert"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Tamper", "tamper")
        self._attr_is_on = False  # No tamper detected by default

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT health topic."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
            self._handle_message,
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle health message for tamper detection."""
        try:
            data = json.loads(msg.payload)
            tamper = data.get("tamper_detected", data.get("tamper", False))
            self._attr_is_on = bool(tamper)
        except (json.JSONDecodeError, TypeError):
            return
        self.async_write_ha_state()
