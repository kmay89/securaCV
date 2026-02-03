"""Binary sensor platform for SecuraCV integration.

Surfaces tamper events and transport health for multi-path resilience.
Canary devices use ANY available transport to communicate - these sensors
show which paths are alive and what threats have been detected.
"""
from __future__ import annotations

import json
import logging
from datetime import datetime, timezone

from homeassistant.components import mqtt
from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
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
    TOPIC_STATUS,
    TOPIC_CHAIN,
    TOPIC_HEALTH,
    TOPIC_TAMPER,
    TOPIC_TRANSPORT,
    TOPIC_MESH,
    TOPIC_CHIRP,
    MANUFACTURER,
    MODEL_KERNEL,
    MODEL_CANARY,
    # Tamper types
    TAMPER_POWER_LOSS,
    TAMPER_SD_REMOVE,
    TAMPER_SD_ERROR,
    TAMPER_GPS_JAMMING,
    TAMPER_MOTION,
    TAMPER_ENCLOSURE,
    TAMPER_GPIO,
    TAMPER_WATCHDOG,
    TAMPER_REBOOT,
    TAMPER_MEMORY,
    # Transport types
    TRANSPORT_WIFI_AP,
    TRANSPORT_WIFI_STA,
    TRANSPORT_MQTT,
    TRANSPORT_BLE,
    TRANSPORT_MESH,
    TRANSPORT_CHIRP,
)

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up SecuraCV binary sensors from a config entry."""
    entry_data = hass.data[DOMAIN][entry.entry_id]
    coordinator = entry_data["coordinator"]

    # Add kernel connectivity sensor (HTTP-based)
    entities: list[BinarySensorEntity] = [
        SecuraCVKernelOnlineSensor(coordinator, entry),
    ]
    async_add_entities(entities)

    # Optionally set up MQTT-based Canary binary sensors
    enable_mqtt = entry.data.get(CONF_ENABLE_MQTT, False)
    mqtt_prefix = entry.data.get(CONF_MQTT_PREFIX)

    if enable_mqtt and mqtt_prefix:
        await _setup_mqtt_binary_sensors(hass, entry, mqtt_prefix, async_add_entities)


async def _setup_mqtt_binary_sensors(
    hass: HomeAssistant,
    entry: ConfigEntry,
    prefix: str,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up MQTT-based binary sensors for Canary devices."""
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

        # Online/connectivity sensor
        if topic_type == TOPIC_STATUS and "online" not in entities_added[device_id]:
            entities_added[device_id].add("online")
            new_entities.append(
                SecuraCVCanaryOnlineSensor(prefix, device_id, entry)
            )

        # Chain integrity sensor
        if topic_type == TOPIC_CHAIN and "chain_valid" not in entities_added[device_id]:
            entities_added[device_id].add("chain_valid")
            new_entities.append(
                SecuraCVCanaryChainValidSensor(prefix, device_id, entry)
            )

        # General tamper sensor (aggregates all tamper types)
        if topic_type == TOPIC_HEALTH and "tamper" not in entities_added[device_id]:
            entities_added[device_id].add("tamper")
            new_entities.append(
                SecuraCVCanaryTamperSensor(prefix, device_id, entry)
            )

        # Individual tamper type sensors (created on first tamper message)
        if topic_type == TOPIC_TAMPER and "tamper_sensors" not in entities_added[device_id]:
            entities_added[device_id].add("tamper_sensors")
            new_entities.extend([
                SecuraCVCanaryTamperTypeSensor(prefix, device_id, entry, TAMPER_POWER_LOSS, "Power Loss", "mdi:power-plug-off"),
                SecuraCVCanaryTamperTypeSensor(prefix, device_id, entry, TAMPER_SD_REMOVE, "SD Removed", "mdi:sd-off"),
                SecuraCVCanaryTamperTypeSensor(prefix, device_id, entry, TAMPER_SD_ERROR, "SD Error", "mdi:alert-circle"),
                SecuraCVCanaryTamperTypeSensor(prefix, device_id, entry, TAMPER_GPS_JAMMING, "GPS Jamming", "mdi:crosshairs-off"),
                SecuraCVCanaryTamperTypeSensor(prefix, device_id, entry, TAMPER_MOTION, "Unexpected Motion", "mdi:motion-sensor"),
                SecuraCVCanaryTamperTypeSensor(prefix, device_id, entry, TAMPER_ENCLOSURE, "Enclosure Open", "mdi:package-variant-closed-remove"),
                SecuraCVCanaryTamperTypeSensor(prefix, device_id, entry, TAMPER_GPIO, "GPIO Tamper", "mdi:alert-circle"),
                SecuraCVCanaryTamperTypeSensor(prefix, device_id, entry, TAMPER_WATCHDOG, "Watchdog Timeout", "mdi:timer-alert"),
                SecuraCVCanaryTamperTypeSensor(prefix, device_id, entry, TAMPER_REBOOT, "Unexpected Reboot", "mdi:restart-alert"),
                SecuraCVCanaryTamperTypeSensor(prefix, device_id, entry, TAMPER_MEMORY, "Memory Critical", "mdi:memory"),
            ])

        # Transport health sensors (created on first transport message)
        if topic_type == TOPIC_TRANSPORT and "transport_sensors" not in entities_added[device_id]:
            entities_added[device_id].add("transport_sensors")
            new_entities.extend([
                SecuraCVCanaryTransportSensor(prefix, device_id, entry, TRANSPORT_WIFI_AP, "WiFi AP", "mdi:access-point"),
                SecuraCVCanaryTransportSensor(prefix, device_id, entry, TRANSPORT_WIFI_STA, "WiFi Station", "mdi:wifi"),
                SecuraCVCanaryTransportSensor(prefix, device_id, entry, TRANSPORT_MQTT, "MQTT", "mdi:message-arrow-right"),
                SecuraCVCanaryTransportSensor(prefix, device_id, entry, TRANSPORT_BLE, "Bluetooth", "mdi:bluetooth"),
                SecuraCVCanaryTransportSensor(prefix, device_id, entry, TRANSPORT_MESH, "Mesh Network", "mdi:lan"),
                SecuraCVCanaryTransportSensor(prefix, device_id, entry, TRANSPORT_CHIRP, "Chirp Network", "mdi:bird"),
            ])

        # Mesh network connected sensor
        if topic_type == TOPIC_MESH and "mesh_connected" not in entities_added[device_id]:
            entities_added[device_id].add("mesh_connected")
            new_entities.append(
                SecuraCVCanaryMeshConnectedSensor(prefix, device_id, entry)
            )

        # Chirp network active sensor
        if topic_type == TOPIC_CHIRP and "chirp_active" not in entities_added[device_id]:
            entities_added[device_id].add("chirp_active")
            new_entities.append(
                SecuraCVCanaryChirpActiveSensor(prefix, device_id, entry)
            )

        if new_entities:
            async_add_entities(new_entities)

    # Subscribe for discovery on all relevant topics
    for topic_suffix in [TOPIC_STATUS, TOPIC_CHAIN, TOPIC_HEALTH, TOPIC_TAMPER, TOPIC_TRANSPORT, TOPIC_MESH, TOPIC_CHIRP]:
        await mqtt.async_subscribe(
            hass,
            f"{prefix}/+/{topic_suffix}",
            _async_discover_binary_sensors,
        )


