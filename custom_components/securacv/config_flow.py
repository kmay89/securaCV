"""Config flow for SecuraCV integration.

One identity everywhere: a kernel entry's unique_id is ``kernel_<device
id>``, the device id GET /api/identity (or the /api/pair response) serves.
Every path that creates or updates a kernel entry — Supervisor discovery,
the MQTT announce, pairing, the manual forms, Automatic, reauth and
reconfigure — reads the identity over the authenticated HTTP channel first
and keys on it, so one kernel is one entry however it was found. The
verifying key that same answer carries is pinned into the entry (trust on
first use over HTTP, never from a retained MQTT message).

Discovery entry points:
  - Supervisor (the SecuraCV app): ``async_step_hassio`` consumes the app's
    discovery payload {host, port, token_file, mqtt_topic_prefix, device_id}
    and ends in a zero-field confirm step.
  - MQTT: a retained ``witness/<id>/announce`` lands in ``async_step_mqtt``
    and routes to ``announce_confirm`` then ``pair`` (a 6-digit code typed
    once; the kernel's key fingerprint is shown beside the field so the
    person compares it with what ``pair`` printed); anything else under
    ``securacv/#`` is a Canary and ends in the zero-field ``confirm``.
  - Automatic (the user step's default): probes the app's hostname, then
    listens five seconds for an announce; finding neither creates the
    MQTT-only entry, says where to get a kernel, and raises Repairs
    ``no_kernel_found``.

Reload is one mechanism: ``_abort_if_unique_id_configured(updates=...)`` and
``async_update_reload_and_abort`` reload a loaded entry when its data
changed. No update listener exists anywhere in the package.
"""
from __future__ import annotations

import asyncio
import logging
import re
from collections.abc import Mapping
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    # Home Assistant 2026.9 and later validate with probatio and install it as
    # `voluptuous` before any integration imports (install_as_voluptuous in
    # homeassistant/__init__.py), so the runtime import below IS probatio on
    # those cores and voluptuous on the 2026.5 floor. The core's flow helpers
    # are typed against probatio alone, so the type checker reads it as that;
    # the names used here are the API both libraries share.
    import probatio as vol
else:
    import voluptuous as vol

from homeassistant import config_entries
from homeassistant.components import mqtt
from homeassistant.config_entries import ConfigEntry, ConfigFlowResult, OptionsFlow
from homeassistant.const import CONF_TOKEN, CONF_URL
from homeassistant.core import HomeAssistant, callback
from homeassistant.data_entry_flow import AbortFlow
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from . import repairs
from .api import (
    PairResult,
    SecuraCVApi,
    SecuraCVApiAuthError,
    SecuraCVApiError,
    SecuraCVApiPairLimitError,
    async_pair_kernel,
)
from .claim import new_claim_nonce
from .const import (
    ADDON_REPOSITORY_LINK,
    AUTH_MODE_PAIRED,
    AUTH_MODE_STATIC,
    AUTH_MODE_TOKEN_FILE,
    CONF_ADAPTER_STATS_URL,
    CONF_AUTH_MODE,
    CONF_CLAIM_NONCE,
    CONF_ENABLE_MQTT,
    CONF_EVIDENCE_KEEP,
    CONF_KERNEL_DEVICE_ID,
    CONF_KERNEL_KEY_PINNED_BUCKET_START,
    CONF_KERNEL_TOKEN_ID,
    CONF_KERNEL_VERIFYING_KEY,
    CONF_MQTT_PREFIX,
    CONF_MQTT_TOPIC_PREFIX,
    CONF_PAIRING_CODE,
    CONF_SETUP_MODE,
    CONF_TOKEN_FILE,
    DEFAULT_EVIDENCE_KEEP,
    DEFAULT_KERNEL_API_PORT,
    DEFAULT_KERNEL_URL,
    DEFAULT_MQTT_PREFIX,
    DEFAULT_MQTT_TOPIC_PREFIX,
    DEFAULT_TOKEN_FILE,
    DOCKER_QUICKSTART_LINK,
    DOMAIN,
    KERNEL_TOPIC_ANNOUNCE,
    KERNEL_TOPIC_CLAIM,
    MAX_EVIDENCE_KEEP,
    MIN_EVIDENCE_KEEP,
    PAIRING_CODE_LENGTH,
    SETUP_MODE_AUTO,
    SETUP_MODE_BOTH,
    SETUP_MODE_KERNEL,
    SETUP_MODE_MQTT,
    TIME_BUCKET_SECONDS,
)
from .data import runtime_for
from .kernel_trust import (
    PAIRING_REQUIRED,
    Announce,
    kernel_key_fingerprint,
    normalize_kernel_key,
    parse_announce,
    valid_kernel_device_id,
    verify_announce_signature,
)

_LOGGER = logging.getLogger(__name__)

# How long Automatic listens for a retained announce before concluding that
# no sidecar is on the broker. Retained messages arrive on subscribe, so a
# running sidecar answers within the first second.
ANNOUNCE_LISTEN_SECONDS = 5.0
# The host Automatic/pairing fall back to when nothing better is known: the
# quickstart compose names its container `securacv`.
FALLBACK_SIDECAR_HOST = "securacv"
PAIRING_CODE_RE = re.compile(rf"^\d{{{PAIRING_CODE_LENGTH}}}$")


class CannotConnect(Exception):
    """Error to indicate we cannot connect."""


class InvalidAuth(Exception):
    """Error to indicate there is invalid auth."""


class IdentityUnavailable(Exception):
    """The kernel answered but serves no identity (an app or sidecar too old)."""


@dataclass(frozen=True)
class KernelProbe:
    """What a successful GET /api/identity told the flow."""

    device_id: str
    verifying_key: str
    kernel_version: str


