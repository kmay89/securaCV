"""The kernel coordinator: push-fed over MQTT, backstopped over HTTP.

What is pinned:
  - every retained bridge topic lands in the coordinator's data through
    async_set_updated_data (the entities are told), junk is ignored;
  - availability is witness/status saying `online` OR the backstop
    answering, and a status transition re-renders the entities;
  - the backstop pins the key on first sight over HTTP, raises Repairs
    tofu_mismatch on a different served key, fills a missed latest event
    without replacing a newer pushed one, and samples storage;
  - a definitive 401 (paired/static token, or an unreadable token file)
    raises ConfigEntryAuthFailed; a readable-but-stale file is a retry;
  - fifteen minutes of consecutive backstop failures raise
    kernel_unreachable, the first success clears it.
"""

from __future__ import annotations

import json
import types

import pytest

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import issues, make_entry, make_runtime, run

from homeassistant.core import HomeAssistant  # noqa: E402
from homeassistant.exceptions import ConfigEntryAuthFailed  # noqa: E402
from homeassistant.helpers.update_coordinator import UpdateFailed  # noqa: E402

from .. import coordinator as coord_mod  # noqa: E402
from ..api import (  # noqa: E402
    SecuraCVApiAuthError,
    SecuraCVApiConnectionError,
    SecuraCVApiResponseError,
)
from ..const import (  # noqa: E402
    AUTH_MODE_PAIRED,
    AUTH_MODE_STATIC,
    AUTH_MODE_TOKEN_FILE,
    CONF_KERNEL_KEY_PINNED_BUCKET_START,
    CONF_KERNEL_VERIFYING_KEY,
    DOMAIN,
    ISSUE_KERNEL_UNREACHABLE,
    ISSUE_STORAGE_REPLACE,
    ISSUE_TOFU_MISMATCH,
)
from ..coordinator import SecuraCVKernelCoordinator  # noqa: E402
from ..kernel_trust import kernel_key_fingerprint  # noqa: E402

KEY = "ab" * 32
OTHER_KEY = "cd" * 32
IDENTITY = {"device_id": "pwk_1a2b3c4d", "verifying_key": KEY, "kernel_version": "2.4.16",
            "chain_length": 3, "latest_hash": "9c" * 32}


class FakeApi:
    """A kernel API whose answers (or errors) a test sets per endpoint."""

    base_url = "http://kernel:8799"

    def __init__(self, **answers) -> None:
        self.answers = {
            "health": {"ok": True},
            "identity": dict(IDENTITY),
            "status": None,
            "latest_event": None,
            "digest": None,
        }
        self.answers.update(answers)
        self.calls: list[str] = []
        self.token_file_unreadable = False

    async def _answer(self, name):
        self.calls.append(name)
        value = self.answers[name]
        if isinstance(value, Exception):
            raise value
        return value

    async def async_get_health(self):
        return await self._answer("health")

    async def async_get_identity(self):
        return await self._answer("identity")

    async def async_get_status(self):
        return await self._answer("status")

    async def async_get_latest_event(self):
        return await self._answer("latest_event")

    async def async_get_digest(self):
        return await self._answer("digest")


def _coordinator(hass=None, *, api=None, auth_mode=AUTH_MODE_TOKEN_FILE, pinned=KEY):
    hass = hass or HomeAssistant()
    hass.data = {}
    data = {CONF_KERNEL_VERIFYING_KEY: pinned} if pinned else {}
    entry = make_entry(hass, data=data)
    api = api or FakeApi()
    coordinator = SecuraCVKernelCoordinator(
        hass, entry, api, auth_mode=auth_mode, topic_prefix="witness", pinned_key=pinned
    )
    entry.runtime_data = make_runtime(hass, coordinator=coordinator, api=api)
    return hass, entry, api, coordinator


def _msg(topic: str, payload) -> types.SimpleNamespace:
    return types.SimpleNamespace(topic=topic, payload=payload, retain=True)


def _listener(coordinator) -> list:
    notified: list = []
    coordinator.async_add_listener(lambda: notified.append(True))
    return notified


# ── the push feed ───────────────────────────────────────────────────────


