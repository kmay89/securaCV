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

from . import watches
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


@callback
def async_tick(hass: HomeAssistant, now: float | None = None) -> None:
    """Evaluate every watch: deliver what fired, announce what ended."""
    now = time.time() if now is None else now
    bucket = _bucket(hass)
    if not bucket:
        return

    ended: list[dict[str, Any]] = []
    for watch in list(bucket):
        try:
            if now >= watch.get("ends_at", 0.0):
                # A watch that ends says so — silence is never rendered as
                # safety. The summary reports what it actually learned.
                _notify(
                    hass,
                    "SecuraCV: a watch ended",
                    watches.speak_ending(watch),
                    f"securacv_watch_end_{watch['id']}",
                )
                ended.append(watch)
                continue

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

    for watch in ended:
        if watch in bucket:
            bucket.remove(watch)

    # State transitions, fired counts and evictions all happened above;
    # one coalesced write carries them.
    async_schedule_save(hass)


# ── Persistence ─────────────────────────────────────────────────────────


def _store(hass: HomeAssistant) -> Store:
    """The domain-level store, created on first use and kept in hass.data.

    It sits beside the entry dicts and the ``watches`` list; every reader
    of ``hass.data[DOMAIN]`` that iterates values already skips anything
    that is not an entry dict (intent._snapshot), so a Store object there
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
