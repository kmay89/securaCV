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
from collections import OrderedDict
from datetime import timedelta
from typing import Any, Callable

from homeassistant.components import mqtt
from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import ConfigEntryAuthFailed
from homeassistant.helpers.dispatcher import async_dispatcher_send
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
    DOMAIN,
    KERNEL_STATUS_OFFLINE,
    KERNEL_STATUS_ONLINE,
    KERNEL_TOPIC_CHAIN_PROBLEM,
    KERNEL_TOPIC_CHAIN_PROBLEM_ATTRS,
    KERNEL_TOPIC_CLAIM_STATE,
    KERNEL_TOPIC_DIGEST,
    KERNEL_TOPIC_EVENTS,
    KERNEL_TOPIC_FRIGATE_LINK,
    KERNEL_TOPIC_LAST_EVENT,
    KERNEL_TOPIC_STATUS,
    TIME_BUCKET_SECONDS,
    VERIFICATION_EVENT_KEY_CHANGED,
    VERIFY_ERROR_KEY_CHANGED,
    normalize_event_type,
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

# How many (zone, bucket, event type) keys the event dedupe remembers. The
# bridge publishes one sealed event per camera + label + 10-minute bucket, so
# a day of a busy fleet is a few hundred; the bound is for a hostile broker.
EVENT_DEDUPE_MAX = 4096


def witness_event_signal(entry_id: str) -> str:
    """Dispatcher signal carrying each NEW witness event (the C5 payload).

    Sent once per dedupe key, never for a retained replay: the event
    entities fire on it and the verifier arms its idle walk on it.
    """
    return f"{DOMAIN}_witness_event_{entry_id}"


def verification_event_signal(entry_id: str) -> str:
    """Dispatcher signal carrying (event_type, attributes) for the
    verification event entity: the integration's walk results and a key
    change."""
    return f"{DOMAIN}_verification_event_{entry_id}"


def chain_problem_signal(entry_id: str) -> str:
    """Dispatcher signal carrying the bridge's chain_problem transitions (bool)."""
    return f"{DOMAIN}_chain_problem_{entry_id}"


# How far back a pushed row may reach and still fire the event entities, in
# bucket sizes before now. The bridge starts its publish cursor one bucket
# back (event_mqtt_bridge `initial_publish_cursor`), so a bridge restart
# re-publishes at most the last two buckets; anything older on the wire is
# history — a `--replay-from-genesis`, a broker replay, a backstop catching
# up after Home Assistant was down — and history updates the last-event
# sensor, never an automation.
LIVE_EVENT_MAX_AGE_BUCKETS = 2


def event_is_live(event: dict[str, Any] | None, *, now: float | None = None) -> bool:
    """Whether a row is recent enough to fire: its canonical bucket started
    no more than `LIVE_EVENT_MAX_AGE_BUCKETS` bucket sizes before now (the
    wire's own size, the kernel's 600 s when it states none)."""
    bucket = event_bucket_start(event)
    if bucket is None or not isinstance(event, dict):
        return False
    size = wire_bucket_size(event)
    if isinstance(size, bool) or not isinstance(size, int) or size <= 0:
        size = TIME_BUCKET_SECONDS
    stamp = time.time() if now is None else now
    return stamp - bucket <= LIVE_EVENT_MAX_AGE_BUCKETS * size


def event_dedupe_key(event: dict[str, Any] | None) -> tuple[str, int, str] | None:
    """(zone_id, bucket start, snake_case event type), the one-event identity.

    The bridge's `seq` is ignored on purpose: the same sealed event reaches
    HA on several topics and through the HTTP backstop, and only this triple
    is on every copy. The bucket start is the CANONICAL one
    (`event_bucket_start`): the wire carries the kernel's export jitter, and
    two copies of one event drawn ±120 s apart are still one event.
    """
    if not isinstance(event, dict):
        return None
    zone_id = event.get("zone_id")
    bucket = event_bucket_start(event)
    event_type = normalize_event_type(event.get("event_type"))
    if not isinstance(zone_id, str) or bucket is None or event_type is None:
        return None
    return (zone_id, bucket, event_type)


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