def test_subscribes_to_every_bridge_topic_under_the_prefix():
    hass, _entry, _api, coordinator = _coordinator()
    run(coordinator.async_subscribe_push())
    topics = [t for t, _cb in hass.data["_securacv_test_mqtt_subscriptions"]]
    assert topics == [
        "witness/last_event",
        "witness/events",
        "witness/digest",
        "witness/chain_problem",
        "witness/chain_problem/attrs",
        "witness/status",
        "witness/claim_state",
        "witness/frigate_link",
    ]
    coordinator.async_unsubscribe_push()
    coordinator.async_unsubscribe_push()  # idempotent


def test_pushed_payloads_land_in_data_and_notify_the_entities():
    _hass, _entry, _api, coordinator = _coordinator()
    notified = _listener(coordinator)
    event = {"event_type": "BoundaryCrossingObjectLarge", "zone_id": "zone:front_door",
             "time_bucket": {"start_epoch_s": 1760140800, "size_s": 600}}
    coordinator._handle_last_event(_msg("witness/last_event", json.dumps(event).encode()))
    coordinator._handle_digest(_msg("witness/digest", json.dumps({"total_events": 4})))
    coordinator._handle_chain_problem(_msg("witness/chain_problem", b"OFF"))
    coordinator._handle_chain_report(_msg("witness/chain_problem/attrs", '{"chain_valid": true}'))
    coordinator._handle_claim_state(_msg("witness/claim_state", "claimed"))
    coordinator._handle_frigate_link(_msg("witness/frigate_link", '{"connected": true}'))
    assert coordinator.data["latest_event"] == event
    assert coordinator.data["digest"] == {"total_events": 4}
    assert coordinator.data["chain_problem"] is False
    assert coordinator.data["chain_report"] == {"chain_valid": True}
    assert coordinator.data["claim_state"] == "claimed"
    assert coordinator.data["frigate_link"] == {"connected": True}
    assert coordinator.data["last_push_bucket_start"] % 600 == 0
    assert len(notified) == 6
    coordinator._handle_chain_problem(_msg("witness/chain_problem", "on"))
    assert coordinator.data["chain_problem"] is True


def test_junk_on_the_push_topics_is_ignored():
    _hass, _entry, _api, coordinator = _coordinator()
    notified = _listener(coordinator)
    for handler, junk in (
        (coordinator._handle_last_event, "not json"),
        (coordinator._handle_last_event, "[1, 2]"),
        (coordinator._handle_digest, b"\xff\xfe{"),
        (coordinator._handle_chain_problem, "maybe"),
        (coordinator._handle_claim_state, "mine"),
        (coordinator._handle_frigate_link, "42"),
        (coordinator._handle_status, "sleeping"),
    ):
        handler(_msg("witness/x", junk))
    assert notified == []
    assert coordinator.data == coord_mod.empty_kernel_data()


def test_availability_is_the_lwt_or_the_backstop(caplog):
    _hass, _entry, _api, coordinator = _coordinator()
    notified = _listener(coordinator)
    assert coordinator.kernel_online is None
    assert coordinator.kernel_available is False, "nothing has answered yet"

    coordinator._handle_status(_msg("witness/status", b"online"))
    assert coordinator.kernel_online is True and coordinator.kernel_available is True
    assert len(notified) == 1, "an availability change re-renders without new data"

    coordinator._handle_status(_msg("witness/status", "online"))
    assert len(notified) == 1, "the same word again changes nothing"

    with caplog.at_level("INFO"):
        coordinator._handle_status(_msg("witness/status", "OFFLINE"))
        coordinator._handle_status(_msg("witness/status", "offline"))
    assert coordinator.kernel_online is False
    assert coordinator.kernel_available is False
    assert sum("reports offline" in r.getMessage() for r in caplog.records) == 1

    # HTTP still answering keeps the entities available while the bridge is down.
    coordinator.backstop_ok = True
    assert coordinator.kernel_available is True
    with caplog.at_level("INFO"):
        coordinator._handle_status(_msg("witness/status", "online"))
    assert any("back online" in r.getMessage() for r in caplog.records)


# ── the HTTP backstop ───────────────────────────────────────────────────


