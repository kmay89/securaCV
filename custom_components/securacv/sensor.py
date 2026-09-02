"""Sensor platform for SecuraCV integration."""
from __future__ import annotations

import json
import logging
import time
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
    TOPIC_STATUS,
    MANUFACTURER,
    MODEL_KERNEL,
    MODEL_CANARY,
    CRITICAL_BATTERY_THRESHOLD_PERCENT,
    WARNING_BATTERY_THRESHOLD_PERCENT,
    WARNING_MEMORY_THRESHOLD_BYTES,
    DEFAULT_EVENT_ICON,
    DEVICE_TYPE_CANARY_SENSE,
    MODALITY_UNKNOWN,
    canonical_device_type,
    event_type_metadata,
    modality_for,
    modality_metadata,
    normalize_attestation,
)
from .device_trust import TrustStore
from . import (
    async_record_verify,
    parse_mqtt_json,
    unsigned_trust_attrs,
    valid_device_id,
)
from .device_trust import TrustVerdict
from .voice import record_canary_event
from .watch_runtime import async_observe_event
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
from .signature import verify_chain, verify_counts, verify_event, verify_sense_event

_LOGGER = logging.getLogger(__name__)


def _trust_store_for(hass: HomeAssistant, entry: ConfigEntry) -> TrustStore | None:
    """Pull the per-entry TrustStore singleton out of hass.data."""
    entry_data = hass.data.get(DOMAIN, {}).get(entry.entry_id)
    if not entry_data:
        return None
    return entry_data.get("trust_store")


# Which payload field is the monotonic counter for each signed kind. The
# canonicals sign these (signature.py) but nothing consumed them: an old,
# validly signed message replayed by anyone on the broker verified green and
# moved entity state.
_REPLAY_COUNTER_FIELD = {
    "verify_event": "event_id",
    "verify_sense_event": "seq",
    "verify_chain": "length",
    "verify_counts": "total",
}


def _replay_gate(
    hass: HomeAssistant,
    entry: ConfigEntry,
    device_id: str,
    payload: dict[str, Any],
    verifier,
    verdict: TrustVerdict,
) -> TrustVerdict:
    """Downgrade a VERIFIED publish whose counter runs backwards to a replay.

    Only a decrease is a replay: the firmware republishes its current chain
    head / counts total unchanged while idle, and a broker re-delivers the
    retained last event on reconnect, so an EQUAL counter is benign and passes.
    A lower counter cannot be the device's present state — it is an older
    message, however valid its signature. The high-water mark is reset when a
    device is (re)pinned via TOFU (async_record_verify), which is what a
    factory reset looks like from here.
    """
    if not verdict.trusted:
        return verdict
    field = _REPLAY_COUNTER_FIELD.get(getattr(verifier, "__name__", ""))
    if field is None or not isinstance(payload, dict):
        return verdict
    raw = payload.get(field)
    if raw is None or isinstance(raw, bool):
        return verdict
    try:
        counter = int(raw)
    except (TypeError, ValueError):
        return verdict
    entry_data = hass.data.get(DOMAIN, {}).get(entry.entry_id)
    if not isinstance(entry_data, dict):
        return verdict
    marks = entry_data.setdefault("replay", {}).setdefault(device_id, {})
    last = marks.get(field)
    if last is not None and counter < last:
        return TrustVerdict(
            trusted=False,
            reason="replay",
            pinned_fingerprint=verdict.pinned_fingerprint,
            received_fingerprint=verdict.received_fingerprint,
            detail=(
                f"{field}={counter} is older than the last verified "
                f"{field}={last}; a validly signed but stale publish"
            ),
        )
    marks[field] = counter
    return verdict


def _verify_and_record(
    hass: HomeAssistant,
    entry: ConfigEntry,
    device_id: str,
    payload: dict[str, Any],
    verifier,
) -> TrustVerdict | None:
    """Run the kind-specific verifier and stamp the result so the
    extra_state_attributes block can surface it next to the entity.

    Verifier signature: `verifier(trust_store, device_id, payload) -> TrustVerdict`.
    Failing payloads (no trust store yet, unsigned firmware) are
    silently treated as "unverified" — we never want to drop entity
    state on a sig issue, only annotate it. The one exception is a
    REPLAY (a verified publish whose counter runs backwards): callers
    get the verdict back so they can refuse to move state on it. The
    persistent_notification fan-out lives in __init__.py's
    async_record_verify."""
    trust_store = _trust_store_for(hass, entry)
    if trust_store is None:
        return None
    verdict = verifier(trust_store, device_id, payload)
    verdict = _replay_gate(hass, entry, device_id, payload, verifier, verdict)
    async_record_verify(hass, entry, device_id, verdict)
    return verdict