def _bucket_start_now() -> int:
    import time

    return int(time.time() // TIME_BUCKET_SECONDS) * TIME_BUCKET_SECONDS


async def _async_probe_kernel(hass: HomeAssistant, data: Mapping[str, Any]) -> KernelProbe:
    """Read the kernel's identity with the credential in `data`.

    The one probe every kernel path shares (test-before-configure): it
    proves the URL answers, the credential is accepted, and yields the
    device id the entry is keyed on plus the key it pins.
    """
    api = SecuraCVApi(
        data[CONF_URL],
        data.get(CONF_TOKEN, ""),
        async_get_clientsession(hass),
        token_file=data.get(CONF_TOKEN_FILE),
    )
    try:
        identity = await api.async_get_identity()
    except SecuraCVApiAuthError as err:
        raise InvalidAuth from err
    except SecuraCVApiError as err:
        raise CannotConnect from err
    if identity is None:
        raise IdentityUnavailable
    device_id = identity.get("device_id")
    key = normalize_kernel_key(identity.get("verifying_key"))
    if not valid_kernel_device_id(device_id) or key is None:
        raise IdentityUnavailable
    return KernelProbe(str(device_id), key, str(identity.get("kernel_version", "")))


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
    fields[
        vol.Optional(
            CONF_MQTT_TOPIC_PREFIX,
            default=values.get(CONF_MQTT_TOPIC_PREFIX, DEFAULT_MQTT_TOPIC_PREFIX),
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


def _auth_mode_for(user_input: Mapping[str, Any]) -> str:
    """token_file when a file path is given, else static."""
    return AUTH_MODE_TOKEN_FILE if user_input.get(CONF_TOKEN_FILE) else AUTH_MODE_STATIC


def _kernel_entry_data(
    *,
    url: str,
    auth_mode: str,
    probe_device_id: str,
    verifying_key: str,
    token: str = "",
    token_file: str | None = None,
    token_id: str | None = None,
    topic_prefix: str = DEFAULT_MQTT_TOPIC_PREFIX,
    enable_mqtt: bool,
    mqtt_prefix: str = DEFAULT_MQTT_PREFIX,
    setup_mode: str,
    adapter_stats_url: str | None = None,
) -> dict[str, Any]:
    """The data of a kernel entry (contracts C2 and C6)."""
    data: dict[str, Any] = {
        CONF_URL: url,
        CONF_TOKEN: token,
        CONF_AUTH_MODE: auth_mode,
        CONF_KERNEL_DEVICE_ID: probe_device_id,
        CONF_KERNEL_VERIFYING_KEY: verifying_key,
        CONF_KERNEL_KEY_PINNED_BUCKET_START: _bucket_start_now(),
        CONF_MQTT_TOPIC_PREFIX: topic_prefix,
        CONF_CLAIM_NONCE: new_claim_nonce(),
        CONF_ENABLE_MQTT: enable_mqtt,
        CONF_SETUP_MODE: setup_mode,
    }
    if token_file:
        data[CONF_TOKEN_FILE] = token_file
    if token_id:
        data[CONF_KERNEL_TOKEN_ID] = token_id
    if enable_mqtt:
        data[CONF_MQTT_PREFIX] = mqtt_prefix
    if adapter_stats_url:
        data[CONF_ADAPTER_STATS_URL] = adapter_stats_url
    return data


class SecuraCVConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Handle a config flow for SecuraCV."""

    VERSION = 3

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        """Return the OptionsFlow handler for this entry.

        The entry is not handed to the flow: ``OptionsFlow.config_entry`` is
        a property the core resolves from the flow's handler (2024.11+), and
        storing the entry on the instance was the pattern that property
        replaced.
        """
        return SecuraCVOptionsFlow()

    # Discovery-step state. Class-level defaults (not an __init__) so the
    # base flow machinery's construction is left completely alone.
    _discovered_prefix: str | None = None
    _announce: Announce | None = None
    _hassio: dict[str, Any] | None = None
    _pair_url: str | None = None
    # The app's name from Supervisor discovery, for the hassio_confirm copy.
    _hassio_name: str = "SecuraCV"

    # ── shared helpers ───────────────────────────────────────────────

    def _mqtt_prefix_already_configured(self, prefix: str) -> bool:
        """True if any existing entry already subscribes to this prefix.

        MQTT-only entries key their unique_id on the prefix while
        kernel entries key on the kernel id, so unique_id alone can't stop
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

    def _mqtt_only_entry(self, prefix: str) -> ConfigEntry | None:
        """The MQTT-only entry owning `prefix`, if any (the J2-then-kernel case)."""
        for entry in self._async_current_entries():
            data = getattr(entry, "data", None) or {}
            if (
                data.get(CONF_ENABLE_MQTT)
                and not data.get(CONF_URL)
                and data.get(CONF_MQTT_PREFIX, DEFAULT_MQTT_PREFIX) == prefix
            ):
                return entry
        return None

    def _legacy_kernel_entry(self) -> ConfigEntry | None:
        """A kernel entry still keyed the pre-v3 way (URL, `hassio`), if any."""
        for entry in self._async_current_entries():
            unique_id = getattr(entry, "unique_id", None) or ""
            data = getattr(entry, "data", None) or {}
            if data.get(CONF_URL) and not unique_id.startswith("kernel_"):
                return entry
        return None

    def _mqtt_configured(self) -> bool:
        """Whether Home Assistant has an MQTT integration entry at all.

        A sidecar is discovered over the broker Home Assistant and the
        sidecar share; without an MQTT entry here there is nothing to hear
        it on, however many announces the sidecar publishes.
        """
        try:
            return bool(self.hass.config_entries.async_entries(mqtt.DOMAIN))
        except Exception:  # noqa: BLE001 - a courtesy, never a dead end
            _LOGGER.debug("mqtt entry lookup failed", exc_info=True)
            return False

    def _broker_host(self) -> str | None:
        """The host of the MQTT broker Home Assistant talks to, if known.

        A sidecar beside Frigate usually runs on the same machine as the
        broker, so it is the best guess for the kernel URL's host.
        """
        try:
            for entry in self.hass.config_entries.async_entries(mqtt.DOMAIN):
                host = (getattr(entry, "data", None) or {}).get("broker")
                if isinstance(host, str) and host.strip():
                    return host.strip()
        except Exception:  # noqa: BLE001 - the guess is a courtesy
            _LOGGER.debug("broker host lookup failed", exc_info=True)
        return None

    def _pair_url_default(self) -> str:
        """The URL the pairing form opens with."""
        if self._pair_url:
            return self._pair_url
        if self._announce is not None:
            url = self._announce.api_url(self._broker_host())
            if url:
                return url
        host = self._broker_host() or FALLBACK_SIDECAR_HOST
        return f"http://{host}:{DEFAULT_KERNEL_API_PORT}"

    def _kernel_placeholders(self, entry: ConfigEntry | None = None) -> dict[str, str]:
        """What the pairing / credential forms show about the kernel at hand.

        The announced key's fingerprint when a sidecar announced itself; the
        pinned one when an existing entry is being re-authed or reconfigured;
        otherwise the instruction to read it off `pair`.
        """
        if self._announce is not None:
            return {"fingerprint": self._announce.fingerprint, "device_id": self._announce.device_id}
        data = (getattr(entry, "data", None) or {}) if entry is not None else {}
        pinned = normalize_kernel_key(data.get(CONF_KERNEL_VERIFYING_KEY))
        return {
            "fingerprint": kernel_key_fingerprint(pinned) if pinned else "the one `pair` prints",
            "device_id": str(data.get(CONF_KERNEL_DEVICE_ID) or "the kernel"),
        }

    def _flow_entry(self) -> ConfigEntry | None:
        """The entry a reauth or reconfigure flow is about, else None."""
        if self.source == config_entries.SOURCE_REAUTH:
            return self._get_reauth_entry()
        if self.source == config_entries.SOURCE_RECONFIGURE:
            return self._get_reconfigure_entry()
        return None

    @callback
    def _async_promote_entry(self, entry: ConfigEntry, data: dict[str, Any]) -> ConfigFlowResult:
        """Give an existing entry the kernel it just found, reload it, abort.

        Keeps whatever the entry already had (its MQTT subscription, the
        nonce of a claim it already published) and adds the kernel fields.
        """
        merged = dict(getattr(entry, "data", None) or {})
        if merged.get(CONF_CLAIM_NONCE):
            data = {**data, CONF_CLAIM_NONCE: merged[CONF_CLAIM_NONCE]}
        merged.update(data)
        unique_id = f"kernel_{data[CONF_KERNEL_DEVICE_ID]}"
        self.hass.config_entries.async_update_entry(entry, data=merged, unique_id=unique_id)
        self.hass.config_entries.async_schedule_reload(entry.entry_id)
        return self.async_abort(reason="already_configured")

    async def _async_create_or_promote(self, data: dict[str, Any]) -> ConfigFlowResult:
        """Create the kernel entry, or promote the MQTT-only one owning the prefix."""
        if data.get(CONF_ENABLE_MQTT):
            prefix = data.get(CONF_MQTT_PREFIX, DEFAULT_MQTT_PREFIX)
            existing = self._mqtt_only_entry(prefix)
            if existing is not None:
                # Canaries first, kernel later: promote that entry to `both`
                # (keeping its subscription) instead of refusing.
                return self._async_promote_entry(existing, data)
            if self._mqtt_prefix_already_configured(prefix):
                # A different kernel entry already subscribes here — a second
                # entry would double every Canary's messages.
                return self.async_abort(reason="already_configured")
        return self.async_create_entry(title="SecuraCV", data=data)

    def _abort_if_other_kernel(self, entry: ConfigEntry, device_id: str) -> None:
        """Reauth/reconfigure must stay on the kernel the entry was made for."""
        configured = (getattr(entry, "data", None) or {}).get(CONF_KERNEL_DEVICE_ID)
        unique_id = getattr(entry, "unique_id", None) or ""
        if (configured and configured != device_id) or (
            unique_id.startswith("kernel_") and unique_id != f"kernel_{device_id}"
        ):
            raise AbortFlow("wrong_kernel")

    # ── the user step ────────────────────────────────────────────────

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

    async def _async_listen_for_announce(self) -> Announce | None:
        """Wait up to ANNOUNCE_LISTEN_SECONDS for a retained kernel announce."""
        found: asyncio.Future[Announce] = asyncio.get_running_loop().create_future()

        @callback
        def _on_announce(msg: mqtt.ReceiveMessage) -> None:
            announce = parse_announce(msg.payload)
            if announce is not None and not found.done():
                found.set_result(announce)

        unsub = None
        try:
            async with asyncio.timeout(ANNOUNCE_LISTEN_SECONDS):
                if not await mqtt.async_wait_for_mqtt_client(self.hass):
                    return None
                unsub = await mqtt.async_subscribe(
                    self.hass,
                    f"{DEFAULT_MQTT_TOPIC_PREFIX}/+/{KERNEL_TOPIC_ANNOUNCE}",
                    _on_announce,
                )
                return await found
        except TimeoutError:
            return None
        except Exception:  # noqa: BLE001 - no broker, no announce; Automatic goes on
            _LOGGER.debug("announce listen failed", exc_info=True)
            return None
        finally:
            if unsub is not None:
                unsub()

    async def _async_finish_auto(self) -> ConfigFlowResult:
        """Zero-decision setup: find the kernel, create the right entry.

        Probes the app's hostname first (nothing to type on Home Assistant
        OS), then listens for a sidecar's announce (one code to type).
        Finding neither creates the MQTT-only entry, says where to get a
        kernel on the final screen, and raises Repairs `no_kernel_found`,
        which clears itself once a kernel entry loads.
        """
        probe_data = {
            CONF_URL: DEFAULT_KERNEL_URL,
            CONF_TOKEN: "",
            CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
        }
        probe: KernelProbe | None = None
        kernel_answered = False
        try:
            probe = await _async_probe_kernel(self.hass, probe_data)
        except (InvalidAuth, IdentityUnavailable):
            # Something is answering at the app's URL; it only cannot be
            # identified yet (token file not readable, or an app too old to
            # serve /api/identity). The entry adopts the identity on the
            # first backstop that reads it.
            kernel_answered = True
        except CannotConnect:
            pass
        except Exception:  # noqa: BLE001 - auto must never dead-end in a form
            _LOGGER.debug("kernel probe failed unexpectedly", exc_info=True)

        if probe is not None:
            await self.async_set_unique_id(f"kernel_{probe.device_id}")
            # No updates: an entry a person pointed at a custom URL for this
            # kernel is never stomped by a rerun of Automatic.
            self._abort_if_unique_id_configured()
            return await self._async_create_or_promote(
                _kernel_entry_data(
                    url=DEFAULT_KERNEL_URL,
                    auth_mode=AUTH_MODE_TOKEN_FILE,
                    probe_device_id=probe.device_id,
                    verifying_key=probe.verifying_key,
                    token_file=DEFAULT_TOKEN_FILE,
                    enable_mqtt=True,
                    setup_mode=SETUP_MODE_BOTH,
                )
            )

        if kernel_answered:
            await self.async_set_unique_id(DEFAULT_KERNEL_URL)
            self._abort_if_unique_id_configured()
            existing = self._mqtt_only_entry(DEFAULT_MQTT_PREFIX)
            legacy_data = {
                CONF_URL: DEFAULT_KERNEL_URL,
                CONF_TOKEN: "",
                CONF_TOKEN_FILE: DEFAULT_TOKEN_FILE,
                CONF_AUTH_MODE: AUTH_MODE_TOKEN_FILE,
                CONF_MQTT_TOPIC_PREFIX: DEFAULT_MQTT_TOPIC_PREFIX,
                CONF_CLAIM_NONCE: new_claim_nonce(),
                CONF_ENABLE_MQTT: True,
                CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
                CONF_SETUP_MODE: SETUP_MODE_BOTH,
            }
            if existing is not None:
                merged = {**(getattr(existing, "data", None) or {}), **legacy_data}
                self.hass.config_entries.async_update_entry(existing, data=merged)
                self.hass.config_entries.async_schedule_reload(existing.entry_id)
                return self.async_abort(reason="already_configured")
            if self._mqtt_prefix_already_configured(DEFAULT_MQTT_PREFIX):
                return self.async_abort(reason="already_configured")
            return self.async_create_entry(title="SecuraCV", data=legacy_data)

        announce = await self._async_listen_for_announce()
        if announce is not None:
            self._announce = announce
            await self.async_set_unique_id(f"kernel_{announce.device_id}")
            self._abort_if_unique_id_configured()
            return await self.async_step_announce_confirm()

        await self.async_set_unique_id(f"mqtt_{DEFAULT_MQTT_PREFIX}")
        self._abort_if_unique_id_configured()
        if self._mqtt_prefix_already_configured(DEFAULT_MQTT_PREFIX):
            return self.async_abort(reason="already_configured")
        repairs.async_report_no_kernel_found(self.hass)
        # Without Home Assistant's MQTT integration no announce can ever be
        # heard, so "connects by itself when one appears" would be a false
        # promise: the final screen then says what to set up first.
        description = "no_kernel_found" if self._mqtt_configured() else "no_kernel_found_no_mqtt"
        return self.async_create_entry(
            title=f"SecuraCV ({DEFAULT_MQTT_PREFIX})",
            data={
                CONF_ENABLE_MQTT: True,
                CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
                CONF_SETUP_MODE: SETUP_MODE_MQTT,
            },
            description=description,
            description_placeholders={
                "addon_repo_url": ADDON_REPOSITORY_LINK,
                "docker_quickstart_url": DOCKER_QUICKSTART_LINK,
            },
        )

    # ── the manual forms ─────────────────────────────────────────────

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

    async def _async_manual_kernel(
        self, step_id: str, user_input: dict[str, Any] | None, *, enable_mqtt: bool
    ) -> ConfigFlowResult:
        """The kernel / both forms: probe, key on the identity, create."""
        errors: dict[str, str] = {}
        if user_input is not None:
            errors = _kernel_auth_errors(user_input)
            probe: KernelProbe | None = None
            if not errors:
                try:
                    probe = await _async_probe_kernel(self.hass, user_input)
                except CannotConnect:
                    errors["base"] = "cannot_connect"
                except InvalidAuth:
                    errors["base"] = "invalid_auth"
                except IdentityUnavailable:
                    errors["base"] = "kernel_identity_unavailable"
            if not errors and probe is not None:
                await self.async_set_unique_id(f"kernel_{probe.device_id}")
                self._abort_if_unique_id_configured()
                return await self._async_create_or_promote(
                    _kernel_entry_data(
                        url=user_input[CONF_URL],
                        auth_mode=_auth_mode_for(user_input),
                        probe_device_id=probe.device_id,
                        verifying_key=probe.verifying_key,
                        token=user_input.get(CONF_TOKEN, ""),
                        token_file=user_input.get(CONF_TOKEN_FILE) or None,
                        topic_prefix=user_input.get(CONF_MQTT_TOPIC_PREFIX)
                        or DEFAULT_MQTT_TOPIC_PREFIX,
                        enable_mqtt=enable_mqtt,
                        mqtt_prefix=user_input.get(CONF_MQTT_PREFIX, DEFAULT_MQTT_PREFIX),
                        setup_mode=SETUP_MODE_BOTH if enable_mqtt else SETUP_MODE_KERNEL,
                        adapter_stats_url=user_input.get(CONF_ADAPTER_STATS_URL) or None,
                    )
                )

        # Rebuild the form from what was just submitted (if anything) so a
        # validation error doesn't throw away the user's typing.
        return self.async_show_form(
            step_id=step_id,
            data_schema=_kernel_schema(user_input, include_prefix=enable_mqtt),
            errors=errors,
        )

    async def async_step_kernel(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Configure Witness Kernel HTTP API connection."""
        return await self._async_manual_kernel("kernel", user_input, enable_mqtt=False)

    async def async_step_both(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Configure both MQTT and Kernel HTTP API."""
        return await self._async_manual_kernel("both", user_input, enable_mqtt=True)

    # ── MQTT discovery: Canaries and the kernel announce ─────────────

    async def async_step_mqtt(self, discovery_info: Any) -> ConfigFlowResult:
        """Handle MQTT auto-discovery.

        Triggered when HA sees a message on a topic matching the 'mqtt'
        key in manifest.json (``securacv/#`` and ``witness/+/announce``).
        Since HA 2022.6 the argument is an attribute-style MqttServiceInfo;
        read it via getattr with a dict fallback so older cores (and test
        stubs) keep working — `.get()` on the ServiceInfo is what used to
        crash this step into "Unknown error".
        """
        topic = getattr(discovery_info, "topic", None)
        payload = getattr(discovery_info, "payload", None)
        if isinstance(discovery_info, dict):
            topic = topic if topic is not None else discovery_info.get("topic")
            payload = payload if payload is not None else discovery_info.get("payload")
        topic = topic or ""

        parts = topic.split("/")
        if len(parts) == 3 and parts[2] == KERNEL_TOPIC_ANNOUNCE:
            return await self._async_step_announce(parts[0], payload)
        if len(parts) >= 2 and parts[1] == KERNEL_TOPIC_CLAIM:
            # The integration's own retained claim is not a device.
            return self.async_abort(reason="not_securacv_device")

        prefix = parts[0] if len(parts) >= 2 and parts[0] else DEFAULT_MQTT_PREFIX

        self._discovered_prefix = prefix
        self.context["title_placeholders"] = {"name": prefix}

        await self.async_set_unique_id(f"mqtt_{prefix}")
        self._abort_if_unique_id_configured()
        if self._mqtt_prefix_already_configured(prefix):
            # A kernel+MQTT entry may already cover this prefix under a
            # kernel-keyed unique_id; don't offer to configure it twice.
            return self.async_abort(reason="already_configured")

        return await self.async_step_confirm()

    async def _async_step_announce(self, prefix: str, payload: Any) -> ConfigFlowResult:
        """A retained ``<prefix>/<device_id>/announce``: a kernel offers itself."""
        announce = parse_announce(payload)
        if announce is None:
            # Malformed, or the empty retained clear of a sidecar that shut
            # down cleanly: nothing to offer.
            return self.async_abort(reason="invalid_discovery_info")
        self._announce = announce
        # The discovered card's title (flow_title "SecuraCV {name}"): "SecuraCV
        # sidecar at <host>" when the sidecar announced an address, else
        # "SecuraCV sidecar <kernel id>" — the docs and the `pair` card name
        # it the same way.
        where = f"at {announce.api_host}" if announce.api_host else announce.device_id
        self.context["title_placeholders"] = {"name": f"sidecar {where}"}

        existing = await self.async_set_unique_id(f"kernel_{announce.device_id}")
        if existing is not None:
            pinned = normalize_kernel_key(
                (getattr(existing, "data", None) or {}).get(CONF_KERNEL_VERIFYING_KEY)
            )
            if pinned is None:
                # No pin yet, so nothing in this announce is verifiable: an
                # entry's topic prefix and URL are never moved (nor the entry
                # reloaded) on the word of an unsigned retained message. The
                # backstop adopts the identity and the pin on its own.
                self._abort_if_unique_id_configured()
                return self.async_abort(reason="already_configured")
            if not verify_announce_signature(announce, pinned):
                return self.async_abort(reason="announce_signature_invalid")
            if announce.verifying_key != pinned:
                repairs.async_report_tofu_mismatch(
                    self.hass,
                    existing,
                    pinned_fingerprint=kernel_key_fingerprint(pinned),
                    received_fingerprint=announce.fingerprint,
                    received_key=announce.verifying_key,
                    url=announce.api_url(None) or str(existing.data.get(CONF_URL, "")),
                )
                return self.async_abort(
                    reason="announce_key_mismatch",
                    description_placeholders={
                        "pinned_fingerprint": kernel_key_fingerprint(pinned),
                        "received_fingerprint": announce.fingerprint,
                    },
                )
            # Signed by the pinned key: the kernel's own word on where its
            # topics and (when it announced a host) its API live.
            updates: dict[str, Any] = {CONF_MQTT_TOPIC_PREFIX: announce.mqtt_topic_prefix}
            url = announce.api_url(None)
            if url:
                # An explicitly announced host, signed by the pinned key,
                # is the kernel telling us it moved (discovery-update-info).
                updates[CONF_URL] = url
            self._abort_if_unique_id_configured(updates=updates, reload_on_update=True)
        return await self.async_step_announce_confirm()

    async def async_step_announce_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Zero-field confirm for an announced kernel; next is pairing."""
        announce = self._announce
        if announce is None:
            return self.async_abort(reason="invalid_discovery_info")
        if user_input is not None:
            if announce.pairing == PAIRING_REQUIRED:
                return await self.async_step_pair()
            return await self.async_step_token_file()
        self._set_confirm_only()
        return self.async_show_form(
            step_id="announce_confirm",
            data_schema=vol.Schema({}),
            description_placeholders={
                "device_id": announce.device_id,
                "fingerprint": announce.fingerprint,
                "kernel_version": announce.kernel_version,
                "url": self._pair_url_default(),
            },
        )

    # ── pairing (the sidecar's 6-digit code) ──────────────────────────

    async def _async_pair_step(
        self, step_id: str, user_input: dict[str, Any] | None
    ) -> ConfigFlowResult:
        """The pairing form, shared by `pair`, reauth and reconfigure."""
        errors: dict[str, str] = {}
        announce = self._announce
        if user_input is not None:
            url = str(user_input.get(CONF_URL, "")).strip().rstrip("/")
            code = str(user_input.get(CONF_PAIRING_CODE, "")).strip()
            self._pair_url = url or None
            if not PAIRING_CODE_RE.fullmatch(code):
                errors[CONF_PAIRING_CODE] = "invalid_pairing_code"
            elif not url.startswith(("http://", "https://")):
                errors[CONF_URL] = "invalid_url"
            else:
                try:
                    result = await async_pair_kernel(
                        async_get_clientsession(self.hass), url, code
                    )
                except SecuraCVApiAuthError:
                    errors["base"] = "invalid_pairing_code"
                except SecuraCVApiPairLimitError:
                    # The code is still valid: the person revokes a token
                    # on the Docker host and submits the same code again.
                    errors["base"] = "paired_token_limit"
                except SecuraCVApiError:
                    errors["base"] = "cannot_connect"
                else:
                    key = normalize_kernel_key(result.verifying_key)
                    if key is None or not valid_kernel_device_id(result.device_id):
                        errors["base"] = "kernel_identity_unavailable"
                    elif announce is not None and (
                        announce.verifying_key != key or announce.device_id != result.device_id
                    ):
                        await self._async_revoke_quietly(url, result)
                        return self.async_abort(
                            reason="announce_key_mismatch",
                            description_placeholders={
                                "pinned_fingerprint": announce.fingerprint,
                                "received_fingerprint": kernel_key_fingerprint(key),
                            },
                        )
                    else:
                        return await self._async_finish_pair(url, result, key)
        return self.async_show_form(
            step_id=step_id,
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_URL, default=self._pair_url_default()): str,
                    vol.Required(CONF_PAIRING_CODE): str,
                }
            ),
            errors=errors,
            description_placeholders=self._kernel_placeholders(self._flow_entry()),
        )

    async def _async_revoke_quietly(self, url: str, result: PairResult) -> None:
        """Revoke a token the flow will not keep (a mismatch, a duplicate)."""
        try:
            await SecuraCVApi(url, result.token, async_get_clientsession(self.hass)).async_revoke_paired_token(
                result.token_id
            )
        except Exception:  # noqa: BLE001 - `unpair` on the sidecar still works
            _LOGGER.debug("could not revoke the unused paired token", exc_info=True)

    async def _async_finish_pair(self, url: str, result: PairResult, key: str) -> ConfigFlowResult:
        """A pairing succeeded: update the entry being re-authed, or create one."""
        paired: dict[str, Any] = {
            CONF_URL: url,
            CONF_TOKEN: result.token,
            CONF_AUTH_MODE: AUTH_MODE_PAIRED,
            CONF_KERNEL_TOKEN_ID: result.token_id,
        }
        await self.async_set_unique_id(f"kernel_{result.device_id}")
        if self.source in (config_entries.SOURCE_REAUTH, config_entries.SOURCE_RECONFIGURE):
            entry = (
                self._get_reauth_entry()
                if self.source == config_entries.SOURCE_REAUTH
                else self._get_reconfigure_entry()
            )
            try:
                self._abort_if_other_kernel(entry, result.device_id)
            except AbortFlow:
                # The code was redeemed on a kernel this entry is not for:
                # the bearer it minted would otherwise stay live with nobody
                # holding it. Revoke it (best effort) before refusing.
                await self._async_revoke_quietly(url, result)
                raise
            # The pin stays: a key the kernel changed is the coordinator's
            # Repairs issue to raise, not something a re-pairing adopts.
            if not (getattr(entry, "data", None) or {}).get(CONF_KERNEL_VERIFYING_KEY):
                paired[CONF_KERNEL_VERIFYING_KEY] = key
                paired[CONF_KERNEL_KEY_PINNED_BUCKET_START] = _bucket_start_now()
            paired[CONF_KERNEL_DEVICE_ID] = result.device_id
            return self.async_update_reload_and_abort(
                entry, data_updates=paired, unique_id=f"kernel_{result.device_id}"
            )
        for entry in self._async_current_entries():
            if getattr(entry, "unique_id", None) == f"kernel_{result.device_id}":
                await self._async_revoke_quietly(url, result)
                break
        self._abort_if_unique_id_configured()
        announce = self._announce
        return await self._async_create_or_promote(
            _kernel_entry_data(
                url=url,
                auth_mode=AUTH_MODE_PAIRED,
                probe_device_id=result.device_id,
                verifying_key=key,
                token=result.token,
                token_id=result.token_id,
                topic_prefix=announce.mqtt_topic_prefix if announce else DEFAULT_MQTT_TOPIC_PREFIX,
                enable_mqtt=True,
                setup_mode=SETUP_MODE_BOTH,
            )
        )

    async def async_step_pair(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Enter the 6-digit code `docker exec securacv pair` printed."""
        return await self._async_pair_step("pair", user_input)

    # ── the token-file form (announce in token_file mode, reauth, reconfigure)

    async def _async_token_file_step(
        self, step_id: str, user_input: dict[str, Any] | None, *, entry: ConfigEntry | None
    ) -> ConfigFlowResult:
        """URL + token file (or static token), probed before anything is stored."""
        errors: dict[str, str] = {}
        entry_data = (getattr(entry, "data", None) or {}) if entry is not None else {}
        if user_input is not None:
            errors = _kernel_auth_errors(user_input)
            probe: KernelProbe | None = None
            if not errors:
                try:
                    probe = await _async_probe_kernel(self.hass, user_input)
                except CannotConnect:
                    errors["base"] = "cannot_connect"
                except InvalidAuth:
                    errors["base"] = "invalid_auth"
                except IdentityUnavailable:
                    errors["base"] = "kernel_identity_unavailable"
            if not errors and probe is not None:
                announce = self._announce
                if announce is not None and (
                    announce.verifying_key != probe.verifying_key
                    or announce.device_id != probe.device_id
                ):
                    return self.async_abort(
                        reason="announce_key_mismatch",
                        description_placeholders={
                            "pinned_fingerprint": announce.fingerprint,
                            "received_fingerprint": kernel_key_fingerprint(probe.verifying_key),
                        },
                    )
                await self.async_set_unique_id(f"kernel_{probe.device_id}")
                if entry is not None:
                    self._abort_if_other_kernel(entry, probe.device_id)
                    updates: dict[str, Any] = {
                        CONF_URL: user_input[CONF_URL],
                        CONF_TOKEN: user_input.get(CONF_TOKEN, ""),
                        CONF_TOKEN_FILE: user_input.get(CONF_TOKEN_FILE) or None,
                        CONF_AUTH_MODE: _auth_mode_for(user_input),
                        CONF_KERNEL_DEVICE_ID: probe.device_id,
                    }
                    if CONF_MQTT_TOPIC_PREFIX in user_input:
                        updates[CONF_MQTT_TOPIC_PREFIX] = (
                            user_input.get(CONF_MQTT_TOPIC_PREFIX) or DEFAULT_MQTT_TOPIC_PREFIX
                        )
                    if CONF_ADAPTER_STATS_URL in user_input:
                        updates[CONF_ADAPTER_STATS_URL] = (
                            user_input.get(CONF_ADAPTER_STATS_URL) or None
                        )
                    if not entry_data.get(CONF_KERNEL_VERIFYING_KEY):
                        updates[CONF_KERNEL_VERIFYING_KEY] = probe.verifying_key
                        updates[CONF_KERNEL_KEY_PINNED_BUCKET_START] = _bucket_start_now()
                    return self.async_update_reload_and_abort(
                        entry, data_updates=updates, unique_id=f"kernel_{probe.device_id}"
                    )
                self._abort_if_unique_id_configured()
                return await self._async_create_or_promote(
                    _kernel_entry_data(
                        url=user_input[CONF_URL],
                        auth_mode=_auth_mode_for(user_input),
                        probe_device_id=probe.device_id,
                        verifying_key=probe.verifying_key,
                        token=user_input.get(CONF_TOKEN, ""),
                        token_file=user_input.get(CONF_TOKEN_FILE) or None,
                        topic_prefix=(
                            announce.mqtt_topic_prefix if announce else DEFAULT_MQTT_TOPIC_PREFIX
                        ),
                        enable_mqtt=True,
                        setup_mode=SETUP_MODE_BOTH,
                    )
                )
        values = dict(user_input or {})
        if not values:
            values = {
                CONF_URL: entry_data.get(CONF_URL)
                or (self._announce.api_url(self._broker_host()) if self._announce else None)
                or DEFAULT_KERNEL_URL,
                CONF_TOKEN_FILE: entry_data.get(CONF_TOKEN_FILE, DEFAULT_TOKEN_FILE),
                CONF_MQTT_TOPIC_PREFIX: entry_data.get(
                    CONF_MQTT_TOPIC_PREFIX, DEFAULT_MQTT_TOPIC_PREFIX
                ),
                CONF_ADAPTER_STATS_URL: entry_data.get(CONF_ADAPTER_STATS_URL),
            }
        return self.async_show_form(
            step_id=step_id,
            data_schema=_kernel_schema(values, include_prefix=False),
            errors=errors,
            description_placeholders=self._kernel_placeholders(entry),
        )

    async def async_step_token_file(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """An announced kernel in token_file mode: where is its token file?"""
        return await self._async_token_file_step("token_file", user_input, entry=None)

    # ── reauth / reconfigure ─────────────────────────────────────────

    async def async_step_reauth(self, entry_data: Mapping[str, Any]) -> ConfigFlowResult:
        """The coordinator raised ConfigEntryAuthFailed: ask for a credential."""
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """A new pairing code for a paired entry; a token file path otherwise."""
        entry = self._get_reauth_entry()
        data = getattr(entry, "data", None) or {}
        if data.get(CONF_AUTH_MODE) == AUTH_MODE_PAIRED:
            if self._pair_url is None:
                self._pair_url = data.get(CONF_URL)
            return await self._async_pair_step("reauth_confirm", user_input)
        return await self._async_token_file_step("reauth_confirm", user_input, entry=entry)

    async def async_step_reconfigure(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Move an entry to a new URL / credential / prefix; same kernel only."""
        entry = self._get_reconfigure_entry()
        data = getattr(entry, "data", None) or {}
        if data.get(CONF_AUTH_MODE) == AUTH_MODE_PAIRED:
            if self._pair_url is None:
                self._pair_url = data.get(CONF_URL)
            return await self._async_pair_step("reconfigure", user_input)
        return await self._async_token_file_step("reconfigure", user_input, entry=entry)

    # ── the Canary confirm ───────────────────────────────────────────

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

        self._set_confirm_only()
        return self.async_show_form(
            step_id="confirm",
            data_schema=vol.Schema({}),
            description_placeholders={"prefix": prefix},
        )

    # ── Supervisor discovery (the app) ───────────────────────────────

    async def async_step_hassio(self, discovery_info: Any) -> ConfigFlowResult:
        """Handle Supervisor add-on discovery (contract C6).

        The app POSTs discovery service "securacv" with config {host, port,
        token_file, mqtt_topic_prefix, device_id}. Accept the HassioServiceInfo
        defensively (attribute-style or plain dict) so older cores and test
        stubs both work. A legacy app that omits `device_id` is asked for
        its identity through the token file before the unique_id is set.
        """
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
        token_file = str(config.get("token_file") or DEFAULT_TOKEN_FILE)
        topic_prefix = str(config.get("mqtt_topic_prefix") or DEFAULT_MQTT_TOPIC_PREFIX)
        device_id = config.get("device_id")

        # flow_title is "SecuraCV {name}"; the app announces itself as
        # "SecuraCV", so the discovered card reads "SecuraCV app" rather
        # than the name twice. The confirm step still shows the app's name.
        name = getattr(discovery_info, "name", None) or config.get("addon") or "SecuraCV"
        self.context["title_placeholders"] = {"name": "app"}
        self._hassio_name = str(name)

        probe: KernelProbe | None = None
        if not valid_kernel_device_id(device_id):
            try:
                probe = await _async_probe_kernel(
                    self.hass, {CONF_URL: url, CONF_TOKEN: "", CONF_TOKEN_FILE: token_file}
                )
            except (CannotConnect, InvalidAuth, IdentityUnavailable):
                probe = None
            device_id = probe.device_id if probe is not None else None

        hassio_data = {
            CONF_URL: url,
            CONF_TOKEN: "",
            CONF_TOKEN_FILE: token_file,
            CONF_AUTH_MODE: AUTH_MODE_TOKEN_FILE,
            CONF_MQTT_TOPIC_PREFIX: topic_prefix,
            CONF_ENABLE_MQTT: True,
            CONF_MQTT_PREFIX: DEFAULT_MQTT_PREFIX,
            CONF_SETUP_MODE: SETUP_MODE_BOTH,
        }

        if device_id is None:
            # Neither the payload nor the kernel could say which kernel this
            # is. An entry that exists can at least follow the new URL
            # (legacy keyed, so the base class cannot do it for us).
            legacy = self._legacy_kernel_entry()
            if legacy is not None:
                merged = {**(getattr(legacy, "data", None) or {}), **hassio_data}
                self.hass.config_entries.async_update_entry(legacy, data=merged)
                self.hass.config_entries.async_schedule_reload(legacy.entry_id)
                return self.async_abort(reason="already_configured")
            return self.async_abort(reason="cannot_connect")

        self._hassio = {**hassio_data, CONF_KERNEL_DEVICE_ID: device_id, "probe": probe}
        await self.async_set_unique_id(f"kernel_{device_id}")
        self._abort_if_unique_id_configured(
            updates={
                CONF_URL: url,
                CONF_TOKEN_FILE: token_file,
                CONF_MQTT_TOPIC_PREFIX: topic_prefix,
                CONF_KERNEL_DEVICE_ID: device_id,
            },
            reload_on_update=True,
        )
        legacy = self._legacy_kernel_entry()
        if legacy is not None:
            # Keyed on the URL or `hassio` by an older version: adopt the
            # identity now so the base class handles the next discovery.
            data = {**hassio_data, CONF_KERNEL_DEVICE_ID: device_id}
            if probe is not None and not (getattr(legacy, "data", None) or {}).get(
                CONF_KERNEL_VERIFYING_KEY
            ):
                data[CONF_KERNEL_VERIFYING_KEY] = probe.verifying_key
                data[CONF_KERNEL_KEY_PINNED_BUCKET_START] = _bucket_start_now()
            return self._async_promote_entry(legacy, data)
        return await self.async_step_hassio_confirm()

    async def async_step_hassio_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Zero-input confirmation for Supervisor add-on discovery."""
        info = self._hassio
        if info is None:
            return self.async_abort(reason="invalid_discovery_info")
        errors: dict[str, str] = {}
        if user_input is not None:
            probe: KernelProbe | None = info.get("probe")
            if probe is None:
                try:
                    probe = await _async_probe_kernel(self.hass, info)
                except InvalidAuth:
                    errors["base"] = "invalid_auth"
                except (CannotConnect, IdentityUnavailable):
                    errors["base"] = "cannot_connect"
            if probe is not None:
                return await self._async_create_or_promote(
                    _kernel_entry_data(
                        url=info[CONF_URL],
                        auth_mode=AUTH_MODE_TOKEN_FILE,
                        probe_device_id=probe.device_id,
                        verifying_key=probe.verifying_key,
                        token_file=info[CONF_TOKEN_FILE],
                        topic_prefix=info[CONF_MQTT_TOPIC_PREFIX],
                        enable_mqtt=True,
                        setup_mode=SETUP_MODE_BOTH,
                    )
                )
        self._set_confirm_only()
        return self.async_show_form(
            step_id="hassio_confirm",
            data_schema=vol.Schema({}),
            errors=errors,
            description_placeholders={
                "name": self._hassio_name,
                "url": info[CONF_URL],
            },
        )


# =============================================================================
# Options flow — device PKI management (pin / rotate / unpin) + settings
# =============================================================================
#
# The "Configure" button on the integration card opens this flow.
# It surfaces a menu of trust-store actions; each action drills into a
# dedicated step that takes the device_id + pubkey hex needed to apply
# it. Storage round-trips happen through TrustStore.async_pin / rotate
# / unpin — the flow is just the UX layer.
#
# We deliberately don't try to fetch the key from the device inside
# the options flow: the device's IP isn't necessarily known to HA, and
# only canary-wap serves /api/device/enroll at all. The pubkey hex is
# pasted verbatim from wherever the product shows it out of band
# (canary-wap's /enroll page; USB serial `j` on the firmware/canary
# build and canary-vision; canary-sense and canary-sentinel show only a
# fingerprint) — docs/device_trust.md, "Where each product shows its
# key".


CONF_PIN_DEVICE_ID = "device_id"
CONF_PIN_PUBKEY_HEX = "pubkey_hex"

PIN_ACTION_PIN = "pin"
PIN_ACTION_ROTATE = "rotate"
PIN_ACTION_UNPIN = "unpin"
OPTIONS_SETTINGS = "settings"


def _looks_like_pubkey_hex(value: str) -> bool:
    """64-char lowercase hex. We lowercase before checking so users
    can paste from any case-preserving source (`/enroll` and the serial
    `j` manifest emit lowercase, but canary-wap's serial `i` prints
    capitals and a manual transcription often comes back uppercase)."""
    v = value.strip().lower()
    if len(v) != 64:
        return False
    try:
        bytes.fromhex(v)
        return True
    except ValueError:
        return False


class SecuraCVOptionsFlow(OptionsFlow):
    """Per-entry trust-store management surface, plus the export settings.

    No ``__init__``: the entry is ``self.config_entry``, which the core
    resolves from the flow's handler once the flow is started.
    """

    async def async_step_init(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Top-level menu — pin / rotate / unpin a device key, or settings."""
        return self.async_show_menu(
            step_id="init",
            menu_options=[PIN_ACTION_PIN, PIN_ACTION_ROTATE, PIN_ACTION_UNPIN, OPTIONS_SETTINGS],
        )

    async def async_step_settings(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """How many evidence exports to keep on disk."""
        errors: dict[str, str] = {}
        current = int(self.config_entry.options.get(CONF_EVIDENCE_KEEP, DEFAULT_EVIDENCE_KEEP))
        if user_input is not None:
            try:
                keep = int(user_input.get(CONF_EVIDENCE_KEEP, current))
            except (TypeError, ValueError):
                keep = -1
            if MIN_EVIDENCE_KEEP <= keep <= MAX_EVIDENCE_KEEP:
                return self.async_create_entry(
                    title="", data={**self.config_entry.options, CONF_EVIDENCE_KEEP: keep}
                )
            errors[CONF_EVIDENCE_KEEP] = "invalid_evidence_keep"
            current = keep if keep > 0 else current
        return self.async_show_form(
            step_id=OPTIONS_SETTINGS,
            data_schema=vol.Schema({vol.Required(CONF_EVIDENCE_KEEP, default=current): int}),
            errors=errors,
        )

    def _forget_replay_marks(self, device_id: str) -> None:
        """Drop the replay high-water marks for a device whose key just
        changed (pin / rotate / unpin). The signed counters the replay gate
        compares belong to the key, so a new key starts them over — kept in
        step with `async_clear_replay_marks` in __init__.py without
        importing the package (the flow module is loaded on its own)."""
        runtime = runtime_for(self.config_entry)
        if runtime is not None:
            runtime.replay.pop(device_id, None)

    def _forget_mismatch_notifications(self, device_id: str) -> None:
        """Drop the mismatch-notification dedup keys for a device whose key
        just changed (pin / rotate / unpin). Without this, if a spoofed fp
        triggered a notification before the operator acted, the dedup set
        would permanently suppress a future real mismatch on that same fp,
        breaking the "warn loudly" guarantee. Pin is a key-changing path too
        — it replaces a TOFU pin with a manual one — and is the action an
        alarmed operator is most likely to reach for."""
        runtime = runtime_for(self.config_entry)
        if runtime is None:
            return
        notified = runtime.mismatch_notified
        notified.difference_update({(d, f) for (d, f) in notified if d == device_id})

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
                    # A new key means new counters: forget the replay
                    # high-water marks or the device's first publishes
                    # under this key all read as replays of the old one.
                    self._forget_replay_marks(device_id)
                    self._forget_mismatch_notifications(device_id)
                    return self.async_create_entry(title="", data=dict(self.config_entry.options))

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
                    self._forget_replay_marks(device_id)
                    # Clear any stuck mismatch notification for this
                    # device so the operator's rotation takes effect
                    # immediately on the UI side.
                    self._forget_mismatch_notifications(device_id)
                    return self.async_create_entry(title="", data=dict(self.config_entry.options))

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
                    # Whatever key TOFU-pins next starts its counters over.
                    self._forget_replay_marks(device_id)
                    self._forget_mismatch_notifications(device_id)
                    return self.async_create_entry(title="", data=dict(self.config_entry.options))

        return self.async_show_form(
            step_id="unpin",
            data_schema=vol.Schema({vol.Required(CONF_PIN_DEVICE_ID): str}),
            errors=errors,
        )

    def _trust_store(self):
        """Lazy accessor — the runtime is created in async_setup_entry, so
        it always exists by the time the options flow runs (the button is
        only enabled after setup)."""
        runtime = runtime_for(self.config_entry)
        return runtime.trust_store if runtime is not None else None
