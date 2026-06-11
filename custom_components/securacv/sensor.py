"""Sensor platform for SecuraCV integration."""
from __future__ import annotations

import json
import logging
from typing import Any

from homeassistant.components import mqtt
from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorStateClass,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_URL, EntityCategory, PERCENTAGE, UnitOfTemperature
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
    CRITICAL_BATTERY_THRESHOLD_PERCENT,
    WARNING_BATTERY_THRESHOLD_PERCENT,
    WARNING_MEMORY_THRESHOLD_BYTES,
    DEFAULT_EVENT_ICON,
    event_type_metadata,
)
from .device_trust import TrustStore
from . import async_record_verify
from .health_metrics import (
    battery_charging,
    battery_percent,
    bytes_per_day_to_mb,
    canary_sd,
    canary_sd_wear_pct,
    kernel_storage,
    kernel_thermal,
    memory_free_bytes,
    round_pct,
)
from .signature import verify_chain, verify_counts, verify_event

_LOGGER = logging.getLogger(__name__)


def _trust_store_for(hass: HomeAssistant, entry: ConfigEntry) -> TrustStore | None:
    """Pull the per-entry TrustStore singleton out of hass.data."""
    entry_data = hass.data.get(DOMAIN, {}).get(entry.entry_id)
    if not entry_data:
        return None
    return entry_data.get("trust_store")


def _verify_and_record(
    hass: HomeAssistant,
    entry: ConfigEntry,
    device_id: str,
    payload: dict[str, Any],
    verifier,
) -> None:
    """Run the kind-specific verifier and stamp the result so the
    extra_state_attributes block can surface it next to the entity.

    Verifier signature: `verifier(trust_store, device_id, payload) -> TrustVerdict`.
    Failing payloads (no trust store yet, unsigned firmware) are
    silently treated as "unverified" — we never want to drop entity
    state on a sig issue, only annotate it. The persistent_notification
    fan-out lives in __init__.py's async_record_verify."""
    trust_store = _trust_store_for(hass, entry)
    if trust_store is None:
        return
    verdict = verifier(trust_store, device_id, payload)
    async_record_verify(hass, entry, device_id, verdict)


def _trust_attrs(
    hass: HomeAssistant, entry: ConfigEntry, device_id: str
) -> dict[str, Any]:
    """Return the verify-state slice that every signed-topic entity
    surfaces as part of extra_state_attributes. Keys are kept short
    and JSON-friendly because they show up directly in HA's UI."""
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
    """Set up SecuraCV sensors from a config entry."""
    entry_data = hass.data[DOMAIN][entry.entry_id]
    coordinator = entry_data.get("coordinator")

    # The kernel HTTP-API sensor only exists when a kernel is configured
    # (SETUP_MODE_KERNEL / SETUP_MODE_BOTH). In SETUP_MODE_MQTT the
    # coordinator is None and entry.data has no CONF_URL — adding the
    # sensor would KeyError on device_info and mislead the UI about a
    # kernel that doesn't exist.
    entities: list[SensorEntity] = []
    if coordinator is not None:
        entities.append(SecuraCVKernelLastEventSensor(coordinator, entry))
        # Storage endurance & health diagnostics (kernel GET /status).
        # Older kernels without the endpoint simply leave these empty.
        entities.extend(
            [
                SecuraCVKernelStorageHealthSensor(coordinator, entry),
                SecuraCVKernelStorageFreeSensor(coordinator, entry),
                SecuraCVKernelStorageWearSensor(coordinator, entry),
                SecuraCVKernelStorageWriteRateSensor(coordinator, entry),
                SecuraCVKernelTemperatureSensor(coordinator, entry),
            ]
        )
    adapter_stats_coordinator = entry_data.get("adapter_stats_coordinator")
    if adapter_stats_coordinator is not None:
        entities.append(
            SecuraCVAdapterStatsSensor(adapter_stats_coordinator, entry)
        )
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
            new_entities.append(
                SecuraCVCanarySDWearSensor(prefix, device_id, entry)
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
    _attr_has_entity_name = True

    def __init__(self, coordinator, entry: ConfigEntry) -> None:
        """Initialize the sensor."""
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{DOMAIN}_{entry.entry_id}_latest_event"

    @property
    def icon(self) -> str:
        """Icon reflects the latest event type, so the dashboard reads at a glance."""
        # coordinator.data may be None before the first successful update.
        if self.coordinator.data and (event := self.coordinator.data.get("latest_event")):
            return event_type_metadata(event.get("event_type"))["icon"]
        return DEFAULT_EVENT_ICON

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
        # Human-readable label for the coarse claim, for nicer dashboard display.
        attrs["friendly_event"] = event_type_metadata(event.get("event_type"))["label"]
        return attrs or None


class SecuraCVKernelStorageSensorBase(CoordinatorEntity, SensorEntity):
    """Base for kernel storage endurance & health diagnostics.

    Data comes from the coordinator's `status` key (the kernel's token-gated
    GET /status report). All sensors tolerate a missing report — kernels
    that predate the endpoint, or have monitoring disabled, return None —
    by reporting no state instead of erroring.
    """

    _attr_has_entity_name = True
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, coordinator, entry: ConfigEntry, name: str, key: str) -> None:
        """Initialize the sensor."""
        super().__init__(coordinator)
        self._entry = entry
        self._attr_name = name
        self._attr_unique_id = f"{DOMAIN}_{entry.entry_id}_{key}"

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

    def _status_payload(self) -> dict[str, Any] | None:
        """The /status payload from the last coordinator refresh, if any."""
        if not self.coordinator.data:
            return None
        return self.coordinator.data.get("status")

    def _storage(self) -> dict[str, Any] | None:
        return kernel_storage(self._status_payload())


