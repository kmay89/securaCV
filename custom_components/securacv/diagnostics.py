"""Diagnostics support for SecuraCV integration."""
from __future__ import annotations

from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_URL
from homeassistant.core import HomeAssistant

from .const import DOMAIN, CONF_MQTT_PREFIX, CONF_ENABLE_MQTT, CONF_SETUP_MODE


async def async_get_config_entry_diagnostics(
    hass: HomeAssistant, entry: ConfigEntry
) -> dict[str, Any]:
    """Return diagnostics for a config entry."""
    entry_data = hass.data.get(DOMAIN, {}).get(entry.entry_id, {})

    coordinator = entry_data.get("coordinator")
    kernel_info: dict[str, Any] = {
        "url": entry.data.get(CONF_URL, "(not configured)"),
        "last_update_success": coordinator.last_update_success if coordinator else None,
        "latest_event": coordinator.data.get("latest_event") if coordinator and coordinator.data else None,
    }

    devices_info: dict[str, Any] = {}
    for device_id, device_data in entry_data.get("devices", {}).items():
        devices_info[device_id] = {
            "status": device_data.get("status", "unknown"),
        }

    trust_store = entry_data.get("trust_store")
    trust_info: dict[str, Any] = {}
    if trust_store:
        for device_id in devices_info:
            entry_trust = trust_store.get(device_id)
            if entry_trust:
                trust_info[device_id] = {
                    "pinned": bool(entry_trust.fingerprint_hex),
                    "pin_source": getattr(entry_trust, "pin_source", "unknown"),
                    # The full fingerprint is exactly 16 hex chars
                    # (device_trust: sha256 digest[:8].hex()), so truncate
                    # to half of it — diagnostics dumps get shared publicly,
                    # and 8 chars is plenty to correlate against /enroll
                    # without republishing the whole identifier.
                    "fingerprint": entry_trust.fingerprint_hex[:8] + "…" if entry_trust.fingerprint_hex else None,
                }

    verify_info: dict[str, Any] = {}
    for device_id, verdict in entry_data.get("verify", {}).items():
        verify_info[device_id] = {
            "trusted": verdict.get("trusted", False),
            "reason": verdict.get("reason", "unknown"),
        }

    mqtt_info = {
        "enabled": entry.data.get(CONF_ENABLE_MQTT, False),
        "prefix": entry_data.get("mqtt_prefix", entry.data.get(CONF_MQTT_PREFIX)),
        "subscription_count": len(entry_data.get("unsub_mqtt", [])),
    }

    return {
        "setup_mode": entry.data.get(CONF_SETUP_MODE, "unknown"),
        "kernel": kernel_info,
        "mqtt": mqtt_info,
        "canary_devices": devices_info,
        "canary_device_count": len(devices_info),
        "trust": trust_info,
        "verification": verify_info,
        "mismatch_notifications_sent": len(entry_data.get("mismatch_notified", set())),
        "platforms_loaded": ["sensor", "binary_sensor"],
    }
