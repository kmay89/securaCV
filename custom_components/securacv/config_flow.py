"""Config flow for SecuraCV integration.

Supports three setup modes:
  A) Canary devices via MQTT (recommended for most users)
  B) Witness Kernel via HTTP API
  C) Both MQTT + HTTP Kernel
"""
from __future__ import annotations

import logging
from typing import TYPE_CHECKING, Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.config_entries import ConfigEntry, ConfigFlowResult, OptionsFlow
from homeassistant.const import CONF_TOKEN, CONF_URL
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.aiohttp_client import async_get_clientsession

if TYPE_CHECKING:
    from homeassistant.helpers.service_info.mqtt import MqttServiceInfo

from .const import (
    DOMAIN,
    CONF_MQTT_PREFIX,
    CONF_ENABLE_MQTT,
    CONF_SETUP_MODE,
    CONF_ADAPTER_STATS_URL,
    CONF_TOKEN_FILE,
    DEFAULT_KERNEL_URL,
    DEFAULT_MQTT_PREFIX,
    DEFAULT_TOKEN_FILE,
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
    api = SecuraCVApi(
        data[CONF_URL],
        data.get(CONF_TOKEN, ""),
        session,
        token_file=data.get(CONF_TOKEN_FILE),
    )
    try:
        await api.async_get_events()
    except SecuraCVApiAuthError as err:
        raise InvalidAuth from err
    except SecuraCVApiError as err:
        raise CannotConnect from err


def _kernel_auth_errors(user_input: dict[str, Any]) -> dict[str, str]:
    """Require at least one of static token / token file.

    The kernel rotates its capability token every 10 minutes, so the token
    file (which the add-on rewrites at /config/api_token) is the resilient
    choice; a static token alone is only useful for remote kernels whose
    token file HA cannot read.
    """
    if not user_input.get(CONF_TOKEN) and not user_input.get(CONF_TOKEN_FILE):
        return {"base": "token_required"}
    return {}


class SecuraCVConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Handle a config flow for SecuraCV."""

    VERSION = 2

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        """Return the OptionsFlow handler for this entry."""
        return SecuraCVOptionsFlow(config_entry)

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

        # Pre-fill with the prefix seen by MQTT discovery, if any.
        default_prefix = self.context.get("mqtt_prefix", DEFAULT_MQTT_PREFIX)
        data_schema = vol.Schema(
            {
                vol.Optional(CONF_MQTT_PREFIX, default=default_prefix): str,
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
            errors = _kernel_auth_errors(user_input)
            if not errors:
                try:
                    await _async_validate_kernel(self.hass, user_input)
                except CannotConnect:
                    errors["base"] = "cannot_connect"
                except InvalidAuth:
                    errors["base"] = "invalid_auth"
            if not errors:
                await self.async_set_unique_id(user_input[CONF_URL])
                self._abort_if_unique_id_configured()

                data = {
                    CONF_URL: user_input[CONF_URL],
                    CONF_TOKEN: user_input.get(CONF_TOKEN, ""),
                    CONF_ENABLE_MQTT: False,
                    CONF_SETUP_MODE: SETUP_MODE_KERNEL,
                }
                if user_input.get(CONF_TOKEN_FILE):
                    data[CONF_TOKEN_FILE] = user_input[CONF_TOKEN_FILE]
                if user_input.get(CONF_ADAPTER_STATS_URL):
                    data[CONF_ADAPTER_STATS_URL] = user_input[CONF_ADAPTER_STATS_URL]
                return self.async_create_entry(title="SecuraCV", data=data)

        data_schema = vol.Schema(
            {
                vol.Required(CONF_URL, default=DEFAULT_KERNEL_URL): str,
                vol.Optional(CONF_TOKEN_FILE, default=DEFAULT_TOKEN_FILE): str,
                vol.Optional(CONF_TOKEN): str,
                vol.Optional(CONF_ADAPTER_STATS_URL): str,
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
            errors = _kernel_auth_errors(user_input)
            if not errors:
                try:
                    await _async_validate_kernel(self.hass, user_input)
                except CannotConnect:
                    errors["base"] = "cannot_connect"
                except InvalidAuth:
                    errors["base"] = "invalid_auth"
            if not errors:
                await self.async_set_unique_id(user_input[CONF_URL])
                self._abort_if_unique_id_configured()

                data = {
                    CONF_URL: user_input[CONF_URL],
                    CONF_TOKEN: user_input.get(CONF_TOKEN, ""),
                    CONF_ENABLE_MQTT: True,
                    CONF_MQTT_PREFIX: user_input.get(
                        CONF_MQTT_PREFIX, DEFAULT_MQTT_PREFIX
                    ),
                    CONF_SETUP_MODE: SETUP_MODE_BOTH,
                }
                if user_input.get(CONF_TOKEN_FILE):
                    data[CONF_TOKEN_FILE] = user_input[CONF_TOKEN_FILE]
                if user_input.get(CONF_ADAPTER_STATS_URL):
                    data[CONF_ADAPTER_STATS_URL] = user_input[CONF_ADAPTER_STATS_URL]
                return self.async_create_entry(title="SecuraCV", data=data)

        data_schema = vol.Schema(
            {
                vol.Required(CONF_URL, default=DEFAULT_KERNEL_URL): str,
                vol.Optional(CONF_TOKEN_FILE, default=DEFAULT_TOKEN_FILE): str,
                vol.Optional(CONF_TOKEN): str,
                vol.Optional(CONF_MQTT_PREFIX, default=DEFAULT_MQTT_PREFIX): str,
                vol.Optional(CONF_ADAPTER_STATS_URL): str,
            }
        )

        return self.async_show_form(
            step_id="both",
            data_schema=data_schema,
            errors=errors,
        )

    async def async_step_mqtt(
        self, discovery_info: MqttServiceInfo
    ) -> ConfigFlowResult:
        """Handle MQTT auto-discovery.

        Triggered when HA sees a message on a topic matching the 'mqtt' key
        in manifest.json (securacv/#). Since HA 2022.6 the flow receives an
        MqttServiceInfo — a slots dataclass with NO dict access, so the
        topic must be read as an attribute; ``.get()`` raises
        AttributeError here and kills the advertised discovery prompt.
        """
        topic = discovery_info.topic or ""
        parts = topic.split("/")

        if len(parts) >= 2:
            prefix = parts[0]
            self.context["title_placeholders"] = {"name": f"SecuraCV ({prefix})"}
            self.context["mqtt_prefix"] = prefix

        return await self.async_step_mqtt_config()


# =============================================================================
# Options flow — device PKI management (pin / rotate / unpin)
# =============================================================================
#
# The "Configure" button on the integration card opens this flow.
# It surfaces a menu of trust-store actions; each action drills into a
# dedicated step that takes the device_id + pubkey hex needed to apply
# it. Storage round-trips happen through TrustStore.async_pin / rotate
# / unpin — the flow is just the UX layer.
#
# We deliberately don't try to fetch /api/device/enroll from inside
# the options flow: the device's IP isn't necessarily known to HA, and
# the captive-portal page exists precisely so an installer can read
# the fingerprint off any phone with WiFi reach. The pubkey hex is
# pasted verbatim from /enroll.


CONF_PIN_DEVICE_ID = "device_id"
CONF_PIN_PUBKEY_HEX = "pubkey_hex"

PIN_ACTION_PIN = "pin"
PIN_ACTION_ROTATE = "rotate"
PIN_ACTION_UNPIN = "unpin"


def _looks_like_pubkey_hex(value: str) -> bool:
    """64-char lowercase hex. We lowercase before checking so users
    can paste from any case-preserving source (`/enroll` emits
    lowercase but a manual transcription often comes back uppercase)."""
    v = value.strip().lower()
    if len(v) != 64:
        return False
    try:
        bytes.fromhex(v)
        return True
    except ValueError:
        return False


class SecuraCVOptionsFlow(OptionsFlow):
    """Per-entry trust-store management surface."""

    def __init__(self, config_entry: ConfigEntry) -> None:
        # `_config_entry` matches HA's recommended naming so HA core
        # internals can introspect it. The deprecated `self.config_entry`
        # alias is still set by the parent class.
        self._config_entry = config_entry

    async def async_step_init(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Top-level menu — pin / rotate / unpin a device key."""
        return self.async_show_menu(
            step_id="init",
            menu_options=[PIN_ACTION_PIN, PIN_ACTION_ROTATE, PIN_ACTION_UNPIN],
        )

    async def async_step_pin(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Pin a device's pubkey for the first time, or replace an
        existing TOFU pin with a manual one."""
        errors: dict[str, str] = {}
        if user_input is not None:
            device_id = user_input[CONF_PIN_DEVICE_ID].strip()
            pubkey_hex = user_input[CONF_PIN_PUBKEY_HEX].strip().lower()
            if not device_id:
                errors["device_id"] = "invalid_device_id"
            elif not _looks_like_pubkey_hex(pubkey_hex):
                errors["pubkey_hex"] = "invalid_pubkey_hex"
            else:
                ts = self._trust_store()
                if ts is None:
                    errors["base"] = "trust_store_unavailable"
                else:
                    from .device_trust import PIN_SOURCE_MANUAL
                    await ts.async_pin(device_id, pubkey_hex,
                                       source=PIN_SOURCE_MANUAL)
                    return self.async_create_entry(title="", data={})

        return self.async_show_form(
            step_id="pin",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_PIN_DEVICE_ID): str,
                    vol.Required(CONF_PIN_PUBKEY_HEX): str,
                }
            ),
            errors=errors,
        )

    async def async_step_rotate(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Replace an existing pin with a new pubkey. Same form as
        `pin` but records the action with source=rotation, which the
        audit-trail surface treats as an operator-confirmed change.
        The previous pubkey is retained in TrustStore's `previous` list
        so a postmortem can recover the old identity."""
        errors: dict[str, str] = {}
        if user_input is not None:
            device_id = user_input[CONF_PIN_DEVICE_ID].strip()
            pubkey_hex = user_input[CONF_PIN_PUBKEY_HEX].strip().lower()
            if not device_id:
                errors["device_id"] = "invalid_device_id"
            elif not _looks_like_pubkey_hex(pubkey_hex):
                errors["pubkey_hex"] = "invalid_pubkey_hex"
            else:
                ts = self._trust_store()
                if ts is None:
                    errors["base"] = "trust_store_unavailable"
                elif not ts.is_pinned(device_id):
                    errors["device_id"] = "device_not_pinned"
                else:
                    await ts.async_rotate(device_id, pubkey_hex)
                    # Clear any stuck mismatch notification for this
                    # device so the operator's rotation takes effect
                    # immediately on the UI side.
                    entry_data = self.hass.data.get(
                        DOMAIN, {}
                    ).get(self._config_entry.entry_id)
                    if entry_data:
                        notified: set = entry_data.get("mismatch_notified", set())
                        notified.difference_update(
                            {(d, f) for (d, f) in notified if d == device_id}
                        )
                    return self.async_create_entry(title="", data={})

        return self.async_show_form(
            step_id="rotate",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_PIN_DEVICE_ID): str,
                    vol.Required(CONF_PIN_PUBKEY_HEX): str,
                }
            ),
            errors=errors,
        )

    async def async_step_unpin(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Drop the pin entirely. Subsequent publishes from this
        device will TOFU-pin to whatever pubkey shows up next — useful
        if the operator can't recover the old keypair (e.g. board
        loss) and wants HA to start fresh."""
        errors: dict[str, str] = {}
        if user_input is not None:
            device_id = user_input[CONF_PIN_DEVICE_ID].strip()
            if not device_id:
                errors["device_id"] = "invalid_device_id"
            else:
                ts = self._trust_store()
                if ts is None:
                    errors["base"] = "trust_store_unavailable"
                elif not await ts.async_unpin(device_id):
                    errors["device_id"] = "device_not_pinned"
                else:
                    # Mirror async_step_rotate: clear any stuck mismatch-
                    # notification dedup keys for this device. Without this,
                    # if a spoofed fp triggered a notification before the
                    # operator unpinned, the dedup set would permanently
                    # suppress a future real mismatch on that same fp,
                    # breaking the "warn loudly" guarantee on re-pin.
                    entry_data = self.hass.data.get(
                        DOMAIN, {}
                    ).get(self._config_entry.entry_id)
                    if entry_data:
                        notified: set = entry_data.get("mismatch_notified", set())
                        notified.difference_update(
                            {(d, f) for (d, f) in notified if d == device_id}
                        )
                    return self.async_create_entry(title="", data={})

        return self.async_show_form(
            step_id="unpin",
            data_schema=vol.Schema({vol.Required(CONF_PIN_DEVICE_ID): str}),
            errors=errors,
        )

    def _trust_store(self):
        """Lazy accessor — entry_data is created in async_setup_entry,
        so it always exists by the time the options flow runs (the
        button is only enabled after setup)."""
        return self.hass.data.get(DOMAIN, {}).get(
            self._config_entry.entry_id, {}
        ).get("trust_store")