class SecuraCVKernelStorageHealthSensor(SecuraCVKernelStorageSensorBase):
    """Overall SD-card / storage health status with full metrics attached."""

    _attr_icon = "mdi:sd"

    def __init__(self, coordinator, entry: ConfigEntry) -> None:
        """Initialize the sensor."""
        super().__init__(coordinator, entry, "Storage Health", "storage_health")

    @property
    def native_value(self) -> str | None:
        """good / degraded / replacement_recommended / critical."""
        storage = self._storage()
        if storage is None:
            return None
        status = storage.get("status")
        return str(status) if status is not None else None

    @property
    def extra_state_attributes(self) -> dict[str, Any] | None:
        """Full storage metrics for dashboards and automations."""
        storage = self._storage()
        if storage is None:
            return None
        return dict(storage)


class SecuraCVKernelStorageFreeSensor(SecuraCVKernelStorageSensorBase):
    """Free space on the filesystem holding the sealed log."""

    _attr_icon = "mdi:harddisk"
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = PERCENTAGE

    def __init__(self, coordinator, entry: ConfigEntry) -> None:
        """Initialize the sensor."""
        super().__init__(coordinator, entry, "Storage Free", "storage_free_pct")

    @property
    def native_value(self) -> float | None:
        """Free space percentage."""
        storage = self._storage()
        if storage is None:
            return None
        return round_pct(storage.get("free_pct"))


class SecuraCVKernelStorageWearSensor(SecuraCVKernelStorageSensorBase):
    """Estimated SD-card wear against its configured endurance rating.

    A conservative estimate (not a measurement — SD cards expose no SMART
    data): whole-device bytes written versus the configured TBW rating.
    """

    _attr_icon = "mdi:sd"
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = PERCENTAGE

    def __init__(self, coordinator, entry: ConfigEntry) -> None:
        """Initialize the sensor."""
        super().__init__(coordinator, entry, "Storage Wear Estimate", "storage_wear_pct")

    @property
    def native_value(self) -> float | None:
        """Estimated wear percentage."""
        storage = self._storage()
        if storage is None:
            return None
        return round_pct(storage.get("wear_pct"))

    @property
    def extra_state_attributes(self) -> dict[str, Any] | None:
        """Endurance context for the estimate."""
        storage = self._storage()
        if storage is None:
            return None
        return {
            "endurance_tbw": storage.get("endurance_tbw"),
            "lifetime_bytes_written": storage.get("lifetime_bytes_written"),
            "estimated_days_remaining": storage.get("estimated_days_remaining"),
            "source_device": storage.get("source_device"),
        }


