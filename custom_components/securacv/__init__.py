"""SecuraCV - Privacy Witness Kernel integration for Home Assistant.

Integrates SecuraCV Canary devices via MQTT. Surfaces semantic witness events,
hash chain integrity, and device health - never raw video or identity data.
"""
from __future__ import annotations

import logging
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant, callback
from homeassistant.components import mqtt
from homeassistant.helpers import device_registry as dr

from .const import (
    DOMAIN,
    CONF_MQTT_PREFIX,
    TOPIC_STATUS,
    MANUFACTURER,
    MODEL_CANARY,
)

_LOGGER = logging.getLogger(__name__)

PLATFORMS: list[Platform] = [Platform.SENSOR, Platform.BINARY_SENSOR]


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up SecuraCV from a config entry."""
    hass.data.setdefault(DOMAIN, {})

    mqtt_prefix = entry.data.get(CONF_MQTT_PREFIX, "securacv")

    # Wait for MQTT client to be available
    await mqtt.async_wait_for_mqtt_client(hass)

    # Store config and runtime data for platforms
    entry_data: dict[str, Any] = {
        "config": entry.data,
        "mqtt_prefix": mqtt_prefix,
        "devices": {},
        "unsub_mqtt": [],
    }
    hass.data[DOMAIN][entry.entry_id] = entry_data

    # Subscribe to device status to auto-register devices
    unsub = await mqtt.async_subscribe(
        hass,
        f"{mqtt_prefix}/+/{TOPIC_STATUS}",
        _async_device_status_received(hass, entry),
    )
    entry_data["unsub_mqtt"].append(unsub)

    # Forward entry setup to sensor and binary_sensor platforms
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    _LOGGER.info(
        "SecuraCV integration loaded (prefix: %s, platforms: %s)",
        mqtt_prefix,
        [p.value for p in PLATFORMS],
    )

    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a SecuraCV config entry."""
    entry_data = hass.data[DOMAIN].get(entry.entry_id, {})

    # Unsubscribe from MQTT topics
    for unsub in entry_data.get("unsub_mqtt", []):
        unsub()

    # Unload platforms
    unload_ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)

    if unload_ok:
        hass.data[DOMAIN].pop(entry.entry_id, None)
        _LOGGER.info("SecuraCV integration unloaded")

    return unload_ok


def _async_device_status_received(
    hass: HomeAssistant, entry: ConfigEntry
):
    """Return callback for device status MQTT messages."""

    @callback
    def _callback(msg: mqtt.ReceiveMessage) -> None:
        """Handle device status message - register device in HA."""
        # Topic format: {prefix}/{device_id}/status
        parts = msg.topic.split("/")
        if len(parts) < 3:
            return

        device_id = parts[-2]
        entry_data = hass.data[DOMAIN].get(entry.entry_id, {})
        devices = entry_data.get("devices", {})

        if device_id not in devices:
            _LOGGER.info("Discovered SecuraCV Canary device: %s", device_id)
            devices[device_id] = {"status": msg.payload}

            # Register in device registry
            dev_registry = dr.async_get(hass)
            dev_registry.async_get_or_create(
                config_entry_id=entry.entry_id,
                identifiers={(DOMAIN, device_id)},
                manufacturer=MANUFACTURER,
                model=MODEL_CANARY,
                name=f"SecuraCV Canary {device_id}",
            )
        else:
            devices[device_id]["status"] = msg.payload

    return _callback
