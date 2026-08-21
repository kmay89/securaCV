"""Config flow for SecuraCV integration.

Setup modes:
  auto) Zero-decision (default): probe for a running Privacy Witness Kernel
        and create the right entry without asking anything further.
  A) Canary devices via MQTT
  B) Witness Kernel via HTTP API
  C) Both MQTT + HTTP Kernel

Discovery entry points:
  - MQTT: a publish under the manifest's `securacv/#` filter lands in
    async_step_mqtt and ends in a zero-field confirm step.
  - Supervisor add-on: the kernel add-on POSTs discovery service "securacv"
    with {host, port}; async_step_hassio configures (or updates) the entry.
"""
from __future__ import annotations

import logging
from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.config_entries import ConfigEntry, ConfigFlowResult, OptionsFlow
from homeassistant.const import CONF_TOKEN, CONF_URL
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.aiohttp_client import async_get_clientsession

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
    SETUP_MODE_AUTO,
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


def _kernel_schema(
    values: dict[str, Any] | None, *, include_prefix: bool
) -> vol.Schema:
    """Kernel/both form schema, seeded with `values` as the defaults.

    On a validation error the form is rebuilt from what the user just
    submitted, so a typo'd URL comes back for correction instead of being
    silently reset to the static defaults.
    """
    values = values or {}
    fields: dict[Any, Any] = {
        vol.Required(
            CONF_URL, default=values.get(CONF_URL, DEFAULT_KERNEL_URL)
        ): str,
        vol.Optional(
            CONF_TOKEN_FILE,
            default=values.get(CONF_TOKEN_FILE, DEFAULT_TOKEN_FILE),
        ): str,
    }
    if values.get(CONF_TOKEN):
        fields[vol.Optional(CONF_TOKEN, default=values[CONF_TOKEN])] = str
    else:
        fields[vol.Optional(CONF_TOKEN)] = str
    if include_prefix:
        fields[
            vol.Optional(
                CONF_MQTT_PREFIX,
                default=values.get(CONF_MQTT_PREFIX, DEFAULT_MQTT_PREFIX),
            )
        ] = str
    if values.get(CONF_ADAPTER_STATS_URL):
        fields[
            vol.Optional(
                CONF_ADAPTER_STATS_URL,
                default=values[CONF_ADAPTER_STATS_URL],
            )
        ] = str
    else:
        fields[vol.Optional(CONF_ADAPTER_STATS_URL)] = str
    return vol.Schema(fields)


class SecuraCVConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Handle a config flow for SecuraCV."""

    VERSION = 2

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        """Return the OptionsFlow handler for this entry."""
        return SecuraCVOptionsFlow(config_entry)

    # Discovery-step state. Class-level defaults (not an __init__) so the
    # base flow machinery's construction is left completely alone.
    _discovered_prefix: str | None = None
    _hassio_url: str | None = None

    def _mqtt_prefix_already_configured(self, prefix: str) -> bool:
        """True if any existing entry already subscribes to this prefix.

        MQTT-only entries key their unique_id on the prefix while
        kernel/both entries key on the URL, so unique_id alone can't stop
        one system from ending up with two subscriptions on the same
        prefix. Every path that would enable MQTT checks here first.
        """
        for entry in self._async_current_entries():
            data = getattr(entry, "data", None) or {}
            if data.get(CONF_ENABLE_MQTT) and (
                data.get(CONF_MQTT_PREFIX, DEFAULT_MQTT_PREFIX) == prefix
            ):
                return True
        return False

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Handle the initial step — choose setup mode."""
        if user_input is not None:
            mode = user_input.get(CONF_SETUP_MODE, SETUP_MODE_AUTO)
            if mode == SETUP_MODE_MQTT:
                return await self.async_step_mqtt_config()
            elif mode == SETUP_MODE_KERNEL:
                return await self.async_step_kernel()
            elif mode == SETUP_MODE_BOTH:
                return await self.async_step_both()
            return await self._async_finish_auto()

        data_schema = vol.Schema(
            {
                vol.Required(CONF_SETUP_MODE, default=SETUP_MODE_AUTO): vol.In(
                    {
                        SETUP_MODE_AUTO: "Automatic — detect what's installed",
                        SETUP_MODE_MQTT: "Canary devices via MQTT",
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

    async def _async_finish_auto(self) -> ConfigFlowResult:
        """Zero-decision setup: probe the kernel, create the right entry.

        Never shows another form. If the kernel answers at the default
        add-on URL (an auth error counts — something answered), create a
        kernel+MQTT "both" entry; otherwise an MQTT-only entry with the
        default prefix. Aborts `already_configured` when an equivalent
        entry exists.
        """
        probe = {
            CONF_URL: DEFAULT_KERNEL_URL,
            CONF_TOKEN: "",
            CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
        }
        try:
            await _async_validate_kernel(self.hass, probe)
            kernel_present = True
        except InvalidAuth:
            # Auth failure proves something is answering at the kernel URL.
            kernel_present = True
        except CannotConnect:
            kernel_present = False
        except Exception:  # noqa: BLE001 - auto must never dead-end in a form
            _LOGGER.debug("kernel probe failed unexpectedly", exc_info=True)
            kernel_present = False

        if kernel_present:
            await self.async_set_unique_id(DEFAULT_KERNEL_URL)
            self._abort_if_unique_id_configured()
            if self._mqtt_prefix_already_configured(DEFAULT_MQTT_PREFIX):
                # An MQTT-only entry already owns the prefix — the common
                # story is "configured Canaries first, installed the kernel
                # later". Aborting here would strand the kernel, so promote
                # that entry to `both` (keeping its subscription) instead of
                # refusing.
                for entry in self._async_current_entries():
                    data = dict(getattr(entry, "data", None) or {})
                    if not data.get(CONF_ENABLE_MQTT):
                        continue
                    if data.get(CONF_MQTT_PREFIX, DEFAULT_MQTT_PREFIX) != (
                        DEFAULT_MQTT_PREFIX
                    ):
                        continue
                    if not data.get(CONF_URL):
                        data[CONF_URL] = DEFAULT_KERNEL_URL
                        data.setdefault(CONF_TOKEN, "")
                        data.setdefault(CONF_TOKEN_FILE, DEFAULT_TOKEN_FILE)
                        data[CONF_SETUP_MODE] = SETUP_MODE_BOTH
                        self.hass.config_entries.async_update_entry(
                            entry, data=data
                        )
                    break
                return self.async_abort(reason="already_configured")
            return self.async_create_entry(
                title="SecuraCV",
                data={
                    CONF_URL: DEFAULT_KERNEL_URL,
                    CONF_TOKEN: "",
                    CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
                    CONF_ENABLE_MQTT: True,
                    CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
                    CONF_SETUP_MODE: SETUP_MODE_BOTH,
                },
            )

        await self.async_set_unique_id(f"mqtt_{DEFAULT_MQTT_PREFIX}")
        self._abort_if_unique_id_configured()
        if self._mqtt_prefix_already_configured(DEFAULT_MQTT_PREFIX):
            return self.async_abort(reason="already_configured")
        return self.async_create_entry(
            title=f"SecuraCV ({DEFAULT_MQTT_PREFIX})",
            data={
                CONF_ENABLE_MQTT: True,
                CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
                CONF_SETUP_MODE: SETUP_MODE_MQTT,
            },
        )

    async def async_step_mqtt_config(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Configure MQTT-only mode for Canary devices."""
        if user_input is not None:
            prefix = user_input.get(CONF_MQTT_PREFIX, DEFAULT_MQTT_PREFIX)
            await self.async_set_unique_id(f"mqtt_{prefix}")
            self._abort_if_unique_id_configured()
            if self._mqtt_prefix_already_configured(prefix):
                return self.async_abort(reason="already_configured")

            return self.async_create_entry(
                title=f"SecuraCV ({prefix})",
                data={
                    CONF_ENABLE_MQTT: True,
                    CONF_MQTT_PREFIX: prefix,
                    CONF_SETUP_MODE: SETUP_MODE_MQTT,
                },
            )

        # Pre-fill with the prefix seen by MQTT discovery, if any.
        default_prefix = self._discovered_prefix or self.context.get(
            "mqtt_prefix", DEFAULT_MQTT_PREFIX
        )
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

        # Rebuild the form from what was just submitted (if anything) so a
        # validation error doesn't throw away the user's typing.
        return self.async_show_form(
            step_id="kernel",
            data_schema=_kernel_schema(user_input, include_prefix=False),
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
                prefix = user_input.get(CONF_MQTT_PREFIX, DEFAULT_MQTT_PREFIX)
                if self._mqtt_prefix_already_configured(prefix):
                    # A different entry (e.g. MQTT-only, keyed mqtt_<prefix>)
                    # already subscribes here — a second entry would double
                    # every Canary's messages.
                    return self.async_abort(reason="already_configured")

                data = {
                    CONF_URL: user_input[CONF_URL],
                    CONF_TOKEN: user_input.get(CONF_TOKEN, ""),
                    CONF_ENABLE_MQTT: True,
                    CONF_MQTT_PREFIX: prefix,
                    CONF_SETUP_MODE: SETUP_MODE_BOTH,
                }
                if user_input.get(CONF_TOKEN_FILE):
                    data[CONF_TOKEN_FILE] = user_input[CONF_TOKEN_FILE]
                if user_input.get(CONF_ADAPTER_STATS_URL):
                    data[CONF_ADAPTER_STATS_URL] = user_input[CONF_ADAPTER_STATS_URL]
                return self.async_create_entry(title="SecuraCV", data=data)

        # Rebuild the form from what was just submitted (if anything) so a
        # validation error doesn't throw away the user's typing.
        return self.async_show_form(
            step_id="both",
            data_schema=_kernel_schema(user_input, include_prefix=True),
            errors=errors,
        )

    async def async_step_mqtt(self, discovery_info: Any) -> ConfigFlowResult:
        """Handle MQTT auto-discovery.

        Triggered when HA sees a message on a topic matching the 'mqtt'
        key in manifest.json (securacv/#). Since HA 2022.6 the argument
        is an attribute-style MqttServiceInfo; read it via getattr with a
        dict fallback so older cores (and test stubs) keep working —
        `.get()` on the ServiceInfo is what used to crash this step into
        "Unknown error".
        """
        topic = getattr(discovery_info, "topic", None)
        if topic is None and isinstance(discovery_info, dict):
            topic = discovery_info.get("topic")
        topic = topic or ""

        parts = topic.split("/")
        prefix = parts[0] if len(parts) >= 2 and parts[0] else DEFAULT_MQTT_PREFIX

        self._discovered_prefix = prefix
        self.context["title_placeholders"] = {"name": prefix}
        self.context["mqtt_prefix"] = prefix

        await self.async_set_unique_id(f"mqtt_{prefix}")
        self._abort_if_unique_id_configured()
        if self._mqtt_prefix_already_configured(prefix):
            # A kernel+MQTT entry may already cover this prefix under a
            # URL-keyed unique_id; don't offer to configure it twice.
            return self.async_abort(reason="already_configured")

        return await self.async_step_confirm()

    async def async_step_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Zero-input confirmation for MQTT discovery.

        Everything is already known (the prefix came off the wire); the
        form has no fields — submitting it creates the entry.
        """
        prefix = self._discovered_prefix or DEFAULT_MQTT_PREFIX
        if user_input is not None:
            if self._mqtt_prefix_already_configured(prefix):
                return self.async_abort(reason="already_configured")
            return self.async_create_entry(
                title=f"SecuraCV ({prefix})",
                data={
                    CONF_ENABLE_MQTT: True,
                    CONF_MQTT_PREFIX: prefix,
                    CONF_SETUP_MODE: SETUP_MODE_MQTT,
                },
            )

        return self.async_show_form(
            step_id="confirm",
            data_schema=vol.Schema({}),
            description_placeholders={"prefix": prefix},
        )

    async def async_step_hassio(self, discovery_info: Any) -> ConfigFlowResult:
        """Handle Supervisor add-on discovery.

        The Privacy Witness Kernel add-on POSTs discovery service
        "securacv" with config {host, port}. Accept the HassioServiceInfo
        defensively (attribute-style or plain dict) so older cores and
        test stubs both work.
        """
        try:
            # Imported for its side effects on typing only; the payload is
            # read defensively below so stub environments need no module.
            from homeassistant.helpers.service_info.hassio import (  # noqa: F401
                HassioServiceInfo,
            )
        except Exception:  # noqa: BLE001 - older cores / test stubs
            pass

        config = getattr(discovery_info, "config", None)
        if config is None and isinstance(discovery_info, dict):
            # Older cores handed the posted config dict straight through,
            # possibly nested under a "config" key.
            config = discovery_info.get("config", discovery_info)
        if not isinstance(config, dict):
            return self.async_abort(reason="invalid_discovery_info")

        host = config.get("host")
        port = config.get("port")
        if not host or not port:
            return self.async_abort(reason="invalid_discovery_info")
        url = f"http://{host}:{port}"

        addon_name = (
            config.get("addon")
            or getattr(discovery_info, "name", None)
            or "Add-on"
        )
        self.context["title_placeholders"] = {"name": str(addon_name)}

        await self.async_set_unique_id("hassio")
        self._abort_if_unique_id_configured(updates={CONF_URL: url})

        # Any existing entry means this system is already set up; refresh
        # its kernel URL from the add-on's announcement rather than
        # prompting for a second entry.
        existing = list(self._async_current_entries())
        if existing:
            entry = next(
                (
                    e
                    for e in existing
                    if (getattr(e, "data", None) or {}).get(CONF_URL)
                ),
                existing[0],
            )
            new_data = dict(getattr(entry, "data", None) or {})
            new_data[CONF_URL] = url
            new_data.setdefault(CONF_TOKEN, "")
            new_data.setdefault(CONF_TOKEN_FILE, DEFAULT_TOKEN_FILE)
            if new_data.get(CONF_SETUP_MODE) == SETUP_MODE_MQTT:
                # The kernel just announced itself; an MQTT-only entry can
                # now use it too.
                new_data[CONF_SETUP_MODE] = SETUP_MODE_BOTH
            self.hass.config_entries.async_update_entry(entry, data=new_data)
            return self.async_abort(reason="already_configured")

        self._hassio_url = url
        return await self.async_step_hassio_confirm()

    async def async_step_hassio_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Zero-input confirmation for Supervisor add-on discovery."""
        url = self._hassio_url or DEFAULT_KERNEL_URL
        if user_input is not None:
            if self._mqtt_prefix_already_configured(DEFAULT_MQTT_PREFIX):
                return self.async_abort(reason="already_configured")
            return self.async_create_entry(
                title="SecuraCV",
                data={
                    CONF_URL: url,
                    CONF_TOKEN: "",
                    CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
                    CONF_ENABLE_MQTT: True,
                    CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
                    CONF_SETUP_MODE: SETUP_MODE_BOTH,
                },
            )

        return self.async_show_form(
            step_id="hassio_confirm",
            data_schema=vol.Schema({}),
            description_placeholders={"url": url},
        )


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