class SecuraCVKernelStorageWriteRateSensor(SecuraCVKernelStorageSensorBase):
    """Whole-device write rate (MB/day) — the pace at which the card wears."""

    _attr_icon = "mdi:database-arrow-down"
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = "MB/d"

    def __init__(self, coordinator, entry: ConfigEntry) -> None:
        """Initialize the sensor."""
        super().__init__(coordinator, entry, "Storage Write Rate", "storage_write_rate")

    @property
    def native_value(self) -> float | None:
        """Write rate in MB/day."""
        storage = self._storage()
        if storage is None:
            return None
        return bytes_per_day_to_mb(storage.get("write_rate_bytes_per_day"))


class SecuraCVKernelTemperatureSensor(SecuraCVKernelStorageSensorBase):
    """SoC temperature on the kernel host (heat accelerates flash wear)."""

    _attr_device_class = SensorDeviceClass.TEMPERATURE
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = UnitOfTemperature.CELSIUS

    def __init__(self, coordinator, entry: ConfigEntry) -> None:
        """Initialize the sensor."""
        super().__init__(coordinator, entry, "SoC Temperature", "soc_temperature")

    @property
    def native_value(self) -> float | None:
        """SoC temperature in °C."""
        thermal = kernel_thermal(self._status_payload())
        if thermal is None:
            return None
        temp = thermal.get("soc_temp_c")
        if temp is None:
            return None
        try:
            return round(float(temp), 1)
        except (TypeError, ValueError):
            return None

    @property
    def extra_state_attributes(self) -> dict[str, Any] | None:
        """Thermal classification (ok / warm / hot)."""
        thermal = kernel_thermal(self._status_payload())
        if thermal is None:
            return None
        return {"thermal_status": thermal.get("status")}


