"""Binary sensor platform for SecuraCV integration.

Surfaces tamper events and transport health for multi-path resilience.
Canary devices use ANY available transport to communicate - these sensors
show which paths are alive and what threats have been detected.
"""
from __future__ import annotations

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
from typing import Any as _Any  # noqa: F401  (used in _read_trust_view signature below)
from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import (
    DOMAIN,
    CONF_MQTT_PREFIX,
    CONF_ENABLE_MQTT,
    TOPIC_STATUS,
    TOPIC_EVENTS,
    TOPIC_CHAIN,
    TOPIC_HEALTH,
    TOPIC_STATE,
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
    # Thresholds
    CRITICAL_MEMORY_THRESHOLD_BYTES,
    # Apple Home projection (see the HomeKit Bridge recipe in docs/integrations)
    HOMEKIT_MOTION_HOLD_SECONDS,
    homekit_signals_for_event,
)
from homeassistant.helpers.event import async_call_later
from . import mqtt_payload_within_cap, parse_mqtt_json
from .health_metrics import (
    canary_sd_replace_recommended,
    replacement_recommended,
    storage_status,
)

_LOGGER = logging.getLogger(__name__)


def _read_trust_view(
    hass: HomeAssistant, entry: ConfigEntry, device_id: str
) -> dict[str, _Any]:
    """Read the verify state stamped by sensor.py's chain handler.

    Binary sensor doesn't *run* the verifier — that's sensor.py's
    job, and re-verifying here would mean signing twice per chain
    publish for no benefit. We just read the cached verdict from
    entry_data so the binary sensor can ANNOTATE its state with the
    same trust info.
    """
    entry_data = hass.data.get(DOMAIN, {}).get(entry.entry_id, {})
    verify = entry_data.get("verify", {}).get(device_id)
    if not verify:
        return {"verified": False, "trust_reason": "no_pubkey"}
    return {
        "verified": bool(verify.get("trusted")),
        "trust_reason": verify.get("reason", "unknown"),
        "pinned_fingerprint": verify.get("pinned_fingerprint"),
        "received_fingerprint": verify.get("received_fingerprint"),
    }


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up SecuraCV binary sensors from a config entry."""
    entry_data = hass.data[DOMAIN][entry.entry_id]
    coordinator = entry_data.get("coordinator")

    # The kernel connectivity sensor only exists when a kernel is
    # configured (SETUP_MODE_KERNEL / SETUP_MODE_BOTH). In SETUP_MODE_MQTT
    # the coordinator is None and entry.data has no CONF_URL — adding the
    # sensor would KeyError on device_info, and its is_on (which reads
    # coordinator.last_update_success) would misleadingly imply a kernel
    # is reachable when none exists.
    entities: list[BinarySensorEntity] = []
    if coordinator is not None:
        entities.append(SecuraCVKernelOnlineSensor(coordinator, entry))
        entities.append(SecuraCVKernelStorageReplaceSensor(coordinator, entry))
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

        # SD card replacement recommendation (storage endurance)
        if topic_type == TOPIC_HEALTH and "sd_replace" not in entities_added[device_id]:
            entities_added[device_id].add("sd_replace")
            new_entities.append(
                SecuraCVCanarySDReplaceSensor(prefix, device_id, entry)
            )

        # Individual tamper type sensors. Created on the first tamper OR
        # health message: several of these parse health fields (sd_errors,
        # free_heap, power_loss_detected), and a device's one tamper publish
        # is non-retained — a hub that boots after the Canary would otherwise
        # never create the sensors that the periodic health flags feed.
        if topic_type in (TOPIC_TAMPER, TOPIC_HEALTH) and "tamper_sensors" not in entities_added[device_id]:
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

        # Motion + occupancy. These exist so the HomeKit Bridge (and any
        # other consumer) sees the standard device classes rather than a
        # template built by hand in someone's YAML — see
        # docs/integrations/apple-home-homekit-bridge.md.
        if topic_type == TOPIC_EVENTS and "presence_sensors" not in entities_added[device_id]:
            entities_added[device_id].add("presence_sensors")
            new_entities.extend([
                SecuraCVCanaryMotionSensor(prefix, device_id, entry),
                SecuraCVCanaryOccupancySensor(prefix, device_id, entry),
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

    # Subscribe for discovery on all relevant topics. The unsubscribe
    # callables land in entry_data["unsub_mqtt"] so async_unload_entry
    # releases them — otherwise every reload leaks wildcard subscriptions
    # whose closures hold the previous entities_added/async_add_entities.
    unsubs = hass.data[DOMAIN][entry.entry_id].setdefault("unsub_mqtt", [])
    for topic_suffix in [TOPIC_STATUS, TOPIC_CHAIN, TOPIC_HEALTH, TOPIC_TAMPER, TOPIC_TRANSPORT, TOPIC_MESH, TOPIC_CHIRP, TOPIC_EVENTS]:
        unsubs.append(
            await mqtt.async_subscribe(
                hass,
                f"{prefix}/+/{topic_suffix}",
                _async_discover_binary_sensors,
            )
        )


# =============================================================================
# Kernel Binary Sensors (HTTP API-based)
# =============================================================================

class SecuraCVKernelOnlineSensor(CoordinatorEntity, BinarySensorEntity):
    """Binary sensor for kernel connectivity status."""

    # With has_entity_name, HA prefixes the device name ("SecuraCV Privacy
    # Witness Kernel"), so the entity name must not repeat the brand.
    _attr_name = "Online"
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


class SecuraCVKernelStorageReplaceSensor(CoordinatorEntity, BinarySensorEntity):
    """Problem sensor: the kernel recommends replacing its SD card.

    Turns on when the storage health status reaches replacement_recommended
    or critical (wear estimate at/over threshold, persistent write errors,
    or critically low space). Driven by the same hysteresis-filtered status
    as the Storage Health sensor, so it never flaps on transient readings.
    """

    _attr_name = "Storage Replacement Recommended"
    _attr_device_class = BinarySensorDeviceClass.PROBLEM
    _attr_icon = "mdi:sd-alert"
    _attr_has_entity_name = True

    def __init__(self, coordinator, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{DOMAIN}_{entry.entry_id}_storage_replace"

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

    def _status(self) -> str | None:
        if not self.coordinator.data:
            return None
        return storage_status(self.coordinator.data.get("status"))

    @property
    def is_on(self) -> bool:
        """True when the card should be replaced."""
        return replacement_recommended(self._status())

    @property
    def extra_state_attributes(self) -> dict[str, _Any] | None:
        """Surface the underlying status for automations."""
        status = self._status()
        if status is None:
            return None
        return {"storage_status": status}


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
        self._entry = entry
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
        """Subscribe to MQTT status topic; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_STATUS}",
                self._handle_message,
            )
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle status message."""
        if not mqtt_payload_within_cap(msg.payload):
            return
        try:
            payload = msg.payload.decode() if isinstance(msg.payload, bytes) else str(msg.payload)
        except UnicodeDecodeError:
            return
        self._attr_is_on = payload.lower().strip() in ("online", "1", "true", "connected")
        self.async_write_ha_state()


class SecuraCVCanaryChainValidSensor(SecuraCVCanaryBinarySensorBase):
    """Binary sensor for hash chain integrity."""

    _attr_icon = "mdi:shield-check"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Chain Valid", "chain_valid")
        # Unknown until a chain publish is actually verified. Defaulting to
        # "on" would say "verified" about a chain nobody has checked yet —
        # inverted from the project's honesty rule. HA renders None honestly
        # as unknown.
        self._attr_is_on: bool | None = None

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT chain topic; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_CHAIN}",
                self._handle_message,
            )
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle chain message."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
        valid = data.get("valid", data.get("integrity", True))
        # Sensor-side chain handler already ran verify_chain and
        # stamped entry_data["verify"][device_id]; just read it
        # here so chain_valid surfaces both the chain-integrity
        # bit (the device's self-report) AND the PKI sig state.
        trust_view = _read_trust_view(self.hass, self._entry, self._device_id)
        self._attr_is_on = bool(valid) and trust_view["verified"]
        self._attr_extra_state_attributes = {
            "chain_length": data.get("length", 0),
            "latest_hash": data.get("latest_hash", ""),
            "verification_error": data.get("error", None),
            **trust_view,
        }
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
        """Subscribe to MQTT health and tamper topics; release on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
                self._handle_message,
            )
        )
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_TAMPER}",
                self._handle_tamper_message,
            )
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle health message for tamper detection."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
        tamper = data.get("tamper_detected", data.get("tamper", False))
        self._attr_is_on = bool(tamper)
        self.async_write_ha_state()

    @callback
    def _handle_tamper_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle dedicated tamper message."""
        data = parse_mqtt_json(msg.payload)
        # Any publish on the tamper topic triggers this sensor; a JSON
        # object additionally carries detail attributes.
        self._attr_is_on = True
        if data is not None:
            self._attr_extra_state_attributes = {
                "tamper_type": data.get("type", "unknown"),
                "timestamp": data.get("timestamp", ""),
                "detail": data.get("detail", ""),
            }
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
        """Subscribe to MQTT tamper and health topics; release on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_TAMPER}",
                self._handle_tamper_message,
            )
        )
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
                self._handle_health_message,
            )
        )

    @callback
    def _handle_tamper_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle dedicated tamper topic message."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
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

    @callback
    def _handle_health_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle health message for tamper indicators."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
        try:
            tamper_data = data.get("tamper", {})
            if not isinstance(tamper_data, dict):
                # The general tamper sensor accepts the boolean spelling
                # ({"tamper": true}); a bare boolean names no specific
                # tamper type, so it contributes nothing to THIS sensor —
                # but it must not AttributeError the callback either.
                tamper_data = {}

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
                is_triggered = free_heap < CRITICAL_MEMORY_THRESHOLD_BYTES
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
        except TypeError:
            # Unexpected value types inside an otherwise well-shaped dict
            # (e.g. "sd_errors" as a string) — skip this publish.
            pass


