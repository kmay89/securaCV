"""Diagnostics support for SecuraCV integration."""
from __future__ import annotations

from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_URL
from homeassistant.core import HomeAssistant

from .const import DOMAIN, CONF_MQTT_PREFIX, CONF_ENABLE_MQTT


async def async_get_config_entry_diagnostics(
    hass: HomeAssistant, entry: ConfigEntry
) -> dict[str, Any]:
    """Return diagnostics for a config entry."""
    entry_data = hass.data.get(DOMAIN, {}).get(entry.entry_id, {})

    # Kernel info
    coordinator = entry_data.get("coordinator")
    kernel_info = {
        "url": entry.data.get(CONF_URL, "unknown"),
        "last_update_success": coordinator.last_update_success if coordinator else None,
        "latest_event": coordinator.data.get("latest_event") if coordinator else None,
    }

    # Canary devices (MQTT)
    devices_info = {}
    for device_id, device_data in entry_data.get("devices", {}).items():
        devices_info[device_id] = {
            "status": device_data.get("status", "unknown"),
        }

    # MQTT configuration
    mqtt_info = {
        "enabled": entry.data.get(CONF_ENABLE_MQTT, False),
        "prefix": entry_data.get("mqtt_prefix", entry.data.get(CONF_MQTT_PREFIX)),
    }

    return {
        "kernel": kernel_info,
        "mqtt": mqtt_info,
        "canary_devices": devices_info,
        "canary_device_count": len(devices_info),
        "platforms_loaded": ["sensor", "binary_sensor"],
    }
