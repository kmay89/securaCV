"""The Apple Home projection's event vocabulary, from the Python side.

`scripts/lint_dictionary_sync.py` already gates this mapping against
`spec/witness_dictionary.json` and the Rust core. These tests cover what a
text-level linter cannot: that the *lookup* behaves correctly on the payload
shapes that actually arrive, and that an unrecognized event stays silent
rather than falling back to something plausible.
"""

from __future__ import annotations

import json
from pathlib import Path

from ..const import (
    HOMEKIT_EVENT_SIGNALS,
    HOMEKIT_MOTION_HOLD_SECONDS,
    homekit_signals_for_event,
)

DICTIONARY = (
    Path(__file__).resolve().parents[3] / "spec" / "witness_dictionary.json"
)


def _dictionary_map() -> dict[str, list[str]]:
    data = json.loads(DICTIONARY.read_text(encoding="utf-8"))
    return data["homekit_projection"]["event_signals"]["map"]


def test_mirror_matches_the_dictionary() -> None:
    """The dictionary is the source of truth; this module is a copy of it."""
    expected = _dictionary_map()
    actual = {k: list(v) for k, v in HOMEKIT_EVENT_SIGNALS.items()}
    assert actual == expected


def test_camel_case_events_resolve() -> None:
    """The kernel publishes serde variant names."""
    assert homekit_signals_for_event("BoundaryCrossingObjectLarge") == ("motion",)
    assert homekit_signals_for_event("PresenceInRestrictedZone") == ("occupancy",)
    assert homekit_signals_for_event("TamperDetected") == ("tamper",)


def test_snake_case_events_resolve() -> None:
    """Some producers use the dictionary's snake_case id instead."""
    assert homekit_signals_for_event("boundary_crossing_object_large") == ("motion",)
    assert homekit_signals_for_event("presence_in_restricted_zone") == ("occupancy",)


def test_an_unknown_event_asserts_nothing() -> None:
    """No fallback. Inventing a claim about someone's home is the failure
    mode this guards against, and 'motion' is the tempting wrong answer."""
    for unknown in ["NotAnEvent", "face_recognized", "", "   ", "motion"]:
        assert homekit_signals_for_event(unknown) == (), unknown


def test_the_two_deliberate_empties_stay_empty() -> None:
    """Both map to nothing, for two different reasons.

    An acoustic impulse has no HAP counterpart, and publishing a sound as
    motion would be a false statement. A contact *change* is not a contact
    *state*, so deriving one from the other would show a door as open every
    time it was closed.
    """
    assert homekit_signals_for_event("AcousticImpulseInZone") == ()
    assert homekit_signals_for_event("ContactStateChange") == ()


def test_no_event_asserts_an_unknown_signal() -> None:
    """Every signal named by the map has to be one the projection can
    actually publish."""
    known = {"motion", "occupancy", "contact", "tamper", "active", "low_battery"}
    for event, signals in HOMEKIT_EVENT_SIGNALS.items():
        for signal in signals:
            assert signal in known, f"{event} names unknown signal {signal!r}"


def test_no_event_asserts_a_class_scoped_signal() -> None:
    """The class-scoped signals are opt-in and ride along with a class word,
    never with a bare event type. An event mapping straight to one would
    publish the coarse object class without anyone turning it on."""
    for event, signals in HOMEKIT_EVENT_SIGNALS.items():
        for signal in signals:
            assert not signal.startswith("motion_"), (
                f"{event} maps to class-scoped {signal!r}, which would bypass consent"
            )


def test_hold_window_is_sane() -> None:
    """A hold of zero would latch nothing; an enormous one would latch
    forever. It also wants to match the kernel's default so automations
    behave the same on either path."""
    assert 1 <= HOMEKIT_MOTION_HOLD_SECONDS <= 300
    data = json.loads(DICTIONARY.read_text(encoding="utf-8"))
    pacing = data["homekit_projection"]["pacing"]
    kernel_hold_s = (
        pacing["default_motion_hold_ticks"] * pacing["default_tick_ms"] / 1000
    )
    assert HOMEKIT_MOTION_HOLD_SECONDS == kernel_hold_s
