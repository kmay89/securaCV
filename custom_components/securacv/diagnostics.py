"""Diagnostics support for SecuraCV integration."""
from __future__ import annotations

from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant

from .const import DOMAIN


async def async_get_config_entry_diagnostics(
    hass: HomeAssistant, entry: ConfigEntry
) -> dict[str, Any]:
    """Return diagnostics for a config entry."""
    entry_data = hass.data.get(DOMAIN, {}).get(entry.entry_id, {})

    devices_info = {}
    for device_id, device_data in entry_data.get("devices", {}).items():
        devices_info[device_id] = {
            "status": device_data.get("status", "unknown"),
        }

    return {
        "config": {
            "mqtt_prefix": entry_data.get("mqtt_prefix", "unknown"),
        },
        "devices": devices_info,
        "device_count": len(devices_info),
        "platforms_loaded": ["sensor", "binary_sensor"],
    }
