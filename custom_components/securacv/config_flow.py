"""Config flow for SecuraCV integration."""
from __future__ import annotations

import logging
from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.const import CONF_TOKEN, CONF_URL
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResult
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .const import (
    DOMAIN,
    CONF_MQTT_PREFIX,
    CONF_ENABLE_MQTT,
    DEFAULT_KERNEL_URL,
    DEFAULT_MQTT_PREFIX,
)

_LOGGER = logging.getLogger(__name__)


class CannotConnect(Exception):
    """Error to indicate we cannot connect."""


class InvalidAuth(Exception):
    """Error to indicate there is invalid auth."""


async def _async_validate_kernel(hass: HomeAssistant, data: dict[str, Any]) -> None:
    """Validate the kernel connection."""
    # Import here to avoid circular dependency
    from . import SecuraCVApi, SecuraCVApiAuthError, SecuraCVApiError

    session = async_get_clientsession(hass)
    api = SecuraCVApi(data[CONF_URL], data[CONF_TOKEN], session)
    try:
        await api.async_get_events()
    except SecuraCVApiAuthError as err:
        raise InvalidAuth from err
    except SecuraCVApiError as err:
        raise CannotConnect from err


class SecuraCVConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Handle a config flow for SecuraCV."""

    VERSION = 1

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> FlowResult:
        """Handle the initial step - kernel connection."""
        errors: dict[str, str] = {}

        if user_input is not None:
            try:
                await _async_validate_kernel(self.hass, user_input)
            except CannotConnect:
                errors["base"] = "cannot_connect"
            except InvalidAuth:
                errors["base"] = "invalid_auth"
            else:
                # Set unique ID based on kernel URL
                await self.async_set_unique_id(user_input[CONF_URL])
                self._abort_if_unique_id_configured()

                # Build entry data
                entry_data = {
                    CONF_URL: user_input[CONF_URL],
                    CONF_TOKEN: user_input[CONF_TOKEN],
                    CONF_ENABLE_MQTT: user_input.get(CONF_ENABLE_MQTT, False),
                }

                # Add MQTT prefix if enabled
                if entry_data[CONF_ENABLE_MQTT]:
                    entry_data[CONF_MQTT_PREFIX] = user_input.get(
                        CONF_MQTT_PREFIX, DEFAULT_MQTT_PREFIX
                    )

                return self.async_create_entry(
                    title="SecuraCV",
                    data=entry_data,
                )

        data_schema = vol.Schema(
            {
                vol.Required(CONF_URL, default=DEFAULT_KERNEL_URL): str,
                vol.Required(CONF_TOKEN): str,
                vol.Optional(CONF_ENABLE_MQTT, default=False): bool,
                vol.Optional(CONF_MQTT_PREFIX, default=DEFAULT_MQTT_PREFIX): str,
            }
        )

        return self.async_show_form(
            step_id="user",
            data_schema=data_schema,
            errors=errors,
        )

    async def async_step_mqtt(
        self, discovery_info: dict[str, Any]
    ) -> FlowResult:
        """Handle MQTT auto-discovery.

        Triggered when HA sees a message on a topic matching
        the 'mqtt' key in manifest.json (securacv/#).
        """
        topic = discovery_info.get("topic", "")
        parts = topic.split("/")

        if len(parts) >= 2:
            prefix = parts[0]
            self.context["title_placeholders"] = {"name": f"SecuraCV ({prefix})"}
            # Store discovered prefix for use in user step
            self.context["mqtt_prefix"] = prefix

        # Show user step to get kernel URL/token
        return await self.async_step_user()
