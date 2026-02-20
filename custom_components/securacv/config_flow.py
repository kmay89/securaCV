"""Config flow for SecuraCV integration.

Supports three setup modes:
  A) Canary devices via MQTT (recommended for most users)
  B) Witness Kernel via HTTP API
  C) Both MQTT + HTTP Kernel
"""
from __future__ import annotations

import logging
from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.config_entries import ConfigFlowResult
from homeassistant.const import CONF_TOKEN, CONF_URL
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .const import (
    DOMAIN,
    CONF_MQTT_PREFIX,
    CONF_ENABLE_MQTT,
    CONF_SETUP_MODE,
    DEFAULT_KERNEL_URL,
    DEFAULT_MQTT_PREFIX,
    SETUP_MODE_MQTT,
    SETUP_MODE_KERNEL,
    SETUP_MODE_BOTH,
)

_LOGGER = logging.getLogger(__name__)


class CannotConnect(Exception):
    """Error to indicate we cannot connect."""


class InvalidAuth(Exception):
    """Error to indicate there is invalid auth."""


async def _async_validate_kernel(hass: HomeAssistant, data: dict[str, Any]) -> None:
    """Validate the kernel connection."""
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

    VERSION = 2

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Handle the initial step — choose setup mode."""
        if user_input is not None:
            mode = user_input.get(CONF_SETUP_MODE, SETUP_MODE_MQTT)
            if mode == SETUP_MODE_MQTT:
                return await self.async_step_mqtt_config()
            elif mode == SETUP_MODE_KERNEL:
                return await self.async_step_kernel()
            else:
                return await self.async_step_both()

        data_schema = vol.Schema(
            {
                vol.Required(CONF_SETUP_MODE, default=SETUP_MODE_MQTT): vol.In(
                    {
                        SETUP_MODE_MQTT: "Canary devices via MQTT (Recommended)",
                        SETUP_MODE_KERNEL: "Witness Kernel via HTTP API",
                        SETUP_MODE_BOTH: "Both MQTT + HTTP Kernel",
                    }
                ),
            }
        )

        return self.async_show_form(
            step_id="user",
            data_schema=data_schema,
        )

    async def async_step_mqtt_config(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Configure MQTT-only mode for Canary devices."""
        if user_input is not None:
            prefix = user_input.get(CONF_MQTT_PREFIX, DEFAULT_MQTT_PREFIX)
            await self.async_set_unique_id(f"mqtt_{prefix}")
            self._abort_if_unique_id_configured()

            return self.async_create_entry(
                title=f"SecuraCV ({prefix})",
                data={
                    CONF_ENABLE_MQTT: True,
                    CONF_MQTT_PREFIX: prefix,
                    CONF_SETUP_MODE: SETUP_MODE_MQTT,
                },
            )

        data_schema = vol.Schema(
            {
                vol.Optional(CONF_MQTT_PREFIX, default=DEFAULT_MQTT_PREFIX): str,
            }
        )

        return self.async_show_form(
            step_id="mqtt_config",
            data_schema=data_schema,
        )

    async def async_step_kernel(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Configure Witness Kernel HTTP API connection."""
        errors: dict[str, str] = {}

        if user_input is not None:
            try:
                await _async_validate_kernel(self.hass, user_input)
            except CannotConnect:
                errors["base"] = "cannot_connect"
            except InvalidAuth:
                errors["base"] = "invalid_auth"
            else:
                await self.async_set_unique_id(user_input[CONF_URL])
                self._abort_if_unique_id_configured()

                return self.async_create_entry(
                    title="SecuraCV",
                    data={
                        CONF_URL: user_input[CONF_URL],
                        CONF_TOKEN: user_input[CONF_TOKEN],
                        CONF_ENABLE_MQTT: False,
                        CONF_SETUP_MODE: SETUP_MODE_KERNEL,
                    },
                )

        data_schema = vol.Schema(
            {
                vol.Required(CONF_URL, default=DEFAULT_KERNEL_URL): str,
                vol.Required(CONF_TOKEN): str,
            }
        )

        return self.async_show_form(
            step_id="kernel",
            data_schema=data_schema,
            errors=errors,
        )

    async def async_step_both(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Configure both MQTT and Kernel HTTP API."""
        errors: dict[str, str] = {}

        if user_input is not None:
            try:
                await _async_validate_kernel(self.hass, user_input)
            except CannotConnect:
                errors["base"] = "cannot_connect"
            except InvalidAuth:
                errors["base"] = "invalid_auth"
            else:
                await self.async_set_unique_id(user_input[CONF_URL])
                self._abort_if_unique_id_configured()

                return self.async_create_entry(
                    title="SecuraCV",
                    data={
                        CONF_URL: user_input[CONF_URL],
                        CONF_TOKEN: user_input[CONF_TOKEN],
                        CONF_ENABLE_MQTT: True,
                        CONF_MQTT_PREFIX: user_input.get(
                            CONF_MQTT_PREFIX, DEFAULT_MQTT_PREFIX
                        ),
                        CONF_SETUP_MODE: SETUP_MODE_BOTH,
                    },
                )

        data_schema = vol.Schema(
            {
                vol.Required(CONF_URL, default=DEFAULT_KERNEL_URL): str,
                vol.Required(CONF_TOKEN): str,
                vol.Optional(CONF_MQTT_PREFIX, default=DEFAULT_MQTT_PREFIX): str,
            }
        )

        return self.async_show_form(
            step_id="both",
            data_schema=data_schema,
            errors=errors,
        )

    async def async_step_mqtt(
        self, discovery_info: dict[str, Any]
    ) -> ConfigFlowResult:
        """Handle MQTT auto-discovery.

        Triggered when HA sees a message on a topic matching
        the 'mqtt' key in manifest.json (securacv/#).
        """
        topic = discovery_info.get("topic", "")
        parts = topic.split("/")

        if len(parts) >= 2:
            prefix = parts[0]
            self.context["title_placeholders"] = {"name": f"SecuraCV ({prefix})"}
            self.context["mqtt_prefix"] = prefix

        return await self.async_step_mqtt_config()
