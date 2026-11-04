"""Tests for voice.py — the pure logic behind the Assist intents.

The spoken answers are quotable out of context, so the vocabulary
discipline is what these tests pin down: "verified" only for a checked
signature, "heard" for anything looser, worst news first, and coarse
(ten-minute-floor) time phrasing.
"""
from __future__ import annotations

import json
from pathlib import Path

from ..const import EVENT_TYPE_METADATA, normalize_event_type
from ..sensor import _SUMMARY_TYPES
from ..voice import (
    _ALERT_EVENT_TYPES,
    _snake,
    ago_phrase,
    fleet_brief,
    match_device,
    record_canary_event,
    speak_device_check,
    speak_fleet_status,
    speak_goodnight,
    speak_help,
    speak_last_event,
    speak_offline_check,
    speak_privacy,
    speak_roster,
    speak_whats_up,
)

NOW = 1_000_000.0


def _entry(devices=None, verify=None, kernel=None):
    return {"devices": devices or {}, "verify": verify or {}, "kernel": kernel}


# ---------------------------------------------------------------- brief


def test_empty_brief():
    brief = fleet_brief([], NOW)
    assert brief["device_count"] == 0
    assert not brief["kernel_configured"]


def test_brief_counts_trust_buckets():
    devices = {"a": {}, "b": {}, "c": {}, "d": {}}
    verify = {
        "a": {"trusted": True, "reason": "ok"},
        "b": {"trusted": False, "reason": "mismatch"},
        "c": {"trusted": False, "reason": "unsigned"},
        # "d" has no verify record at all -> unknown
    }
    brief = fleet_brief([_entry(devices, verify)], NOW)
    assert brief["device_count"] == 4
    assert brief["verified"] == ["a"]
    assert brief["mismatched"] == ["b"]
    assert brief["unsigned"] == ["c"]
    assert brief["unknown"] == ["d"]


def test_brief_picks_newest_canary_event_across_entries():
    e1 = _entry({"a": {"last_event": {"event_type": "x", "received_at": 10.0}}})
    e2 = _entry({"b": {"last_event": {"event_type": "y", "received_at": 20.0}}})
    brief = fleet_brief([e1, e2], NOW)
    assert brief["canary_latest"]["device_id"] == "b"
    assert brief["canary_latest"]["event_type"] == "y"


def test_brief_kernel_ok_is_anded_across_entries():
    ok = _entry(kernel={"ok": True, "latest_event": None})
    down = _entry(kernel={"ok": False, "latest_event": None})
    assert fleet_brief([ok], NOW)["kernel_ok"] is True
    assert fleet_brief([ok, down], NOW)["kernel_ok"] is False
    assert fleet_brief([], NOW)["kernel_ok"] is None


# ------------------------------------------------------- record helper


def test_record_canary_event_creates_device_slot():
    devices: dict = {}
    record_canary_event(devices, "cv-1", "contact_state_change", 42.0, trusted=True, reason="ok")
    assert devices["cv-1"]["last_event"] == {
        "event_type": "contact_state_change",
        "received_at": 42.0,
        "trusted": True,
        "reason": "ok",
    }
    # A newer event replaces the old one in place; no verdict -> None, which
    # must speak as unverified, never as trusted-by-default.
    record_canary_event(devices, "cv-1", "tamper_detected", 43.0)
    assert devices["cv-1"]["last_event"]["event_type"] == "tamper_detected"
    assert devices["cv-1"]["last_event"]["trusted"] is None
    # Empty device_id is ignored rather than minting a phantom device.
    record_canary_event(devices, "", "x", 44.0)
    assert set(devices) == {"cv-1"}


# ------------------------------------------------------------- speech


def test_speak_nothing_configured():
    speech = speak_fleet_status(fleet_brief([], NOW))
    assert "hasn't heard from any Canaries" in speech
    assert "no witness kernel" in speech


def test_speak_all_verified_uses_the_reserved_word():
    verify = {k: {"trusted": True, "reason": "ok"} for k in ("a", "b", "c")}
    speech = speak_fleet_status(fleet_brief([_entry({"a": {}, "b": {}, "c": {}}, verify)], NOW))
    assert "3 Canaries in the fleet" in speech
    assert "all signature-verified against their pinned keys" in speech


def test_speak_mismatch_leads_the_answer():
    devices = {"a": {}, "b": {}}
    verify = {
        "a": {"trusted": True, "reason": "ok"},
        "b": {"trusted": False, "reason": "mismatch"},
    }
    speech = speak_fleet_status(fleet_brief([_entry(devices, verify)], NOW))
    assert speech.startswith("Key mismatch on b")
    assert "1 signature-verified" in speech


def test_speak_unverified_devices_are_only_heard():
    # A device with no verify record must never be called verified.
    speech = speak_fleet_status(fleet_brief([_entry({"a": {}})], NOW))
    assert "verified" not in speech.split("heard but not yet verified")[0]
    assert "1 heard but not yet verified" in speech


def test_speak_kernel_reachability():
    up = fleet_brief([_entry(kernel={"ok": True, "latest_event": None})], NOW)
    down = fleet_brief([_entry(kernel={"ok": False, "latest_event": None})], NOW)
    assert "kernel is reachable" in speak_fleet_status(up)
    assert "kernel is not reachable" in speak_fleet_status(down)


def test_speak_last_event_uses_friendly_label_and_coarse_time():
    devices = {
        "gate": {
            "last_event": {
                "event_type": "boundary_crossing_object_large",
                "received_at": NOW - 120,
                "trusted": True,
                "reason": "ok",
            }
        }
    }
    speech = speak_last_event(fleet_brief([_entry(devices)], NOW))
    # The label is const.py's D13 wording for the coarse class ("Person or
    # vehicle"), the same one the dashboard shows.
    assert speech == (
        "Person or vehicle, within the last ten minutes, "
        "from Canary gate. The event signature is verified against the "
        "device's pinned key."
    )


