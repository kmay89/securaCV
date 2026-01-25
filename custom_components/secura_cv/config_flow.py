"""Config flow for SecuraCV."""
from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.const import CONF_TOKEN, CONF_URL
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from . import DOMAIN, SecuraCvApi, SecuraCvApiAuthError, SecuraCvApiError

DEFAULT_URL = "http://privacy_witness_kernel:8799"


class CannotConnect(Exception):
    """Error to indicate we cannot connect."""


class InvalidAuth(Exception):
    """Error to indicate there is invalid auth."""


async def _async_validate_input(hass: HomeAssistant, data: dict[str, Any]) -> None:
    session = async_get_clientsession(hass)
    api = SecuraCvApi(data[CONF_URL], data[CONF_TOKEN], session)
    try:
        await api.async_get_events()
    except SecuraCvApiAuthError as err:
        raise InvalidAuth from err
    except SecuraCvApiError as err:
        raise CannotConnect from err


class SecuraCvConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Handle a config flow for SecuraCV."""

    VERSION = 1

    async def async_step_user(self, user_input: dict[str, Any] | None = None):
        errors: dict[str, str] = {}

        if user_input is not None:
            try:
                await _async_validate_input(self.hass, user_input)
            except CannotConnect:
                errors["base"] = "cannot_connect"
            except InvalidAuth:
                errors["base"] = "invalid_auth"
            else:
                await self.async_set_unique_id(user_input[CONF_URL])
                self._abort_if_unique_id_configured()
                return self.async_create_entry(title="SecuraCV", data=user_input)

        data_schema = vol.Schema(
            {
                vol.Required(CONF_URL, default=DEFAULT_URL): str,
                vol.Required(CONF_TOKEN): str,
            }
        )

        return self.async_show_form(
            step_id="user",
            data_schema=data_schema,
            errors=errors,
        )