def _is_replay(verdict: TrustVerdict | None) -> bool:
    return verdict is not None and verdict.reason == "replay"


def _trust_attrs(
    hass: HomeAssistant, entry: ConfigEntry, device_id: str
) -> dict[str, Any]:
    """Return the verify-state slice that every signed-topic entity
    surfaces as part of extra_state_attributes. Keys are kept short
    and JSON-friendly because they show up directly in HA's UI.

    Entities moved by a topic the firmware never signs (health, and the
    binary_sensor tamper / transport / mesh / chirp family) stamp the
    same keys via ``unsigned_trust_attrs`` in __init__.py instead, so an
    unsigned publish and a verified one differ by value, not by absence."""
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


def _record_last_event(
    hass: HomeAssistant, entry: ConfigEntry, device_id: str, event_type: Any
) -> None:
    """Stash a Canary's newest event where the Assist intents read it.

    The last-event sensor holds its state on the entity; the voice brief
    (voice.py, via intent.py) reads entry_data instead, so the event is
    mirrored there with its hub arrival time. Must be called AFTER
    _verify_and_record so the fresh trust verdict rides along — a spoken
    answer that omitted it would launder an unsigned or key-mismatched
    publish into "the latest witness event".
    """
    domain_data = hass.data.get(DOMAIN)
    if not isinstance(domain_data, dict):
        return
    entry_data = domain_data.get(entry.entry_id)
    if not isinstance(entry_data, dict):
        return
    devices = entry_data.get("devices")
    if not isinstance(devices, dict):
        return
    verdict = entry_data.get("verify", {}).get(device_id)
    trusted: bool | None = None
    reason: str | None = None
    if isinstance(verdict, dict):
        trusted = bool(verdict.get("trusted"))
        reason = verdict.get("reason")
    now = time.time()
    record_canary_event(
        devices,
        device_id,
        str(event_type) if event_type is not None else None,
        now,
        trusted=trusted,
        reason=reason,
    )
    # Feed any watch bound to this device, so a started watch is really
    # watching rather than only recorded (docs/design/watches.md).
    async_observe_event(hass, device_id, now)


