"""Watch runtime — the live half of watches.

``watches.py`` is the pure engine; this is the thin Home Assistant layer
that actually feeds it and speaks for it. It exists so a started watch is
genuinely watching: without it the spoken promise ("I'll tell you if
anything changes") would be a claim the system cannot keep, which is the
one thing this project treats as worse than a missing feature.

Two lanes:

  - ``async_observe_event`` — called from the MQTT event path, records one
    observation against every watch bound to that device.
  - ``async_tick`` — called on a timer, evaluates every watch, delivers
    anything that fired, and announces expiry (silence is never rendered
    as safety, so a watch that ends says so).

Delivery is a ``persistent_notification``, the same lane the integration
already uses for a key mismatch: local, no cloud, no new dependency.

Starting, listing and ending go through here too (``async_start_watch``,
``async_watch_bucket``, ``async_end_watch``): the voice intents and the
``securacv.*`` actions share one path, so a watch is the same object
however it began and every change is persisted the same way.

Persistence: the bucket lives in ``hass.data[DOMAIN]["watches"]`` and is
mirrored to HA's ``Store`` (``.storage/securacv_watches``) by coalesced,
delayed saves, then restored once per HA instance by ``async_load_watches``
during setup — so a watch survives a hub restart. A watch that ended while
the hub was down is restored too, and the first tick announces it rather
than letting it vanish.
"""
from __future__ import annotations

import logging
import math
import time
from typing import Any

from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.storage import Store

from . import voice, watches
from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)

# How often the tick runs. Watches reason in days, so a slow beat is
# plenty and keeps a sleeping hub asleep.
TICK_INTERVAL_SECONDS = 300

# The trailing window the deviation concerns measure event rate over. One
# day, because that is the rhythm the design speaks in ("about three a
# day" — docs/design/watches.md).
EVENT_RATE_WINDOW_SECONDS = watches.DAY

# Persistence. One domain-level store, not one per config entry: watches
# are domain-scoped and bind by device_id. Saves are delayed and coalesced
# so a busy event stream does not hammer the hub's flash.
STORAGE_VERSION = 1
STORAGE_KEY = "securacv_watches"
SAVE_DELAY_SECONDS = 10

_VALID_STATES = (watches.STATE_SETTLING, watches.STATE_WATCHING, watches.STATE_ENDED)


def _bucket(hass: HomeAssistant) -> list[dict[str, Any]]:
    domain_data = hass.data.get(DOMAIN)
    if not isinstance(domain_data, dict):
        return []
    bucket = domain_data.get("watches")
    return bucket if isinstance(bucket, list) else []


def _event_value(watch: dict[str, Any], now: float) -> float:
    """What one event arrival is worth to this watch's concern.

    ``every`` and ``stopped`` reason about *timing* — each event is one
    beat, and the value is irrelevant — so they observe a plain 1.0.

    The deviation concerns (``unusual``/``more``/``less``, and ``unusual``
    is the default) compare a LEVEL against a learned baseline. Feeding
    them the constant 1.0 made them mathematically unable to fire: the
    baseline median was 1, every delta was 0, and "I'll tell you if
    anything changes" was a promise the code could not keep. Instead each
    event observes the trailing daily rate (this event included), so the
    baseline learns a real rhythm ("about three a day") and a busier or
    quieter signal actually moves the number.
    """
    concern = watch.get("concern")
    if concern in (watches.CONCERN_EVERY, watches.CONCERN_STOPPED):
        return 1.0
    # Each prior event contributed exactly one observation, so counting
    # observation timestamps inside the window counts events.
    cutoff = now - EVENT_RATE_WINDOW_SECONDS
    recent = sum(1 for t, _v in watch.get("observations", []) if t > cutoff)
    return float(recent + 1)


@callback
def async_observe_event(hass: HomeAssistant, device_id: str, now: float) -> None:
    """Record one event against every watch bound to this device.

    Timing concerns observe one beat; deviation concerns observe the
    trailing daily rate (see ``_event_value``), so the engine's baseline
    learns a rhythm it can actually miss.
    """
    if not device_id:
        return
    touched = False
    for watch in _bucket(hass):
        subject = watch.get("subject") or {}
        if subject.get("kind") == "event" and subject.get("ref") == device_id:
            watches.observe(watch, _event_value(watch, now), now)
            touched = True
    if touched:
        async_schedule_save(hass)


