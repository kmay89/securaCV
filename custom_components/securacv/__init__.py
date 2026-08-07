"""SecuraCV - Privacy Witness Kernel integration for Home Assistant.

Connects to SecuraCV Canary devices via MQTT and/or the Privacy Witness
Kernel via HTTP API. Surfaces semantic witness events, hash chain integrity,
and device health — never raw video or identity data. Privacy by design.

Setup modes:
  - MQTT only: Auto-discovers Canary devices via MQTT (recommended)
  - Kernel only: Polls the PWK Event API via HTTP
  - Both: MQTT for Canary devices + HTTP for the kernel
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

import json

from .device_trust import TrustStore, TrustVerdict
from .const import (
    DOMAIN,
    CONF_MQTT_PREFIX,
    CONF_ENABLE_MQTT,
    CONF_SETUP_MODE,
    CONF_ADAPTER_STATS_URL,
    CONF_TOKEN_FILE,
    TOPIC_STATUS,
    MANUFACTURER,
    MODEL_KERNEL,
    MODEL_CANARY,
    SETUP_MODE_KERNEL,
    SETUP_MODE_BOTH,
)

_LOGGER = logging.getLogger(__name__)

PLATFORMS: list[Platform] = [Platform.SENSOR, Platform.BINARY_SENSOR]
DEFAULT_UPDATE_INTERVAL = timedelta(seconds=30)

# Untrusted-broker hardening: cap how much of any single MQTT payload this
# integration will decode/parse. Real device publishes are well under 8 KiB;
# anything larger is malformed or hostile and gets dropped before json.loads.
MAX_MQTT_PAYLOAD_BYTES = 64 * 1024

# Lovelace cards (custom_components/securacv/www/). Served and auto-loaded
# best-effort so `type: custom:securacv-timeline-card` /
# `custom:securacv-aim-card` resolve without the user hand-adding a frontend
# resource.
LOVELACE_CARD_FILENAMES = (
    "securacv-timeline-card.js",
    "securacv-aim-card.js",
)
TIMELINE_CARD_FILENAME = LOVELACE_CARD_FILENAMES[0]
TIMELINE_CARD_URL = f"/{DOMAIN}_www/{TIMELINE_CARD_FILENAME}"


async def _async_register_frontend(hass: HomeAssistant) -> None:
    """Serve and auto-load the Lovelace cards. Never fatal.

    The frontend is optional — the integration is just as useful headless — so
    any registration hiccup is logged at debug and swallowed rather than failing
    setup. Runs once per HA instance (guarded in hass.data), and tolerates the
    static-path API change across HA versions.
    """
    domain_data = hass.data.setdefault(DOMAIN, {})
    if domain_data.get("_frontend_registered"):
        return
    domain_data["_frontend_registered"] = True
    try:
        from pathlib import Path

        registered_any = False
        for filename in LOVELACE_CARD_FILENAMES:
            card_path = Path(__file__).parent / "www" / filename
            if not card_path.is_file():
                continue
            card_url = f"/{DOMAIN}_www/{filename}"
            try:
                from homeassistant.components.http import StaticPathConfig

                await hass.http.async_register_static_paths(
                    [StaticPathConfig(card_url, str(card_path), False)]
                )
            except (ImportError, AttributeError):
                # Older HA without the bulk async API.
                hass.http.register_static_path(card_url, str(card_path), False)

            from homeassistant.components.frontend import add_extra_js_url

            add_extra_js_url(hass, card_url)
            registered_any = True
            _LOGGER.debug("SecuraCV card registered at %s", card_url)
        if not registered_any:
            domain_data["_frontend_registered"] = False
    except Exception:  # noqa: BLE001 - frontend is optional, never block setup
        domain_data["_frontend_registered"] = False
        _LOGGER.debug("SecuraCV Lovelace cards not registered", exc_info=True)


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
        self,
        base_url: str,
        token: str,
        session: aiohttp.ClientSession,
        token_file: str | None = None,
    ) -> None:
        """Initialize the API client."""
        self._base_url = base_url.rstrip("/")
        self._token = token
        self._session = session
        self._token_file = token_file
        self._refresh_lock = asyncio.Lock()

    def _read_token_file(self) -> str | None:
        """Blocking read of the token file (run in executor)."""
        if not self._token_file:
            return None
        try:
            with open(self._token_file, encoding="utf-8") as fh:
                # The token is 64 hex chars; cap the read so a misconfigured
                # path (device node, huge file) can't balloon memory.
                token = fh.read(4096).strip()
        except OSError as err:
            _LOGGER.debug("could not read token file %s: %s", self._token_file, err)
            return None
        return token or None

    async def _async_refresh_token(self, current_token: str) -> bool:
        """Re-read the token file; True if a token differing from
        `current_token` is now in place (whether loaded here or by a
        concurrent task that refreshed first).

        The kernel rotates its capability token every 10-minute bucket and
        rewrites the token file (src/api/mod.rs, privacy_witness_kernel/run.sh
        writes it to /config/api_token). A statically configured token
        therefore goes stale within minutes; when a token file is configured
        we re-read it on 401 so the integration follows the rotation.
        """
        if not self._token_file:
            return False
        async with self._refresh_lock:
            if self._token != current_token:
                # Another task already refreshed while we waited; retry
                # with what it loaded instead of re-reading the file.
                return True
            loop = asyncio.get_running_loop()
            token = await loop.run_in_executor(None, self._read_token_file)
            if token is None or token == current_token:
                return False
            self._token = token
            return True

    async def _async_get_json(
        self, path: str, *, none_on_404: bool = False
    ) -> dict[str, Any] | None:
        """GET a kernel endpoint, retrying once with a re-read token on 401."""
        # Token-file-only setups start with an empty token; prime it from the
        # file so the first request isn't a guaranteed 401 round-trip (and so
        # a restart across a rotation boundary still has the one 401-retry in
        # reserve for a stale file).
        if not self._token and self._token_file:
            await self._async_refresh_token("")
        url = f"{self._base_url}{path}"
        for attempt in (0, 1):
            current_token = self._token
            headers = {"Authorization": f"Bearer {current_token}"}
            try:
                async with self._session.get(
                    url, headers=headers, timeout=10
                ) as resp:
                    if resp.status == 401:
                        if attempt == 0 and await self._async_refresh_token(
                            current_token
                        ):
                            continue
                        raise SecuraCVApiAuthError("unauthorized")
                    if none_on_404 and resp.status == 404:
                        return None
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
        raise SecuraCVApiAuthError("unauthorized")

    async def async_get_events(self) -> dict[str, Any]:
        """Fetch events from the kernel Event API."""
        data = await self._async_get_json("/events")
        return data if data is not None else {}

    async def async_get_latest_event(self) -> dict[str, Any] | None:
        """Fetch the latest event from the kernel."""
        return await self._async_get_json("/events/latest", none_on_404=True)

    async def async_get_status(self) -> dict[str, Any] | None:
        """Fetch the kernel's storage endurance & health report.

        Returns None when the kernel predates the /status endpoint or has
        storage-health monitoring disabled (404), so storage sensors stay
        empty instead of erroring on older kernels.
        """
        return await self._async_get_json("/status", none_on_404=True)

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
            latest_event = await self.api.async_get_latest_event()
        except SecuraCVApiError as err:
            raise UpdateFailed(str(err)) from err
        # Storage health is auxiliary: a failure here must never take down
        # the event pipeline, so degrade to None rather than UpdateFailed.
        try:
            status = await self.api.async_get_status()
        except SecuraCVApiError:
            status = None
        return {"latest_event": latest_event, "status": status}


class SecuraCVAdapterStatsCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Polls the adapter_host read-only stats endpoint (operational counters only)."""

    def __init__(
        self, hass: HomeAssistant, url: str, session: aiohttp.ClientSession
    ) -> None:
        """Initialize the coordinator."""
        super().__init__(
            hass,
            logger=_LOGGER,
            name="SecuraCV Adapter Stats",
            update_interval=DEFAULT_UPDATE_INTERVAL,
        )
        self._url = url.rstrip("/")
        self._session = session

    async def _async_update_data(self) -> dict[str, Any]:
        """Fetch the per-adapter stats JSON. Returns {} only on an empty body."""
        try:
            async with self._session.get(self._url, timeout=10) as resp:
                if resp.status != 200:
                    raise UpdateFailed(f"stats endpoint status {resp.status}")
                # The endpoint sends application/json; tolerate a missing/odd content-type.
                return await resp.json(content_type=None)
        # ValueError covers a misconfigured URL (e.g. missing scheme) raised by aiohttp.
        except (aiohttp.ClientError, asyncio.TimeoutError, ValueError) as err:
            raise UpdateFailed(f"unable to reach adapter stats endpoint: {err}") from err


