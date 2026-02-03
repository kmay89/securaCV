"""SecuraCV - Privacy Witness Kernel integration for Home Assistant.

Connects to the SecuraCV Privacy Witness Kernel via HTTP API. Optionally
subscribes to MQTT for real-time updates from Canary devices.

Surfaces semantic witness events, hash chain integrity, and device health -
never raw video or identity data. Privacy by design.
"""
from __future__ import annotations

import asyncio
import logging
from datetime import timedelta
from typing import Any

import aiohttp

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_TOKEN, CONF_URL, Platform
from homeassistant.core import HomeAssistant, callback
from homeassistant.components import mqtt
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed
from homeassistant.helpers import device_registry as dr

from .const import (
    DOMAIN,
    CONF_MQTT_PREFIX,
    CONF_ENABLE_MQTT,
    TOPIC_STATUS,
    MANUFACTURER,
    MODEL_KERNEL,
    MODEL_CANARY,
)

_LOGGER = logging.getLogger(__name__)

PLATFORMS: list[Platform] = [Platform.SENSOR, Platform.BINARY_SENSOR]
DEFAULT_UPDATE_INTERVAL = timedelta(seconds=30)


class SecuraCVApiError(Exception):
    """Base error for the SecuraCV API client."""


class SecuraCVApiAuthError(SecuraCVApiError):
    """Error for authentication failures."""


class SecuraCVApiConnectionError(SecuraCVApiError):
    """Error for connectivity failures."""


class SecuraCVApiResponseError(SecuraCVApiError):
    """Error for unexpected API responses."""


class SecuraCVApi:
    """API client for the SecuraCV Privacy Witness Kernel Event API."""

    def __init__(
        self, base_url: str, token: str, session: aiohttp.ClientSession
    ) -> None:
        """Initialize the API client."""
        self._base_url = base_url.rstrip("/")
        self._token = token
        self._session = session

    async def async_get_events(self) -> dict[str, Any]:
        """Fetch events from the kernel Event API."""
        url = f"{self._base_url}/events"
        headers = {"Authorization": f"Bearer {self._token}"}
        try:
            async with self._session.get(url, headers=headers, timeout=10) as resp:
                if resp.status == 401:
                    raise SecuraCVApiAuthError("unauthorized")
                if resp.status != 200:
                    raise SecuraCVApiResponseError(
                        f"unexpected status: {resp.status}"
                    )
                try:
                    return await resp.json()
                except aiohttp.ContentTypeError as err:
                    raise SecuraCVApiResponseError(
                        f"invalid JSON response: {err}"
                    ) from err
        except (aiohttp.ClientError, asyncio.TimeoutError) as err:
            raise SecuraCVApiConnectionError("unable to reach API") from err

    async def async_get_latest_event(self) -> dict[str, Any] | None:
        """Fetch the latest event from the kernel."""
        url = f"{self._base_url}/events/latest"
        headers = {"Authorization": f"Bearer {self._token}"}
        try:
            async with self._session.get(url, headers=headers, timeout=10) as resp:
                if resp.status == 401:
                    raise SecuraCVApiAuthError("unauthorized")
                if resp.status == 404:
                    return None  # No events yet
                if resp.status != 200:
                    raise SecuraCVApiResponseError(
                        f"unexpected status: {resp.status}"
                    )
                try:
                    return await resp.json()
                except aiohttp.ContentTypeError as err:
                    raise SecuraCVApiResponseError(
                        f"invalid JSON response: {err}"
                    ) from err
        except (aiohttp.ClientError, asyncio.TimeoutError) as err:
            raise SecuraCVApiConnectionError("unable to reach API") from err

    async def async_get_health(self) -> dict[str, Any]:
        """Check kernel health status."""
        url = f"{self._base_url}/health"
        try:
            async with self._session.get(url, timeout=5) as resp:
                if resp.status != 200:
                    return {"status": "error", "code": resp.status}
                return await resp.json()
        except (aiohttp.ClientError, asyncio.TimeoutError):
            return {"status": "offline"}


