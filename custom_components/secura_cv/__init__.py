"""SecuraCV integration."""
from __future__ import annotations

import asyncio
import logging
from datetime import timedelta
from typing import Any

import aiohttp

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_TOKEN, CONF_URL, Platform
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

DOMAIN = "secura_cv"
PLATFORMS: list[Platform] = [Platform.SENSOR]
DEFAULT_UPDATE_INTERVAL = timedelta(seconds=30)


class SecuraCvApiError(Exception):
    """Base error for the SecuraCV API client."""


class SecuraCvApiAuthError(SecuraCvApiError):
    """Error for authentication failures."""


class SecuraCvApiConnectionError(SecuraCvApiError):
    """Error for connectivity failures."""


class SecuraCvApiResponseError(SecuraCvApiError):
    """Error for unexpected API responses."""


class SecuraCvApi:
    """API client for the SecuraCV event API."""

    def __init__(
        self, base_url: str, token: str, session: "aiohttp.client.ClientSession"
    ) -> None:
        self._base_url = base_url.rstrip("/")
        self._token = token
        self._session = session

    async def async_get_events(self) -> dict[str, Any]:
        url = f"{self._base_url}/events"
        headers = {"Authorization": f"Bearer {self._token}"}
        try:
            async with self._session.get(url, headers=headers, timeout=10) as resp:
                if resp.status == 401:
                    raise SecuraCvApiAuthError("unauthorized")
                if resp.status != 200:
                    raise SecuraCvApiResponseError(
                        f"unexpected status: {resp.status}"
                    )
                try:
                    return await resp.json()
                except aiohttp.ContentTypeError as err:
                    raise SecuraCvApiResponseError(
                        f"invalid JSON response: {err}"
                    ) from err
        except (aiohttp.ClientError, asyncio.TimeoutError) as err:
            raise SecuraCvApiConnectionError("unable to reach API") from err


def _select_latest_event(payload: dict[str, Any]) -> dict[str, Any] | None:
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
        try:
            payload = await self._api.async_get_events()
        except SecuraCvApiError as err:
            raise UpdateFailed(str(err)) from err
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
