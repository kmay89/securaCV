"""Watches survive a Home Assistant restart (watch_runtime persistence).

Before this, the bucket lived only in ``hass.data`` and every reboot of the
hub silently ended every watch — "I'll tell you if anything changes" became
a promise that lasted until the next update. Now ``watch_runtime`` mirrors
the bucket to HA's ``Store`` with coalesced saves and restores it during
``async_setup_entry``, before anything can feed it.

The restart is simulated the way test_replay_persistence.py does it: a
Store stub whose payloads outlive the instance that wrote them, keyed as
HA's real one is, and a fresh ``HomeAssistant`` booted through the
integration's real setup.
"""

from __future__ import annotations

import asyncio
import copy
import logging
import time
import types

from . import conftest  # noqa: F401  (installs the base HA stubs)
from .conftest import run

# Installs the intent-platform stubs at import; also lends us the voice
# path so a watch here is started exactly the way a person starts one.
from .test_intent_start_watch import DEVICES, GATE, _start  # noqa: E402

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from .. import async_setup_entry, watch_runtime, watches  # noqa: E402
from ..const import (  # noqa: E402
    CONF_ENABLE_MQTT,
    CONF_SETUP_MODE,
    DOMAIN,
    SETUP_MODE_MQTT,
)

LOGGER_NAME = watch_runtime._LOGGER.name
NOW = 1_700_000_000.0
DAY = watches.DAY

ENTRY = types.SimpleNamespace(
    entry_id="e1",
    data={CONF_SETUP_MODE: SETUP_MODE_MQTT, CONF_ENABLE_MQTT: False},
)
SECOND_ENTRY = types.SimpleNamespace(
    entry_id="e2",
    data={CONF_SETUP_MODE: SETUP_MODE_MQTT, CONF_ENABLE_MQTT: False},
)


def _persistent_storage(monkeypatch) -> dict:
    """A Store stub whose payloads outlive the instance that wrote them,
    keyed as HA's real one is — without that there is no restart to test."""
    saved: dict[str, dict] = {}

    class _Store:
        def __init__(self, hass, version, key) -> None:
            self._key = key

        async def async_load(self):
            return copy.deepcopy(saved.get(self._key))

        async def async_save(self, data) -> None:
            saved[self._key] = copy.deepcopy(data)

        def async_delay_save(self, data_func, delay: float = 0) -> None:
            saved[self._key] = copy.deepcopy(data_func())

    # watch_runtime did `from ... import Store`, so rebind it there.
    monkeypatch.setattr(watch_runtime, "Store", _Store)
    return saved


def _stored(saved: dict) -> list:
    return saved.get(watch_runtime.STORAGE_KEY, {}).get("watches", [])


def _boot() -> HomeAssistant:
    """One Home Assistant start, through the integration's real setup."""
    hass = HomeAssistant()
    hass.data = {}

    async def _forward(entry, platforms):
        return True

    hass.config_entries = types.SimpleNamespace(async_forward_entry_setups=_forward)
    assert run(async_setup_entry(hass, ENTRY)) is True
    # What the status handler would have learned over MQTT: the fleet the
    # voice binds a subject against.
    hass.data[DOMAIN]["e1"]["devices"] = dict(DEVICES)
    return hass


def _bucket(hass) -> list:
    return hass.data[DOMAIN]["watches"]


def _bound_watch(
    watch_id: str = "w1",
    days: float = 14,
    concern: str = watches.CONCERN_EVERY,
    now: float = NOW,
):
    return watches.make_watch(
        watch_id, "the gate", {"kind": "event", "ref": GATE}, now, days=days, concern=concern
    )


def _silence(monkeypatch) -> list[tuple[str, str]]:
    delivered: list[tuple[str, str]] = []
    monkeypatch.setattr(
        watch_runtime, "_notify",
        lambda _hass, title, message, _nid: delivered.append((title, message)),
    )
    return delivered