def _select_latest_event(payload: dict[str, Any]) -> dict[str, Any] | None:
    """Extract the latest event from a batched export payload."""
    all_events = (
        event
        for batch in payload.get("batches", [])
        for bucket in batch.get("buckets", [])
        for event in bucket.get("events", [])
    )

    def _start_epoch(event: dict[str, Any]) -> int:
        start = event.get("time_bucket", {}).get("start_epoch_s")
        return start if isinstance(start, int) else -1

    return max(all_events, key=_start_epoch, default=None)


class SecuraCVCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Coordinator for SecuraCV data updates via HTTP API."""

    def __init__(self, hass: HomeAssistant, api: SecuraCVApi) -> None:
        """Initialize the coordinator."""
        super().__init__(
            hass,
            logger=_LOGGER,
            name="SecuraCV",
            update_interval=DEFAULT_UPDATE_INTERVAL,
        )
        self.api = api

    async def _async_update_data(self) -> dict[str, Any]:
        """Fetch data from the kernel API."""
        try:
            payload = await self.api.async_get_events()
        except SecuraCVApiError as err:
            raise UpdateFailed(str(err)) from err
        return {"latest_event": _select_latest_event(payload)}


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up SecuraCV from a config entry."""
    hass.data.setdefault(DOMAIN, {})

    # Initialize HTTP API client for kernel connection
    session = async_get_clientsession(hass)
    api = SecuraCVApi(entry.data[CONF_URL], entry.data[CONF_TOKEN], session)
    coordinator = SecuraCVCoordinator(hass, api)

    # Fetch initial data
    await coordinator.async_config_entry_first_refresh()

    # Store entry data
    entry_data: dict[str, Any] = {
        "api": api,
        "coordinator": coordinator,
        "devices": {},
        "unsub_mqtt": [],
    }
    hass.data[DOMAIN][entry.entry_id] = entry_data

    # Register the kernel as a device
    dev_registry = dr.async_get(hass)
    dev_registry.async_get_or_create(
        config_entry_id=entry.entry_id,
        identifiers={(DOMAIN, entry.data[CONF_URL])},
        manufacturer=MANUFACTURER,
        model=MODEL_KERNEL,
        name="SecuraCV Privacy Witness Kernel",
        configuration_url=entry.data[CONF_URL],
    )

    # Optionally set up MQTT subscriptions for Canary devices
    mqtt_prefix = entry.data.get(CONF_MQTT_PREFIX)
    enable_mqtt = entry.data.get(CONF_ENABLE_MQTT, False)

    if enable_mqtt and mqtt_prefix:
        try:
            await mqtt.async_wait_for_mqtt_client(hass)
            unsub = await mqtt.async_subscribe(
                hass,
                f"{mqtt_prefix}/+/{TOPIC_STATUS}",
                _async_device_status_received(hass, entry),
            )
            entry_data["unsub_mqtt"].append(unsub)
            entry_data["mqtt_prefix"] = mqtt_prefix
            _LOGGER.info("SecuraCV MQTT subscriptions active (prefix: %s)", mqtt_prefix)
        except Exception as err:
            _LOGGER.warning("MQTT setup failed, continuing without MQTT: %s", err)

    # Forward to platforms
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    _LOGGER.info(
        "SecuraCV integration loaded (kernel: %s, mqtt: %s)",
        entry.data[CONF_URL],
        "enabled" if enable_mqtt else "disabled",
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


def _async_device_status_received(hass: HomeAssistant, entry: ConfigEntry):
    """Return callback for Canary device status MQTT messages."""

    @callback
    def _callback(msg: mqtt.ReceiveMessage) -> None:
        """Handle device status message - register Canary device in HA."""
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

            # Register Canary in device registry
            dev_registry = dr.async_get(hass)
            dev_registry.async_get_or_create(
                config_entry_id=entry.entry_id,
                identifiers={(DOMAIN, f"canary_{device_id}")},
                manufacturer=MANUFACTURER,
                model=MODEL_CANARY,
                name=f"SecuraCV Canary {device_id}",
            )
        else:
            devices[device_id]["status"] = msg.payload

    return _callback