def test_backstop_reads_identity_status_and_latest_event():
    event = {"event_type": "ContactStateChange", "time_bucket_start": 1760140800}
    api = FakeApi(status={"storage": {"status": "ok", "free_pct": 70}}, latest_event=event,
                  digest={"total_events": 1})
    _hass, _entry, _api, coordinator = _coordinator(api=api)
    run(coordinator.async_refresh())
    assert coordinator.last_update_success is True
    assert coordinator.backstop_ok is True
    assert coordinator.data["identity"] == IDENTITY
    assert coordinator.data["health"] == {"ok": True}
    assert coordinator.data["status"] == {"storage": {"status": "ok", "free_pct": 70}}
    assert coordinator.data["latest_event"] == event
    assert coordinator.data["digest"] == {"total_events": 1}, "nothing pushed yet, so fetched"
    assert coordinator.data["backstop_bucket_start"] % 600 == 0
    assert coordinator.data["key_mismatch"] is False
    assert api.calls == ["health", "identity", "status", "latest_event", "digest"]


def test_backstop_never_replaces_a_newer_pushed_event_and_leaves_pushed_digests_alone():
    older = {"event_type": "ContactStateChange", "time_bucket": {"start_epoch_s": 1760140200}}
    newer = {"event_type": "TamperDetected", "time_bucket": {"start_epoch_s": 1760140800}}
    api = FakeApi(latest_event=older, digest={"total_events": 99})
    _hass, _entry, _api, coordinator = _coordinator(api=api)
    coordinator._handle_last_event(_msg("witness/last_event", json.dumps(newer)))
    coordinator._handle_digest(_msg("witness/digest", json.dumps({"total_events": 4})))
    run(coordinator.async_refresh())
    assert coordinator.data["latest_event"] == newer
    assert coordinator.data["digest"] == {"total_events": 4}
    assert "digest" not in api.calls

    # A missed event (nothing pushed yet, HA was down) is filled in.
    _hass, _entry, _api, fresh = _coordinator(api=FakeApi(latest_event=older))
    run(fresh.async_refresh())
    assert fresh.data["latest_event"] == older


def test_first_identity_pins_the_key_over_http_and_persists_it():
    hass, entry, _api, coordinator = _coordinator(pinned=None)
    assert coordinator.pinned_key is None
    run(coordinator.async_refresh())
    assert coordinator.pinned_key == KEY
    assert coordinator.pinned_fingerprint == kernel_key_fingerprint(KEY)
    assert entry.data[CONF_KERNEL_VERIFYING_KEY] == KEY
    assert entry.data[CONF_KERNEL_KEY_PINNED_BUCKET_START] % 600 == 0
    assert issues(hass) == {}


def test_a_different_served_key_is_a_repairs_issue_not_a_repin():
    api = FakeApi(identity={**IDENTITY, "verifying_key": OTHER_KEY})
    hass, entry, _api, coordinator = _coordinator(api=api)
    run(coordinator.async_refresh())
    assert coordinator.last_update_success is True, "the kernel answered; the key is the issue"
    assert coordinator.pinned_key == KEY, "never re-pinned automatically"
    assert coordinator.key_mismatch is True and coordinator.data["key_mismatch"] is True
    assert entry.runtime_data.verification.key_mismatch is True
    issue = issues(hass)[(DOMAIN, f"{ISSUE_TOFU_MISMATCH}_{entry.entry_id}")]
    assert issue["is_fixable"] is True
    assert issue["data"]["received_key"] == OTHER_KEY
    assert issue["translation_placeholders"]["pinned_fingerprint"] == kernel_key_fingerprint(KEY)

    # The kernel serving the pinned key again clears it.
    api.answers["identity"] = dict(IDENTITY)
    run(coordinator.async_refresh())
    assert coordinator.key_mismatch is False
    assert (DOMAIN, f"{ISSUE_TOFU_MISMATCH}_{entry.entry_id}") not in issues(hass)
    assert entry.runtime_data.verification.key_mismatch is False


@pytest.mark.parametrize("auth_mode", [AUTH_MODE_PAIRED, AUTH_MODE_STATIC])
def test_a_definitive_401_starts_reauth(auth_mode):
    api = FakeApi(identity=SecuraCVApiAuthError("unauthorized"))
    _hass, _entry, _api, coordinator = _coordinator(api=api, auth_mode=auth_mode)
    with pytest.raises(ConfigEntryAuthFailed):
        run(coordinator._async_update_data())
    assert coordinator.backstop_ok is False


