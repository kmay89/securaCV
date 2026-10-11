"""The kernel coordinators: push-fed over MQTT, backstopped over HTTP.

The kernel's MQTT bridge publishes everything the entities show as
retained state under one prefix (``witness/last_event``, ``witness/digest``,
``witness/chain_problem`` + ``/attrs``, ``witness/claim_state``,
``witness/frigate_link``) and its liveness as a last-will on
``witness/status``. ``SecuraCVKernelCoordinator`` subscribes to those and
hands each message to its entities through ``async_set_updated_data`` — no
polling latency, no receipt rows in the kernel's export ledger (D5).

Every five minutes it also walks the authenticated HTTP surface as a
backstop: ``/health`` (liveness), ``/api/identity`` (the verifying key the
TOFU pin is checked against, the chain head, the kernel's own last verify),
``/status`` (storage endurance) and ``/events/latest`` (so a missed push is
noticed, never double-fired). Kernel availability is ``witness/status``
saying ``online`` OR the backstop succeeding, so an app whose bridge is
switched off still reads as reachable through HTTP.

A 401 after the client's one token-file retry raises ``ConfigEntryAuthFailed``
— which starts Home Assistant's own reauth flow — only when it is
definitive: the entry holds a paired or static token (nothing will rotate
it back), or the token file cannot be read at all. A readable file that is
one rotation behind is a transient the next backstop clears.
"""
from __future__ import annotations

import logging
import time
from datetime import timedelta
from typing import Any, Callable

from homeassistant.components import mqtt
from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import ConfigEntryAuthFailed
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

import aiohttp

from . import repairs
from .api import (
    SecuraCVApi,
    SecuraCVApiAuthError,
    SecuraCVApiConnectionError,
    SecuraCVApiError,
)
from .const import (
    AUTH_MODE_PAIRED,
    AUTH_MODE_STATIC,
    CLAIM_STATE_CLAIMED,
    CLAIM_STATE_UNCLAIMED,
    CONF_KERNEL_KEY_PINNED_BUCKET_START,
    CONF_KERNEL_VERIFYING_KEY,
    KERNEL_STATUS_OFFLINE,
    KERNEL_STATUS_ONLINE,
    KERNEL_TOPIC_CHAIN_PROBLEM,
    KERNEL_TOPIC_CHAIN_PROBLEM_ATTRS,
    KERNEL_TOPIC_CLAIM_STATE,
    KERNEL_TOPIC_DIGEST,
    KERNEL_TOPIC_FRIGATE_LINK,
    KERNEL_TOPIC_LAST_EVENT,
    KERNEL_TOPIC_STATUS,
    TIME_BUCKET_SECONDS,
)
from .data import SecuraCVConfigEntry, runtime_for
from .health_metrics import replacement_recommended, storage_status
from .kernel_trust import kernel_key_fingerprint, normalize_kernel_key
from .mqtt_payload import mqtt_payload_text, parse_mqtt_json

_LOGGER = logging.getLogger(__name__)

# The HTTP backstop cadence. Push carries the state; this only catches a
# missed message, re-checks the pin and samples storage.
HEALTH_BACKSTOP_INTERVAL = timedelta(minutes=5)
# How long consecutive backstop failures must last before the kernel is
# reported unreachable in Repairs (a restart, an update, a reboot are shorter).
KERNEL_UNREACHABLE_AFTER = timedelta(minutes=15)
# The adapter_host stats endpoint has no push side; it stays a poll.
ADAPTER_STATS_INTERVAL = timedelta(seconds=30)

# Keys of the coordinator's data dict — stage b reads the same names.
DATA_LATEST_EVENT = "latest_event"
DATA_STATUS = "status"
DATA_IDENTITY = "identity"
DATA_HEALTH = "health"
DATA_DIGEST = "digest"
DATA_CHAIN_PROBLEM = "chain_problem"
DATA_CHAIN_REPORT = "chain_report"
DATA_CLAIM_STATE = "claim_state"
DATA_FRIGATE_LINK = "frigate_link"
DATA_KEY_MISMATCH = "key_mismatch"
DATA_LAST_PUSH_BUCKET_START = "last_push_bucket_start"
DATA_BACKSTOP_BUCKET_START = "backstop_bucket_start"