def test_a_voice_started_watch_survives_a_restart(monkeypatch) -> None:
    _persistent_storage(monkeypatch)
    hass = _boot()
    _start(hass, "the gate canary")
    before = _bucket(hass)[0]
    assert before["subject"] == {"kind": "event", "ref": GATE}

    hass2 = _boot()
    after = _bucket(hass2)
    assert len(after) == 1
    assert after[0] == before
    assert after[0] is not before, "restored from storage, not the old process's memory"
    for key in ("id", "label", "subject", "started_at", "ends_at", "settle_until", "concern"):
        assert after[0][key] == before[key]


def test_observations_fed_before_the_restart_are_there_after_it(monkeypatch) -> None:
    _persistent_storage(monkeypatch)
    hass = _boot()
    _start(hass, "the gate canary")
    watch = _bucket(hass)[0]
    t1 = watch["started_at"] + 60
    t2 = watch["started_at"] + 3600
    watch_runtime.async_observe_event(hass, GATE, t1)
    watch_runtime.async_observe_event(hass, GATE, t2)
    assert len(watch["observations"]) == 2

    hass2 = _boot()
    restored = _bucket(hass2)[0]
    assert [t for t, _v in restored["observations"]] == [t1, t2]
    # ...and it keeps being fed after the restart, the same watch object
    # the runtime now holds.
    watch_runtime.async_observe_event(hass2, GATE, t2 + 60)
    assert len(restored["observations"]) == 3


def test_the_tick_s_bookkeeping_persists_and_an_evicted_watch_stays_gone(monkeypatch) -> None:
    saved = _persistent_storage(monkeypatch)
    delivered = _silence(monkeypatch)
    hass = _boot()
    _bucket(hass).append(_bound_watch(concern=watches.CONCERN_EVERY))
    watch_runtime.async_observe_event(hass, GATE, NOW + 60)
    watch_runtime.async_tick(hass, NOW + 120)
    assert len(delivered) == 1, "an `every` watch relays its first beat"

    hass2 = _boot()
    restored = _bucket(hass2)[0]
    assert restored["fired"] == 1
    assert restored["last_fired_at"] == NOW + 120
    assert restored["state"] == watches.STATE_SETTLING

    # It runs its course: the tick announces the end and evicts it, and
    # that eviction is what the NEXT restart sees.
    watch_runtime.async_tick(hass2, NOW + 15 * DAY)
    assert delivered[-1][0] == "SecuraCV: a watch ended"
    assert _bucket(hass2) == []
    assert _stored(saved) == []
    hass3 = _boot()
    assert _bucket(hass3) == []


def test_a_watch_that_ended_while_the_hub_was_down_is_announced_not_dropped(monkeypatch) -> None:
    """Silence is never rendered as safety: a reboot must not turn an end
    into a disappearance. The watch is restored, the FIRST tick says it
    ended (with what it learned), and only then is it evicted."""
    saved = _persistent_storage(monkeypatch)
    delivered = _silence(monkeypatch)
    hass = _boot()
    _bucket(hass).append(_bound_watch(days=1))
    for i in range(6):
        watch_runtime.async_observe_event(hass, GATE, NOW + 60 + i * 3600)
    watch_runtime.async_schedule_save(hass)
    assert len(_stored(saved)) == 1

    # ...the hub is off for three days...
    hass2 = _boot()
    assert len(_bucket(hass2)) == 1, "an expired watch is restored, so it can be announced"
    watch_runtime.async_tick(hass2, NOW + 3 * DAY)
    assert len(delivered) == 1
    title, message = delivered[0]
    assert title == "SecuraCV: a watch ended"
    # What it learned, not a reassuring blank: six beats, all alike.
    assert "watch ended" in message and "holding steady around 1" in message, message
    assert _bucket(hass2) == []
    assert _stored(saved) == []