def test_speak_last_event_trust_qualifiers_never_launder():
    # A key-mismatch publish must be called out, not spoken as the truth.
    mismatch = {
        "gate": {
            "last_event": {
                "event_type": "tamper_detected",
                "received_at": NOW - 60,
                "trusted": False,
                "reason": "mismatch",
            }
        }
    }
    speech = speak_last_event(fleet_brief([_entry(mismatch)], NOW))
    assert "key does not match its pin" in speech
    assert "treat this event as unverified" in speech

    # No verdict at all (verifier never ran) speaks as unverified too —
    # never trusted-by-default.
    unknown = {
        "gate": {
            "last_event": {
                "event_type": "contact_state_change",
                "received_at": NOW - 60,
            }
        }
    }
    speech = speak_last_event(fleet_brief([_entry(unknown)], NOW))
    assert "published without a verified signature" in speech
    assert "verified against" not in speech


def test_speak_last_event_names_both_sources_when_they_differ():
    devices = {
        "gate": {
            "last_event": {
                "event_type": "boundary_crossing_object_large",
                "received_at": NOW - 60,
                "trusted": True,
                "reason": "ok",
            }
        }
    }
    kernel = {"ok": True, "latest_event": {"event_type": "TamperDetected"}}
    speech = speak_last_event(fleet_brief([_entry(devices, kernel=kernel)], NOW))
    assert "from Canary gate" in speech
    assert "The kernel log's latest event is Tamper detected." in speech

    # Same label on both sides -> no redundant second sentence.
    kernel_same = {
        "ok": True,
        "latest_event": {"event_type": "BoundaryCrossingObjectLarge"},
    }
    speech = speak_last_event(fleet_brief([_entry(devices, kernel=kernel_same)], NOW))
    assert "kernel log's latest" not in speech


def test_speak_last_event_kernel_fallback_and_none():
    kernel = {"ok": True, "latest_event": {"event_type": "TamperDetected"}}
    speech = speak_last_event(fleet_brief([_entry(kernel=kernel)], NOW))
    assert speech == "Tamper detected — the latest event in the kernel log."
    assert (
        speak_last_event(fleet_brief([], NOW))
        == "No witness events since the hub started listening."
    )


# Sweep HA42: a type the dictionary does not list was spoken as its raw id
# ("daily_summary, within ..."). It is now said in words, capitalized only
# where it starts the sentence; a listed type is said exactly as before.


def _canary_event(event_type):
    return {
        "gate": {
            "last_event": {
                "event_type": event_type,
                "received_at": NOW - 60,
                "trusted": True,
                "reason": "ok",
            }
        }
    }


def test_speak_last_event_says_a_daily_summary_in_words():
    speech = speak_last_event(fleet_brief([_entry(_canary_event("daily_summary"))], NOW))
    assert speech == (
        "Daily summary, within the last ten minutes, from Canary gate. The event "
        "signature is verified against the device's pinned key."
    )
    assert "_" not in speech


def test_speak_last_event_says_an_unlisted_kernel_type_in_words():
    """The kernel sends an event type as its Rust variant (src/lib.rs's
    EventType derives Serialize with no rename), so a type newer than this
    integration arrives CamelCase; it is said in words, as is the same type
    in snake_case."""
    for spelled in ("HeldSummary", "held_summary"):
        kernel = {"ok": True, "latest_event": {"event_type": spelled}}
        # Beside a Canary event: mid-sentence, so not capitalized.
        devices = _canary_event("boundary_crossing_object_large")
        speech = speak_last_event(fleet_brief([_entry(devices, kernel=kernel)], NOW))
        assert speech.startswith("Person or vehicle, within the last ten minutes, from Canary gate."), spelled
        assert speech.endswith(" The kernel log's latest event is held summary."), spelled
        # Alone: it starts the sentence.
        speech = speak_last_event(fleet_brief([_entry(kernel=kernel)], NOW))
        assert speech == "Held summary — the latest event in the kernel log.", spelled
    kernel = {"ok": True, "latest_event": {"event_type": "DailySummary"}}
    speech = speak_last_event(fleet_brief([_entry(kernel=kernel)], NOW))
    assert speech == "Daily summary — the latest event in the kernel log."


def test_speak_last_event_names_an_unlisted_type_once_when_both_sources_agree():
    """The both-sources comparison sets the two labels side by side as said
    mid-sentence: an unlisted type the Canary and the kernel both report is
    named once, not twice for the capital that starts the first sentence."""
    kernel = {"ok": True, "latest_event": {"event_type": "daily_summary"}}
    speech = speak_last_event(
        fleet_brief([_entry(_canary_event("daily_summary"), kernel=kernel)], NOW)
    )
    assert speech.startswith("Daily summary, ")
    assert "kernel log's latest" not in speech
    other = {"ok": True, "latest_event": {"event_type": "held_summary"}}
    speech = speak_last_event(
        fleet_brief([_entry(_canary_event("daily_summary"), kernel=other)], NOW)
    )
    assert speech.endswith(" The kernel log's latest event is held summary.")


def test_speak_last_event_names_an_unlisted_type_once_in_each_sources_spelling():
    """In practice the two sources spell one type differently: the Canary
    snake_case, the kernel its CamelCase variant. Both are read into the
    same words, so the type is named once; a different type still is not."""
    kernel = {"ok": True, "latest_event": {"event_type": "HeldSummary"}}
    speech = speak_last_event(
        fleet_brief([_entry(_canary_event("held_summary"), kernel=kernel)], NOW)
    )
    assert speech == (
        "Held summary, within the last ten minutes, from Canary gate. The event "
        "signature is verified against the device's pinned key."
    )
    other = {"ok": True, "latest_event": {"event_type": "DailySummary"}}
    speech = speak_last_event(
        fleet_brief([_entry(_canary_event("held_summary"), kernel=other)], NOW)
    )
    assert speech.endswith(" The kernel log's latest event is daily summary.")


def _camel(event_type):
    """The kernel's spelling of a dictionary id (the Rust enum variant)."""
    return "".join(part.capitalize() for part in event_type.split("_"))