class SecuraCVAdapterStatsSensor(CoordinatorEntity, SensorEntity):
    """Diagnostic sensor surfacing adapter_host per-adapter counters.

    State is the total number of sealed events across all adapters; the full per-adapter breakdown
    (and totals) is exposed as attributes. Operational counts only — no event content.
    """

    _attr_name = "SecuraCV Adapter Host"
    _attr_icon = "mdi:hub"
    _attr_has_entity_name = True
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_state_class = SensorStateClass.MEASUREMENT

    _COUNTERS = (
        "claims_emitted",
        "claims_sealed",
        "claims_filtered",
        "claims_rejected",
        "poll_errors",
    )

    def __init__(self, coordinator, entry: ConfigEntry) -> None:
        """Initialize the sensor."""
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{DOMAIN}_{entry.entry_id}_adapter_stats"

    @property
    def device_info(self) -> DeviceInfo:
        """Group adapter-host diagnostics under their own device."""
        return DeviceInfo(
            identifiers={(DOMAIN, f"{self._entry.entry_id}_adapter_host")},
            manufacturer=MANUFACTURER,
            model="SecuraCV Adapter Host",
            name="SecuraCV Adapter Host",
        )

    @staticmethod
    def _adapters(data: Any) -> dict[str, dict[str, Any]]:
        if not isinstance(data, dict):
            return {}
        return {k: v for k, v in data.items() if isinstance(v, dict)}

    @property
    def native_value(self) -> int | None:
        """Total sealed events across all adapters."""
        adapters = self._adapters(self.coordinator.data)
        if not adapters:
            return None
        # `or 0` guards against a null value in the JSON (not just an absent key).
        return sum(int(s.get("claims_sealed") or 0) for s in adapters.values())

    @property
    def extra_state_attributes(self) -> dict[str, Any] | None:
        """Per-adapter breakdown plus totals."""
        adapters = self._adapters(self.coordinator.data)
        if not adapters:
            return None
        attrs: dict[str, Any] = {"adapters": len(adapters), "per_adapter": adapters}
        for counter in self._COUNTERS:
            attrs[f"total_{counter}"] = sum(
                int(s.get(counter) or 0) for s in adapters.values()
            )
        return attrs


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
            _verify_and_record(self.hass, self._entry, self._device_id,
                               data, verify_counts)
            self._attr_extra_state_attributes = _trust_attrs(
                self.hass, self._entry, self._device_id)
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
            _verify_and_record(self.hass, self._entry, self._device_id,
                               data, verify_chain)
            self._attr_extra_state_attributes = {
                "latest_hash": data.get("latest_hash", ""),
                "algorithm": data.get("algorithm", "ed25519"),
                **_trust_attrs(self.hass, self._entry, self._device_id),
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
            _verify_and_record(self.hass, self._entry, self._device_id,
                               data, verify_event)
            self._attr_extra_state_attributes = {
                "timestamp": data.get("timestamp", ""),
                "zone": data.get("zone", ""),
                "confidence": data.get("confidence", ""),
                "signed": data.get("signed", False),
                **_trust_attrs(self.hass, self._entry, self._device_id),
            }
        except (json.JSONDecodeError, TypeError):
            payload = msg.payload.decode(errors="ignore") if isinstance(msg.payload, bytes) else str(msg.payload)
            self._attr_native_value = payload[:255]
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
            # Both firmware spellings for battery ("battery" /
            # "battery_soc") and memory ("memory_free" / "free_heap").
            battery = battery_percent(data)
            memory_free = memory_free_bytes(data)

            # Battery thresholds apply only to a discharging battery:
            # mains-powered devices (battery is None) and charging
            # devices are not at power-loss risk, and alerting on them
            # would be a false alarm.
            battery_for_status = (
                100 if battery is None or battery_charging(data) else battery
            )

            if (
                battery_for_status < CRITICAL_BATTERY_THRESHOLD_PERCENT
                or memory_free < WARNING_MEMORY_THRESHOLD_BYTES
            ):
                self._attr_native_value = "critical"
            elif battery_for_status < WARNING_BATTERY_THRESHOLD_PERCENT:
                self._attr_native_value = "warning"
            else:
                self._attr_native_value = "healthy"

            self._attr_extra_state_attributes = {
                "battery_percent": 100 if battery is None else battery,
                "memory_free_bytes": memory_free,
                "uptime_seconds": data.get("uptime", 0),
                "firmware_version": data.get("firmware_version", ""),
                "public_key": data.get("public_key", ""),
            }
            # Battery detail, when the firmware reports it.
            for key in (
                "battery_present",
                "charge_state",
                "battery_health_pct",
                "battery_mv",
            ):
                if (val := data.get(key)) is not None:
                    self._attr_extra_state_attributes[key] = val
            # SD endurance metrics, when the firmware reports them.
            if (sd := canary_sd(data)) is not None:
                self._attr_extra_state_attributes["sd"] = sd
            if (temp_c := data.get("temp_c")) is not None:
                self._attr_extra_state_attributes["temp_c"] = temp_c
        except (json.JSONDecodeError, TypeError):
            self._attr_native_value = "unknown"
        self.async_write_ha_state()


class SecuraCVCanarySDWearSensor(SecuraCVCanarySensorBase):
    """Estimated SD-card wear reported by a Canary device.

    Conservative estimate from NVS-persisted lifetime write counters
    against the configured endurance rating; stays empty on firmware
    that does not yet report the `sd` health object.
    """

    _attr_icon = "mdi:sd"
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = "%"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "SD Wear Estimate", "sd_wear")

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT when added."""
        await mqtt.async_subscribe(
            self.hass,
            f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
            self._handle_message,
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle health message for SD endurance data."""
        try:
            data = json.loads(msg.payload)
        except (json.JSONDecodeError, TypeError):
            return
        wear = canary_sd_wear_pct(data)
        if wear is None and canary_sd(data) is None:
            # Firmware without SD reporting: leave the sensor untouched.
            return
        self._attr_native_value = wear
        if (sd := canary_sd(data)) is not None:
            self._attr_extra_state_attributes = {
                "mounted": sd.get("mounted"),
                "usage_pct": sd.get("usage_pct"),
                "writes": sd.get("writes"),
                "errors": sd.get("errors"),
                "lifetime_kb": sd.get("lifetime_kb"),
                "replace_recommended": sd.get("replace_recommended"),
            }
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
