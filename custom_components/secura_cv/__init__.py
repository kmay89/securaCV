"""SecuraCV integration."""
from __future__ import annotations

from datetime import timedelta
from typing import Any

import logging

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_TOKEN, CONF_URL, Platform
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

DOMAIN = "secura_cv"
PLATFORMS: list[Platform] = [Platform.SENSOR]
DEFAULT_UPDATE_INTERVAL = timedelta(seconds=30)


class SecuraCvApi:
    """API client for the SecuraCV event API."""

    def __init__(self, base_url: str, token: str, session) -> None:
        self._base_url = base_url.rstrip("/")
        self._token = token
        self._session = session

    async def async_get_events(self) -> dict[str, Any]:
        url = f"{self._base_url}/events"
        headers = {"Authorization": f"Bearer {self._token}"}
        async with self._session.get(url, headers=headers, timeout=10) as resp:
            if resp.status == 401:
                raise UpdateFailed("unauthorized")
            if resp.status != 200:
                raise UpdateFailed(f"unexpected status: {resp.status}")
            return await resp.json()


def _select_latest_event(payload: dict[str, Any]) -> dict[str, Any] | None:
    batches = payload.get("batches", [])
    latest_event: dict[str, Any] | None = None
    latest_start = -1
    for batch in batches:
        for bucket in batch.get("buckets", []):
            for event in bucket.get("events", []):
                time_bucket = event.get("time_bucket", {})
                start = time_bucket.get("start_epoch_s")
                if isinstance(start, int) and start >= latest_start:
                    latest_start = start
                    latest_event = event
                elif latest_event is None:
                    latest_event = event
    return latest_event


class SecuraCvCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Coordinator for SecuraCV data updates."""

    def __init__(self, hass: HomeAssistant, api: SecuraCvApi) -> None:
        super().__init__(
            hass,
            logger=logging.getLogger(__name__),
            name="SecuraCV",
            update_interval=DEFAULT_UPDATE_INTERVAL,
        )
        self._api = api

    async def _async_update_data(self) -> dict[str, Any]:
        payload = await self._api.async_get_events()
        return {"latest_event": _select_latest_event(payload)}


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up SecuraCV from a config entry."""
    session = async_get_clientsession(hass)
    api = SecuraCvApi(entry.data[CONF_URL], entry.data[CONF_TOKEN], session)
    coordinator = SecuraCvCoordinator(hass, api)
    await coordinator.async_config_entry_first_refresh()

    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    unload_ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unload_ok:
        hass.data[DOMAIN].pop(entry.entry_id, None)
    return unload_ok