def test_speak_last_event_says_every_listed_type_as_the_dictionary_writes_it():
    """A listed type is said byte for byte as before HA42: its dictionary
    label, unchanged at a sentence start and mid-sentence, in the Canary's
    snake_case and the kernel's CamelCase alike; a missing type is said as
    the dictionary's "Unknown", as before."""
    for event_type, meta in EVENT_TYPE_METADATA.items():
        label = meta["label"]
        for spelled in (event_type, _camel(event_type)):
            canary = speak_last_event(fleet_brief([_entry(_canary_event(spelled))], NOW))
            assert canary.startswith(f"{label}, within the last ten minutes, from Canary gate."), spelled
            kernel = {"ok": True, "latest_event": {"event_type": spelled}}
            alone = speak_last_event(fleet_brief([_entry(kernel=kernel)], NOW))
            assert alone == f"{label} — the latest event in the kernel log.", spelled
            # Mid-sentence, beside a Canary event of another type.
            other = "daily_summary"
            both = speak_last_event(fleet_brief([_entry(_canary_event(other), kernel=kernel)], NOW))
            assert both.endswith(f" The kernel log's latest event is {label}."), spelled
            # And beside the same type: named once.
            same = speak_last_event(
                fleet_brief([_entry(_canary_event(event_type), kernel=kernel)], NOW)
            )
            assert "kernel log's latest" not in same, spelled
    for missing in (None, "", "   ", 5):
        speech = speak_last_event(fleet_brief([_entry(_canary_event(missing))], NOW))
        assert speech.startswith("Unknown, within the last ten minutes,"), missing


def test_speak_last_event_says_every_summary_type_in_words():
    """The Canary's stateless summaries (sensor.py's _SUMMARY_TYPES, the Last
    event state for a summary row) are not dictionary types: each is said as
    words, never as its id."""
    for event_type in _SUMMARY_TYPES:
        assert event_type not in EVENT_TYPE_METADATA, event_type
        said = event_type.replace("_", " ")
        speech = speak_last_event(fleet_brief([_entry(_canary_event(event_type))], NOW))
        assert speech.startswith(f"{said[:1].upper()}{said[1:]}, within the last ten minutes,"), event_type
        assert "_" not in speech, event_type


def test_speak_last_event_never_says_an_underscore():
    """Underscore runs read as one space, the ends trimmed; an id of
    underscores alone names no type and is said as none is ("Unknown")."""
    for event_type, said in (
        ("a__b", "A b"),
        ("_x", "X"),
        ("x_", "X"),
        ("___", "Unknown"),
        ("überwachung_alarm", "Überwachung alarm"),
        ("GPS_lost", "GPS lost"),
        ("HeldSummary", "Held summary"),
        ("Überwachung", "Überwachung"),
        # The first letter takes its titlecase, which is not always its
        # uppercase: one capital, not two.
        ("ßtraße_x", "Sstraße x"),
        ("ﬁre_alarm", "Fire alarm"),
        ("ǆungla_x", "ǅungla x"),
    ):
        speech = speak_last_event(fleet_brief([_entry(_canary_event(event_type))], NOW))
        assert speech.startswith(f"{said}, within the last ten minutes,"), event_type
        assert "_" not in speech, event_type
        kernel = {"ok": True, "latest_event": {"event_type": event_type}}
        alone = speak_last_event(fleet_brief([_entry(kernel=kernel)], NOW))
        assert alone == f"{said} — the latest event in the kernel log.", event_type


# The four other answers that name an event (sweep HA58): what's up by day
# and at night, the device check and goodnight, each from a Canary event and
# (what's up and goodnight) from the kernel log's alone. Each maps to the
# sentence the label sits in: `mid` is the label said mid-sentence, `start`
# the label opening a sentence (the night answer's alarm line).
def _answers_naming(event_type):
    devices = {
        "gate": {
            "status": "online",
            "last_event": {
                "event_type": event_type,
                "received_at": NOW - 60,
                "trusted": True,
                "reason": "ok",
            },
        }
    }
    verify = {"gate": {"trusted": True, "reason": "ok"}}
    canary = fleet_brief([_entry(devices, verify)], NOW)
    night = fleet_brief([_entry(devices, verify)], NOW, local_hour=2)
    kernel = {"ok": True, "latest_event": {"event_type": event_type}}
    quiet = {"gate": {"status": "online"}}
    from_kernel = fleet_brief([_entry(quiet, verify, kernel=kernel)], NOW)
    alert = _snake(event_type) in _ALERT_EVENT_TYPES
    return {
        "what's up": (
            speak_whats_up(canary),
            "First thing: {mid}, within the last ten minutes, from the gate Canary."
            if alert
            else "Some activity lately — the last thing witnessed was {mid}, "
            "within the last ten minutes, from the gate Canary.",
        ),
        "what's up at night": (
            speak_whats_up(night),
            "{start}, within the last ten minutes, from the gate Canary."
            if alert
            else "One thing in the last hour: {mid}, within the last ten minutes, "
            "from the gate Canary. Nothing else.",
        ),
        "device check": (
            speak_device_check(canary, "gate"),
            "The last thing it witnessed was {mid}, within the last ten minutes.",
        ),
        "goodnight": (
            speak_goodnight(canary),
            "Last hour's only note: {mid}, from the gate Canary.",
        ),
        "what's up, kernel log": (
            speak_whats_up(from_kernel),
            "Pretty quiet — the kernel log's latest event is {mid}.",
        ),
        "goodnight, kernel log": (
            speak_goodnight(from_kernel),
            "Nothing new from the Canaries; the latest in the kernel log is {mid}.",
        ),
    }


def _assert_said(event_type, mid, start):
    for answer, (speech, sentence) in _answers_naming(event_type).items():
        expected = sentence.format(mid=mid, start=start)
        assert expected in speech, (event_type, answer, speech)
        assert "_" not in speech, (event_type, answer, speech)
        assert "  " not in speech, (event_type, answer, speech)