def _notify(hass: HomeAssistant, title: str, message: str, note_id: str) -> None:
    hass.async_create_task(
        hass.services.async_call(
            "persistent_notification",
            "create",
            {"title": title, "message": message, "notification_id": note_id},
            blocking=False,
        )
    )


def _async_expire(hass: HomeAssistant, bucket: list[dict[str, Any]], now: float) -> bool:
    """Announce and evict every watch past its end. Returns whether any went.

    A watch that ends says so — silence is never rendered as safety — and
    the summary reports what it actually learned. The eviction follows the
    announcement, never precedes it: a watch whose ending could not be
    delivered stays for the next pass to try again.
    """
    evicted = False
    for watch in list(bucket):
        try:
            if now < watch.get("ends_at", 0.0):
                continue
            _notify(
                hass,
                "SecuraCV: a watch ended",
                watches.speak_ending(watch),
                f"securacv_watch_end_{watch['id']}",
            )
        except Exception:  # noqa: BLE001 - one bad watch must not stop the rest
            _LOGGER.debug("watch ending not announced for %s", watch.get("id"), exc_info=True)
            continue
        bucket.remove(watch)
        evicted = True
    return evicted


@callback
def async_tick(hass: HomeAssistant, now: float | None = None) -> None:
    """Evaluate every watch: deliver what fired, announce what ended."""
    now = time.time() if now is None else now
    bucket = _bucket(hass)
    if not bucket:
        return

    _async_expire(hass, bucket, now)
    for watch in list(bucket):
        try:
            watches.refresh_state(watch, now)
            verdict = watches.evaluate(watch, now)
            if verdict.get("fire"):
                _notify(
                    hass,
                    f"SecuraCV: {watch['label']}",
                    watches.speak_fired(watch, verdict),
                    f"securacv_watch_{watch['id']}",
                )
                watches.note_fired(watch, now)
        except Exception:  # noqa: BLE001 - one bad watch must not stop the rest
            _LOGGER.debug("watch tick failed for %s", watch.get("id"), exc_info=True)

    # State transitions, fired counts and evictions all happened above;
    # one coalesced write carries them.
    async_schedule_save(hass)


# ── Starting, listing, ending: the one path every surface shares ────────


class WatchError(Exception):
    """A refusal the calling surface turns into its own words."""


class WatchLimitReached(WatchError):
    """The bounded roster is full (watches.MAX_WATCHES)."""

    def __init__(self, count: int) -> None:
        super().__init__(f"already running {count} watches, which is as many as the hub keeps")
        self.count = count


class WatchNotFound(WatchError):
    """No watch has that id or label."""

    def __init__(self, ref: str) -> None:
        super().__init__(f"no watch is called {ref!r}")
        self.ref = ref


class WatchAmbiguous(WatchError):
    """More than one watch has that label; the caller must use an id."""

    def __init__(self, ref: str, ids: list[str]) -> None:
        super().__init__(f"{ref!r} names {len(ids)} watches ({', '.join(ids)})")
        self.ref = ref
        self.ids = ids


def fleet_snapshot(hass: HomeAssistant) -> list[dict[str, Any]]:
    """Plain-dict view of every config entry's runtime state.

    hass.data[DOMAIN] maps entry_id -> entry_data, plus domain-level values
    (``_frontend_registered``, the ``watches`` list, the watch Store); only
    dicts that carry a ``devices`` slice are entries. This is what
    ``voice.fleet_brief`` reads, for the intents and for binding a watch's
    subject to a Canary.
    """
    entries: list[dict[str, Any]] = []
    for entry_data in hass.data.get(DOMAIN, {}).values():
        if not isinstance(entry_data, dict) or "devices" not in entry_data:
            continue
        kernel: dict[str, Any] | None = None
        coordinator = entry_data.get("coordinator")
        if coordinator is not None:
            kernel = {
                "ok": bool(getattr(coordinator, "last_update_success", False)),
                "latest_event": (getattr(coordinator, "data", None) or {}).get(
                    "latest_event"
                ),
            }
        entries.append(
            {
                "devices": entry_data.get("devices", {}),
                "verify": entry_data.get("verify", {}),
                "kernel": kernel,
            }
        )
    return entries


