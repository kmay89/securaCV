"""The live event lane feeding watches (watch_runtime.py).

The regression this file exists for: the lane once fed every deviation
watch a constant 1.0 per event, which made the baseline's median 1 and
every delta 0 — so the DEFAULT concern (``unusual``, the flagship "keep an
eye on the litter box" case) was mathematically unable to fire, ever,
while its spoken confirmation promised "I'll tell you if anything
changes". Deviation concerns now observe the trailing daily event rate;
these tests prove a default-concern watch CAN fire, and that the
timing-based concerns (``stopped``/``every``) still get their per-event
beats.
"""

from __future__ import annotations

from . import conftest  # noqa: F401  (installs ha stubs at import time)

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from .. import watch_runtime, watches  # noqa: E402
from ..const import DOMAIN  # noqa: E402

NOW = 1_000_000.0
DAY = watches.DAY
DEVICE = "litterbox"


def _hass_with(watch: dict) -> HomeAssistant:
    hass = HomeAssistant()
    hass.data = {DOMAIN: {"watches": [watch]}}
    return hass


def _watch(concern: str, days: float = 14) -> dict:
    return watches.make_watch(
        "w1", "the litter box", {"kind": "event", "ref": DEVICE}, NOW,
        days=days, concern=concern,
    )


def _feed_daily_rhythm(hass, per_day: int, days: float, start: float = NOW + 60):
    """Events at an even spacing, the way a settled routine arrives."""
    step = DAY / per_day
    count = int(days * per_day)
    times = [start + i * step for i in range(count)]
    for t in times:
        watch_runtime.async_observe_event(hass, DEVICE, t)
    return times


def test_default_concern_watch_can_fire_on_a_rate_change() -> None:
    """The flagship case: unusual (the default) fires when the rhythm breaks."""
    watch = _watch(watches.CONCERN_UNUSUAL)
    hass = _hass_with(watch)

    # Settling in: about three a day for four days (settle ends at 3.5).
    _feed_daily_rhythm(hass, per_day=3, days=4)
    base = watches.baseline(watch)
    assert base is not None
    assert base["median"] == 3  # the promised "about three a day"

    # A quiet day would not have fired; then the cat's routine breaks —
    # a burst of visits inside a couple of hours on day five.
    burst_start = NOW + 4.5 * DAY
    for i in range(6):
        watch_runtime.async_observe_event(hass, DEVICE, burst_start + i * 1800)

    verdict = watches.evaluate(watch, burst_start + 6 * 1800)
    assert verdict["fire"] is True, verdict
    assert verdict["reason"] == "deviation"
    assert "up from the usual 3" in verdict["detail"]


def test_default_concern_watch_stays_quiet_on_a_steady_rhythm() -> None:
    """The fix must not turn the lane into a false-alarm machine."""
    watch = _watch(watches.CONCERN_UNUSUAL)
    hass = _hass_with(watch)
    times = _feed_daily_rhythm(hass, per_day=3, days=6)
    verdict = watches.evaluate(watch, times[-1] + 60)
    assert verdict["fire"] is False, verdict


def test_less_concern_fires_when_the_rate_drops() -> None:
    watch = _watch(watches.CONCERN_LESS)
    hass = _hass_with(watch)
    _feed_daily_rhythm(hass, per_day=6, days=4)
    assert watches.baseline(watch)["median"] == 6

    # The signal slows to one event a day; its trailing rate falls with it.
    slow_at = NOW + 6 * DAY
    watch_runtime.async_observe_event(hass, DEVICE, slow_at)
    verdict = watches.evaluate(watch, slow_at + 60)
    assert verdict["fire"] is True, verdict
    assert "down from the usual 6" in verdict["detail"]


def test_timing_concerns_still_observe_one_beat_per_event() -> None:
    """stopped/every reason about timing; their values stay a plain 1.0."""
    for concern in (watches.CONCERN_STOPPED, watches.CONCERN_EVERY):
        watch = _watch(concern)
        hass = _hass_with(watch)
        for i in range(5):
            watch_runtime.async_observe_event(hass, DEVICE, NOW + 60 + i * 3600)
        assert [v for (_t, v) in watch["observations"]] == [1.0] * 5


def test_events_only_feed_watches_bound_to_that_device() -> None:
    watch = _watch(watches.CONCERN_UNUSUAL)
    hass = _hass_with(watch)
    watch_runtime.async_observe_event(hass, "some-other-canary", NOW + 60)
    watch_runtime.async_observe_event(hass, "", NOW + 60)
    assert watch["observations"] == []


def test_tick_delivers_a_fired_default_concern_watch(monkeypatch) -> None:
    """End to end through the tick: the burst reaches a notification."""
    watch = _watch(watches.CONCERN_UNUSUAL)
    hass = _hass_with(watch)
    _feed_daily_rhythm(hass, per_day=3, days=4)
    burst_start = NOW + 4.5 * DAY
    for i in range(6):
        watch_runtime.async_observe_event(hass, DEVICE, burst_start + i * 1800)

    delivered: list[tuple[str, str]] = []
    monkeypatch.setattr(
        watch_runtime, "_notify",
        lambda _hass, title, message, _nid: delivered.append((title, message)),
    )
    watch_runtime.async_tick(hass, burst_start + 6 * 1800)
    assert len(delivered) == 1
    title, message = delivered[0]
    assert title == "SecuraCV: the litter box"
    assert "up from the usual 3" in message
    assert watch["fired"] == 1