def test_every_answer_says_an_unlisted_type_as_the_last_event_answer_does():
    """Sweep HA58: _spoken_label lowered the first letter of every label and
    read each underscore as a space, one for one, so an unlisted id was said
    "gPS lost" for GPS_lost, "heldSummary" for the kernel's HeldSummary, with
    two spaces for a__b and as blanks for underscores alone, in every answer
    but the last-event one. It now says an unlisted id as _said_label does:
    underscore runs one space, the kernel's CamelCase in words, the id's own
    capitals kept mid-sentence and the first letter raised only where the
    label opens a sentence; an id of underscores alone is said as none is."""
    for event_type, mid, start in (
        ("GPS_lost", "GPS lost", "GPS lost"),
        ("HeldSummary", "held summary", "Held summary"),
        ("held_summary", "held summary", "Held summary"),
        ("DailySummary", "daily summary", "Daily summary"),
        ("a__b", "a b", "A b"),
        ("held__summary", "held summary", "Held summary"),
        ("_x", "x", "X"),
        ("x_", "x", "X"),
        ("___", "unknown", "Unknown"),
        ("_", "unknown", "Unknown"),
        ("überwachung_alarm", "überwachung alarm", "Überwachung alarm"),
        ("Überwachung", "überwachung", "Überwachung"),
        # Alert types: the night answer opens on the label.
        ("acoustic_smoke_alarm", "acoustic smoke alarm", "Acoustic smoke alarm"),
        ("AcousticSmokeAlarm", "acoustic smoke alarm", "Acoustic smoke alarm"),
        ("power__loss", "power loss", "Power loss"),
        ("PowerLoss", "power loss", "Power loss"),
        ("UnexpectedReboot", "unexpected reboot", "Unexpected reboot"),
    ):
        _assert_said(event_type, mid, start)


def test_every_answer_keeps_an_acronym_an_id_spells_in_capitals():
    """An id's own capitals are kept wherever it is said, an acronym's
    among them: a run of capitals in a CamelCase id is one word that keeps
    its capitals ("GPSLost" is "GPS lost", not "g p s lost"), and an id with
    no lowercase letter is said as written, not split letter by letter."""
    for event_type, mid, start in (
        ("GPSLost", "GPS lost", "GPS lost"),
        ("GPS", "GPS", "GPS"),
        ("SDRemove", "SD remove", "SD remove"),
        ("HTTPServer2Down", "HTTP server2 down", "HTTP server2 down"),
        ("ACOUSTIC_CO_ALARM", "ACOUSTIC CO ALARM", "ACOUSTIC CO ALARM"),
        ("AcousticCOAlarm", "acoustic CO alarm", "Acoustic CO alarm"),
        ("POWER_LOSS", "POWER LOSS", "POWER LOSS"),
    ):
        _assert_said(event_type, mid, start)
    # And the last-event answer, which says it the same way.
    for event_type, said in (("GPSLost", "GPS lost"), ("GPS", "GPS"), ("SDRemove", "SD remove")):
        speech = speak_last_event(fleet_brief([_entry(_canary_event(event_type))], NOW))
        assert speech.startswith(f"{said}, within the last ten minutes,"), event_type


def test_a_camelcase_id_with_no_capital_run_splits_as_the_dictionary_folds_it():
    """The split keeps a run of capitals whole; with no such run it is
    normalize_event_type's fold, word for word, so the kernel's spelling of
    a type and the Canary's are said alike (HA42's promise, kept)."""
    from ..voice import _id_words

    ids = set(EVENT_TYPE_METADATA) | set(_unlisted_types_the_integration_sees())
    ids |= {"boundary_crossing_object_large", "sd2_error", "plan_a", "über_wachung"}
    for event_type in sorted(ids):
        camel = _camel(event_type)
        assert not any(a.isupper() and b.isupper() for a, b in zip(camel, camel[1:])), camel
        assert _id_words(camel) == (normalize_event_type(camel) or "").replace("_", " "), camel
        assert _id_words(camel) == _id_words(event_type).lower(), camel


def _unlisted_types_the_integration_sees():
    """Every event id this integration can be sent that the dictionary does
    not list: the alert types (the acoustic alarm grammars and the
    system.integrity tamper kinds, whose wire event_type is the kind), the
    summaries Last event names (sensor.py's _SUMMARY_TYPES) and the tamper
    kinds strings.json labels for the Tamper events entity."""
    strings = json.loads((Path(__file__).resolve().parents[1] / "strings.json").read_text("utf-8"))
    tamper = strings["entity"]["event"]["tamper_events"]["state_attributes"]["event_type"]["state"]
    seen = set(_ALERT_EVENT_TYPES) | set(_SUMMARY_TYPES) | set(tamper)
    return sorted(seen - set(EVENT_TYPE_METADATA))


def test_every_unlisted_type_the_integration_sees_is_said_in_words_everywhere():
    """Each in the Canary's snake_case, the kernel's CamelCase, all capitals,
    with doubled and trailing underscores, through every answer: never an
    underscore or a doubled space, the words of the id in its own case.

    A lowercase id is said in its own case, so `sd_remove` is "sd remove"
    and `acoustic_co_alarm` "acoustic co alarm": the id spells no capital
    to keep. Words of their own for those ids, as the card has for the
    tamper kinds, would be a change to what the voice says, not this one."""
    unlisted = _unlisted_types_the_integration_sees()
    assert {"daily_summary", "held_summary", "acoustic_co_alarm", "sd_remove"} <= set(unlisted)
    for event_type in unlisted:
        words = event_type.replace("_", " ")
        said_start = words[:1].upper() + words[1:]
        for spelled in (event_type, _camel(event_type), event_type.replace("_", "__"),
                        f"_{event_type}_"):
            _assert_said(spelled, words, said_start)
        _assert_said(event_type.upper(), words.upper(), words.upper())


def test_every_answer_says_a_listed_type_as_it_always_has():
    """A type the dictionary lists keeps today's words in every answer, in
    both spellings: its label with the first letter lowered mid-sentence,
    and as written where it opens the night alarm line."""
    for event_type, meta in EVENT_TYPE_METADATA.items():
        label = meta["label"]
        for spelled in (event_type, _camel(event_type)):
            _assert_said(spelled, label[:1].lower() + label[1:], label)


def test_a_listed_label_that_opens_on_an_acronym_keeps_its_capitals(monkeypatch):
    """Lowering a listed label's first letter is for a word, not an
    acronym: a label that opens on two capitals is said as written."""
    monkeypatch.setitem(EVENT_TYPE_METADATA, "co_alarm", {"label": "CO alarm", "icon": "mdi:alarm"})
    _assert_said("co_alarm", "CO alarm", "CO alarm")
    _assert_said("CoAlarm", "CO alarm", "CO alarm")