def watches_restored(hass: HomeAssistant) -> bool:
    """Whether the restore has run on this hub — i.e. the bucket is the
    persisted truth and changes to it are being written back."""
    domain_data = hass.data.get(DOMAIN)
    return isinstance(domain_data, dict) and bool(domain_data.get("_watches_loaded"))


@callback
def async_watch_bucket(hass: HomeAssistant, now: float | None = None) -> list[dict[str, Any]]:
    """The bucket, created on demand.

    With ``now``, anything already past its end is announced and evicted
    first — the same path the tick takes, so a person who asks in the
    minutes before a tick still hears the ending rather than losing it.
    """
    domain_data = hass.data.setdefault(DOMAIN, {})
    bucket = domain_data.get("watches")
    if not isinstance(bucket, list):
        bucket = []
        domain_data["watches"] = bucket
    if now is not None and _async_expire(hass, bucket, now):
        async_schedule_save(hass)
    return bucket


def _new_id(bucket: list[dict[str, Any]], now: float) -> str:
    """``w<n>-<epoch>``, unique within the bucket.

    An id is how an automation ends a watch, so two starts in the same
    second after an eviction must not share one.
    """
    taken = {watch.get("id") for watch in bucket}
    n = len(bucket) + 1
    while f"w{n}-{int(now)}" in taken:
        n += 1
    return f"w{n}-{int(now)}"


def _label_key(text: Any) -> str:
    """A label as a person would match it: case, spacing and the leading
    article do not count."""
    key = " ".join(str(text or "").lower().split())
    for filler in ("the ", "my "):
        if key.startswith(filler):
            key = key[len(filler):]
    return key


@callback
def async_start_watch(
    hass: HomeAssistant,
    subject_text: str,
    duration_text: str | None = None,
    now: float | None = None,
    *,
    concern: str | None = None,
) -> tuple[dict[str, Any], str | None]:
    """Start a watch the way a person would say it. Returns (watch, device_id).

    The subject binds to a Canary when its words name one — friendly names
    too, exactly as DeviceCheck matches them, because a serial-like
    device_id is not a word anyone says. Otherwise the watch is created
    against the spoken subject as kind "unbound" and ``device_id`` is None,
    so the caller can say plainly that nothing feeds it yet: never a silent
    no-op. ``concern`` overrides the one read off the wording. Raises
    ``WatchLimitReached`` at the cap.
    """
    now = time.time() if now is None else now
    subject_text = str(subject_text or "").strip()
    if not subject_text:
        raise ValueError("a watch needs a subject")
    bucket = async_watch_bucket(hass, now)
    if len(bucket) >= watches.MAX_WATCHES:
        raise WatchLimitReached(len(bucket))
    days = watches.parse_duration_days(duration_text)
    if concern is None:
        concern = watches.concern_from_text(subject_text)
    label = subject_text
    for filler in ("the ", "my "):
        if label.startswith(filler):
            label = label[len(filler):]
    label = "the " + label

    brief = voice.fleet_brief(fleet_snapshot(hass), now)
    device_id = voice.match_device(
        brief.get("device_ids") or [], subject_text, brief.get("device_names")
    )
    subject = (
        {"kind": "event", "ref": device_id}
        if device_id
        else {"kind": "unbound", "ref": subject_text}
    )
    watch = watches.make_watch(
        _new_id(bucket, now), label, subject, now, days=days, concern=concern
    )
    bucket.append(watch)
    async_schedule_save(hass)
    return watch, device_id