def test_a_401_with_a_readable_token_file_is_a_retry_not_reauth():
    api = FakeApi(identity=SecuraCVApiAuthError("unauthorized"))
    hass, entry, _api, coordinator = _coordinator(api=api, auth_mode=AUTH_MODE_TOKEN_FILE)
    with pytest.raises(UpdateFailed):
        run(coordinator._async_update_data())
    assert (DOMAIN, f"{ISSUE_KERNEL_UNREACHABLE}_{entry.entry_id}") not in issues(hass)
    # ...unless the file cannot be read at all: then nothing will ever rotate it back.
    api.token_file_unreadable = True
    with pytest.raises(ConfigEntryAuthFailed):
        run(coordinator._async_update_data())


def test_an_old_kernel_without_identity_still_backstops():
    api = FakeApi(identity=SecuraCVApiResponseError("unexpected status: 404"))
    _hass, _entry, _api, coordinator = _coordinator(api=api, pinned=None)
    run(coordinator.async_refresh())
    assert coordinator.last_update_success is True
    assert coordinator.data["identity"] is None
    assert coordinator.pinned_key is None


def test_fifteen_minutes_of_failures_raise_kernel_unreachable(monkeypatch):
    api = FakeApi(health=SecuraCVApiConnectionError("unable to reach API"))
    hass, entry, _api, coordinator = _coordinator(api=api)
    clock = {"now": 1000.0}
    monkeypatch.setattr(coord_mod.time, "monotonic", lambda: clock["now"])
    issue_id = (DOMAIN, f"{ISSUE_KERNEL_UNREACHABLE}_{entry.entry_id}")

    run(coordinator.async_refresh())
    assert coordinator.last_update_success is False and coordinator.backstop_ok is False
    assert issue_id not in issues(hass), "a single miss is not an outage"
    clock["now"] += 14 * 60
    run(coordinator.async_refresh())
    assert issue_id not in issues(hass)
    clock["now"] += 61
    run(coordinator.async_refresh())
    assert issues(hass)[issue_id]["translation_placeholders"] == {"url": "http://kernel:8799"}
    assert issues(hass)[issue_id]["is_persistent"] is False

    api.answers["health"] = {"ok": True}
    run(coordinator.async_refresh())
    assert coordinator.backstop_ok is True
    assert issue_id not in issues(hass)


def test_storage_replace_follows_the_status_report():
    api = FakeApi(status={"storage": {"status": "replacement_recommended", "wear_pct": 91}})
    hass, entry, _api, coordinator = _coordinator(api=api)
    issue_id = (DOMAIN, f"{ISSUE_STORAGE_REPLACE}_{entry.entry_id}")
    run(coordinator.async_refresh())
    assert issues(hass)[issue_id]["translation_placeholders"] == {"wear_pct": "91"}
    api.answers["status"] = {"storage": {"status": "ok", "wear_pct": 12}}
    run(coordinator.async_refresh())
    assert issue_id not in issues(hass)


def test_auxiliary_endpoints_never_fail_the_backstop():
    api = FakeApi(status=SecuraCVApiResponseError("500"),
                  latest_event=SecuraCVApiConnectionError("x"),
                  digest=SecuraCVApiResponseError("500"))
    _hass, _entry, _api, coordinator = _coordinator(api=api)
    run(coordinator.async_refresh())
    assert coordinator.last_update_success is True
    assert coordinator.data["status"] is None and coordinator.data["latest_event"] is None


def test_event_bucket_start_reads_both_payload_shapes():
    assert coord_mod.event_bucket_start({"time_bucket": {"start_epoch_s": 600}}) == 600
    assert coord_mod.event_bucket_start({"time_bucket_start": 1200}) == 1200
    assert coord_mod.event_bucket_start({"time_bucket": {"start_epoch_s": True}}) is None
    assert coord_mod.event_bucket_start({}) is None
    assert coord_mod.event_bucket_start(None) is None


# The kernel's export jitter (src/lib.rs jitter_time_bucket): `choice` uniform
# in -steps..=steps times jitter_step_s, defaults 120 s / 60 s. Every offset
# the kernel can draw, for the default bucket and the kernel's minimum one.
JITTER_OFFSETS = (-120, -60, 0, 60, 120)