def canonical_bucket_start(start: int, size_s: Any = None) -> int:
    """The true, grid-aligned bucket start behind a wire bucket start.

    The kernel's export view (`Kernel::view_events`, src/lib.rs
    `jitter_time_bucket`) re-draws every exported bucket start on every
    fetch by `choice * jitter_step_s` with `choice` uniform in
    `-steps..=steps` (`steps = jitter_s / jitter_step_s`; the defaults are
    120 s in 60 s steps, so the offsets are -120, -60, 0, +60, +120 s),
    clamped at zero. The bridge republishes that drawn start in the C5
    payload and `/events/latest` draws afresh per fetch, while the sealed
    log the chain walk reads carries the TRUE start. The jitter is
    two-signed and bounded by `jitter_s`, which the kernel keeps below half
    the bucket (120 s against the 300 s minimum bucket), so the nearest
    multiple of the bucket size is exactly the true start — the same rule
    the bridge's own `stable_bucket_start` applies for its publish cursor.
    A missing or unusable size is the dictionary's 600 s bucket.

    This is the ONE normalizer: the dedupe key, the covered-index key the
    verifier builds from the sealed log, the key an event row is looked up
    by, and the backstop's newer-than comparison all go through it.
    """
    size = size_s if isinstance(size_s, int) and not isinstance(size_s, bool) else 0
    if size <= 0:
        size = TIME_BUCKET_SECONDS
    return (start + size // 2) // size * size


def wire_bucket_start(event: dict[str, Any] | None) -> int | None:
    """The bucket start exactly as the payload carries it (nested or flat)."""
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


def wire_bucket_size(event: dict[str, Any]) -> Any:
    """The bucket size the payload carries (nested or flat), unchecked."""
    bucket = event.get("time_bucket")
    if isinstance(bucket, dict):
        return bucket.get("size_s")
    return event.get("time_bucket_size")


def event_bucket_start(event: dict[str, Any] | None) -> int | None:
    """The CANONICAL bucket start of a C5 event payload (nested or flat).

    The wire's start is undone of the kernel's export jitter
    (`canonical_bucket_start`); a sealed-log record, which carries the true
    start, passes through unchanged. Every identity decision keys on this;
    only display attributes show the wire's own window.
    """
    start = wire_bucket_start(event)
    if start is None or not isinstance(event, dict):
        return None
    return canonical_bucket_start(start, wire_bucket_size(event))


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
        # Dedupe keys of the events already handed to the event entities,
        # oldest first; a key is fired once however many topics carry it.
        self._seen_events: OrderedDict[tuple[str, int, str], None] = OrderedDict()
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

    # ── the kernel's OWN last verify (an attribute, never "verified") ──

    @property
    def kernel_reported_chain_ok(self) -> bool | None:
        """What the kernel says about its own chain: `witness/chain_problem`
        first (push), then the `/attrs` report, then `/api/identity`'s
        `last_verify`. None until the kernel has said anything."""
        data = self.data if isinstance(self.data, dict) else {}
        problem = data.get(DATA_CHAIN_PROBLEM)
        if isinstance(problem, bool):
            return not problem
        report = data.get(DATA_CHAIN_REPORT)
        if isinstance(report, dict) and isinstance(report.get("chain_valid"), bool):
            return bool(report["chain_valid"])
        identity = data.get(DATA_IDENTITY)
        if isinstance(identity, dict):
            last_verify = identity.get("last_verify")
            if isinstance(last_verify, dict) and isinstance(last_verify.get("ok"), bool):
                return bool(last_verify["ok"])
        return None

    @property
    def kernel_reported_verified_bucket_start(self) -> int | None:
        """The bucket the kernel's own last verify ran in (coarse time)."""
        data = self.data if isinstance(self.data, dict) else {}
        report = data.get(DATA_CHAIN_REPORT)
        if isinstance(report, dict):
            for key in ("verified_bucket_start", "verified_at_bucket_start"):
                value = report.get(key)
                if isinstance(value, int) and not isinstance(value, bool):
                    return value
        identity = data.get(DATA_IDENTITY)
        if isinstance(identity, dict):
            last_verify = identity.get("last_verify")
            if isinstance(last_verify, dict):
                value = last_verify.get("at")
                if isinstance(value, int) and not isinstance(value, bool):
                    return value
        return None

    @property
    def kernel_chain_length(self) -> int | None:
        """`/api/identity`.chain_length, the chain_length sensor's fallback."""
        data = self.data if isinstance(self.data, dict) else {}
        identity = data.get(DATA_IDENTITY)
        if isinstance(identity, dict):
            value = identity.get("chain_length")
            if isinstance(value, int) and not isinstance(value, bool):
                return value
        return None

    @property
    def kernel_latest_hash(self) -> str | None:
        """`/api/identity`.latest_hash (hex), for the chain_length attributes."""
        data = self.data if isinstance(self.data, dict) else {}
        identity = data.get(DATA_IDENTITY)
        if isinstance(identity, dict):
            value = identity.get("latest_hash")
            if isinstance(value, str) and value:
                return value
        return None

    # ── the push feed ───────────────────────────────────────────────

    async def async_subscribe_push(self) -> None:
        """Subscribe to the bridge's retained state topics.

        Called by async_setup_entry after ``mqtt.async_wait_for_mqtt_client``
        succeeded; a failure here is the caller's to report (Repairs
        ``mqtt_subscribe_failed``), the HTTP backstop keeps the entities alive.
        """
        handlers: dict[str, Callable[[mqtt.ReceiveMessage], None]] = {
            KERNEL_TOPIC_LAST_EVENT: self._handle_last_event,
            KERNEL_TOPIC_EVENTS: self._handle_event,
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
    def _handle_event(self, msg: mqtt.ReceiveMessage) -> None:
        """`<prefix>/events`: the live feed the event entities fire on.

        Only a NON-retained message that carries a dedupe key not seen
        before is dispatched (contract C10): a retained replay — every
        restart and every broker reconnect replays the retained topics —
        feeds the last-event sensor only, so a proof notification never
        re-fires on a reboot.
        """
        event = parse_mqtt_json(msg.payload)
        if event is None:
            return
        if getattr(msg, "retain", False):
            return
        self.async_dispatch_event(event)

    @callback
    def async_dispatch_event(self, event: dict[str, Any]) -> bool:
        """Hand one new event to the event entities, once per dedupe key.

        Returns True when the event was new AND live. The latest-event slot
        follows whenever the event's bucket is newer than what the retained
        topic said; a row older than the live window (`event_is_live`)
        takes the slot, is remembered, and fires nothing — a bridge that
        restarted, or was told to replay the log, is not a camera seeing
        someone now.
        """
        key = event_dedupe_key(event)
        if key is None or key in self._seen_events:
            return False
        self._seen_events[key] = None
        while len(self._seen_events) > EVENT_DEDUPE_MAX:
            self._seen_events.popitem(last=False)
        current = event_bucket_start(self.data.get(DATA_LATEST_EVENT) if self.data else None)
        if current is None or key[1] >= current:
            self._async_push(DATA_LATEST_EVENT, event)
        if not event_is_live(event):
            _LOGGER.debug(
                "witness event in bucket %s is older than the live window; shown, not fired",
                key[1],
            )
            return False
        async_dispatcher_send(self.hass, witness_event_signal(self.entry.entry_id), event)
        return True

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
        problem = word == "ON"
        previous = self.data.get(DATA_CHAIN_PROBLEM) if self.data else None
        self._async_push(DATA_CHAIN_PROBLEM, problem)
        # Only a transition seen in this load is the kernel's own report
        # MOVING; the retained word that arrives at every (re)connect is a
        # replay and fires nothing (the state is on the chain_valid sensor,
        # and the integration's own walk reaches its verdict regardless).
        if isinstance(previous, bool) and previous != problem:
            async_dispatcher_send(self.hass, chain_problem_signal(self.entry.entry_id), problem)

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
    def async_note_served_key(self, served_key: str) -> None:
        """The verifier saw the sealed log signed by `served_key`: the same
        pin check the identity backstop runs, so one owner raises Repairs."""
        self._check_pin({"verifying_key": served_key})

    @callback
    def _set_key_mismatch(self, mismatch: bool) -> None:
        flipped_on = mismatch and not self.key_mismatch
        self.key_mismatch = mismatch
        runtime = runtime_for(self.entry)
        if runtime is not None:
            runtime.verification.key_mismatch = mismatch
            # The chain_valid sensor's `error` names the key change while it
            # lasts (the card and the strategy header read it beside
            # `key_mismatch`), and only that message is cleared with it.
            if mismatch:
                runtime.verification.error = VERIFY_ERROR_KEY_CHANGED
            elif runtime.verification.error == VERIFY_ERROR_KEY_CHANGED:
                runtime.verification.error = None
        if flipped_on:
            # Once per flip: the verification event entity says the key
            # changed; the Repairs issue says what to do about it.
            async_dispatcher_send(
                self.hass,
                verification_event_signal(self.entry.entry_id),
                VERIFICATION_EVENT_KEY_CHANGED,
                {
                    "pinned_fingerprint": self.pinned_fingerprint,
                    "trigger": "identity",
                },
            )

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
        # never re-fires anything. Both buckets compare canonical: the kernel
        # re-jitters `/events/latest` on every fetch, so the raw start of the
        # same event reads newer one poll and older the next.
        try:
            latest = await self.api.async_get_latest_event()
        except SecuraCVApiError as err:
            _LOGGER.debug("latest event unavailable: %s", err)
        else:
            current = event_bucket_start(data.get(DATA_LATEST_EVENT))
            incoming = event_bucket_start(latest)
            if latest is not None and (current is None or (incoming or 0) > current):
                data[DATA_LATEST_EVENT] = latest
                # A message the push feed missed (HA was down, the broker
                # dropped it): the event entities hear it exactly once —
                # while it is still live; a backstop catching up after a
                # long outage shows the row and fires nothing.
                key = event_dedupe_key(latest)
                if key is not None and key not in self._seen_events:
                    self._seen_events[key] = None
                    if event_is_live(latest):
                        async_dispatcher_send(
                            self.hass, witness_event_signal(self.entry.entry_id), latest
                        )

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