@callback
def async_end_watch(hass: HomeAssistant, ref: str, now: float | None = None) -> dict[str, Any]:
    """End a watch early and remove it; returns the watch, marked ended.

    ``ref`` is matched as an id first, then as a label (case, spacing and
    the leading article do not count). An ambiguous label is refused
    rather than guessed — ending is the silencing direction, so this never
    picks for you. The early end is announced like an expiry, with what
    the watch learned: whoever set it up may not be whoever (or whatever
    automation) ended it, and a watch that ends says so.
    """
    now = time.time() if now is None else now
    ref = str(ref or "").strip()
    bucket = _bucket(hass)
    matches = [watch for watch in bucket if watch.get("id") == ref]
    if not matches and ref:
        key = _label_key(ref)
        matches = [watch for watch in bucket if _label_key(watch.get("label")) == key]
    if not matches:
        raise WatchNotFound(ref)
    if len(matches) > 1:
        raise WatchAmbiguous(ref, [str(watch.get("id")) for watch in matches])
    watch = matches[0]
    bucket.remove(watch)
    watch["ends_at"] = min(float(watch.get("ends_at", now)), now)
    watch["state"] = watches.STATE_ENDED
    async_schedule_save(hass)
    try:
        _notify(
            hass,
            "SecuraCV: a watch was ended early",
            watches.speak_ending(watch),
            f"securacv_watch_end_{watch['id']}",
        )
    except Exception:  # noqa: BLE001 - the end was asked for; the notice is best-effort
        _LOGGER.debug("early end not announced for %s", watch.get("id"), exc_info=True)
    return watch


# ── Persistence ─────────────────────────────────────────────────────────


def _store(hass: HomeAssistant) -> Store:
    """The domain-level store, created on first use and kept in hass.data.

    It sits beside the entry dicts and the ``watches`` list; every reader
    of ``hass.data[DOMAIN]`` that iterates values already skips anything
    that is not an entry dict (fleet_snapshot), so a Store object there
    is inert to them.
    """
    domain_data = hass.data.setdefault(DOMAIN, {})
    store = domain_data.get("_watch_store")
    if not isinstance(store, Store):
        store = Store(hass, STORAGE_VERSION, STORAGE_KEY)
        domain_data["_watch_store"] = store
    return store


def _data_to_save(hass: HomeAssistant) -> dict[str, Any]:
    return {
        "version": STORAGE_VERSION,
        "watches": [dict(watch) for watch in _bucket(hass)],
    }


@callback
def async_schedule_save(hass: HomeAssistant) -> None:
    """Queue one coalesced write of the bucket. Never raises.

    Persistence must not be able to break a tick or an intent, so any
    surprise is logged at debug and the in-memory bucket stays the truth
    for this session. Silently a no-op until ``async_load_watches`` has
    run: a write before the restore would overwrite the very rows the
    restore is about to read back.
    """
    domain_data = hass.data.get(DOMAIN)
    if not isinstance(domain_data, dict) or not domain_data.get("_watches_loaded"):
        return
    try:
        _store(hass).async_delay_save(lambda: _data_to_save(hass), SAVE_DELAY_SECONDS)
    except Exception:  # noqa: BLE001 - persistence is best-effort, the bucket is not
        _LOGGER.debug("watch save not scheduled", exc_info=True)


