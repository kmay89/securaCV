"""Tests for voice.py — the pure logic behind the Assist intents.

The spoken answers are quotable out of context, so the vocabulary
discipline is what these tests pin down: "verified" only for a checked
signature, "heard" for anything looser, worst news first, and coarse
(ten-minute-floor) time phrasing.
"""
from __future__ import annotations

from ..voice import (
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
    assert speech == (
        "Large object crossed boundary, within the last ten minutes, "
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
        "Pretty quiet — the last thing witnessed was large object crossed "
        "boundary, about 2 hours ago, from the gate Canary. "
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


def test_speak_help_names_the_limit():
    speech = speak_help()
    for phrase in ("what's up", "is anything offline", "are you listening", "goodnight"):
        assert phrase in speech
    # The refusal is part of the help, not a footnote.
    assert "can't arm, disarm, unlock, or open anything" in speech
