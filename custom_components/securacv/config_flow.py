"""Config flow for SecuraCV integration."""
from __future__ import annotations

import logging
from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.components.mqtt import valid_subscribe_topic
from homeassistant.data_entry_flow import FlowResult

from .const import DOMAIN, CONF_MQTT_PREFIX

_LOGGER = logging.getLogger(__name__)

STEP_USER_DATA_SCHEMA = vol.Schema(
    {
        vol.Required(CONF_MQTT_PREFIX, default="securacv"): str,
    }
)


class SecuraCVConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Handle a config flow for SecuraCV."""

    VERSION = 1

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> FlowResult:
        """Handle the initial step - user provides MQTT prefix."""
        errors: dict[str, str] = {}

        if user_input is not None:
            prefix = user_input[CONF_MQTT_PREFIX].strip().rstrip("/")

            # Validate MQTT topic prefix
            try:
                valid_subscribe_topic(f"{prefix}/#")
            except vol.Invalid:
                errors["base"] = "invalid_mqtt_prefix"
            else:
                # Prevent duplicate entries with same prefix
                await self.async_set_unique_id(prefix)
                self._abort_if_unique_id_configured()

                return self.async_create_entry(
                    title=f"SecuraCV ({prefix})",
                    data={CONF_MQTT_PREFIX: prefix},
                )

        return self.async_show_form(
            step_id="user",
            data_schema=STEP_USER_DATA_SCHEMA,
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
            await self.async_set_unique_id(prefix)
            self._abort_if_unique_id_configured()
            self.context["title_placeholders"] = {"name": f"SecuraCV ({prefix})"}

        # Fall through to user step for confirmation
        return await self.async_step_user()