@pytest.mark.parametrize("offset", JITTER_OFFSETS)
@pytest.mark.parametrize("size", [600, 300])
def test_canonical_bucket_start_undoes_every_jitter_the_kernel_can_draw(offset, size):
    true_start = 1760140800  # on both grids (a multiple of 600 is one of 300)
    assert coord_mod.canonical_bucket_start(true_start + offset, size) == true_start
    # The nested and the flat wire shapes, with the size beside the start.
    nested = {"time_bucket": {"start_epoch_s": true_start + offset, "size_s": size}}
    flat = {"time_bucket_start": true_start + offset, "time_bucket_size": size}
    assert coord_mod.event_bucket_start(nested) == true_start
    assert coord_mod.event_bucket_start(flat) == true_start
    # The wire's own window is still readable, untouched, for display.
    assert coord_mod.wire_bucket_start(nested) == true_start + offset


def test_canonical_bucket_start_falls_back_to_the_dictionary_bucket():
    # No size, a junk size, a bool: the 600 s bucket; a true start is a no-op.
    for size in (None, 0, -5, True, "600"):
        assert coord_mod.canonical_bucket_start(1760140800 - 120, size) == 1760140800
    assert coord_mod.event_bucket_start({"time_bucket_start": 1760140800 + 60}) == 1760140800
    assert coord_mod.canonical_bucket_start(0, 600) == 0
    # The kernel clamps a negative draw at zero (saturating_sub): still bucket 0.
    assert coord_mod.canonical_bucket_start(0, 600) == coord_mod.canonical_bucket_start(120, 600)


def _jittered(event: dict, offset: int) -> dict:
    """The same sealed event, exported with the kernel's draw `offset`."""
    bucket = dict(event["time_bucket"])
    bucket["start_epoch_s"] += offset
    return dict(event, time_bucket=bucket)


def _clock_at(monkeypatch, stamp: float) -> None:
    """Freeze the clock beside a test's buckets: a pushed row fires only while
    it is live (`event_is_live`, two bucket sizes back from now)."""
    import time as _time

    monkeypatch.setattr(_time, "time", lambda: stamp)


def test_event_is_live_is_two_bucket_sizes_back_from_now():
    """The live window the dispatch gate applies: the row's canonical bucket
    started at most two bucket sizes (the wire's own size; 600 s unsaid)
    before now. The bridge republishes one bucket back on a restart, so a
    restart never fires an older row as if a camera saw someone now."""
    row = {"event_type": "BoundaryCrossingObjectLarge", "zone_id": "zone:a",
           "time_bucket": {"start_epoch_s": 1760140800, "size_s": 600}}
    assert coord_mod.LIVE_EVENT_MAX_AGE_BUCKETS == 2
    assert coord_mod.event_is_live(row, now=1760140800 + 1200) is True
    assert coord_mod.event_is_live(row, now=1760140800 + 1201) is False
    assert coord_mod.event_is_live(row, now=1760140800 - 60) is True, "a bucket ahead of this clock is live"
    # The kernel's export jitter is undone first: a draw 120 s early is the same bucket.
    drawn = dict(row, time_bucket={"start_epoch_s": 1760140800 - 120, "size_s": 600})
    assert coord_mod.event_is_live(drawn, now=1760140800 + 1200) is True
    # The wire's own bucket size sets the window; a size it does not state is 600 s.
    short = dict(row, time_bucket={"start_epoch_s": 1760140800, "size_s": 300})
    assert coord_mod.event_is_live(short, now=1760140800 + 600) is True
    assert coord_mod.event_is_live(short, now=1760140800 + 601) is False
    flat = {"event_type": "TamperDetected", "zone_id": "zone:a", "time_bucket_start": 1760140800}
    assert coord_mod.event_is_live(flat, now=1760140800 + 1200) is True
    assert coord_mod.event_is_live(flat, now=1760140800 + 1201) is False
    # No bucket, no row: never live.
    assert coord_mod.event_is_live({"event_type": "TamperDetected"}, now=1760140800) is False
    assert coord_mod.event_is_live(None, now=1760140800) is False


