"""The watch tick must be scheduled as a LOOP callback, not an executor job.

The regression: ``async_setup_entry`` scheduled the tick with a bare
lambda. HA's ``HassJob`` classifies a target that is neither a coroutine
function nor ``@callback``-marked as ``HassJobType.Executor`` and runs it
in a worker thread — where ``watch_runtime._notify``'s
``hass.async_create_task`` raises on modern HA. ``async_tick`` swallows
that per watch, so a fired watch's notification was never delivered and an
ended watch was never evicted: "I'll tell you if anything changes" became a
promise the code could not keep, silently.

The mark is the whole fix, so the mark is what these tests assert — the
stub ``callback`` in conftest sets ``_hass_callback`` exactly as HA's does,
or an identity decorator would make a lambda look decorated and the test
would pass on the broken code.
"""

from __future__ import annotations

import sys
import types
from datetime import timedelta

from . import conftest  # noqa: F401  (installs/augments ha stubs at import time)
from .conftest import run

from homeassistant.core import HomeAssistant, is_callback  # noqa: E402  (the stub)

from .. import async_setup_entry, watch_runtime  # noqa: E402
from ..const import (  # noqa: E402
    CONF_ENABLE_MQTT,
    CONF_SETUP_MODE,
    DOMAIN,
    SETUP_MODE_MQTT,
)

EVENT_STUB = sys.modules["homeassistant.helpers.event"]

# MQTT-only with MQTT disabled: the leanest entry that still reaches the
# tick-scheduling block (no kernel coordinator, no subscriptions).
ENTRY = types.SimpleNamespace(
    entry_id="e1",
    data={CONF_SETUP_MODE: SETUP_MODE_MQTT, CONF_ENABLE_MQTT: False},
)


def _hass() -> HomeAssistant:
    hass = HomeAssistant()
    hass.data = {}

    async def _forward(entry, platforms):
        return True

    hass.config_entries = types.SimpleNamespace(async_forward_entry_setups=_forward)
    return hass


def _setup_capturing_the_tick(monkeypatch):
    """Run setup with a recording async_track_time_interval."""
    scheduled: list = []

    def _track(hass, action, interval):
        scheduled.append((action, interval))
        return lambda: None

    monkeypatch.setattr(EVENT_STUB, "async_track_time_interval", _track)
    hass = _hass()
    assert run(async_setup_entry(hass, ENTRY)) is True
    assert len(scheduled) == 1, "the watch tick was not scheduled at all"
    return hass, scheduled[0]


def test_watch_tick_is_scheduled_as_a_loop_callback(monkeypatch) -> None:
    _hass_obj, (action, interval) = _setup_capturing_the_tick(monkeypatch)

    assert is_callback(action), (
        "the tick's scheduled action is not @callback-marked, so HassJob "
        "runs it in a worker thread and every watch notification is lost"
    )
    assert interval == timedelta(seconds=watch_runtime.TICK_INTERVAL_SECONDS)


def test_a_bare_lambda_would_fail_that_assertion() -> None:
    """The guard on the guard: an identity `callback` stub would let the
    broken code pass, so prove the mark actually discriminates."""
    assert not is_callback(lambda _now: None)


def test_the_scheduled_action_runs_the_tick_against_this_hass(monkeypatch) -> None:
    # Patched BEFORE setup: async_setup_entry from-imports async_tick into
    # the closure, so a later patch would not be seen.
    ticked: list = []
    monkeypatch.setattr(watch_runtime, "async_tick", lambda hass: ticked.append(hass))

    hass, (action, _interval) = _setup_capturing_the_tick(monkeypatch)
    action(None)

    assert ticked == [hass]


def test_the_tick_unsubscribe_is_released_on_unload(monkeypatch) -> None:
    """The scheduled interval belongs to the entry: a reload must not leave
    a second tick running against the old entry_data."""
    released: list = []

    def _track(hass, action, interval):
        return lambda: released.append("tick")

    monkeypatch.setattr(EVENT_STUB, "async_track_time_interval", _track)
    hass = _hass()
    assert run(async_setup_entry(hass, ENTRY)) is True

    for unsub in hass.data[DOMAIN]["e1"]["unsub_mqtt"]:
        unsub()
    assert released == ["tick"]