def test_malformed_stored_rows_are_dropped_with_a_warning_and_the_rest_kept(
    monkeypatch, caplog
) -> None:
    saved = _persistent_storage(monkeypatch)
    good = _bound_watch("keep-me")
    good["observations"] = [[NOW + 1, 1.0], "not a pair", [NOW + 2, "x"], [NOW + 3, 2.0]]
    saved[watch_runtime.STORAGE_KEY] = {
        "version": 1,
        "watches": [
            good,
            "not an object",
            {k: v for k, v in _bound_watch("no-end").items() if k != "ends_at"},
            dict(_bound_watch("obs-string"), observations="lots"),
            dict(_bound_watch("keep-me"), label="a duplicate id"),
        ],
    }

    with caplog.at_level(logging.WARNING, logger=LOGGER_NAME):
        hass = _boot()

    bucket = _bucket(hass)
    assert [w["id"] for w in bucket] == ["keep-me"]
    # Bad pairs drop the reading, not the watch; the coercion is to floats.
    assert bucket[0]["observations"] == [[NOW + 1, 1.0], [NOW + 3, 2.0]]
    warnings = [r.getMessage() for r in caplog.records if r.levelno == logging.WARNING]
    assert len(warnings) == 4, warnings
    assert any("not an object" in w for w in warnings)
    assert any("ends_at" in w for w in warnings)
    assert any("observations is not a list" in w for w in warnings)
    assert any("duplicate id" in w for w in warnings)


def test_restore_defaults_the_fields_the_engine_can_default() -> None:
    row = {
        "id": "w9",
        "label": "  the shed  ",
        "subject": {"kind": "event", "ref": "shed"},
        "started_at": NOW,
        "ends_at": NOW + DAY,
        "settle_until": NOW + DAY / 4,
        "concern": "panic",
        "sensitivity": "very",
        "state": "confused",
        "fired": -3,
        "last_fired_at": "yesterday",
    }
    (watch,) = watch_runtime.restore_watches({"watches": [row]})
    assert watch["label"] == "the shed"
    assert watch["concern"] == watches.CONCERN_UNUSUAL
    assert watch["sensitivity"] == watches.DEFAULT_SENSITIVITY
    assert watch["state"] == watches.STATE_SETTLING
    assert watch["fired"] == 0
    assert watch["last_fired_at"] is None
    assert watch["observations"] == []
    # The whole payload being the wrong shape yields nothing, never a crash.
    assert watch_runtime.restore_watches(None) == []
    assert watch_runtime.restore_watches({"watches": "nope"}) == []
    assert watch_runtime.restore_watches(["w1"]) == []


def test_restore_is_idempotent_and_bounded(monkeypatch) -> None:
    saved = _persistent_storage(monkeypatch)
    # Dated against the real clock: the voice start path purges anything
    # already expired, and these must still be alive when it runs.
    fresh = time.time()
    saved[watch_runtime.STORAGE_KEY] = {
        "version": 1,
        "watches": [
            _bound_watch(f"w{i}", now=fresh) for i in range(watches.MAX_WATCHES + 5)
        ],
    }
    hass = _boot()
    bucket = _bucket(hass)
    assert len(bucket) == watches.MAX_WATCHES

    # A second entry on the same hub neither duplicates nor resets the
    # bucket — the same list object the tick and the intents hold.
    _start(hass, "the gate canary")
    assert len(bucket) == watches.MAX_WATCHES, "the cap holds against a start"
    bucket.pop()
    _start(hass, "the gate canary")
    assert len(bucket) == watches.MAX_WATCHES
    assert run(async_setup_entry(hass, SECOND_ENTRY)) is True
    assert _bucket(hass) is bucket
    assert len(bucket) == watches.MAX_WATCHES