def test_whats_up_reads_naturally_all_good():
    devices = {
        "gate": {
            "status": "online",
            "last_event": {
                "event_type": "boundary_crossing_object_large",
                "received_at": NOW - 7200,
                "trusted": True,
                "reason": "ok",
            },
        },
        "porch": {"status": '{"status": "online", "firmware_version": "1.0"}'},
    }
    verify = {k: {"trusted": True, "reason": "ok"} for k in ("gate", "porch")}
    speech = speak_whats_up(fleet_brief([_entry(devices, verify)], NOW))
    assert speech == (
        "Pretty quiet — the last thing witnessed was person or vehicle, "
        "about 2 hours ago, from the gate Canary. "
        "All 2 Canaries are online, every signature verified."
    )


def test_whats_up_never_calls_a_stale_device_online():
    # A discovered, verified device whose retained status says offline must
    # not be spoken as online — "in the fleet" is the honest word.
    devices = {"gate": {"status": "online"}, "shed": {"status": "offline"}}
    verify = {k: {"trusted": True, "reason": "ok"} for k in ("gate", "shed")}
    speech = speak_whats_up(fleet_brief([_entry(devices, verify)], NOW))
    assert "are online" not in speech
    assert "2 Canaries in the fleet, 1 online, 2 verified." in speech


def test_whats_up_kernel_outage_is_never_spoken_as_quiet():
    # Kernel configured but unreachable, nothing cached: silence means
    # "can't see", not "nothing happened".
    down = _entry(kernel={"ok": False, "latest_event": None})
    speech = speak_whats_up(fleet_brief([down], NOW))
    assert "All quiet" not in speech
    assert "won't claim it's been quiet" in speech
    # And the outage is not repeated as a second sentence.
    assert speech.count("can't reach the witness kernel") == 1

    # A cached kernel event while the kernel is down is labeled stale.
    stale = _entry(kernel={"ok": False, "latest_event": {"event_type": "TamperDetected"}})
    speech = speak_whats_up(fleet_brief([stale], NOW))
    assert "may be stale" in speech
    assert "Pretty quiet" not in speech


def test_whats_up_recent_activity_changes_the_opener():
    devices = {
        "gate": {
            "last_event": {
                "event_type": "contact_state_change",
                "received_at": NOW - 300,
                "trusted": True,
                "reason": "ok",
            }
        }
    }
    verify = {"gate": {"trusted": True, "reason": "ok"}}
    speech = speak_whats_up(fleet_brief([_entry(devices, verify)], NOW))
    assert speech.startswith("Some activity lately —")
    assert "within the last ten minutes" in speech


def test_whats_up_quiet_and_empty_cases():
    # Devices but no events yet: an honest all-quiet, not an error.
    speech = speak_whats_up(fleet_brief([_entry({"gate": {}})], NOW))
    assert "All quiet — nothing witnessed since I started listening." in speech
    # Nothing configured at all: says so, invites a later ask.
    assert "haven't heard from any Canaries" in speak_whats_up(fleet_brief([], NOW))


def test_whats_up_leads_with_trouble_and_holds_untrusted_loosely():
    # A tamper event is alert-class, so it takes the first sentence; the
    # key-mismatch heads-up follows, softened to "Also, heads up".
    devices = {
        "gate": {
            "last_event": {
                "event_type": "tamper_detected",
                "received_at": NOW - 60,
                "trusted": False,
                "reason": "mismatch",
            }
        }
    }
    verify = {"gate": {"trusted": False, "reason": "mismatch"}}
    speech = speak_whats_up(fleet_brief([_entry(devices, verify)], NOW))
    assert speech.startswith("First thing: tamper detected")
    assert "Also, heads up: gate is publishing" in speech
    assert "hold it loosely" in speech
    # The reserved word never leaks into an unverified fleet's health line.
    assert "every signature verified" not in speech

    # A non-alert event with a mismatch keeps the mismatch first.
    quiet_devices = {
        "gate": {
            "last_event": {
                "event_type": "contact_state_change",
                "received_at": NOW - 60,
                "trusted": False,
                "reason": "mismatch",
            }
        }
    }
    speech = speak_whats_up(fleet_brief([_entry(quiet_devices, verify)], NOW))
    assert speech.startswith("Heads up first: gate is publishing")


def test_whats_up_alert_class_event_leads_even_over_mismatch():
    # A smoke-alarm pattern is the first sentence, ahead of the key
    # mismatch heads-up, which softens to "Also, heads up".
    devices = {
        "kitchen": {
            "status": "online",
            "last_event": {
                "event_type": "acoustic_smoke_alarm",
                "received_at": NOW - 300,
                "trusted": True,
                "reason": "ok",
            },
        },
        "gate": {"status": "online"},
    }
    verify = {
        "kitchen": {"trusted": True, "reason": "ok"},
        "gate": {"trusted": False, "reason": "mismatch"},
    }
    speech = speak_whats_up(fleet_brief([_entry(devices, verify)], NOW))
    assert speech.startswith(
        "First thing: acoustic smoke alarm, within the last ten minutes, "
        "from the kitchen Canary."
    )
    assert "Also, heads up: gate" in speech
    # The kernel's CamelCase spelling of an alert type leads too.
    tamper = {
        "shed": {
            "last_event": {
                "event_type": "TamperDetected",
                "received_at": NOW - 60,
                "trusted": True,
                "reason": "ok",
            }
        }
    }
    assert speak_whats_up(fleet_brief([_entry(tamper)], NOW)).startswith("First thing:")


def test_whats_up_weather_close():
    brief = fleet_brief(
        [_entry({"gate": {"status": "online"}}, {"gate": {"trusted": True, "reason": "ok"}})],
        NOW,
        weather={"condition": "partlycloudy", "temp": 71.6},
    )
    speech = speak_whats_up(brief)
    assert (
        "Outside it's 72 degrees and partly cloudy — plenty of bright spells."
        in speech
    )
    # Condition-only and absent-weather cases.
    just_condition = fleet_brief([_entry({"gate": {}})], NOW, weather={"condition": "rainy"})
    assert "Outside it looks rainy — the garden will be glad." in speak_whats_up(just_condition)
    no_weather = fleet_brief([_entry({"gate": {}})], NOW)
    assert "Outside" not in speak_whats_up(no_weather)