# =============================================================================
# Kernel Binary Sensors (HTTP API-based)
# =============================================================================

class SecuraCVKernelOnlineSensor(CoordinatorEntity, BinarySensorEntity):
    """Binary sensor for kernel connectivity status."""

    _attr_name = "SecuraCV Kernel Online"
    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY
    _attr_icon = "mdi:server-network"
    _attr_has_entity_name = True

    def __init__(self, coordinator, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{DOMAIN}_{entry.entry_id}_kernel_online"

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
    def is_on(self) -> bool:
        """Return True if the kernel is reachable."""
        return self.coordinator.last_update_success


# =============================================================================
# Canary Binary Sensors (MQTT-based)
# =============================================================================

class SecuraCVCanaryBinarySensorBase(BinarySensorEntity):
    """Base class for SecuraCV Canary binary sensors."""

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


class SecuraCVCanaryOnlineSensor(SecuraCVCanaryBinarySensorBase):
    """Binary sensor for Canary device online/offline status."""

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


class SecuraCVCanaryChainValidSensor(SecuraCVCanaryBinarySensorBase):
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


class SecuraCVCanaryTamperSensor(SecuraCVCanaryBinarySensorBase):
    """Binary sensor for general tamper detection (any tamper type)."""

    _attr_device_class = BinarySensorDeviceClass.TAMPER
    _attr_icon = "mdi:shield-alert"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Tamper", "tamper")
        self._attr_is_on = False

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT health and tamper topics."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
            self._handle_message,
        )
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_TAMPER}",
            self._handle_tamper_message,
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

    @callback
    def _handle_tamper_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle dedicated tamper message."""
        try:
            data = json.loads(msg.payload)
            # Any tamper event triggers this sensor
            self._attr_is_on = True
            self._attr_extra_state_attributes = {
                "tamper_type": data.get("type", "unknown"),
                "timestamp": data.get("timestamp", ""),
                "detail": data.get("detail", ""),
            }
        except (json.JSONDecodeError, TypeError):
            self._attr_is_on = True  # Raw tamper message = tamper detected
        self.async_write_ha_state()


class SecuraCVCanaryTamperTypeSensor(SecuraCVCanaryBinarySensorBase):
    """Binary sensor for specific tamper type detection.

    Each tamper type gets its own sensor so automations can trigger on
    specific threats (e.g., GPS jamming vs power loss vs enclosure open).
    """

    _attr_device_class = BinarySensorDeviceClass.TAMPER

    def __init__(
        self,
        prefix: str,
        device_id: str,
        entry: ConfigEntry,
        tamper_type: str,
        display_name: str,
        icon: str,
    ) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, display_name, f"tamper_{tamper_type}")
        self._tamper_type = tamper_type
        self._attr_icon = icon
        self._attr_is_on = False
        self._last_triggered: str | None = None

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT tamper and health topics."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_TAMPER}",
            self._handle_tamper_message,
        )
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
            self._handle_health_message,
        )

    @callback
    def _handle_tamper_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle dedicated tamper topic message."""
        try:
            data = json.loads(msg.payload)
            # Check if this tamper type is active
            if data.get("type") == self._tamper_type or data.get(self._tamper_type):
                self._attr_is_on = True
                self._last_triggered = datetime.now(timezone.utc).isoformat()
                self._attr_extra_state_attributes = {
                    "last_triggered": self._last_triggered,
                    "detail": data.get("detail", ""),
                    "severity": data.get("severity", "tamper"),
                }
                self.async_write_ha_state()
        except (json.JSONDecodeError, TypeError):
            pass

    @callback
    def _handle_health_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle health message for tamper indicators."""
        try:
            data = json.loads(msg.payload)
            tamper_data = data.get("tamper", {})

            # Check various tamper fields
            is_triggered = False

            if self._tamper_type == TAMPER_SD_REMOVE:
                is_triggered = not data.get("sd_mounted", True)
            elif self._tamper_type == TAMPER_SD_ERROR:
                is_triggered = data.get("sd_errors", 0) > 0
            elif self._tamper_type == TAMPER_GPS_JAMMING:
                is_triggered = data.get("gps_fix_lost", False) or data.get("gps_jamming", False)
            elif self._tamper_type == TAMPER_MOTION:
                is_triggered = tamper_data.get("motion", False) or data.get("unexpected_motion", False)
            elif self._tamper_type == TAMPER_MEMORY:
                free_heap = data.get("free_heap", 100000)
                is_triggered = free_heap < 10000  # Critical if < 10KB
            elif self._tamper_type == TAMPER_WATCHDOG:
                is_triggered = data.get("watchdog_triggered", False)
            elif self._tamper_type == TAMPER_GPIO:
                is_triggered = tamper_data.get("gpio", False) or data.get("tamper_gpio", False)
            elif self._tamper_type == TAMPER_REBOOT:
                is_triggered = data.get("unexpected_reboot", False)
            elif self._tamper_type == TAMPER_POWER_LOSS:
                is_triggered = data.get("power_loss_detected", False)
            elif self._tamper_type == TAMPER_ENCLOSURE:
                is_triggered = tamper_data.get("enclosure", False) or data.get("enclosure_open", False)
            elif self._tamper_type in tamper_data:
                is_triggered = bool(tamper_data.get(self._tamper_type))

            if is_triggered and not self._attr_is_on:
                self._last_triggered = datetime.now(timezone.utc).isoformat()

            self._attr_is_on = is_triggered
            self._attr_extra_state_attributes = {
                "last_triggered": self._last_triggered,
            }
            self.async_write_ha_state()
        except (json.JSONDecodeError, TypeError):
            pass


class SecuraCVCanaryTransportSensor(SecuraCVCanaryBinarySensorBase):
    """Binary sensor for transport channel health.

    Shows which communication paths are alive. Canary uses ANY available
    transport to get witness data out - this shows path resilience.
    """

    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY

    def __init__(
        self,
        prefix: str,
        device_id: str,
        entry: ConfigEntry,
        transport_type: str,
        display_name: str,
        icon: str,
    ) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, display_name, f"transport_{transport_type}")
        self._transport_type = transport_type
        self._attr_icon = icon
        self._attr_is_on = False

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT transport topic."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_TRANSPORT}",
            self._handle_message,
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle transport status message."""
        try:
            data = json.loads(msg.payload)
            transport_data = data.get(self._transport_type, {})

            if isinstance(transport_data, dict):
                self._attr_is_on = transport_data.get("connected", False)
                self._attr_extra_state_attributes = {
                    "rssi": transport_data.get("rssi"),
                    "message_count": transport_data.get("messages", 0),
                    "error_count": transport_data.get("errors", 0),
                    "last_activity": transport_data.get("last_activity"),
                }
            elif isinstance(transport_data, bool):
                self._attr_is_on = transport_data

            self.async_write_ha_state()
        except (json.JSONDecodeError, TypeError):
            pass