def _device_type_for(
    hass: HomeAssistant, entry: ConfigEntry, device_id: str
) -> str | None:
    """Best-effort device_type for a Canary, from its cached status payload.

    __init__.py's status handler stores the raw `{prefix}/{id}/status` payload
    in entry_data["devices"][device_id]["status"]; the device advertises its
    `device_type` (e.g. "canary-sense") there. Used to (a) derive a sensing
    modality when an individual event omits it and (b) gate device-type-specific
    entities like the radar-link diagnostic. Returns None when the device hasn't
    published a parseable status with a device_type yet.
    """
    domain_data = hass.data.get(DOMAIN)
    if not isinstance(domain_data, dict):
        return None
    entry_data = domain_data.get(entry.entry_id)
    if not isinstance(entry_data, dict):
        return None
    devices = entry_data.get("devices")
    if not isinstance(devices, dict):
        return None
    device = devices.get(device_id)
    if not isinstance(device, dict):
        return None
    status = device.get("status")
    if isinstance(status, dict):
        return canonical_device_type(status.get("device_type"))
    if isinstance(status, str):
        try:
            data = json.loads(status)
        except (json.JSONDecodeError, TypeError):
            return None
        if isinstance(data, dict):
            return canonical_device_type(data.get("device_type"))
    return None


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
        if not valid_device_id(device_id):
            return

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

        # Radar-link diagnostic (Phase 3): only for radar witnesses. Created
        # when the device advertises device_type "canary-sense" in its status
        # payload — mirrors the conditional, device-type-aware creation the
        # design asks for so non-radar canaries never grow a phantom sensor.
        # Discovery fires on STATUS (carries device_type) and HEALTH (carries
        # the radar-link metrics) so the sensor appears regardless of which
        # topic arrives first, as long as a status has identified the device.
        if (
            topic_type in (TOPIC_STATUS, TOPIC_HEALTH)
            and "radar_link" not in entities_added[device_id]
            and _device_type_for(hass, entry, device_id) == DEVICE_TYPE_CANARY_SENSE
        ):
            entities_added[device_id].add("radar_link")
            new_entities.append(
                SecuraCVCanaryRadarLinkSensor(prefix, device_id, entry)
            )

        if new_entities:
            async_add_entities(new_entities)

    # Subscribe to all device topics for sensor discovery. STATUS is included
    # so the device_type-gated radar-link sensor can be created once the device
    # identifies itself, independent of health-topic timing. The unsubscribe
    # callables land in entry_data["unsub_mqtt"] so async_unload_entry
    # releases them — otherwise every reload leaks wildcard subscriptions
    # whose closures hold the previous entities_added/async_add_entities.
    unsubs = hass.data[DOMAIN][entry.entry_id].setdefault("unsub_mqtt", [])
    for topic_suffix in [TOPIC_COUNTS, TOPIC_CHAIN, TOPIC_EVENTS, TOPIC_HEALTH, TOPIC_STATUS]:
        unsubs.append(
            await mqtt.async_subscribe(
                hass,
                f"{prefix}/+/{topic_suffix}",
                _async_discover_sensors,
            )
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
        keys = (
            "zone_id",
            "time_bucket",
            "confidence",
            "kernel_version",
            "ruleset_id",
            # Track B provenance ("adapter" / "ha-bridged") stamped by the
            # kernel export; absent on device/kernel-attested events. The
            # timeline card renders it as a provenance chip.
            "attestation",
        )
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
        """Subscribe to MQTT when added; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_COUNTS}",
                self._handle_message,
            )
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle count message."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            # Not a JSON object (bare count, junk, oversized): try the plain
            # integer fallback the firmware's earliest publishes used.
            try:
                self._attr_native_value = int(msg.payload)
            except (ValueError, TypeError):
                return
        else:
            verdict = _verify_and_record(self.hass, self._entry, self._device_id,
                                         data, verify_counts)
            if not _is_replay(verdict):
                self._attr_native_value = data.get("total", data.get("count", 0))
            self._attr_extra_state_attributes = _trust_attrs(
                self.hass, self._entry, self._device_id)
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
        """Subscribe to MQTT when added; release the subscription on removal."""
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
        verdict = _verify_and_record(self.hass, self._entry, self._device_id,
                                     data, verify_chain)
        if _is_replay(verdict):
            # A stale, validly signed head: annotate, never move the length
            # or the hash the dashboard shows as current.
            self._attr_extra_state_attributes = {
                **(getattr(self, "_attr_extra_state_attributes", None) or {}),
                **_trust_attrs(self.hass, self._entry, self._device_id),
            }
            self.async_write_ha_state()
            return
        self._attr_native_value = data.get("length", data.get("chain_length", 0))
        self._attr_extra_state_attributes = {
            "latest_hash": data.get("latest_hash", ""),
            "algorithm": data.get("algorithm", "ed25519"),
            **_trust_attrs(self.hass, self._entry, self._device_id),
        }
        self.async_write_ha_state()


class SecuraCVCanaryLastEventSensor(SecuraCVCanarySensorBase):
    """Sensor for last witness event from a Canary device."""

    _attr_icon = "mdi:eye-outline"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Last Event", "last_event")

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT when added; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_EVENTS}",
                self._handle_message,
            )
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle event message."""
        try:
            data = parse_mqtt_json(msg.payload)
            if data is None:
                # Not a JSON object (bare scalar/list, junk, oversized):
                # degrade to the bounded raw-payload fallback rather than
                # AttributeError out of the @callback and stall the entity.
                raise TypeError("Event payload is not a JSON object")
            previous_value = getattr(self, "_attr_native_value", None)
            self._attr_native_value = data.get(
                "event_type", data.get("type", data.get("event", "unknown"))
            )
            # Two event dialects share the events topic: the CSI canary's
            # (event_id/state/category/...) and the radar witness's
            # canary-sense shape (event/seq/occupants/range). Dispatch on
            # the payload shape so each verifies against its own canonical —
            # the wrong verifier would mark a validly signed payload
            # "unsigned".
            if "event_id" not in data and "occupants" in data:
                verdict = _verify_and_record(self.hass, self._entry, self._device_id,
                                             data, verify_sense_event)
            else:
                verdict = _verify_and_record(self.hass, self._entry, self._device_id,
                                             data, verify_event)
            if _is_replay(verdict):
                # An older event re-sent with a valid signature: keep the
                # newer state we already hold and only annotate the trust view.
                self._attr_native_value = previous_value
                self._attr_extra_state_attributes = {
                    **(getattr(self, "_attr_extra_state_attributes", None) or {}),
                    **_trust_attrs(self.hass, self._entry, self._device_id),
                }
                self.async_write_ha_state()
                return
            # Mirror the event for the voice brief AFTER the verifier has
            # stamped its verdict, so a spoken answer can carry the trust
            # qualifier a forged/unsigned publish deserves (voice.py).
            _record_last_event(
                self.hass, self._entry, self._device_id, self._attr_native_value
            )
            attrs: dict[str, Any] = {
                "timestamp": data.get("timestamp", ""),
                "zone": data.get("zone", ""),
                "confidence": data.get("confidence", ""),
                "signed": data.get("signed", False),
                **_trust_attrs(self.hass, self._entry, self._device_id),
            }
            # Sensing-modality awareness (Phase 3): surface what *kind* of
            # sensor produced the claim so the timeline can show a glyph.
            # Derive from the event payload, falling back to the device's
            # advertised device_type. Only attach attributes when a modality
            # actually resolves, so pre-Phase-3 events render exactly as before.
            modality = modality_for(
                data,
                _device_type_for(self.hass, self._entry, self._device_id),
            )
            if modality != MODALITY_UNKNOWN:
                attrs["modality"] = modality
                meta = modality_metadata(modality)
                if meta:
                    attrs["modality_label"] = meta["label"]
            # Attestation provenance (Phase 3): default "device" so device-
            # signed events are unchanged; Track B payloads that mark
            # adapter/ha-bridged surface the weaker provenance for an honest
            # badge. Only emit the attribute when the payload is explicit.
            if data.get("attestation") is not None:
                attrs["attestation"] = normalize_attestation(data.get("attestation"))
            self._attr_extra_state_attributes = attrs
        except (json.JSONDecodeError, TypeError):
            # Slice BEFORE decoding so an oversized payload never costs a
            # full decode just to keep 255 characters of it.
            raw = msg.payload[:1024]
            payload = raw.decode(errors="ignore") if isinstance(raw, bytes) else str(raw)
            self._attr_native_value = payload[:255]
        self.async_write_ha_state()


class SecuraCVCanaryHealthSensor(SecuraCVCanarySensorBase):
    """Sensor for device health status from a Canary device."""

    _attr_icon = "mdi:heart-pulse"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Health", "health_status")

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT when added; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
                self._handle_message,
            )
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle health message."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            # Not a JSON object (array/number/junk/oversized): the helpers
            # below would crash on it, so report an honest "unknown".
            self._attr_native_value = "unknown"
            self.async_write_ha_state()
            return
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
            # Health is not signed: say so with the same slice the signed
            # entities carry, rather than moving with no verdict at all.
            **unsigned_trust_attrs(self.hass, self._entry, self._device_id),
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
        """Subscribe to MQTT when added; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
                self._handle_message,
            )
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle health message for SD endurance data."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
        wear = canary_sd_wear_pct(data)
        if wear is None and canary_sd(data) is None:
            # Firmware without SD reporting: leave the sensor untouched.
            return
        self._attr_native_value = wear
        attrs: dict[str, Any] = {}
        if (sd := canary_sd(data)) is not None:
            attrs = {
                "mounted": sd.get("mounted"),
                "usage_pct": sd.get("usage_pct"),
                "writes": sd.get("writes"),
                "errors": sd.get("errors"),
                "lifetime_kb": sd.get("lifetime_kb"),
                "replace_recommended": sd.get("replace_recommended"),
            }
        attrs.update(unsigned_trust_attrs(self.hass, self._entry, self._device_id))
        self._attr_extra_state_attributes = attrs
        self.async_write_ha_state()


class SecuraCVCanaryGPSSensor(SecuraCVCanarySensorBase):
    """Sensor for GPS fix status from a Canary device."""

    _attr_icon = "mdi:crosshairs-gps"

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "GPS Fix", "gps_fix")

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT when added; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
                self._handle_message,
            )
        )

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle health message for GPS data."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
        gps = data.get("gps", {})

        trust = unsigned_trust_attrs(self.hass, self._entry, self._device_id)
        if isinstance(gps, dict):
            self._attr_native_value = gps.get("fix_type", "no_fix")
            self._attr_extra_state_attributes = {
                "satellites": gps.get("satellites", 0),
                "hdop": gps.get("hdop", 0),
                "latitude": gps.get("latitude", ""),
                "longitude": gps.get("longitude", ""),
                **trust,
            }
        else:
            self._attr_native_value = str(gps) if gps else "no_fix"
            self._attr_extra_state_attributes = {
                **(getattr(self, "_attr_extra_state_attributes", None) or {}),
                **trust,
            }
        self.async_write_ha_state()


class SecuraCVCanaryRadarLinkSensor(SecuraCVCanarySensorBase):
    """Radar-link health diagnostic for canary-sense (MR60BHA2) devices.

    Surfaces the UART link between the ESP32-C6 host and the 60GHz radar
    module: the link state (ok / stale / down) plus the age of the last
    radar frame. A silent radar (UART timeout / frame CRC storm) is the
    canary-sense failure mode the firmware health log flags under
    HEALTH_CAT_SENSOR; this diagnostic gives the dashboard the same signal.

    Reads from the `radar` object of the health payload (Track A native
    firmware). Tolerant of older/partial payloads: stays empty until the
    device reports a radar object, so it never invents a state.

    Wire contract (firmware health payload, `radar` object):
        {
          "radar": {
            "link_ok":         bool,   # host<->radar UART healthy
            "last_frame_ms":   int,    # millis() of last good frame (device clock)
            "last_frame_age_ms": int,  # age of last good frame (preferred; ms)
            "frames":          int,    # total good frames since boot (optional)
            "frame_errors":    int,    # CRC/parse errors since boot (optional)
            "reboots":         int     # radar-module reboots detected (optional)
          }
        }
    """

    _attr_icon = "mdi:radar"
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    # Treat the link as stale (vs. down) past this frame age. The radar streams
    # presence frames continuously; a few seconds without one is the early
    # warning before a hard UART-timeout "down".
    STALE_FRAME_AGE_MS = 5000

    def __init__(self, prefix: str, device_id: str, entry: ConfigEntry) -> None:
        """Initialize."""
        super().__init__(prefix, device_id, entry, "Radar Link", "radar_link")

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT when added; release the subscription on removal."""
        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass,
                f"{self._prefix}/{self._device_id}/{TOPIC_HEALTH}",
                self._handle_message,
            )
        )

    @staticmethod
    def _frame_age_ms(radar: dict[str, Any]) -> int | None:
        """Last-frame age in ms: prefer an explicit age, else derive from
        the device-clock timestamps if both are present."""
        age = radar.get("last_frame_age_ms")
        if isinstance(age, (int, float)):
            return int(age)
        now = radar.get("now_ms")
        last = radar.get("last_frame_ms")
        if isinstance(now, (int, float)) and isinstance(last, (int, float)):
            # Wrap-safe signed delta, matching the firmware millis() idiom.
            delta = (int(now) - int(last)) & 0xFFFFFFFF
            if delta >= 0x80000000:
                delta -= 0x100000000
            return max(delta, 0)
        return None

    @classmethod
    def _link_state(cls, radar: dict[str, Any]) -> str:
        """down / stale / ok / unknown from the radar object.

        An explicit ``link_ok: false`` is a hard down (UART silent). Otherwise
        a frame older than STALE_FRAME_AGE_MS is "stale" (early warning); a
        recent frame or ``link_ok: true`` is "ok"; nothing to judge → unknown.
        """
        age_ms = cls._frame_age_ms(radar)
        link_ok = radar.get("link_ok")
        if link_ok is False:
            return "down"
        if age_ms is not None and age_ms > cls.STALE_FRAME_AGE_MS:
            return "stale"
        if link_ok is True or age_ms is not None:
            return "ok"
        return "unknown"

    @callback
    def _handle_message(self, msg: mqtt.ReceiveMessage) -> None:
        """Handle health message for radar-link data."""
        data = parse_mqtt_json(msg.payload)
        if data is None:
            return
        radar = data.get("radar")
        if not isinstance(radar, dict):
            # Firmware without radar-link reporting: leave the sensor untouched.
            return

        age_ms = self._frame_age_ms(radar)
        self._attr_native_value = self._link_state(radar)

        attrs: dict[str, Any] = {}
        if age_ms is not None:
            attrs["last_frame_age_ms"] = age_ms
        for key in ("frames", "frame_errors", "reboots"):
            if (val := radar.get(key)) is not None:
                attrs[key] = val
        attrs.update(unsigned_trust_attrs(self.hass, self._entry, self._device_id))
        self._attr_extra_state_attributes = attrs
        self.async_write_ha_state()