class SecuraCVCanarySDReplaceSensor(SecuraCVCanaryBinarySensorBase):
    """Problem sensor: a Canary device recommends replacing its SD card.

    Driven by the firmware's NVS-persisted lifetime write counters and
    wear estimate, published in the `sd` object of the health payload.
    Firmware without SD endurance reporting simply never turns this on.
    """

    _attr_device_class = BinarySensorDeviceClass.PROBLEM
    _attr_icon = "mdi:sd-alert"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "SD Replacement Recommended", "sd_replace")
        self._attr_is_on = False

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT health topic; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
                self._handle_message,
            )
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle health message for the SD replacement flag."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
        self._attr_is_on = canary_sd_replace_recommended(data)
        self.async_write_ha_state()


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
        """Subscribe to MQTT transport topic; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_TRANSPORT}",
                self._handle_message,
            )
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle transport status message."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
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


class SecuraCVCanaryProjectedSensorBase(SecuraCVCanaryBinarySensorBase):
    """A sensor driven by the Apple Home projection's event vocabulary.

    Which events assert which signal is NOT decided here. It comes from
    `HOMEKIT_EVENT_SIGNALS`, which mirrors `signals_for_event` in
    src/bridge/homekit.rs; both are generated from
    `homekit_projection.event_signals` in spec/witness_dictionary.json and
    gated by scripts/lint_dictionary_sync.py. Hand-writing the mapping in a
    third place is exactly the drift that linter exists to catch.

    The signal auto-clears after a hold window, the way a real motion sensor
    does. Without it a single event would latch the sensor on forever and
    every automation written against it would fire once and never reset.

    Trust honesty: state follows the repo-wide warn-loudly-accept policy
    (an unsigned or key-mismatched publish still moves the sensor — the
    Apple Home recipe documents this hop as unverified by design), but the
    verdict must ride along like it does on every other surface. Each
    assertion stamps the device's current trust view into the attributes so
    downstream consumers can see "verified: false" instead of a laundered
    signal.
    """

    _signal: str = ""

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry,
                 name_suffix: str, key: str) -> None:
        super().__init__(prefix, device_id, entry, name_suffix, key)
        self._attr_is_on = False
        self._cancel_clear = None

    async def async_added_to_hass(self) -> None:
        """Subscribe to the events topic; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_EVENTS}",
                self._handle_event,
            )
        )

    @callback
    def _handle_event(self, msg: mqtt.ReceiveMessage) -> None:
        """Assert this signal if the event maps to it."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
        event_type = data.get("event_type", data.get("type", data.get("event", "")))
        if self._signal not in homekit_signals_for_event(str(event_type)):
            return
        self._assert()

    @callback
    def _stamp_trust(self) -> None:
        """Annotate the state with the device's current trust verdict.

        Reads the verify slice sensor.py's events handler stamps (both
        callbacks share the events topic), the same way the chain-valid
        sensor does — this entity never runs a verifier itself.
        """
        self._attr_extra_state_attributes = _read_trust_view(
            self.hass, self._entry, self._device_id
        )

    @callback
    def _assert(self) -> None:
        """Turn on, and (re)arm the auto-clear."""
        self._attr_is_on = True
        self._stamp_trust()
        if self._cancel_clear is not None:
            self._cancel_clear()
        self._cancel_clear = async_call_later(
            self.hass, HOMEKIT_MOTION_HOLD_SECONDS, self._clear
        )
        self.async_write_ha_state()

    @callback
    def _clear(self, _now) -> None:
        """The hold window expired."""
        self._cancel_clear = None
        self._attr_is_on = False
        self.async_write_ha_state()

    async def async_will_remove_from_hass(self) -> None:
        """Cancel a pending clear so it cannot fire into a dead entity."""
        if self._cancel_clear is not None:
            self._cancel_clear()
            self._cancel_clear = None


class SecuraCVCanaryMotionSensor(SecuraCVCanaryProjectedSensorBase):
    """Motion, as a standard HA motion sensor.

    `device_class: motion` is the point: it is what makes Home Assistant's
    HomeKit Bridge expose this as a HomeKit motion sensor, and what lets any
    other consumer treat a Canary like a sensor it already understands.
    """

    _attr_device_class = BinarySensorDeviceClass.MOTION
    _attr_icon = "mdi:motion-sensor"
    _signal = "motion"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        super().__init__(prefix, device_id, entry, "Motion", "motion")


class SecuraCVCanaryOccupancySensor(SecuraCVCanaryProjectedSensorBase):
    """Occupancy — a presence being sensed, not an identity being known.

    Driven by the events vocabulary like its sibling, and additionally by the
    retained `state` snapshot, which is where the firmware actually publishes
    presence (canary-sense's topics.h builds `securacv/<id>/state` and its own
    HA discovery reads `value_json.presence`). A snapshot without the field
    leaves the sensor alone rather than asserting "nobody is there".
    """

    _attr_device_class = BinarySensorDeviceClass.OCCUPANCY
    _attr_icon = "mdi:account-question"
    _signal = "occupancy"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        super().__init__(prefix, device_id, entry, "Occupancy", "occupancy")

    async def async_added_to_hass(self) -> None:
        """Subscribe to events and to the retained state snapshot."""
        await super().async_added_to_hass()
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_STATE}",
                self._handle_state,
            )
        )

    @callback
    def _handle_state(self, msg: mqtt.ReceiveMessage) -> None:
        """Track reported presence, which is a state rather than an event."""
        data = parse_mqtt_json(msg.payload)
        if data is None or "presence" not in data:
            return
        present = data["presence"]
        if isinstance(present, str):
            present = present.strip().lower() in ("1", "on", "true", "yes", "detected")
        # Reported state persists until the witness says otherwise, so this
        # path deliberately does NOT arm the hold-window auto-clear.
        if self._cancel_clear is not None:
            self._cancel_clear()
            self._cancel_clear = None
        self._attr_is_on = bool(present)
        self._stamp_trust()
        self.async_write_ha_state()


class SecuraCVCanaryMeshConnectedSensor(SecuraCVCanaryBinarySensorBase):
    """Binary sensor for Opera mesh network connection status."""

    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY
    _attr_icon = "mdi:lan-connect"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Mesh Connected", "mesh_connected")
        self._attr_is_on = False

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT mesh topic; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_MESH}",
                self._handle_message,
            )
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle mesh status message."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
        try:
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
        except TypeError:
            # Unexpected value types inside an otherwise well-shaped dict
            # (e.g. "peers" as a number) — skip this publish.
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
        """Subscribe to MQTT chirp topic; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_CHIRP}",
                self._handle_message,
            )
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle chirp status message."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
        self._attr_is_on = bool(data.get("enabled", False) and data.get("ready", False))

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