@pytest.mark.parametrize("push_offset,backstop_offset", [
    (-120, 60), (60, -120), (0, 120), (120, 0), (-60, -60),
])
def test_one_event_seen_by_push_and_by_the_backstop_fires_exactly_once(push_offset, backstop_offset, monkeypatch):
    """The bridge publishes one draw of a sealed event's bucket; five minutes
    later /events/latest serves the same event under a fresh draw. That is
    one event: one dispatch, one latest-event slot, however the draws fell."""
    from homeassistant.helpers.dispatcher import async_dispatcher_connect

    _clock_at(monkeypatch, 1760140800 + 300.0)
    sealed = {"event_type": "BoundaryCrossingObjectLarge", "zone_id": "zone:front_door",
              "time_bucket": {"start_epoch_s": 1760140800, "size_s": 600}, "seq": 7}
    pushed = _jittered(sealed, push_offset)
    served = _jittered(sealed, backstop_offset)
    api = FakeApi(latest_event=served)
    hass, entry, _api, coordinator = _coordinator(api=api)
    seen: list = []
    async_dispatcher_connect(hass, coord_mod.witness_event_signal(entry.entry_id), seen.append)

    coordinator._handle_event(types.SimpleNamespace(topic="witness/events", payload=json.dumps(pushed), retain=False))
    assert seen == [pushed]
    run(coordinator.async_refresh())
    run(coordinator.async_refresh())
    assert seen == [pushed], "a re-jittered copy of the same event is not a new event"
    assert coordinator.data["latest_event"] == pushed, "the push copy keeps the slot"
    assert coord_mod.event_dedupe_key(pushed) == coord_mod.event_dedupe_key(served) == (
        "zone:front_door", 1760140800, "boundary_crossing_object_large")

    # The other order: the backstop fills the gap first, then the push copy arrives.
    api2 = FakeApi(latest_event=served)
    hass2, entry2, _api2, coordinator2 = _coordinator(api=api2)
    seen2: list = []
    async_dispatcher_connect(hass2, coord_mod.witness_event_signal(entry2.entry_id), seen2.append)
    run(coordinator2.async_refresh())
    assert seen2 == [served]
    coordinator2._handle_event(types.SimpleNamespace(topic="witness/events", payload=json.dumps(pushed), retain=False))
    assert seen2 == [served]


def test_a_retained_replay_and_a_rejittered_backstop_never_refire_on_reload(monkeypatch):
    """After a restart the retained last_event replays one draw and the first
    backstop serves another: nothing fires (C10), and the newer-than test
    does not flap on the draw."""
    from homeassistant.helpers.dispatcher import async_dispatcher_connect

    _clock_at(monkeypatch, 1760141400 + 300.0)

    sealed = {"event_type": "TamperDetected", "zone_id": "zone:a",
              "time_bucket": {"start_epoch_s": 1760140800, "size_s": 600}}
    api = FakeApi(latest_event=_jittered(sealed, 120))
    hass, entry, _api, coordinator = _coordinator(api=api)
    seen: list = []
    async_dispatcher_connect(hass, coord_mod.witness_event_signal(entry.entry_id), seen.append)
    coordinator._handle_last_event(_msg("witness/last_event", json.dumps(_jittered(sealed, -120))))
    run(coordinator.async_refresh())
    assert seen == []
    assert coordinator.data["latest_event"] == _jittered(sealed, -120)
    # A genuinely newer bucket served by the backstop, drawn downwards, still lands.
    newer = {"event_type": "TamperDetected", "zone_id": "zone:a",
             "time_bucket": {"start_epoch_s": 1760141400 - 120, "size_s": 600}}
    api.answers["latest_event"] = newer
    run(coordinator.async_refresh())
    assert seen == [newer] and coordinator.data["latest_event"] == newer


# ── the kernel's own report, and the signals stage b reads ──────────────


def test_kernel_reported_chain_ok_prefers_push_then_report_then_identity():
    hass, _entry, _api, coordinator = _coordinator()
    assert coordinator.kernel_reported_chain_ok is None
    run(coordinator.async_refresh())
    assert coordinator.kernel_reported_chain_ok is None, "identity without last_verify says nothing"
    coordinator.data["identity"]["last_verify"] = {"at": 1760140200, "bucket_size": 600, "ok": True}
    assert coordinator.kernel_reported_chain_ok is True
    assert coordinator.kernel_reported_verified_bucket_start == 1760140200
    coordinator._handle_chain_report(_msg("witness/chain_problem/attrs",
                                          json.dumps({"chain_valid": False, "verified_bucket_start": 1760140800})))
    assert coordinator.kernel_reported_chain_ok is False
    assert coordinator.kernel_reported_verified_bucket_start == 1760140800
    coordinator._handle_chain_problem(_msg("witness/chain_problem", "OFF"))
    assert coordinator.kernel_reported_chain_ok is True, "the push topic wins"
    assert coordinator.kernel_chain_length == 3 and coordinator.kernel_latest_hash == "9c" * 32