def test_a_watch_spoken_before_the_restore_is_neither_lost_nor_written_over_the_store(
    monkeypatch,
) -> None:
    """Two hazards in the moment between import and restore: a save then
    would overwrite the rows about to be read back, and a replace-on-load
    would throw the spoken watch away. Neither happens."""
    saved = _persistent_storage(monkeypatch)
    saved[watch_runtime.STORAGE_KEY] = {"version": 1, "watches": [_bound_watch("persisted")]}

    hass = HomeAssistant()
    hass.data = {DOMAIN: {"e1": {"devices": DEVICES, "verify": {}}}}
    _start(hass, "back door")  # before any setup ran
    assert [w["id"] for w in _stored(saved)] == ["persisted"], "no write before the restore"

    async def _forward(entry, platforms):
        return True

    hass.config_entries = types.SimpleNamespace(async_forward_entry_setups=_forward)
    assert run(async_setup_entry(hass, ENTRY)) is True
    ids = [w["id"] for w in _bucket(hass)]
    assert ids[0] == "persisted"
    assert len(ids) == 2
    # From here on, saves flow: the next change lands both in the store.
    watch_runtime.async_schedule_save(hass)
    assert [w["id"] for w in _stored(saved)] == ids


def test_a_store_that_cannot_be_read_is_never_written_over(monkeypatch, caplog) -> None:
    """HA's Store quarantines a corrupt file itself (renamed, a repair
    issue, None returned), so a load that RAISES is something else: an
    OSError, a version this code cannot migrate after a downgrade. The rows
    are still on disk. Marking the restore done anyway would let the next
    start write a near-empty bucket over them, so it is not: saves stay
    off, the actions say why they refuse, and a reload tries again."""
    saved = _persistent_storage(monkeypatch)
    saved[watch_runtime.STORAGE_KEY] = {"version": 1, "watches": [_bound_watch("on-disk")]}
    on_disk = copy.deepcopy(saved)
    readable = False
    working_store = watch_runtime.Store

    class _FlakyStore(working_store):
        async def async_load(self):
            if not readable:
                raise OSError("Input/output error")
            return await super().async_load()

    monkeypatch.setattr(watch_runtime, "Store", _FlakyStore)
    with caplog.at_level(logging.WARNING, logger=LOGGER_NAME):
        hass = _boot()
    assert any("could not be read" in r.getMessage() for r in caplog.records)
    assert hass.data[DOMAIN].get("watches", []) == []
    assert not watch_runtime.watches_restored(hass)
    assert watch_runtime.watches_unreadable(hass)

    # Voice still starts a watch (in memory); nothing reaches the disk.
    _start(hass, "the gate canary")
    watch_runtime.async_tick(hass, time.time())
    assert saved == on_disk, "the unread rows were written over"

    # The actions refuse with the reason, not "not loaded yet".
    from homeassistant.core import ServiceCall
    from homeassistant.exceptions import ServiceValidationError

    from .. import async_setup

    run(async_setup(hass, {}))
    handler = hass.services.registered[(DOMAIN, "list_watches")].func
    try:
        handler(ServiceCall(DOMAIN, "list_watches", {}))
    except ServiceValidationError as err:
        assert "could not read its stored watches" in str(err)
    else:
        raise AssertionError("list_watches answered from a bucket that is not the truth")

    # A reload whose read succeeds restores the rows, keeps the spoken
    # watch behind them, and turns saves back on.
    readable = True
    assert run(async_setup_entry(hass, SECOND_ENTRY)) is True
    assert watch_runtime.watches_restored(hass)
    assert not watch_runtime.watches_unreadable(hass)
    ids = [w["id"] for w in _bucket(hass)]
    assert ids[0] == "on-disk" and len(ids) == 2
    watch_runtime.async_schedule_save(hass)
    assert [w["id"] for w in _stored(saved)] == ids


def test_a_file_ha_quarantined_as_corrupt_restores_nothing_and_saves_flow(monkeypatch) -> None:
    """What HA's Store hands back after renaming a corrupt file: None. That
    is a clean, empty start, so the restore is done and saves write."""
    saved = _persistent_storage(monkeypatch)
    hass = _boot()
    assert _bucket(hass) == []
    assert watch_runtime.watches_restored(hass)
    _start(hass, "the gate canary")
    assert len(_stored(saved)) == 1