# The complete HA weather condition vocabulary. If HA adds a condition,
# add a phrase — the fallback keeps it speakable meanwhile.
_ALL_HA_CONDITIONS = (
    "sunny", "clear-night", "partlycloudy", "cloudy", "windy",
    "windy-variant", "fog", "rainy", "pouring", "lightning",
    "lightning-rainy", "hail", "snowy", "snowy-rainy", "exceptional",
)


def test_weather_speaks_every_ha_condition_year_round():
    for condition in _ALL_HA_CONDITIONS:
        brief = fleet_brief([_entry({"g": {}})], NOW, weather={"condition": condition, "temp": 40})
        speech = speak_whats_up(brief)
        # Every condition gets a real phrase: no raw hyphens, no camel
        # squish, and always the warm second clause.
        assert "Outside it's 40 degrees and " in speech
        weather_line = speech.split("Outside it's 40 degrees and ", 1)[1]
        assert "-" not in weather_line.split(" — ")[0]
        assert "partlycloudy" not in speech
        assert " — " in weather_line, condition


def test_weather_seasonal_spot_checks_stay_optimistic():
    def line(condition, temp):
        brief = fleet_brief([_entry({"g": {}})], NOW, weather={"condition": condition, "temp": temp})
        return speak_whats_up(brief).split("Outside it's ", 1)[1]

    # Winter, spring, summer, fall — the year-round demo.
    assert line("snowy", 28) == "28 degrees and snowing — it'll be pretty out there."
    assert line("rainy", 55) == "55 degrees and rainy — the garden will be glad."
    assert line("sunny", 84) == "84 degrees and sunny — a good one to step out in."
    assert line("windy", 61) == "61 degrees and windy — the fresh kind."
    # Night gets its stars.
    assert line("clear-night", 48) == "48 degrees and clear — good stars if you look up."


def test_weather_exceptional_is_never_sugarcoated():
    brief = fleet_brief([_entry({"g": {}})], NOW, weather={"condition": "exceptional", "temp": 90})
    speech = speak_whats_up(brief)
    assert "worth checking the forecast" in speech
    # An unknown custom condition degrades to plain words, not silence.
    custom = fleet_brief([_entry({"g": {}})], NOW, weather={"condition": "volcanic-ash"})
    assert "Outside it looks volcanic ash." in speak_whats_up(custom)


def test_whats_up_mentions_pending_updates_last():
    brief = fleet_brief(
        [_entry({"gate": {}}, {"gate": {"trusted": True, "reason": "ok"}})],
        NOW,
        pending_updates=["Home Assistant Core", "Whisper", "Piper"],
    )
    speech = speak_whats_up(brief)
    assert speech.endswith(
        "Also, 3 updates are waiting when you have a minute — "
        "Home Assistant Core and Whisper and 1 more."
    )
    one = fleet_brief([_entry({"gate": {}})], NOW, pending_updates=["Whisper"])
    assert "Whisper has an update waiting" in speak_whats_up(one)


def test_sentences_yaml_matches_registered_intents():
    """docs/voice_sentences_en.yaml must name exactly the intents intent.py
    registers — otherwise the wizard installs sentences that error, or an
    intent exists that no sentence can reach. Parsed with regex on purpose:
    no yaml dependency, and the intent names are rigid identifiers."""
    import re
    from pathlib import Path

    repo = Path(__file__).resolve().parents[3]
    yaml_text = (repo / "docs" / "voice_sentences_en.yaml").read_text()
    yaml_intents = set(re.findall(r"^  (Securacv\w+):$", yaml_text, re.MULTILINE))

    intent_src = (repo / "custom_components" / "securacv" / "intent.py").read_text()
    registered = set(re.findall(r'^INTENT_\w+ = "(Securacv\w+)"$', intent_src, re.MULTILINE))

    assert yaml_intents, "no intents found in voice_sentences_en.yaml"
    assert yaml_intents == registered


def test_ago_phrase_is_coarse():
    assert ago_phrase(0) == "within the last ten minutes"
    assert ago_phrase(599) == "within the last ten minutes"
    assert ago_phrase(600) == "about 10 minutes ago"
    assert ago_phrase(1799) == "about 20 minutes ago"
    assert ago_phrase(3600) == "about 1 hour ago"
    assert ago_phrase(7300) == "about 2 hours ago"
    assert ago_phrase(90000) == "about 1 day ago"
    # Never a seconds-precision phrase, never negative.
    assert ago_phrase(-5) == "within the last ten minutes"


# ─────────────────────────────────────────────────────────────────────────
# The conversational intents
# ─────────────────────────────────────────────────────────────────────────

def test_match_device_is_tolerant_but_never_guesses():
    ids = ["gate", "front-door", "cv-1"]
    # Exact, and with the words people wrap around a name.
    assert match_device(ids, "gate") == "gate"
    assert match_device(ids, "the gate canary") == "gate"
    # Speech-to-text drops the hyphen.
    assert match_device(ids, "front door") == "front-door"
    assert match_device(ids, "the front door canary") == "front-door"
    # Nothing plausible -> None, so the answer can say so.
    assert match_device(ids, "basement") is None
    assert match_device(ids, "") is None
    assert match_device([], "gate") is None
    # Ambiguity is not resolved by guessing.
    assert match_device(["porch-north", "porch-south"], "porch") is None


def test_speak_device_check_reports_one_device():
    devices = {
        "gate": {
            "status": "online",
            "last_event": {
                "event_type": "contact_state_change",
                "received_at": NOW - 1800,
                "trusted": True,
                "reason": "ok",
            },
        },
        "shed": {"status": "offline"},
    }
    verify = {
        "gate": {"trusted": True, "reason": "ok"},
        "shed": {"trusted": True, "reason": "ok"},
    }
    brief = fleet_brief([_entry(devices, verify)], NOW)

    speech = speak_device_check(brief, "the gate canary")
    assert speech.startswith("The gate Canary is online")
    assert "signature checks out against the pinned key" in speech
    assert "contact state change, about 30 minutes ago" in speech