async def async_migrate_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Migrate old config entries to the current version.

    The config flow declares VERSION = 2. Without this handler, HA refuses
    to load any entry stored with a lower version ("Migration handler not
    found"), permanently bricking it on upgrade. Version 1 predates this
    repository's history and used a compatible data schema, so the
    migration is a straight version bump.
    """
    if entry.version > 2:
        # Downgrade from a future version — refuse rather than guess.
        return False
    if entry.version < 2:
        hass.config_entries.async_update_entry(entry, version=2)
        _LOGGER.info("Migrated SecuraCV config entry from version %s to 2", entry.version)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up SecuraCV from a config entry."""
    hass.data.setdefault(DOMAIN, {})

    # Serve + auto-load the Lovelace timeline card (best-effort, non-fatal).
    await _async_register_frontend(hass)

    setup_mode = entry.data.get(CONF_SETUP_MODE, SETUP_MODE_KERNEL)
    has_kernel = setup_mode in (SETUP_MODE_KERNEL, SETUP_MODE_BOTH)
    enable_mqtt = entry.data.get(CONF_ENABLE_MQTT, False)

    # Initialize the HTTP-API coordinator iff a kernel is configured.
    # MQTT-only mode has no HTTP surface, so there's no coordinator to
    # build; the kernel sensor platforms gate themselves on coordinator
    # presence in entry_data.
    if has_kernel:
        session = async_get_clientsession(hass)
        api = SecuraCVApi(
            entry.data[CONF_URL],
            entry.data.get(CONF_TOKEN, ""),
            session,
            token_file=entry.data.get(CONF_TOKEN_FILE),
        )
        # Type is inferred as `SecuraCVCoordinator | None` across this if/else;
        # within this branch it's known non-None, so the refresh call is safe.
        coordinator = SecuraCVCoordinator(hass, api)
        await coordinator.async_config_entry_first_refresh()
    else:
        api = None
        coordinator = None

    # Optional adapter_host stats endpoint (read-only operational counters). Setup must not fail
    # if it is unreachable, so we use a tolerant refresh rather than first_refresh.
    adapter_stats_url = entry.data.get(CONF_ADAPTER_STATS_URL)
    adapter_stats_coordinator: SecuraCVAdapterStatsCoordinator | None = None
    if adapter_stats_url:
        adapter_stats_coordinator = SecuraCVAdapterStatsCoordinator(
            hass, adapter_stats_url, async_get_clientsession(hass)
        )
        await adapter_stats_coordinator.async_refresh()

    # Trust store — persisted Ed25519 pubkey pins per device_id.
    # Created (and storage loaded) before the MQTT subscribe so the
    # first inbound message can already consult it for TOFU pinning.
    trust_store = TrustStore(hass, entry.entry_id)
    await trust_store.async_load()

    # Store entry data
    entry_data: dict[str, Any] = {
        "api": api,
        "coordinator": coordinator,
        "adapter_stats_coordinator": adapter_stats_coordinator,
        "devices": {},
        "unsub_mqtt": [],
        "setup_mode": setup_mode,
        "trust_store": trust_store,
        # Per-device verify state cache. Sensors read this to set
        # extra_state_attributes["verified"] on their entities.
        # Shape: { device_id: { "trusted": bool, "reason": str,
        #                       "pinned_fingerprint": str|None,
        #                       "received_fingerprint": str|None } }
        "verify": {},
        # Mismatches we've already surfaced as persistent_notification —
        # one entry per (device_id, fp) so we don't spam the user.
        "mismatch_notified": set(),
    }
    hass.data[DOMAIN][entry.entry_id] = entry_data

    # Register the kernel as a device (only if kernel mode)
    if has_kernel:
        dev_registry = dr.async_get(hass)
        dev_registry.async_get_or_create(
            config_entry_id=entry.entry_id,
            identifiers={(DOMAIN, entry.data[CONF_URL])},
            manufacturer=MANUFACTURER,
            model=MODEL_KERNEL,
            name="SecuraCV Privacy Witness Kernel",
            configuration_url=entry.data[CONF_URL],
        )

    # Set up MQTT subscriptions for Canary devices
    mqtt_prefix = entry.data.get(CONF_MQTT_PREFIX)

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
            # Health-topic subscription: snags the device's pubkey_hex
            # the first time we see it and TOFU-pins via the trust
            # store. Sensor.py also subscribes to health for its own
            # state — that's fine, MQTT allows multiple subscribers
            # per topic and the two callbacks operate on independent
            # entry_data slices.
            unsub_health = await mqtt.async_subscribe(
                hass,
                f"{mqtt_prefix}/+/health",
                _async_health_for_tofu(hass, entry),
            )
            entry_data["unsub_mqtt"].append(unsub_health)
            _LOGGER.info("SecuraCV MQTT subscriptions active (prefix: %s)", mqtt_prefix)
        except Exception as err:
            _LOGGER.warning("MQTT setup failed, continuing without MQTT: %s", err)

    # The watch tick: evaluates every watch, delivers what fired, and
    # announces expiry. Without it a started watch would be recorded but
    # not actually watching, and its spoken promise would be untrue.
    try:
        from homeassistant.helpers.event import async_track_time_interval

        from .watch_runtime import TICK_INTERVAL_SECONDS, async_tick

        entry_data["unsub_mqtt"].append(
            async_track_time_interval(
                hass,
                lambda _now: async_tick(hass),
                timedelta(seconds=TICK_INTERVAL_SECONDS),
            )
        )
    except Exception:  # noqa: BLE001 - watches are optional, setup is not
        _LOGGER.debug("watch tick not scheduled", exc_info=True)

    # Forward to platforms
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    _LOGGER.info(
        "SecuraCV integration loaded (mode: %s, mqtt: %s)",
        setup_mode,
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


def _safe_config_url(ap_ip: Any) -> str | None:
    """Build a configuration_url from a broker-supplied address, or None.

    The `ap_ip` field arrives over MQTT and is untrusted: it becomes a
    clickable link on the HA device page. Devices only ever report a LAN
    address (their AP/STA IP, e.g. 192.168.4.1) or their mDNS hostname
    (canary-<id>.local), so accept exactly those forms: a private or
    link-local IP address, or a `.local` hostname. Public IPs, arbitrary
    hostnames, and anything with URL syntax (scheme, port, path,
    credentials) are rejected — a hostile broker must not be able to plant
    an off-LAN phishing link.
    """
    if not isinstance(ap_ip, str) or not ap_ip or len(ap_ip) > 253:
        return None
    import ipaddress
    import re

    try:
        ip = ipaddress.ip_address(ap_ip)
    except ValueError:
        pass
    else:
        if not (ip.is_private or ip.is_link_local):
            return None
        # IPv6 literals need brackets in URLs.
        return f"http://[{ap_ip}]" if ip.version == 6 else f"http://{ap_ip}"
    if ap_ip.lower().endswith(".local") and re.fullmatch(
        r"[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?)+",
        ap_ip,
    ):
        return f"http://{ap_ip}"
    return None


def _async_device_status_received(hass: HomeAssistant, entry: ConfigEntry):
    """Return callback for Canary device status MQTT messages."""

    @callback
    def _callback(msg: mqtt.ReceiveMessage) -> None:
        """Handle device status message - register Canary device in HA."""
        # Topic format: {prefix}/{device_id}/status
        parts = msg.topic.split("/")
        if len(parts) < 3:
            return
        if len(msg.payload) > MAX_MQTT_PAYLOAD_BYTES:
            return

        device_id = parts[-2]
        entry_data = hass.data[DOMAIN].get(entry.entry_id)
        if not entry_data:
            return

        devices = entry_data["devices"]
        status_payload = msg.payload.decode() if isinstance(msg.payload, bytes) else str(msg.payload)

        if device_id not in devices:
            _LOGGER.info("Discovered SecuraCV Canary device: %s", device_id)
            devices[device_id] = {"status": status_payload}

            # Parse status for device info enrichment
            fw_version = None
            hw_version = None
            config_url = None
            friendly_name = None
            try:
                status_data = json.loads(status_payload) if isinstance(status_payload, str) else {}
                if not isinstance(status_data, dict):
                    status_data = {}
                fw_version = status_data.get("firmware_version") or status_data.get("fw_version")
                hw_version = status_data.get("hardware") or status_data.get("board")
                friendly_name = status_data.get("device_name") or status_data.get("name")
                config_url = _safe_config_url(
                    status_data.get("ap_ip") or status_data.get("ip")
                )
            except (json.JSONDecodeError, TypeError):
                pass

            display_name = friendly_name or f"SecuraCV Canary {device_id}"

            dev_registry = dr.async_get(hass)
            dev_registry.async_get_or_create(
                config_entry_id=entry.entry_id,
                identifiers={(DOMAIN, f"canary_{device_id}")},
                manufacturer=MANUFACTURER,
                model=MODEL_CANARY,
                name=display_name,
                sw_version=fw_version,
                hw_version=hw_version,
                configuration_url=config_url,
            )
        else:
            devices[device_id]["status"] = status_payload

            try:
                status_data = json.loads(status_payload) if isinstance(status_payload, str) else {}
                if not isinstance(status_data, dict):
                    status_data = {}
                fw = status_data.get("firmware_version") or status_data.get("fw_version")
                if fw and fw != devices[device_id].get("_last_fw"):
                    devices[device_id]["_last_fw"] = fw
                    dev_registry = dr.async_get(hass)
                    hw = status_data.get("hardware") or status_data.get("board")
                    dev_registry.async_get_or_create(
                        config_entry_id=entry.entry_id,
                        identifiers={(DOMAIN, f"canary_{device_id}")},
                        sw_version=fw,
                        hw_version=hw,
                        configuration_url=_safe_config_url(
                            status_data.get("ap_ip") or status_data.get("ip")
                        ),
                    )
            except (json.JSONDecodeError, TypeError):
                pass

    return _callback


# =============================================================================
# Trust / verification helpers
# =============================================================================
#
# The signed-MQTT path on the firmware side (PR adding per-device PKI)
# stamps every chain/events/counts publish with an Ed25519 signature
# the device produced over a canonical message. HA verifies that sig
# against the device's pinned pubkey from the TrustStore.
#
# The sensor + binary_sensor handlers call `async_handle_signed_payload`
# inside their MQTT callbacks with the parsed payload and a verifier
# function. The helper:
#   1. TOFU-pins on first sight (when the payload carries a pubkey we
#      can use, via a /api/device/enroll round-trip kicked off async).
#   2. Calls the kind-specific verifier (signature.verify_chain etc.).
#   3. Stamps the result into entry_data["verify"][device_id] so any
#      entity reading it can surface "verified: true/false".
#   4. Fires a one-shot persistent_notification on mismatch (dedup'd
#      via entry_data["mismatch_notified"]).
#
# We deliberately *accept* the payload regardless of verdict — the
# scope decision is "warn loudly, accept" so a benign re-flash doesn't
# stall a live deployment.


def _async_health_for_tofu(hass: HomeAssistant, entry: ConfigEntry):
    """Subscriber for `{prefix}/+/health` that TOFU-pins the device's
    pubkey the first time it appears.

    The health publish has always carried `public_key` (64-char hex)
    because the HA dashboard's health sensor already surfaces it as an
    attribute. We piggy-back on it for the trust store.

    Trust model — be honest about the limits of TOFU: first-sight pinning
    trusts whoever publishes to `{prefix}/{device_id}/health` FIRST. Any
    client with publish access to the broker (or a hostile broker) can
    pre-emptively pin its own key for a device_id before the genuine
    device connects, after which its spoofed publishes verify green.
    TOFU therefore upgrades an *honest* broker from "trust every payload"
    to "detect later tampering"; it does not defend against a broker (or
    co-tenant publisher) that is hostile from the start. Users whose
    threat model includes a hostile broker must pin keys manually from
    the device's /enroll page (Options → Pin a device pubkey) and should
    use broker ACLs to restrict who may publish under the prefix.
    Subsequent publishes are verified against the pin; the warn-loudly-
    accept policy handles the "device legitimately re-flashed" case.
    """

    @callback
    def _callback(msg: mqtt.ReceiveMessage) -> None:
        parts = msg.topic.split("/")
        if len(parts) < 3:
            return
        if len(msg.payload) > MAX_MQTT_PAYLOAD_BYTES:
            return
        device_id = parts[-2]
        entry_data = hass.data[DOMAIN].get(entry.entry_id)
        if not entry_data:
            return
        trust_store: TrustStore = entry_data["trust_store"]
        if trust_store.is_pinned(device_id):
            return
        try:
            payload = msg.payload.decode() if isinstance(msg.payload, bytes) else str(msg.payload)
            data = json.loads(payload)
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if not isinstance(data, dict):
            return
        pubkey_hex = data.get("public_key")
        if not pubkey_hex or not isinstance(pubkey_hex, str) or len(pubkey_hex) != 64:
            return
        try:
            bytes.fromhex(pubkey_hex)
        except ValueError:
            # 64 chars but not hex — would raise later inside the pin task.
            return
        # async_pin needs the loop; schedule as a task so the @callback
        # context returns synchronously.
        hass.async_create_task(
            trust_store.async_tofu_pin_if_unknown(device_id, pubkey_hex)
        )
        _LOGGER.info(
            "TOFU-pinning Canary %s with pubkey %s…", device_id, pubkey_hex[:16]
        )

    return _callback


@callback
def async_record_verify(
    hass: HomeAssistant,
    entry: ConfigEntry,
    device_id: str,
    verdict: "TrustVerdict",
) -> None:
    """Stamp the verify outcome into entry_data and surface mismatches."""
    entry_data = hass.data[DOMAIN].get(entry.entry_id)
    if not entry_data:
        return
    entry_data["verify"][device_id] = {
        "trusted": verdict.trusted,
        "reason": verdict.reason,
        "pinned_fingerprint": verdict.pinned_fingerprint,
        "received_fingerprint": verdict.received_fingerprint,
        "detail": verdict.detail,
    }
    if verdict.reason == "mismatch":
        # Dedup by (device_id, received_fingerprint) so a steady stream
        # of mismatched publishes only notifies the user once. Cleared
        # when the operator either re-pins or unpins the device.
        key = (device_id, verdict.received_fingerprint or "")
        notified: set = entry_data["mismatch_notified"]
        if key in notified:
            return
        notified.add(key)
        # `persistent_notification.create` is awaitable but we're in a
        # @callback context; schedule it on the event loop. The
        # notification ID is stable per device so a follow-up create
        # replaces the previous payload rather than stacking.
        hass.async_create_task(
            hass.services.async_call(
                "persistent_notification",
                "create",
                {
                    "title": f"SecuraCV: device {device_id} key mismatch",
                    "message": (
                        f"Canary `{device_id}` published with fingerprint "
                        f"`{verdict.received_fingerprint}` but the pinned "
                        f"fingerprint is `{verdict.pinned_fingerprint}`. "
                        "Entities are still updating, marked as unverified. "
                        "If you intentionally re-flashed this device, rotate "
                        "the pin from the integration's options menu."
                    ),
                    "notification_id": f"securacv_mismatch_{device_id}",
                },
                blocking=False,
            )
        )


@callback
def async_get_trust_store(hass: HomeAssistant, entry: ConfigEntry) -> TrustStore | None:
    entry_data = hass.data[DOMAIN].get(entry.entry_id)
    if not entry_data:
        return None
    return entry_data.get("trust_store")
