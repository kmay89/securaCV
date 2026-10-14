"""A summary row's Last event reads its type, not ``unknown`` (HA28).

The CSI dialect's ``event_type`` is the row's state.
firmware/common/csi/src/csi_event_wire.h writes the placeholder ``unknown``
there, and as ``state``, when a row has no state of its own, and the row's
name rides only in ``type``. Several rows are stateless:
meta.daily_summary's ``daily_summary`` (every night at 23:55 on a canary or
canary-wap with a synced clock, since F121), meta.quiet_hours'
``held_summary`` (when a Quiet Hours window ends), core.activity_ribbon's
``ribbon_bucket_advanced`` (every 15 minutes, both trees) and ble.events'
link and discovery rows (``ble_client_connected``,
``ble_client_disconnected``, ``canary_discovered``, ``canary_lost``, which
the canary-wap registers but no firmware emits today). The canary Last
event sensor read ``event_type`` first, so it turned ``unknown`` at each,
and the voice brief recorded the same ``unknown``.

The sensor now names the two summary rows by their type and leaves every
other stateless row at ``unknown``, so the ribbon's quarter-hourly row does
not take over Last event, the voice brief or the timeline card. An
``event_type`` that is missing, null or empty (no firmware writes the last
two) falls back to ``type``, then to ``event`` (the canary-sense and
canary-sentinel dialects), then to ``unknown``. The fix is in the consumer
only: the wire is unchanged, the kernel's Last event sensor is unchanged,
and the signed canonical carries ``state``, so verification is unchanged.

The CSI bodies below are byte for byte what ``build_event_body`` writes for
those rows with no signer (each was printed from the header by a scratch
program); the signed ones carry a real Ed25519 envelope over the canonical
Home Assistant rebuilds. In the monorepo the placeholder itself is read
out of firmware/tests_host/test_csi_event_wire.cpp, the firmware's own pin
of a stateless body; the HACS mirror carries no firmware and skips that
one test.
"""

from __future__ import annotations

import base64
import json
import re
from pathlib import Path

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from .conftest import make_runtime, run
from .test_mqtt_payload_hardening import ENTRY, HomeAssistant, _msg

from .. import sensor as sensor_platform
from ..const import DOMAIN
from ..device_trust import TrustStore
from ..signature import build_event_canonical
from ..voice import _spoken_label, fleet_brief, speak_last_event, speak_whats_up

DEVICE = "canary01"

_WIRE_TEST = (
    Path(__file__).resolve().parents[3] / "firmware" / "tests_host" / "test_csi_event_wire.cpp"
)

# build_event_body's bytes for meta.daily_summary's row (event 41, 12 rows
# walked) and meta.quiet_hours' held_summary (event 42, 3 rows held): no
# state_name, so `event_type` and `state` are both the placeholder.
DAILY_SUMMARY_BODY = (
    '{"event_id":41,"event_type":"unknown","timestamp":86100,"zone":"",'
    '"confidence":"tentative","signed":false,"module":"meta.daily_summary",'
    '"type":"daily_summary","category":"event","privacy":"p0",'
    '"state":"unknown","motion":0,"breathing":0,"bpm":0,"duration_sec":0,'
    '"bundled":12,"replay":false,"v":1}'
)
HELD_SUMMARY_BODY = (
    '{"event_id":42,"event_type":"unknown","timestamp":86400,"zone":"",'
    '"confidence":"tentative","signed":false,"module":"meta.quiet_hours",'
    '"type":"held_summary","category":"event","privacy":"p0",'
    '"state":"unknown","motion":0,"breathing":0,"bpm":0,"duration_sec":0,'
    '"bundled":3,"replay":false,"v":1}'
)
# A stateful row from the same builder: core.presence entering `present`.
PRESENT_BODY = (
    '{"event_id":43,"event_type":"present","timestamp":86460,"zone":"",'
    '"confidence":"tentative","signed":false,"module":"core.presence",'
    '"type":"presence_changed","category":"event","privacy":"p0",'
    '"state":"present","motion":0,"breathing":0,"bpm":0,"duration_sec":0,'
    '"bundled":1,"replay":false,"v":1}'
)
# Two stateless rows that are not summaries: core.activity_ribbon's
# quarter-hourly row (both trees) and a ble.events discovery row, as the
# builder would write one (no firmware emits it today; its peer hash rides
# in `note`, which the body does not carry).
RIBBON_BODY = (
    '{"event_id":45,"event_type":"unknown","timestamp":87300,"zone":"",'
    '"confidence":"tentative","signed":false,"module":"core.activity_ribbon",'
    '"type":"ribbon_bucket_advanced","category":"event","privacy":"p0",'
    '"state":"unknown","motion":0,"breathing":0,"bpm":0,"duration_sec":0,'
    '"bundled":1,"replay":false,"v":1}'
)
BLE_DISCOVERED_BODY = (
    '{"event_id":46,"event_type":"unknown","timestamp":87360,"zone":"",'
    '"confidence":"tentative","signed":false,"module":"ble.events",'
    '"type":"canary_discovered","category":"event","privacy":"p0",'
    '"state":"unknown","motion":0,"breathing":0,"bpm":0,"duration_sec":0,'
    '"bundled":1,"replay":false,"v":1}'
)