def test_device_check_never_launders_an_old_untrusted_event():
    # The device verifies NOW, but the cached event was published without
    # a verified signature. The event must carry its own verdict, or the
    # device-level "checks out" clause would launder it.
    devices = {
        "gate": {
            "status": "online",
            "last_event": {
                "event_type": "contact_state_change",
                "received_at": NOW - 600,
                "trusted": False,
                "reason": "unsigned",
            },
        }
    }
    verify = {"gate": {"trusted": True, "reason": "ok"}}
    speech = speak_device_check(fleet_brief([_entry(devices, verify)], NOW), "gate")
    assert "signature checks out against the pinned key" in speech
    assert "that one arrived without a verified signature" in speech

    # A mismatched event says so in the event clause too.
    devices["gate"]["last_event"]["reason"] = "mismatch"
    speech = speak_device_check(fleet_brief([_entry(devices, verify)], NOW), "gate")
    assert "under the mismatched key" in speech


def test_speak_device_check_offline_unknown_and_empty():
    devices = {"gate": {"status": "online"}, "shed": {"status": "offline"}}
    verify = {k: {"trusted": True, "reason": "ok"} for k in devices}
    brief = fleet_brief([_entry(devices, verify)], NOW)

    # Offline and eventless reads honestly, never as "online".
    shed = speak_device_check(brief, "shed")
    assert "not reporting as online right now" in shed
    assert "hasn't witnessed anything" in shed

    # An unknown name offers what it does know instead of failing.
    unknown = speak_device_check(brief, "basement")
    assert "don't have a Canary by that name" in unknown
    assert "gate" in unknown and "shed" in unknown

    # No fleet at all.
    assert "haven't heard from any Canaries yet" in speak_device_check(
        fleet_brief([], NOW), "gate"
    )


def test_speak_offline_check_answers_the_yes_or_no():
    all_up = {"gate": {"status": "online"}, "shed": {"status": "online"}}
    speech = speak_offline_check(fleet_brief([_entry(all_up)], NOW))
    assert speech == "Everything's online — all 2 Canaries are reporting in."

    mixed = {"gate": {"status": "online"}, "shed": {"status": "offline"}}
    speech = speak_offline_check(fleet_brief([_entry(mixed)], NOW))
    assert speech.startswith("Shed isn't reporting as online right now.")
    assert "1 of 2 still is." in speech

    two_down = {"a": {"status": "offline"}, "b": {"status": "offline"}, "c": {"status": "online"}}
    speech = speak_offline_check(fleet_brief([_entry(two_down)], NOW))
    assert speech.startswith("A and b aren't reporting")

    assert "no Canaries in the fleet yet" in speak_offline_check(fleet_brief([], NOW))


def test_speak_roster_lists_the_fleet():
    devices = {"gate": {"status": "online"}, "shed": {"status": "online"}, "porch": {}}
    speech = speak_roster(fleet_brief([_entry(devices)], NOW))
    assert speech == "3 Canaries: Gate, porch, and shed — 2 of them online right now."

    one = speak_roster(fleet_brief([_entry({"gate": {"status": "online"}})], NOW))
    assert one == "One Canary: Gate — all online."

    assert "No Canaries have checked in yet" in speak_roster(fleet_brief([], NOW))


def test_speak_goodnight_is_forward_looking():
    devices = {"gate": {"status": "online"}, "shed": {"status": "online"}}
    verify = {k: {"trusted": True, "reason": "ok"} for k in devices}
    speech = speak_goodnight(fleet_brief([_entry(devices, verify)], NOW))
    assert speech.startswith("Goodnight.")
    assert "All 2 Canaries are online and watching." in speech
    assert "It's been quiet." in speech
    assert speech.endswith("Sleep well.")

    # Trouble is raised before bed, not hidden by the ritual.
    down = {"gate": {"status": "online"}, "shed": {"status": "offline"}}
    speech = speak_goodnight(fleet_brief([_entry(down)], NOW))
    assert "1 of 2 Canaries are watching — Shed isn't reporting in." in speech

    # Updates are explicitly deferred rather than nagged about.
    withupd = fleet_brief([_entry(devices, verify)], NOW, pending_updates=["Whisper"])
    assert "1 update can wait until morning." in speak_goodnight(withupd)

    # Nothing configured: still a kind answer, no false comfort.
    assert "Nothing's set up to keep watch yet" in speak_goodnight(fleet_brief([], NOW))


def test_goodnight_never_claims_quiet_it_cannot_see():
    devices = {"gate": {"status": "online"}}
    verify = {"gate": {"trusted": True, "reason": "ok"}}

    # An unreachable kernel must not be followed by "It's been quiet."
    blind = _entry(devices, verify, kernel={"ok": False, "latest_event": None})
    speech = speak_goodnight(fleet_brief([blind], NOW))
    assert "can't reach the witness kernel" in speech
    assert "It's been quiet." not in speech

    # A kernel-only event is reported, not silently swallowed as quiet.
    kernel_only = _entry(
        devices, verify, kernel={"ok": True, "latest_event": {"event_type": "TamperDetected"}}
    )
    speech = speak_goodnight(fleet_brief([kernel_only], NOW))
    assert "the latest in the kernel log is tamper detected" in speech
    assert "It's been quiet." not in speech

    # With no kernel configured and nothing cached, quiet is honest.
    assert "It's been quiet." in speak_goodnight(fleet_brief([_entry(devices, verify)], NOW))


def test_speak_privacy_is_honest_about_the_wake_word_residue():
    speech = speak_privacy()
    # It admits the false-wake window rather than claiming purity...
    assert "false-wakes me" in speech
    assert "thrown away" in speech
    # ...and states the structural Canary fact.
    assert "no faces" in speech
    assert "code that was never written" in speech
    # It never claims to be always-off or to have a setting that isn't real.
    assert "turned off" not in speech


def test_speak_privacy_never_asserts_one_listening_mode():
    speech = speak_privacy()
    # Push-to-talk is the blessed default and does NOT listen for a name,
    # so a flat "I listen for my name" would be false in the recommended
    # setup. Both modes must be described, neither asserted as active.
    assert "pressing the button" in speech
    assert "only while you're holding it" in speech
    assert "If you turned on a wake word" in speech
    assert "I listen for my name and nothing else" not in speech
    # The mode-independent promises still stand unconditionally.
    assert "Either way, I don't record you" in speech