class SecuraCVCanaryMeshConnectedSensor(SecuraCVCanaryBinarySensorBase):
    """Binary sensor for Opera mesh network connection status."""

    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY
    _attr_icon = "mdi:lan-connect"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Mesh Connected", "mesh_connected")
        self._attr_is_on = False

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT mesh topic."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_MESH}",
            self._handle_message,
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle mesh status message."""
        try:
            data = json.loads(msg.payload)
            peer_count = data.get("peer_count", len(data.get("peers", [])))
            self._attr_is_on = peer_count > 0

            self._attr_extra_state_attributes = {
                "peer_count": peer_count,
                "peers": data.get("peers", []),
                "messages_sent": data.get("sent", 0),
                "messages_received": data.get("received", 0),
                "relay_count": data.get("relayed", 0),
            }
            self.async_write_ha_state()
        except (json.JSONDecodeError, TypeError):
            pass


class SecuraCVCanaryChirpActiveSensor(SecuraCVCanaryBinarySensorBase):
    """Binary sensor for Chirp community network status.

    Chirp uses ephemeral identities (3-emoji) and template-only messages
    for community awareness without surveillance.
    """

    _attr_icon = "mdi:bird"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Chirp Active", "chirp_active")
        self._attr_is_on = False

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT chirp topic."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_CHIRP}",
            self._handle_message,
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle chirp status message."""
        try:
            data = json.loads(msg.payload)
            self._attr_is_on = data.get("enabled", False) and data.get("ready", False)

            self._attr_extra_state_attributes = {
                "session_emoji": data.get("session_id", ""),  # 3-emoji identity
                "cooldown_tier": data.get("cooldown_tier", 0),
                "presence_minutes": data.get("presence_minutes", 0),
                "can_broadcast": data.get("can_broadcast", False),
                "alerts_sent": data.get("sent", 0),
                "alerts_received": data.get("received", 0),
                "confirmations_given": data.get("confirmed", 0),
            }
            self.async_write_ha_state()
        except (json.JSONDecodeError, TypeError):
            pass