def test_a_canceled_restore_is_not_marked_done(monkeypatch) -> None:
    class _CanceledStore:
        def __init__(self, hass, version, key) -> None:
            pass

        async def async_load(self):
            raise asyncio.CancelledError

        def async_delay_save(self, data_func, delay: float = 0) -> None:
            raise AssertionError("nothing may be written after a canceled restore")

    monkeypatch.setattr(watch_runtime, "Store", _CanceledStore)
    hass = HomeAssistant()
    hass.data = {}
    try:
        run(watch_runtime.async_load_watches(hass))
    except asyncio.CancelledError:
        pass
    assert not watch_runtime.watches_restored(hass)
    watch_runtime.async_schedule_save(hass)  # a no-op, not a write


def test_two_setups_reading_at_once_restore_once(monkeypatch) -> None:
    """Of two restores in flight together, the first to finish wins; the
    second must not swap in a second copy of the same rows, or the objects
    the tick and the event path already hold would stop being the bucket."""
    saved = _persistent_storage(monkeypatch)
    saved[watch_runtime.STORAGE_KEY] = {"version": 1, "watches": [_bound_watch("w1")]}
    working_store = watch_runtime.Store

    class _SlowStore(working_store):
        async def async_load(self):
            await asyncio.sleep(0)
            return await super().async_load()

    monkeypatch.setattr(watch_runtime, "Store", _SlowStore)
    hass = HomeAssistant()
    hass.data = {}

    async def _both():
        return await asyncio.gather(
            watch_runtime.async_load_watches(hass), watch_runtime.async_load_watches(hass)
        )

    first, second = run(_both())
    assert first is second is _bucket(hass)
    assert [w["id"] for w in _bucket(hass)] == ["w1"]


def test_a_row_the_engine_could_not_have_built_is_dropped(caplog) -> None:
    """"Fewer watches, never wrong ones": make_watch clamps a watch to a
    year and settles it inside its own span, so a stored row outside that
    shape is dropped with a warning instead of running as, say, a
    ten-year watch. Checked without the clock, which can be wrong at boot."""
    longest = watches.make_watch("longest", "the gate", {"kind": "event", "ref": GATE}, NOW, days=10_000)
    shortest = watches.make_watch("shortest", "the gate", {"kind": "event", "ref": GATE}, NOW, days=0)
    assert longest["ends_at"] - longest["started_at"] == watch_runtime.MAX_WATCH_SPAN_SECONDS
    assert shortest["settle_until"] == shortest["ends_at"], "the tightest legal settle"
    rows = [
        longest,
        shortest,
        dict(_bound_watch("far"), ends_at=NOW + 3650 * DAY),
        dict(_bound_watch("backwards"), ends_at=NOW - DAY),
        dict(_bound_watch("settles-late"), settle_until=NOW + 15 * DAY),
        dict(_bound_watch("settles-early"), settle_until=NOW - 1),
    ]
    with caplog.at_level(logging.WARNING, logger=LOGGER_NAME):
        restored = watch_runtime.restore_watches({"watches": rows})
    assert [w["id"] for w in restored] == ["longest", "shortest"]
    warnings = [r.getMessage() for r in caplog.records if r.levelno == logging.WARNING]
    assert len(warnings) == 4, warnings
    assert any("far: runs longer than a year" in w for w in warnings)
    assert any("backwards: ends_at is before started_at" in w for w in warnings)
    assert any("settles-late: settle_until is outside" in w for w in warnings)
    assert any("settles-early: settle_until is outside" in w for w in warnings)


def test_existing_start_path_still_works_without_any_setup() -> None:
    """test_intent_start_watch.py never boots: a bare HomeAssistant() with
    no store and no restore must keep starting watches (saves simply wait
    for a restore that, in that harness, never comes)."""
    hass = HomeAssistant()
    hass.data = {DOMAIN: {"e1": {"devices": DEVICES, "verify": {}}}}
    _start(hass, "the gate canary")
    assert len(_bucket(hass)) == 1
    assert "_watch_store" not in hass.data[DOMAIN]