def test_speak_help_names_the_limit():
    speech = speak_help()
    for phrase in ("what's up", "is anything offline", "are you listening", "goodnight"):
        assert phrase in speech
    # The refusal is part of the help, not a footnote.
    assert "can't arm, disarm, unlock, or open anything" in speech


# ─────────────────────────────────────────────────────────────────────────
# The design laws (docs/design/voice_moments.md)
# ─────────────────────────────────────────────────────────────────────────


def _busy_night_brief(hour, **kw):
    devices = {"gate": {"status": "online"}, "shed": {"status": "online"}}
    verify = {k: {"trusted": True, "reason": "ok"} for k in devices}
    return fleet_brief(
        [_entry(devices, verify)], NOW,
        weather={"condition": "rainy", "temp": 50},
        pending_updates=["Whisper", "Piper"],
        local_hour=hour,
        **kw,
    )


def test_night_register_drops_the_small_talk():
    # Law 2: at 2 a.m. the question is "can I go back to sleep?" — the
    # weather and a pending-update nag are noise between it and the answer.
    night = speak_whats_up(_busy_night_brief(2))
    assert night == "All quiet. Everything's online. Go back to sleep."
    assert "Outside" not in night
    assert "update" not in night

    # The same fleet in daylight still gets the full catch-up.
    day = speak_whats_up(_busy_night_brief(14))
    assert "Outside it's 50 degrees" in day
    assert "updates are waiting" in day


def test_night_register_window_edges_and_unknown_hour():
    from ..voice import is_night

    assert is_night(22) is True and is_night(2) is True and is_night(5) is True
    assert is_night(6) is False and is_night(21) is False and is_night(14) is False
    # An unknown or unparseable hour reads as daytime: the shortened
    # answer is the surprising one, never given by accident.
    assert is_night(None) is False
    assert is_night("half past") is False


def test_night_register_still_leads_with_trouble():
    devices = {"gate": {"status": "online"}}
    verify = {"gate": {"trusted": False, "reason": "mismatch"}}
    speech = speak_whats_up(fleet_brief([_entry(devices, verify)], NOW, local_hour=3))
    assert speech.startswith("Worth knowing: Gate is publishing")

    # And never claims all-well when it cannot see the kernel.
    blind = _entry({"gate": {"status": "online"}}, {"gate": {"trusted": True, "reason": "ok"}},
                   kernel={"ok": False, "latest_event": None})
    speech = speak_whats_up(fleet_brief([blind], NOW, local_hour=3))
    assert "won't tell you all is well" in speech
    assert "Go back to sleep" not in speech


def test_night_answers_are_short():
    # One breath. Not a rule of taste — speech is serial and vanishes.
    for hour in (23, 2, 5):
        speech = speak_whats_up(_busy_night_brief(hour))
        assert len(speech.split()) <= 25, speech


def test_law_three_never_reads_out_a_long_list():
    devices = {f"cv{i}": {"status": "online"} for i in range(12)}
    verify = {k: {"trusted": True, "reason": "ok"} for k in devices}
    brief = fleet_brief([_entry(devices, verify)], NOW)

    speech = speak_roster(brief)
    assert speech == (
        "12 Canaries, all online. That's more than I'd read out — "
        "the dashboard has them all."
    )
    # A handful is still enumerated, because that reads fine aloud.
    few = fleet_brief([_entry({"gate": {"status": "online"}, "shed": {}})], NOW)
    assert "Gate and shed" in speak_roster(few)


def test_law_three_caps_names_inside_a_sentence():
    devices = {f"cv{i}": {} for i in range(9)}
    speech = speak_offline_check(fleet_brief([_entry(devices)], NOW))
    assert "and 6 others" in speech
    assert speech.count(",") <= 4


def test_night_register_never_suppresses_an_alarm():
    # A smoke alarm must outrank the key-mismatch summary, whose closing
    # "nothing else is out of place" would otherwise be a false all-clear
    # spoken over an alarm at 2 a.m.
    devices = {
        "kitchen": {
            "status": "online",
            "last_event": {
                "event_type": "acoustic_smoke_alarm",
                "received_at": NOW - 300,
                "trusted": True,
                "reason": "ok",
            },
        },
        "gate": {"status": "online"},
    }
    verify = {
        "kitchen": {"trusted": True, "reason": "ok"},
        "gate": {"trusted": False, "reason": "mismatch"},
    }
    speech = speak_whats_up(fleet_brief([_entry(devices, verify)], NOW, local_hour=2))
    assert speech.startswith("Acoustic smoke alarm")
    assert "Nothing else is out of place" not in speech


def test_night_register_keeps_the_trust_qualifier():
    devices = {
        "gate": {
            "status": "online",
            "last_event": {
                "event_type": "contact_state_change",
                "received_at": NOW - 300,
                "trusted": False,
                "reason": "unsigned",
            },
        }
    }
    verify = {"gate": {"trusted": True, "reason": "ok"}}
    speech = speak_whats_up(fleet_brief([_entry(devices, verify)], NOW, local_hour=3))
    assert "came in unverified" in speech
    # Brevity is never a permit to launder a mismatched publish either.
    devices["gate"]["last_event"]["reason"] = "mismatch"
    speech = speak_whats_up(fleet_brief([_entry(devices, verify)], NOW, local_hour=3))
    assert "hold it loosely" in speech


def test_match_device_uses_the_friendly_name_people_say():
    # A serial-like id with an advertised device_name must resolve from
    # the word a person actually says.
    devices = {
        "cv-a1b2c3": {"status": '{"status": "online", "device_name": "Gate"}'},
        "cv-d4e5f6": {"status": '{"status": "online", "device_name": "Back Door"}'},
    }
    brief = fleet_brief([_entry(devices)], NOW)
    assert brief["device_names"]["cv-a1b2c3"] == "Gate"

    assert match_device(brief["device_ids"], "the gate canary", brief["device_names"]) == "cv-a1b2c3"
    assert match_device(brief["device_ids"], "back door", brief["device_names"]) == "cv-d4e5f6"
    # The raw id still works for anyone who says it.
    assert match_device(brief["device_ids"], "cv-a1b2c3", brief["device_names"]) == "cv-a1b2c3"
    # And the device check speaks the friendly name back.
    assert speak_device_check(brief, "the gate canary").startswith("The Gate Canary is online")