def test_chain_problem_transitions_are_dispatched_replays_are_not():
    hass, entry, _api, coordinator = _coordinator()
    seen: list = []
    from homeassistant.helpers.dispatcher import async_dispatcher_connect

    async_dispatcher_connect(hass, coord_mod.chain_problem_signal(entry.entry_id), seen.append)
    coordinator._handle_chain_problem(_msg("witness/chain_problem", "OFF"))  # the retained state
    assert seen == []
    coordinator._handle_chain_problem(_msg("witness/chain_problem", "OFF"))
    assert seen == []
    coordinator._handle_chain_problem(_msg("witness/chain_problem", "ON"))
    coordinator._handle_chain_problem(_msg("witness/chain_problem", "ON"))
    coordinator._handle_chain_problem(_msg("witness/chain_problem", "OFF"))
    assert seen == [True, False]
    # A kernel whose FIRST word is a problem: the retained replay that every
    # restart delivers is not a transition, so nothing fires (the state is on
    # the chain_valid sensor and the integration's own walk says the rest).
    hass2, entry2, _api2, coordinator2 = _coordinator()
    seen2: list = []
    async_dispatcher_connect(hass2, coord_mod.chain_problem_signal(entry2.entry_id), seen2.append)
    coordinator2._handle_chain_problem(_msg("witness/chain_problem", "ON"))
    assert seen2 == []
    assert coordinator2.kernel_reported_chain_ok is False


def test_a_served_key_other_than_the_pin_raises_repairs_and_the_key_changed_event_once():
    hass, entry, _api, coordinator = _coordinator()
    seen: list = []
    from homeassistant.helpers.dispatcher import async_dispatcher_connect

    async_dispatcher_connect(hass, coord_mod.verification_event_signal(entry.entry_id),
                             lambda event_type, attrs: seen.append((event_type, attrs)))
    coordinator.async_note_served_key(OTHER_KEY)
    coordinator.async_note_served_key(OTHER_KEY)
    assert coordinator.key_mismatch is True
    assert entry.runtime_data.verification.key_mismatch is True
    assert (DOMAIN, f"{ISSUE_TOFU_MISMATCH}_{entry.entry_id}") in issues(hass)
    assert [e[0] for e in seen] == ["witness_key_changed"], "once per flip"
    assert seen[0][1]["pinned_fingerprint"] == kernel_key_fingerprint(KEY)
    coordinator.async_note_served_key(KEY)
    assert coordinator.key_mismatch is False
    assert (DOMAIN, f"{ISSUE_TOFU_MISMATCH}_{entry.entry_id}") not in issues(hass)
    coordinator.async_note_served_key(OTHER_KEY)
    assert [e[0] for e in seen] == ["witness_key_changed", "witness_key_changed"]


def test_the_dedupe_key_is_the_zone_bucket_and_folded_type():
    event = {"event_type": "BoundaryCrossingObjectLarge", "zone_id": "zone:a",
             "time_bucket": {"start_epoch_s": 600, "size_s": 600}}
    assert coord_mod.event_dedupe_key(event) == ("zone:a", 600, "boundary_crossing_object_large")
    flat = {"event_type": "boundary_crossing_object_large", "zone_id": "zone:a", "time_bucket_start": 600}
    assert coord_mod.event_dedupe_key(flat) == coord_mod.event_dedupe_key(event)
    assert coord_mod.event_dedupe_key({"zone_id": "zone:a"}) is None
    assert coord_mod.event_dedupe_key(None) is None


def test_the_dedupe_memory_is_bounded():
    hass, _entry, _api, coordinator = _coordinator()
    for i in range(coord_mod.EVENT_DEDUPE_MAX + 10):
        coordinator.async_dispatch_event({"event_type": "AcousticImpulseInZone", "zone_id": f"zone:{i}",
                                          "time_bucket": {"start_epoch_s": 600, "size_s": 600}})
    assert len(coordinator._seen_events) == coord_mod.EVENT_DEDUPE_MAX