def _b64url_nopad(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode("ascii")


def _hass(trust_store: TrustStore | None = None) -> HomeAssistant:
    """A Home Assistant whose ENTRY runtime holds `trust_store` (a fresh,
    empty one when None, which verifies nothing as trusted)."""
    hass = HomeAssistant()
    hass.data = {DOMAIN: {}}
    if trust_store is None:
        ENTRY.runtime_data = make_runtime(hass)
    else:
        ENTRY.runtime_data = make_runtime(hass, trust_store=trust_store)
    return hass


def _sensor(hass: HomeAssistant):
    inst = sensor_platform.SecuraCVCanaryLastEventSensor("securacv", DEVICE, ENTRY)
    inst.hass = hass
    inst.writes = []
    inst.async_write_ha_state = lambda: inst.writes.append(True)
    inst.async_on_remove = lambda unsub: None
    return inst


def _pinned():
    """A key pinned for DEVICE, and the store that holds the pin."""
    priv = Ed25519PrivateKey.from_private_bytes(b"\x42" * 32)
    hass = HomeAssistant()
    store = TrustStore(hass, entry_id="e1")
    run(store.async_load())
    run(store.async_pin(DEVICE, priv.public_key().public_bytes_raw().hex(), source="manual"))
    return priv, store


def _signed(body: str, priv, store: TrustStore, **changes) -> str:
    """`body` with `changes` applied, signed the way the firmware signs it:
    the pinned fingerprint and an Ed25519 signature over the canonical
    Home Assistant rebuilds from the body's own fields."""
    fields = dict(json.loads(body), **changes)
    canonical = build_event_canonical(
        DEVICE, fields["event_id"], fields["state"], fields["category"],
        fields["privacy"], fields["motion"], fields["breathing"], fields["bpm"],
    )
    fields.update(
        signed=True, alg="ed25519", fp=store.get(DEVICE).fingerprint_hex,
        sig=_b64url_nopad(priv.sign(canonical)),
    )
    return json.dumps(fields, separators=(",", ":"))


def test_a_daily_summary_reads_daily_summary_not_unknown() -> None:
    ent = _sensor(_hass())
    ent._handle_message(_msg(DAILY_SUMMARY_BODY))
    assert ent._attr_native_value == "daily_summary"
    assert ent.writes == [True]


def test_a_held_summary_reads_held_summary() -> None:
    ent = _sensor(_hass())
    ent._handle_message(_msg(HELD_SUMMARY_BODY))
    assert ent._attr_native_value == "held_summary"


@pytest.mark.parametrize(
    "event_type",
    ["", None, "unknown", "<missing>", 0, False],
    ids=["empty", "null", "wire-placeholder", "missing", "zero", "false"],
)
def test_an_event_type_that_says_nothing_falls_back_to_type(event_type) -> None:
    body = json.loads(DAILY_SUMMARY_BODY)
    if event_type == "<missing>":
        del body["event_type"]
    else:
        body["event_type"] = event_type
    ent = _sensor(_hass())
    ent._handle_message(_msg(json.dumps(body)))
    assert ent._attr_native_value == "daily_summary"


@pytest.mark.parametrize(
    "body", [RIBBON_BODY, BLE_DISCOVERED_BODY], ids=["activity-ribbon", "ble-discovered"]
)
def test_a_stateless_row_that_is_not_a_summary_still_reads_unknown(body) -> None:
    """The ribbon's row lands every 15 minutes: naming it would make Last
    event, the voice brief and the timeline read it most of a quiet day."""
    ent = _sensor(_hass())
    ent._handle_message(_msg(body))
    assert ent._attr_native_value == "unknown"
    last = ENTRY.runtime_data.devices[DEVICE]["last_event"]
    assert last["event_type"] == "unknown"


@pytest.mark.parametrize(
    "body, expected",
    [
        # Only the wire's placeholder is limited to the summaries; an empty
        # event_type (no firmware writes one) takes any type, as the chain says.
        ({"event_id": 7, "event_type": "", "type": "canary_discovered"}, "canary_discovered"),
        ({"event_id": 7, "type": "ribbon_bucket_advanced"}, "ribbon_bucket_advanced"),
        # The placeholder rule reads event_type only: a type that says
        # `unknown` is taken as it stands.
        ({"event_id": 7, "event_type": "", "type": "unknown", "event": "x"}, "unknown"),
        # A placeholder row whose type is not a string is no summary.
        ({"event_id": 7, "event_type": "unknown", "type": ["daily_summary"]}, "unknown"),
        # The limit reads the row's type, not its module.
        ({"event_id": 7, "event_type": "unknown", "type": "daily_summary"}, "daily_summary"),
    ],
    ids=[
        "empty-takes-any-type", "missing-takes-any-type", "type-unknown-kept", "type-a-list",
        "summary-without-module",
    ],
)
def test_where_the_summary_limit_applies(body, expected) -> None:
    ent = _sensor(_hass())
    ent._handle_message(_msg(json.dumps(body)))
    assert ent._attr_native_value == expected


@pytest.mark.parametrize(
    "body",
    [
        {"event_id": 7},
        {"event_id": 7, "event_type": "", "type": ""},
        {"event_id": 7, "event_type": None, "type": None},
        {"event_id": 7, "event_type": "unknown", "type": "", "module": ""},
        {"event_id": 7, "event_type": "", "event": ""},
    ],
    ids=["nothing", "both-empty", "both-null", "placeholder-no-type", "empty-event"],
)
def test_no_type_at_all_reads_unknown(body) -> None:
    ent = _sensor(_hass())
    ent._handle_message(_msg(json.dumps(body)))
    assert ent._attr_native_value == "unknown"


def test_a_stateful_row_still_reads_its_state_over_its_type() -> None:
    ent = _sensor(_hass())
    ent._handle_message(_msg(PRESENT_BODY))
    assert ent._attr_native_value == "present"
    # The sense / sentinel dialects carry neither event_type nor type.
    ent._handle_message(_msg('{"event":"presence_start","seq":3,"occupants":1}'))
    assert ent._attr_native_value == "presence_start"


def test_a_later_stateful_row_replaces_the_summary() -> None:
    ent = _sensor(_hass())
    ent._handle_message(_msg(DAILY_SUMMARY_BODY))
    assert ent._attr_native_value == "daily_summary"
    ent._handle_message(_msg(PRESENT_BODY))
    assert ent._attr_native_value == "present"
    # And a later summary replaces the stateful row in turn.
    ent._handle_message(_msg(HELD_SUMMARY_BODY.replace('"event_id":42', '"event_id":44')))
    assert ent._attr_native_value == "held_summary"
    # The next ribbon row (at most 15 minutes on) reads unknown again.
    ent._handle_message(_msg(RIBBON_BODY))
    assert ent._attr_native_value == "unknown"


def test_a_signed_summary_verifies_and_reads_its_type() -> None:
    priv, store = _pinned()
    ent = _sensor(_hass(store))
    ent._handle_message(_msg(_signed(DAILY_SUMMARY_BODY, priv, store)))
    assert ent._attr_native_value == "daily_summary"
    assert ent._attr_extra_state_attributes["verified"] is True
    assert ent._attr_extra_state_attributes["trust_reason"] == "ok"


def test_a_replayed_older_signed_row_keeps_the_newer_summary() -> None:
    priv, store = _pinned()
    ent = _sensor(_hass(store))
    ent._handle_message(_msg(_signed(DAILY_SUMMARY_BODY, priv, store)))
    assert ent._attr_native_value == "daily_summary"
    # An older (event 40), validly signed stateful row re-sent onto the broker.
    ent._handle_message(_msg(_signed(PRESENT_BODY, priv, store, event_id=40)))
    assert ent._attr_native_value == "daily_summary"
    assert ent._attr_extra_state_attributes["trust_reason"] == "replay"
    assert ent._attr_extra_state_attributes["verified"] is False


def test_a_replayed_older_signed_summary_keeps_the_newer_state() -> None:
    priv, store = _pinned()
    ent = _sensor(_hass(store))
    ent._handle_message(_msg(_signed(PRESENT_BODY, priv, store)))
    assert ent._attr_native_value == "present"
    ent._handle_message(_msg(_signed(DAILY_SUMMARY_BODY, priv, store)))  # event 41 < 43
    assert ent._attr_native_value == "present"
    assert ent._attr_extra_state_attributes["trust_reason"] == "replay"
    # The voice brief still holds the newer event, too.
    last = ENTRY.runtime_data.devices[DEVICE]["last_event"]
    assert last["event_type"] == "present"


def test_the_voice_brief_records_the_same_value() -> None:
    priv, store = _pinned()
    hass = _hass(store)
    ent = _sensor(hass)
    ent._handle_message(_msg(_signed(DAILY_SUMMARY_BODY, priv, store)))
    last = ENTRY.runtime_data.devices[DEVICE]["last_event"]
    assert last["event_type"] == ent._attr_native_value == "daily_summary"
    assert last["trusted"] is True
    brief = fleet_brief(
        [{"devices": ENTRY.runtime_data.devices, "verify": ENTRY.runtime_data.verify}],
        now=last["received_at"] + 60,
    )
    assert brief["canary_latest"]["event_type"] == "daily_summary"
    # The casual and night answers speak it as words, where they used to
    # say "unknown"; the last-event answer reads the dictionary's label,
    # which for a type it does not list is the id itself.
    assert _spoken_label(brief["canary_latest"]["event_type"]) == "daily summary"
    assert "the last thing witnessed was daily summary," in speak_whats_up(brief)
    night = fleet_brief(
        [{"devices": ENTRY.runtime_data.devices, "verify": ENTRY.runtime_data.verify}],
        now=last["received_at"] + 60,
        local_hour=2,
    )
    assert speak_whats_up(night).startswith("One thing in the last hour: daily summary,")
    assert speak_last_event(brief).startswith("daily_summary, ")


@pytest.mark.parametrize("payload", ["not json at all", "5", '["daily_summary"]'])
def test_the_raw_payload_fallback_is_unchanged(payload) -> None:
    """Junk and JSON that is not an object never reach the fallback chain:
    the bounded raw-payload state is what it was."""
    ent = _sensor(_hass())
    ent._handle_message(_msg(payload))
    assert ent._attr_native_value == payload


@pytest.mark.skipif(
    not _WIRE_TEST.is_file(),
    reason="monorepo-only: the HACS mirror carries no firmware source",
)
def test_the_placeholder_is_the_one_the_firmware_test_pins() -> None:
    """test_csi_event_wire.cpp builds a body from values with no state and
    pins what `state` and `event_type` say; that word is the one the
    sensor treats as saying nothing."""
    text = _WIRE_TEST.read_text(encoding="utf-8")
    m = re.search(r"static int test_replay_flag_and_defaults\(\)\s*\{(.*?)\n\}", text, re.S)
    assert m, "test_replay_flag_and_defaults moved; update this test"
    fn = m.group(1)
    assert "no state" in fn, "the firmware case no longer builds a stateless body"
    state = re.findall(r'\\"state\\":\\"([a-z_]+)\\"', fn)
    event_type = re.findall(r'\\"event_type\\":\\"([a-z_]+)\\"', fn)
    assert state == event_type == [sensor_platform._WIRE_NO_STATE]