def bucket_start(now: float | None = None) -> int:
    """The start of the 10-minute bucket `now` falls in (coarse time)."""
    stamp = time.time() if now is None else now
    return int(stamp // TIME_BUCKET_SECONDS) * TIME_BUCKET_SECONDS


def empty_kernel_data() -> dict[str, Any]:
    """The data shape before anything has arrived."""
    return {
        DATA_LATEST_EVENT: None,
        DATA_STATUS: None,
        DATA_IDENTITY: None,
        DATA_HEALTH: None,
        DATA_DIGEST: None,
        DATA_CHAIN_PROBLEM: None,
        DATA_CHAIN_REPORT: None,
        DATA_CLAIM_STATE: None,
        DATA_FRIGATE_LINK: None,
        DATA_KEY_MISMATCH: False,
        DATA_LAST_PUSH_BUCKET_START: None,
        DATA_BACKSTOP_BUCKET_START: None,
    }


def event_bucket_start(event: dict[str, Any] | None) -> int | None:
    """The bucket start of a C5 event payload (nested or flattened), if any."""
    if not isinstance(event, dict):
        return None
    bucket = event.get("time_bucket")
    if isinstance(bucket, dict):
        start = bucket.get("start_epoch_s")
    else:
        start = event.get("time_bucket_start")
    if isinstance(start, bool) or not isinstance(start, int):
        return None
    return start


class SecuraCVKernelCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Kernel state: pushed by the bridge, backstopped over HTTP."""

    def __init__(
        self,
        hass: HomeAssistant,
        entry: SecuraCVConfigEntry,
        api: SecuraCVApi,
        *,
        auth_mode: str,
        topic_prefix: str,
        pinned_key: str | None,
    ) -> None:
        """Initialize the coordinator."""
        super().__init__(
            hass,
            logger=_LOGGER,
            config_entry=entry,
            name="SecuraCV kernel",
            update_interval=HEALTH_BACKSTOP_INTERVAL,
        )
        self.entry = entry
        self.api = api
        self.auth_mode = auth_mode
        self.topic_prefix = topic_prefix
        self.pinned_key = normalize_kernel_key(pinned_key) if pinned_key else None
        # What witness/status last said: None until the retained LWT arrives.
        self.kernel_online: bool | None = None
        # Whether the last HTTP backstop reached the kernel.
        self.backstop_ok = False
        self.key_mismatch = False
        self._unsubs: list[Callable[[], None]] = []
        self._failing_since: float | None = None
        self._unreachable_reported = False
        self._offline_logged = False
        # Topics pushed at least once this load: the backstop then leaves
        # that key to the push feed instead of re-fetching it.
        self._pushed: set[str] = set()
        self.data = empty_kernel_data()

    # ── availability ─────────────────────────────────────────────────

    @property
    def kernel_available(self) -> bool:
        """Kernel entities' availability: the LWT says online, or HTTP answers."""
        return self.kernel_online is True or self.backstop_ok

    @property
    def pinned_fingerprint(self) -> str | None:
        """The pinned key's fingerprint, for the device page and diagnostics."""
        return kernel_key_fingerprint(self.pinned_key) if self.pinned_key else None

    # ── the push feed ───────────────────────────────────────────────

    async def async_subscribe_push(self) -> None:
        """Subscribe to the bridge's retained state topics.

        Called by async_setup_entry after ``mqtt.async_wait_for_mqtt_client``
        succeeded; a failure here is the caller's to report (Repairs
        ``mqtt_subscribe_failed``), the HTTP backstop keeps the entities alive.
        """
        handlers: dict[str, Callable[[mqtt.ReceiveMessage], None]] = {
            KERNEL_TOPIC_LAST_EVENT: self._handle_last_event,
            KERNEL_TOPIC_DIGEST: self._handle_digest,
            KERNEL_TOPIC_CHAIN_PROBLEM: self._handle_chain_problem,
            KERNEL_TOPIC_CHAIN_PROBLEM_ATTRS: self._handle_chain_report,
            KERNEL_TOPIC_STATUS: self._handle_status,
            KERNEL_TOPIC_CLAIM_STATE: self._handle_claim_state,
            KERNEL_TOPIC_FRIGATE_LINK: self._handle_frigate_link,
        }
        for suffix, handler in handlers.items():
            self._unsubs.append(
                await mqtt.async_subscribe(self.hass, f"{self.topic_prefix}/{suffix}", handler)
            )

    @callback
    def async_unsubscribe_push(self) -> None:
        """Release the push subscriptions (unload; idempotent)."""
        for unsub in self._unsubs:
            unsub()
        self._unsubs.clear()

    @callback
    def _async_push(self, key: str, value: Any) -> None:
        """Hand one pushed value to the entities."""
        self._pushed.add(key)
        data = dict(self.data) if self.data else empty_kernel_data()
        data[key] = value
        data[DATA_LAST_PUSH_BUCKET_START] = bucket_start()
        self.async_set_updated_data(data)

    @callback
    def _handle_last_event(self, msg: mqtt.ReceiveMessage) -> None:
        event = parse_mqtt_json(msg.payload)
        if event is None:
            return
        self._async_push(DATA_LATEST_EVENT, event)

    @callback
    def _handle_digest(self, msg: mqtt.ReceiveMessage) -> None:
        digest = parse_mqtt_json(msg.payload)
        if digest is None:
            return
        self._async_push(DATA_DIGEST, digest)

    @callback
    def _handle_chain_problem(self, msg: mqtt.ReceiveMessage) -> None:
        text = mqtt_payload_text(msg.payload)
        if text is None:
            return
        word = text.strip().upper()
        if word not in ("ON", "OFF"):
            return
        self._async_push(DATA_CHAIN_PROBLEM, word == "ON")

    @callback
    def _handle_chain_report(self, msg: mqtt.ReceiveMessage) -> None:
        report = parse_mqtt_json(msg.payload)
        if report is None:
            return
        self._async_push(DATA_CHAIN_REPORT, report)

    @callback
    def _handle_status(self, msg: mqtt.ReceiveMessage) -> None:
        """The bridge's last-will topic: the kernel-side liveness signal."""
        text = mqtt_payload_text(msg.payload)
        if text is None:
            return
        word = text.strip().lower()
        if word == KERNEL_STATUS_ONLINE:
            online = True
        elif word == KERNEL_STATUS_OFFLINE:
            online = False
        else:
            return
        if online == self.kernel_online:
            return
        self.kernel_online = online
        if online:
            if self._offline_logged:
                _LOGGER.info("SecuraCV kernel at %s is back online", self.api.base_url)
                self._offline_logged = False
        elif not self._offline_logged:
            _LOGGER.info(
                "SecuraCV kernel at %s reports offline on %s/%s",
                self.api.base_url,
                self.topic_prefix,
                KERNEL_TOPIC_STATUS,
            )
            self._offline_logged = True
        # Availability changed; the data did not — re-render the entities.
        self.async_update_listeners()

    @callback
    def _handle_claim_state(self, msg: mqtt.ReceiveMessage) -> None:
        text = mqtt_payload_text(msg.payload)
        if text is None:
            return
        word = text.strip().lower()
        if word not in (CLAIM_STATE_CLAIMED, CLAIM_STATE_UNCLAIMED):
            return
        self._async_push(DATA_CLAIM_STATE, word)

    @callback
    def _handle_frigate_link(self, msg: mqtt.ReceiveMessage) -> None:
        link = parse_mqtt_json(msg.payload)
        if link is None:
            return
        self._async_push(DATA_FRIGATE_LINK, link)

    # ── the HTTP backstop ───────────────────────────────────────────

    def _auth_failure_is_definitive(self) -> bool:
        """Whether a 401 after the client's one retry can only mean reauth."""
        if self.auth_mode in (AUTH_MODE_PAIRED, AUTH_MODE_STATIC):
            return True
        return self.api.token_file_unreadable

    def _note_backstop_failure(self) -> None:
        self.backstop_ok = False
        now = time.monotonic()
        if self._failing_since is None:
            self._failing_since = now
            return
        if (
            not self._unreachable_reported
            and now - self._failing_since >= KERNEL_UNREACHABLE_AFTER.total_seconds()
        ):
            self._unreachable_reported = True
            repairs.async_report_kernel_unreachable(self.hass, self.entry, self.api.base_url)

    def _note_backstop_success(self) -> None:
        self.backstop_ok = True
        self._failing_since = None
        if self._unreachable_reported:
            self._unreachable_reported = False
            repairs.async_clear_kernel_unreachable(self.hass, self.entry)

    @callback
    def _check_pin(self, identity: dict[str, Any]) -> None:
        """Compare the served verifying key with the pin; pin on first sight.

        The pin is taken here only over the authenticated HTTP channel — a
        retained announce is never a pin source. A served key that differs
        from the pin is a Repairs issue the person resolves by comparing
        fingerprints with the kernel's own printout, never an automatic
        re-pin and never "verification failed".
        """
        received = normalize_kernel_key(identity.get("verifying_key"))
        if received is None:
            return
        if self.pinned_key is None:
            self.pinned_key = received
            self.hass.config_entries.async_update_entry(
                self.entry,
                data={
                    **self.entry.data,
                    CONF_KERNEL_VERIFYING_KEY: received,
                    CONF_KERNEL_KEY_PINNED_BUCKET_START: bucket_start(),
                },
            )
            _LOGGER.info(
                "Pinned the witness kernel's verifying key (fingerprint %s)",
                kernel_key_fingerprint(received),
            )
            self._set_key_mismatch(False)
            return
        if received != self.pinned_key:
            self._set_key_mismatch(True)
            repairs.async_report_tofu_mismatch(
                self.hass,
                self.entry,
                pinned_fingerprint=kernel_key_fingerprint(self.pinned_key),
                received_fingerprint=kernel_key_fingerprint(received),
                received_key=received,
                url=self.api.base_url,
            )
            return
        if self.key_mismatch:
            repairs.async_clear_tofu_mismatch(self.hass, self.entry)
        self._set_key_mismatch(False)

    @callback
    def _set_key_mismatch(self, mismatch: bool) -> None:
        self.key_mismatch = mismatch
        runtime = runtime_for(self.entry)
        if runtime is not None:
            runtime.verification.key_mismatch = mismatch

    @callback
    def _check_storage(self, status: dict[str, Any] | None) -> None:
        """Raise or clear the storage_replace Repairs issue from /status."""
        state = storage_status(status)
        if replacement_recommended(state):
            wear = None
            if isinstance(status, dict):
                storage = status.get("storage")
                if isinstance(storage, dict):
                    wear = storage.get("wear_pct")
            repairs.async_report_storage_replace(self.hass, self.entry, wear)
        else:
            repairs.async_clear_storage_replace(self.hass, self.entry)

    async def _async_update_data(self) -> dict[str, Any]:
        """The 5-minute HTTP backstop."""
        data = dict(self.data) if self.data else empty_kernel_data()
        try:
            data[DATA_HEALTH] = await self.api.async_get_health()
        except SecuraCVApiError as err:
            self._note_backstop_failure()
            raise UpdateFailed(f"kernel unreachable: {err}") from err

        try:
            identity = await self.api.async_get_identity()
        except SecuraCVApiAuthError as err:
            # Reachable, but it refused the credential. That is not
            # "unreachable" — the unreachable timer must not run.
            self.backstop_ok = False
            if self._auth_failure_is_definitive():
                raise ConfigEntryAuthFailed("the witness kernel rejected the token") from err
            raise UpdateFailed(
                "the witness kernel rejected the token; waiting for the token file"
            ) from err
        except SecuraCVApiConnectionError as err:
            self._note_backstop_failure()
            raise UpdateFailed(f"kernel unreachable: {err}") from err
        except SecuraCVApiError as err:
            _LOGGER.debug("identity unavailable: %s", err)
            identity = None
        if identity is not None:
            data[DATA_IDENTITY] = identity
            self._check_pin(identity)
        data[DATA_KEY_MISMATCH] = self.key_mismatch

        # Storage health is auxiliary: a failure here must never take down
        # the event pipeline, so keep the previous sample.
        try:
            status = await self.api.async_get_status()
        except SecuraCVApiError as err:
            _LOGGER.debug("status unavailable: %s", err)
        else:
            data[DATA_STATUS] = status
            self._check_storage(status)

        # The push feed owns the latest event once it has delivered one; the
        # backstop only fills a gap (a message missed while HA was down),
        # never re-fires anything.
        try:
            latest = await self.api.async_get_latest_event()
        except SecuraCVApiError as err:
            _LOGGER.debug("latest event unavailable: %s", err)
        else:
            current = event_bucket_start(data.get(DATA_LATEST_EVENT))
            incoming = event_bucket_start(latest)
            if latest is not None and (current is None or (incoming or 0) > current):
                data[DATA_LATEST_EVENT] = latest

        if DATA_DIGEST not in self._pushed:
            try:
                digest = await self.api.async_get_digest()
            except SecuraCVApiError as err:
                _LOGGER.debug("digest unavailable: %s", err)
            else:
                if digest is not None:
                    data[DATA_DIGEST] = digest

        data[DATA_BACKSTOP_BUCKET_START] = bucket_start()
        self._note_backstop_success()
        return data


class SecuraCVAdapterStatsCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Polls the adapter_host read-only stats endpoint (operational counters only)."""

    def __init__(
        self,
        hass: HomeAssistant,
        entry: SecuraCVConfigEntry,
        url: str,
        session: aiohttp.ClientSession,
    ) -> None:
        """Initialize the coordinator."""
        super().__init__(
            hass,
            logger=_LOGGER,
            config_entry=entry,
            name="SecuraCV Adapter Stats",
            update_interval=ADAPTER_STATS_INTERVAL,
        )
        self._url = url.rstrip("/")
        self._session = session

    async def _async_update_data(self) -> dict[str, Any]:
        """Fetch the per-adapter stats JSON. Returns {} only on an empty body."""
        try:
            async with self._session.get(
                self._url, timeout=aiohttp.ClientTimeout(total=10)
            ) as resp:
                if resp.status != 200:
                    raise UpdateFailed(f"stats endpoint status {resp.status}")
                # The endpoint sends application/json; tolerate a missing/odd content-type.
                data = await resp.json(content_type=None)
                return data if isinstance(data, dict) else {}
        # ValueError covers a misconfigured URL (e.g. missing scheme) raised by aiohttp.
        except (aiohttp.ClientError, TimeoutError, ValueError) as err:
            raise UpdateFailed(f"unable to reach adapter stats endpoint: {err}") from err