def _number(value: Any) -> float | None:
    """A finite float from a stored scalar, or None (bool is not a number)."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    number = float(value)
    return number if math.isfinite(number) else None


def _coerce_watch(row: Any) -> tuple[dict[str, Any] | None, str]:
    """One stored row as a watch dict, or ``(None, why)``.

    Strict about the fields the engine computes with (identity, subject,
    the three timestamps, the observation pairs) and lenient about the
    ones it can safely default (concern, sensitivity, state, counters):
    a half-written or hand-edited store yields fewer watches, never wrong
    ones. A bad observation pair drops that one reading, not the watch.
    """
    if not isinstance(row, dict):
        return None, "not an object"
    watch_id = row.get("id")
    if not isinstance(watch_id, str) or not watch_id:
        return None, "missing id"
    label = row.get("label")
    if not isinstance(label, str) or not label.strip():
        return None, f"{watch_id}: missing label"
    subject = row.get("subject")
    if not isinstance(subject, dict):
        return None, f"{watch_id}: missing subject"
    times: dict[str, float] = {}
    for key in ("started_at", "ends_at", "settle_until"):
        number = _number(row.get(key))
        if number is None:
            return None, f"{watch_id}: {key} is not a number"
        times[key] = number
    raw_observations = row.get("observations", [])
    if not isinstance(raw_observations, list):
        return None, f"{watch_id}: observations is not a list"
    observations: list[list[float]] = []
    for pair in raw_observations:
        if isinstance(pair, (list, tuple)) and len(pair) == 2:
            when, value = _number(pair[0]), _number(pair[1])
            if when is not None and value is not None:
                observations.append([when, value])
                continue
        _LOGGER.debug("dropping a malformed observation on watch %s", watch_id)
    del observations[: max(0, len(observations) - watches.MAX_OBSERVATIONS)]

    concern = row.get("concern")
    sensitivity = row.get("sensitivity")
    state = row.get("state")
    fired = _number(row.get("fired"))
    last_fired_at = row.get("last_fired_at")
    watch: dict[str, Any] = {
        "id": watch_id,
        "label": label.strip(),
        "subject": dict(subject),
        "concern": concern if concern in watches.CONCERNS else watches.CONCERN_UNUSUAL,
        "sensitivity": (
            sensitivity if sensitivity in watches.SENSITIVITY_K else watches.DEFAULT_SENSITIVITY
        ),
        "started_at": times["started_at"],
        "ends_at": times["ends_at"],
        "settle_until": times["settle_until"],
        "observations": observations,
        "state": state if state in _VALID_STATES else watches.STATE_SETTLING,
        "fired": int(fired) if fired is not None and fired >= 0 else 0,
        "last_fired_at": None if last_fired_at is None else _number(last_fired_at),
    }
    return watch, ""


def restore_watches(raw: Any) -> list[dict[str, Any]]:
    """Watches from a stored payload; malformed rows are dropped with a
    warning, never guessed at, and the result is bounded by MAX_WATCHES.

    Expired watches are deliberately KEPT: the first tick after a restart
    announces them and reports what they learned. Dropping them here would
    turn a hub reboot into a silent end, and silence is never rendered as
    safety.
    """
    if raw is None:
        return []
    rows = raw.get("watches") if isinstance(raw, dict) else None
    if not isinstance(rows, list):
        _LOGGER.warning("stored watches are unreadable (%s); starting with none", type(raw).__name__)
        return []
    restored: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, row in enumerate(rows):
        watch, reason = _coerce_watch(row)
        if watch is None:
            _LOGGER.warning("dropping stored watch #%d: %s", index, reason)
            continue
        if watch["id"] in seen:
            _LOGGER.warning("dropping stored watch #%d: duplicate id %s", index, watch["id"])
            continue
        seen.add(watch["id"])
        restored.append(watch)
    if len(restored) > watches.MAX_WATCHES:
        _LOGGER.warning(
            "stored %d watches, keeping the first %d", len(restored), watches.MAX_WATCHES
        )
        del restored[watches.MAX_WATCHES:]
    return restored


async def async_load_watches(hass: HomeAssistant) -> list[dict[str, Any]]:
    """Restore the bucket from the store, once per Home Assistant instance.

    Idempotent: a second config entry, or a reload, finds it already done
    and leaves the live bucket alone. Anything already in the bucket — a
    watch spoken in the moment between the integration importing and this
    restore — is kept behind the restored rows rather than thrown away. A
    store that cannot be read yields a warning and an empty bucket; the
    next save then writes a readable one, so a corrupt file heals itself.
    """
    domain_data = hass.data.setdefault(DOMAIN, {})
    if domain_data.get("_watches_loaded"):
        return _bucket(hass)
    domain_data["_watches_loaded"] = True
    try:
        raw = await _store(hass).async_load()
    except Exception:  # noqa: BLE001 - a corrupt store must not stop setup
        _LOGGER.warning("stored watches could not be read; starting with none", exc_info=True)
        raw = None
    restored = restore_watches(raw)
    restored_ids = {watch["id"] for watch in restored}
    existing = domain_data.get("watches")
    if isinstance(existing, list):
        restored.extend(
            watch
            for watch in existing
            if isinstance(watch, dict) and watch.get("id") not in restored_ids
        )
        del restored[watches.MAX_WATCHES:]
    domain_data["watches"] = restored
    if restored:
        _LOGGER.debug("restored %d watch(es)", len(restored))
    return restored
