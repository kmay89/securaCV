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
"""
from __future__ import annotations

import logging
import time
from typing import Any

from homeassistant.core import HomeAssistant, callback

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
    for watch in _bucket(hass):
        subject = watch.get("subject") or {}
        if subject.get("kind") == "event" and subject.get("ref") == device_id:
            watches.observe(watch, _event_value(watch, now), now)


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
