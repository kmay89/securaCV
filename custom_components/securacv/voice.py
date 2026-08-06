"""Fleet voice briefs — the pure logic behind the Assist intents.

This module is the host-testable half of the hub's local voice support
(``intent.py`` is the thin Home Assistant-facing half). It turns the
integration's runtime state — the per-entry ``devices`` and ``verify``
dicts plus the kernel coordinator's latest data — into short, honest
spoken answers for the read-only intents:

  - "is the fleet OK?"        -> speak_fleet_status()
  - "what was the last event" -> speak_last_event()
  - "what's up?"              -> speak_whats_up()
  - "how's the gate Canary?"  -> speak_device_check()
  - "is anything offline?"    -> speak_offline_check()
  - "what Canaries do I have" -> speak_roster()
  - "goodnight"               -> speak_goodnight()
  - "are you listening?"      -> speak_privacy()
  - "what can I ask you?"     -> speak_help()

Vocabulary discipline (docs/GLOSSARY.md) is load-bearing here, because a
spoken sentence is quoted out of context by design:

  - "verified" is said ONLY when an Ed25519 signature checked against a
    pinned key (the verify dict's ``trusted`` flag, stamped by
    ``async_record_verify``). Everything looser is "heard" or "publishing
    unsigned".
  - Time is spoken coarsely (ten-minute floor), matching the project's
    stance that precision is a privacy cost. The times here are hub
    arrival times, not the sealed coarse buckets, so "about" phrasing is
    also simply honest.

There are deliberately no action intents and no identity answers — voice
may query, never change the security posture (AGENTS.md rule 1, and the
voice contract in docs/research/whisper_local_voice.md §3.1).

No Home Assistant imports here: pure functions over plain dicts, tested
by ``tests/test_voice.py`` under the same stub harness as the rest of the
component's logic.
"""
from __future__ import annotations

import json
from typing import Any

from .const import event_type_metadata

# Verify-dict reasons (async_record_verify) that mean "publishes without a
# checkable signature" rather than "signature failed".
_UNSIGNED_REASONS = ("unsigned", "no_pubkey")

# The liveness vocabulary the online binary sensor accepts — mirrored here
# so speech and the dashboard can never disagree about what "online" means.
_ONLINE_WORDS = ("online", "1", "true", "connected")

# The waking-hours window, matching the display firmware's audible
# self-test. Outside it, the casual answer switches to the night register:
# shorter, calmer, no small talk (docs/design/voice_moments.md, law 2).
NIGHT_FROM = 22
NIGHT_UNTIL = 6

# Past this many names, speech summarizes instead of enumerating — a
# twelve-item list read aloud is noise, not information (law 3).
MAX_SPOKEN_NAMES = 4


def is_night(local_hour: int | None) -> bool:
    """True inside the night window. An unknown hour reads as daytime,
    because the shortened answer is the surprising one and should never
    be given by accident."""
    if local_hour is None:
        return False
    try:
        hour = int(local_hour) % 24
    except (TypeError, ValueError):
        return False
    return hour >= NIGHT_FROM or hour < NIGHT_UNTIL


# Alert-class event types: when the latest event is one of these, the casual
# answer leads with it — a smoke alarm outranks small talk.
_ALERT_EVENT_TYPES = frozenset(
    {"tamper_detected", "acoustic_smoke_alarm", "acoustic_co_alarm"}
)

# The complete HA weather-entity condition vocabulary, each with a warm,
# year-round spoken phrase. Optimistic on purpose — weather small talk is
# allowed to be kind — with one honesty override: "exceptional" is HA's
# severe/unusual marker, and danger is never spun as charm. A condition
# not in this table (a custom integration's invention) falls back to
# hyphens-to-spaces, spoken plainly.
_WEATHER_SPEECH = {
    "sunny": "sunny — a good one to step out in",
    "clear-night": "clear — good stars if you look up",
    "partlycloudy": "partly cloudy — plenty of bright spells",
    "cloudy": "cloudy — soft light all day",
    "windy": "windy — the fresh kind",
    "windy-variant": "windy — the fresh kind",
    "fog": "foggy — it usually lifts",
    "rainy": "rainy — the garden will be glad",
    "pouring": "pouring — a good day to be cozy inside",
    "lightning": "a thunderstorm — quite a light show from a window",
    "lightning-rainy": "stormy — dramatic out there, snug in here",
    "hail": "hailing — it passes quickly, best let it",
    "snowy": "snowing — it'll be pretty out there",
    "snowy-rainy": "sleety — the kind to admire from indoors",
    "exceptional": "unusual out there — worth checking the forecast before heading out",
}


def _spoken_label(event_type: Any) -> str:
    """A speakable, lowercase-leading label for an event type.

    Uses the dictionary label when one exists; an event type with no
    metadata entry (e.g. the acoustic alarm grammars) falls back to its
    own name with underscores read as spaces — never spoken raw.
    """
    label = event_type_metadata(event_type if isinstance(event_type, str) else None)["label"]
    if label == event_type:
        label = label.replace("_", " ")
    return label[:1].lower() + label[1:]


def _snake(event_type: Any) -> str:
    """Event type as snake_case, accepting the kernel's CamelCase too."""
    if not isinstance(event_type, str):
        return ""
    key = event_type.strip()
    if not key or "_" in key or key.islower():
        return key.lower()
    out = ""
    for i, ch in enumerate(key):
        if ch.isupper() and i > 0:
            out += "_"
        out += ch.lower()
    return out


def _status_online(raw: Any) -> bool:
    """True only when a device's retained status payload says it is online.

    Mirrors SecuraCVCanaryOnlineSensor's rule for bare-word payloads, plus
    the JSON shape (a ``status``/``state`` field with the same words). A
    payload we can't read means we don't know — and "don't know" is never
    spoken as online.
    """
    if not isinstance(raw, str) or not raw.strip():
        return False
    text = raw.lower().strip()
    if text in _ONLINE_WORDS:
        return True
    if text.startswith("{"):
        try:
            data = json.loads(raw)
        except (json.JSONDecodeError, TypeError):
            return False
        if isinstance(data, dict):
            field = str(data.get("status") or data.get("state") or "").lower().strip()
            return field in _ONLINE_WORDS
    return False


def record_canary_event(
    devices: dict[str, Any],
    device_id: str,
    event_type: str | None,
    received_at: float,
    trusted: bool | None = None,
    reason: str | None = None,
) -> None:
    """Stash the newest event for a device where fleet_brief() can read it.

    ``devices`` is entry_data["devices"]; the device slot may not exist yet
    (an event can arrive before the first status publish), so create it.
    ``trusted``/``reason`` are the verify verdict stamped for this publish
    (call this AFTER the verifier ran); ``None`` means no verdict exists,
    which speaks as unverified — never as trusted-by-default.
    """
    if not device_id:
        return
    devices.setdefault(device_id, {})["last_event"] = {
        "event_type": event_type,
        "received_at": received_at,
        "trusted": trusted,
        "reason": reason,
    }


def fleet_brief(
    entries: list[dict[str, Any]],
    now: float,
    pending_updates: list[str] | None = None,
    weather: dict[str, Any] | None = None,
    local_hour: int | None = None,
) -> dict[str, Any]:
    """Reduce one or more config entries' runtime state to a fleet brief.

    Each entry dict carries:
      - "devices": entry_data["devices"] (MQTT status + stashed last_event)
      - "verify":  entry_data["verify"]  (per-device trust verdicts)
      - "kernel":  None when no kernel is configured, else
                   {"ok": bool, "latest_event": dict | None}

    ``pending_updates`` is an optional list of human names for updates the
    hub is waiting to install (HA ``update`` entities that are on), and
    ``weather`` an optional ``{"condition", "temp"}`` snapshot from the
    hub's weather entity — the casual "what's up" answer mentions both;
    the crisp status answer deliberately does not.

    ``local_hour`` (0-23) decides the night register: inside the night
    window the casual answer is shortened and stripped of small talk,
    because the 2 a.m. question is "can I go back to sleep?" and nothing
    else (docs/design/voice_moments.md, law 2).
    """
    device_ids: list[str] = []
    online: list[str] = []
    verified: list[str] = []
    unsigned: list[str] = []
    mismatched: list[str] = []
    unknown: list[str] = []
    detail: dict[str, dict[str, Any]] = {}
    canary_latest: dict[str, Any] | None = None
    kernel_configured = False
    kernel_ok: bool | None = None
    kernel_latest_event: dict[str, Any] | None = None

    for entry in entries:
        devices = entry.get("devices") or {}
        verify = entry.get("verify") or {}
        for device_id in sorted(devices):
            device_ids.append(device_id)
            is_online = _status_online(devices[device_id].get("status"))
            if is_online:
                online.append(device_id)
            verdict = verify.get(device_id)
            if not isinstance(verdict, dict):
                unknown.append(device_id)
            elif verdict.get("trusted"):
                verified.append(device_id)
            elif verdict.get("reason") == "mismatch":
                mismatched.append(device_id)
            elif verdict.get("reason") in _UNSIGNED_REASONS:
                unsigned.append(device_id)
            else:
                unknown.append(device_id)
            detail[device_id] = {
                "online": is_online,
                "trusted": verdict.get("trusted") if isinstance(verdict, dict) else None,
                "reason": verdict.get("reason") if isinstance(verdict, dict) else None,
                "last_event": devices[device_id].get("last_event"),
            }
            last = devices[device_id].get("last_event")
            if isinstance(last, dict) and isinstance(
                last.get("received_at"), (int, float)
            ):
                if canary_latest is None or (
                    last["received_at"] > canary_latest["received_at"]
                ):
                    canary_latest = {
                        "device_id": device_id,
                        "event_type": last.get("event_type"),
                        "received_at": float(last["received_at"]),
                        "trusted": last.get("trusted"),
                        "reason": last.get("reason"),
                    }
        kernel = entry.get("kernel")
        if isinstance(kernel, dict):
            kernel_configured = True
            # Any reachable kernel counts; a second, unreachable one keeps
            # kernel_ok False so the answer names the trouble.
            ok = bool(kernel.get("ok"))
            kernel_ok = ok if kernel_ok is None else (kernel_ok and ok)
            if kernel_latest_event is None and isinstance(
                kernel.get("latest_event"), dict
            ):
                kernel_latest_event = kernel["latest_event"]

    return {
        "now": now,
        "pending_updates": list(pending_updates or []),
        "weather": dict(weather) if weather else None,
        "night": is_night(local_hour),
        "device_count": len(device_ids),
        "device_ids": device_ids,
        "device_detail": detail,
        "online": online,
        "verified": verified,
        "unsigned": unsigned,
        "mismatched": mismatched,
        "unknown": unknown,
        "kernel_configured": kernel_configured,
        "kernel_ok": kernel_ok,
        "kernel_latest_event": kernel_latest_event,
        "canary_latest": canary_latest,
    }


def _plural(count: int, noun: str) -> str:
    return f"{count} {noun}{'' if count == 1 else 's'}"


def ago_phrase(seconds: float) -> str:
    """Coarse relative-time phrase. Floor is the ten-minute window."""
    if seconds < 0:
        seconds = 0
    if seconds < 600:
        return "within the last ten minutes"
    if seconds < 3600:
        minutes = int(seconds // 600) * 10
        return f"about {minutes} minutes ago"
    if seconds < 86400:
        hours = int(seconds // 3600)
        return f"about {_plural(hours, 'hour')} ago"
    days = int(seconds // 86400)
    return f"about {_plural(days, 'day')} ago"


def speak_fleet_status(brief: dict[str, Any]) -> str:
    """One to three short sentences on fleet health, worst news first."""
    count = brief["device_count"]
    parts: list[str] = []

    if count == 0 and not brief["kernel_configured"]:
        return (
            "The hub hasn't heard from any Canaries yet, and no witness "
            "kernel is configured."
        )

    # Worst news first: a key mismatch outranks everything else the fleet
    # could have to say.
    if brief["mismatched"]:
        names = ", ".join(brief["mismatched"])
        parts.append(
            f"Key mismatch on {names} — the published key does not match "
            "the pinned one. Check the notification on the hub."
        )

    if count:
        trust_bits: list[str] = []
        n_verified = len(brief["verified"])
        if n_verified == count:
            trust_bits.append("all signature-verified against their pinned keys")
        elif n_verified:
            trust_bits.append(f"{n_verified} signature-verified")
        if brief["unsigned"]:
            trust_bits.append(f"{len(brief['unsigned'])} publishing unsigned")
        # Devices with no verify record yet have only been heard, and
        # "heard" is deliberately the strongest word they get.
        if brief["unknown"] and n_verified != count:
            trust_bits.append(f"{len(brief['unknown'])} heard but not yet verified")
        summary = f"{count} {'Canary' if count == 1 else 'Canaries'} in the fleet"
        if trust_bits:
            summary += ": " + ", ".join(trust_bits)
        parts.append(summary + ".")

    if brief["kernel_configured"]:
        if brief["kernel_ok"]:
            parts.append("The witness kernel is reachable.")
        else:
            parts.append("The witness kernel is not reachable.")

    return " ".join(parts)


def _canary_trust_clause(canary: dict[str, Any]) -> str:
    """The spoken trust qualifier for a Canary event. Never silent when weak.

    The verdict stamped at record time rides in the event; ``None`` (no
    verifier ran) speaks as unverified, never as trusted-by-default — the
    spoken sentence is quotable, so it keeps the same honesty ladder as
    the dashboards.
    """
    if canary.get("trusted"):
        return " The event signature is verified against the device's pinned key."
    if canary.get("reason") == "mismatch":
        return (
            " Caution: that device's key does not match its pin — "
            "treat this event as unverified."
        )
    return " The event was published without a verified signature."


def _speak_whats_up_night(brief: dict[str, Any]) -> str:
    """The 2 a.m. answer: can I go back to sleep, yes or no.

    Same facts, different shape. Trouble still leads, but everything that
    is small talk in daylight — the weather, a pending update, the roster
    — is dropped, because at 2 a.m. it is noise between the question and
    the only thing being asked (docs/design/voice_moments.md, moment 1).
    """
    if brief["mismatched"]:
        names = _join_names([_spoken_name(d) for d in brief["mismatched"]])
        return (
            f"Worth knowing: {names} is publishing under a key that doesn't "
            "match its pin. Nothing else is out of place."
        )

    canary = brief.get("canary_latest")
    if canary and (brief["now"] - canary["received_at"]) < 3600:
        label = _spoken_label(canary.get("event_type"))
        when = ago_phrase(brief["now"] - canary["received_at"])
        return (
            f"One thing in the last hour: {label}, {when}, from the "
            f"{_spoken_name(canary['device_id'])} Canary. Nothing else."
        )

    if brief["kernel_configured"] and not brief["kernel_ok"]:
        return (
            "The Canaries are quiet, but I can't reach the witness kernel "
            "right now, so I won't tell you all is well."
        )

    count = brief["device_count"]
    if count and len(brief.get("online") or []) == count:
        return "All quiet. Everything's online. Go back to sleep."
    if count:
        missing = [d for d in (brief.get("device_ids") or []) if d not in set(brief.get("online") or [])]
        return (
            f"Quiet, though {_join_names([_spoken_name(d) for d in missing])} "
            "isn't reporting in. Nothing witnessed."
        )
    return "All quiet — nothing witnessed since I started listening."


def speak_whats_up(brief: dict[str, Any]) -> str:
    """The casual answer — "Hey Canary, what's up?"

    One warm, honest reply instead of a status readout: whatever needs
    attention first, then the latest activity (or an honest "all quiet"),
    then fleet health in a breath, then anything waiting (updates). The
    same vocabulary discipline as the crisp answers — "verified" keeps its
    exact meaning, an untrusted event is held loosely out loud — just worn
    casually. Deterministic on purpose: the phrasing varies with the
    state of the fleet, never with a dice roll, so it stays testable.
    """
    count = brief["device_count"]
    if count == 0 and not brief["kernel_configured"]:
        return (
            "Not much to tell yet — I haven't heard from any Canaries, and "
            "no witness kernel is set up. Once your fleet is online, ask me "
            "again."
        )

    if brief.get("night"):
        return _speak_whats_up_night(brief)

    parts: list[str] = []
    kernel_outage_spoken = False

    # The latest activity, or an honest quiet. An alert-class event —
    # tamper, a smoke or CO alarm pattern — outranks everything, even the
    # key-mismatch heads-up.
    canary = brief.get("canary_latest")
    kernel_event = brief.get("kernel_latest_event")
    activity: str | None = None
    alert_leads = False
    if canary:
        label = _spoken_label(canary.get("event_type"))
        when = ago_phrase(brief["now"] - canary["received_at"])
        recent = (brief["now"] - canary["received_at"]) < 3600
        if _snake(canary.get("event_type")) in _ALERT_EVENT_TYPES:
            alert_leads = True
            opener = "First thing:"
        elif recent:
            opener = "Some activity lately — the last thing witnessed was"
        else:
            opener = "Pretty quiet — the last thing witnessed was"
        activity = (
            f"{opener} {label}, {when}, "
            f"from the {canary['device_id']} Canary"
        )
        if canary.get("trusted"):
            activity += "."
        elif canary.get("reason") == "mismatch":
            activity += ", though that one's from the mismatched key, so hold it loosely."
        else:
            activity += ", though it came in without a verified signature."

    if alert_leads and activity:
        parts.append(activity)

    if brief["mismatched"]:
        names = ", ".join(brief["mismatched"])
        verb = "is" if len(brief["mismatched"]) == 1 else "are"
        opener = "Also, heads up:" if alert_leads else "Heads up first:"
        parts.append(
            f"{opener} {names} {verb} publishing with a key that "
            "doesn't match the pin — there's a notification on the hub "
            "worth a look."
        )

    if activity and not alert_leads:
        parts.append(activity)
    elif activity:
        pass
    elif kernel_event:
        label = _spoken_label(kernel_event.get("event_type"))
        if brief["kernel_configured"] and not brief["kernel_ok"]:
            # A cached event from a kernel that is unreachable NOW is stale
            # by definition — say so instead of presenting it as current.
            parts.append(
                f"The last I saw from the kernel log was {label}, though I "
                "can't reach the kernel right now, so that may be stale."
            )
            kernel_outage_spoken = True
        else:
            parts.append(f"Pretty quiet — the kernel log's latest event is {label}.")
    elif brief["kernel_configured"] and not brief["kernel_ok"]:
        # No events AND the event source is unreachable: silence here means
        # "can't see", not "nothing happened" — never speak it as quiet.
        parts.append(
            "I can't reach the witness kernel right now, so I won't claim "
            "it's been quiet — worth a look at the hub."
        )
        kernel_outage_spoken = True
    else:
        parts.append("All quiet — nothing witnessed since I started listening.")

    # Fleet health, in a breath. "Online" is only ever said for a device
    # whose retained status actually says so — a cached entry with a stale
    # or offline status is "in the fleet", never "online".
    if count:
        n_verified = len(brief["verified"])
        n_online = len(brief["online"])
        if n_online == count and n_verified == count:
            parts.append(
                "Your one Canary is online, signature verified."
                if count == 1
                else f"All {count} Canaries are online, every signature verified."
            )
        else:
            noun = "Canary" if count == 1 else "Canaries"
            health = f"{count} {noun} in the fleet"
            if n_online:
                health += f", {n_online} online"
            if n_verified:
                health += f", {n_verified} verified"
            parts.append(health + ".")

    if brief["kernel_configured"] and not brief["kernel_ok"] and not kernel_outage_spoken:
        parts.append("One more thing — I can't reach the witness kernel right now.")

    # Weather, if the hub knows it — the small talk a person would add.
    weather = brief.get("weather")
    if isinstance(weather, dict) and (weather.get("condition") or weather.get("temp") is not None):
        condition = str(weather.get("condition") or "").lower().strip()
        condition = _WEATHER_SPEECH.get(condition, condition.replace("-", " "))
        temp = weather.get("temp")
        if temp is not None and condition:
            parts.append(f"Outside it's {round(temp)} degrees and {condition}.")
        elif temp is not None:
            parts.append(f"Outside it's {round(temp)} degrees.")
        else:
            parts.append(f"Outside it looks {condition}.")

    # Anything waiting on the owner: pending updates, casually.
    updates = brief.get("pending_updates") or []
    if updates:
        if len(updates) == 1:
            parts.append(f"Also, {updates[0]} has an update waiting when you have a minute.")
        else:
            first_two = " and ".join(updates[:2])
            more = f" and {len(updates) - 2} more" if len(updates) > 2 else ""
            parts.append(
                f"Also, {len(updates)} updates are waiting when you have a "
                f"minute — {first_two}{more}."
            )

    return " ".join(parts)


def speak_last_event(brief: dict[str, Any]) -> str:
    """The newest witness event, spoken with its coarse label, time, and trust.

    When both sources exist (setup mode "both"), the Canary event's arrival
    time and the kernel log's latest are not comparable — the kernel export
    carries a coarse bucket, not an arrival stamp — so rather than invent an
    ordering, both are spoken when they differ.
    """
    canary = brief.get("canary_latest")
    kernel_event = brief.get("kernel_latest_event")

    if canary:
        label = event_type_metadata(canary.get("event_type"))["label"]
        when = ago_phrase(brief["now"] - canary["received_at"])
        speech = f"{label}, {when}, from Canary {canary['device_id']}."
        speech += _canary_trust_clause(canary)
        if kernel_event:
            kernel_label = event_type_metadata(kernel_event.get("event_type"))["label"]
            if kernel_label != label:
                speech += f" The kernel log's latest event is {kernel_label}."
        return speech

    if kernel_event:
        label = event_type_metadata(kernel_event.get("event_type"))["label"]
        return f"{label} — the latest event in the kernel log."

    return "No witness events since the hub started listening."


# ─────────────────────────────────────────────────────────────────────────
# The conversational intents — how people actually ask
# ─────────────────────────────────────────────────────────────────────────


def _spoken_name(device_id: str) -> str:
    """A device_id said out loud: underscores and hyphens read as spaces."""
    return device_id.replace("_", " ").replace("-", " ").strip() or device_id


def _normalize(text: str) -> str:
    """Lowercase, alphanumerics only — for tolerant name matching."""
    return "".join(ch for ch in str(text).lower() if ch.isalnum())


def match_device(device_ids: list[str], spoken: str) -> str | None:
    """Best device_id for a spoken name, or None.

    Speech-to-text will not reproduce an id exactly: "cv-1" comes back as
    "cv one", "the gate Canary" as "gate canary". So matching is tolerant
    and ordered — exact, then normalized-exact, then either containing the
    other — and returns None rather than guessing between two equally good
    candidates, so the answer can ask which one instead of picking wrong.
    """
    if not spoken or not device_ids:
        return None
    # Strip the words people wrap a name in.
    words = [w for w in str(spoken).lower().split() if w not in ("the", "canary", "canaries", "one")]
    cleaned = " ".join(words) or str(spoken).lower()
    for device_id in device_ids:
        if device_id.lower() == cleaned:
            return device_id
    target = _normalize(cleaned)
    if not target:
        return None
    exact = [d for d in device_ids if _normalize(d) == target]
    if len(exact) == 1:
        return exact[0]
    partial = [
        d for d in device_ids
        if target and (target in _normalize(d) or _normalize(d) in target)
    ]
    if len(partial) == 1:
        return partial[0]
    return None


def speak_device_check(brief: dict[str, Any], spoken_name: str) -> str:
    """"How's the gate Canary?" — one device, plainly."""
    device_ids = brief.get("device_ids") or []
    if not device_ids:
        return "I haven't heard from any Canaries yet, so I can't tell you about that one."

    device_id = match_device(device_ids, spoken_name)
    if device_id is None:
        known = ", ".join(_spoken_name(d) for d in device_ids[:4])
        more = " and others" if len(device_ids) > 4 else ""
        return (
            f"I don't have a Canary by that name. I know {known}{more}."
        )

    info = (brief.get("device_detail") or {}).get(device_id) or {}
    name = _spoken_name(device_id)
    parts = [f"The {name} Canary is {'online' if info.get('online') else 'not reporting as online right now'}"]
    if info.get("trusted"):
        parts.append("and its signature checks out against the pinned key.")
    elif info.get("reason") == "mismatch":
        parts.append("and — worth knowing — its key doesn't match the pin.")
    elif info.get("reason") in _UNSIGNED_REASONS:
        parts.append("though it publishes without a verified signature.")
    else:
        parts.append("though I haven't verified a signature from it yet.")

    speech = " ".join(parts)
    last = info.get("last_event")
    if isinstance(last, dict) and isinstance(last.get("received_at"), (int, float)):
        label = _spoken_label(last.get("event_type"))
        when = ago_phrase(brief["now"] - float(last["received_at"]))
        speech += f" The last thing it witnessed was {label}, {when}"
        # The EVENT's own verdict, not the device's current one — a device
        # that verifies today may have published this event unsigned, and
        # the device-level clause above must not launder it.
        if last.get("trusted"):
            speech += "."
        elif last.get("reason") == "mismatch":
            speech += " — though that one came in under the mismatched key, so hold it loosely."
        else:
            speech += " — though that one arrived without a verified signature."
    else:
        speech += " It hasn't witnessed anything since I started listening."
    return speech


def speak_offline_check(brief: dict[str, Any]) -> str:
    """"Is anything offline?" — the question with a yes-or-no shape."""
    count = brief["device_count"]
    if count == 0:
        return "There are no Canaries in the fleet yet, so nothing to be offline."

    online = set(brief.get("online") or [])
    missing = [d for d in (brief.get("device_ids") or []) if d not in online]
    if not missing:
        return (
            "Everything's online — your one Canary is reporting in."
            if count == 1
            else f"Everything's online — all {count} Canaries are reporting in."
        )
    names = _join_names([_spoken_name(d) for d in missing])
    verb = "isn't" if len(missing) == 1 else "aren't"
    return (
        f"{names} {verb} reporting as online right now. "
        f"{len(online)} of {count} still {'is' if len(online) == 1 else 'are'}."
    )


def _join_names(names: list[str]) -> str:
    """Human list: 'a', 'a and b', 'a, b, and c'. Capitalized for a lead.

    Past MAX_SPOKEN_NAMES it stops enumerating and counts the rest —
    speech is serial, so a twelve-item list read aloud is noise (law 3).
    """
    if not names:
        return ""
    if len(names) > MAX_SPOKEN_NAMES:
        shown = names[: MAX_SPOKEN_NAMES - 1]
        rest = len(names) - len(shown)
        joined = ", ".join(shown) + f", and {rest} others"
        return joined[:1].upper() + joined[1:]
    if len(names) == 1:
        joined = names[0]
    elif len(names) == 2:
        joined = f"{names[0]} and {names[1]}"
    else:
        joined = ", ".join(names[:-1]) + f", and {names[-1]}"
    return joined[:1].upper() + joined[1:]


def speak_roster(brief: dict[str, Any]) -> str:
    """"What Canaries do I have?" — the inventory, said conversationally."""
    device_ids = brief.get("device_ids") or []
    count = len(device_ids)
    if count == 0:
        return "No Canaries have checked in yet. Once one does, it'll show up here."
    n_online = len(brief.get("online") or [])
    if count > MAX_SPOKEN_NAMES:
        # Too many to say. Summarize, and name the surface that does lists
        # properly rather than reading a dozen names into the air.
        state = (
            "all online"
            if n_online == count
            else f"{n_online} of them online right now"
        )
        return (
            f"{count} Canaries, {state}. That's more than I'd read out — "
            "the dashboard has them all."
        )
    names = _join_names([_spoken_name(d) for d in device_ids])
    lead = "One Canary:" if count == 1 else f"{count} Canaries:"
    tail = (
        "all online."
        if n_online == count
        else f"{n_online} of them online right now."
    )
    return f"{lead} {names} — {tail}"


def speak_goodnight(brief: dict[str, Any]) -> str:
    """"Goodnight" — the bedtime check: who's watching, and anything pending.

    Forward-looking where the other answers look back: the useful thing at
    bedtime is what will be watching while you sleep, and whether anything
    would stop it.
    """
    count = brief["device_count"]
    if count == 0 and not brief["kernel_configured"]:
        return "Goodnight. Nothing's set up to keep watch yet, so I'll just say sleep well."

    parts = ["Goodnight."]
    online = brief.get("online") or []
    n_online = len(online)
    if brief["mismatched"]:
        parts.append(
            f"Before you turn in — {_join_names([_spoken_name(d) for d in brief['mismatched']])} "
            "is publishing with a key that doesn't match its pin, worth a look tomorrow."
        )
    if count:
        if n_online == count:
            parts.append(
                "Your Canary is online and watching."
                if count == 1
                else f"All {count} Canaries are online and watching."
            )
        elif n_online:
            missing = [d for d in (brief.get("device_ids") or []) if d not in set(online)]
            parts.append(
                f"{n_online} of {count} Canaries are watching — "
                f"{_join_names([_spoken_name(d) for d in missing])} isn't reporting in."
            )
        else:
            parts.append("Worth knowing: none of your Canaries are reporting in right now.")
    if brief["kernel_configured"] and not brief["kernel_ok"]:
        parts.append("I can't reach the witness kernel at the moment, either.")

    # "It's been quiet" is a claim about every source, so it may only be
    # spoken when every configured source is reachable AND empty. A blind
    # kernel or a kernel-only event both forbid it.
    canary = brief.get("canary_latest")
    kernel_event = brief.get("kernel_latest_event")
    kernel_blind = bool(brief["kernel_configured"]) and not brief["kernel_ok"]
    if canary and (brief["now"] - canary["received_at"]) < 3600:
        label = _spoken_label(canary.get("event_type"))
        parts.append(f"Last hour's only note: {label}, from the {_spoken_name(canary['device_id'])} Canary.")
    elif kernel_blind:
        # The outage was already named above; adding "it's been quiet"
        # there would contradict it in the same breath.
        pass
    elif kernel_event:
        parts.append(
            f"Nothing new from the Canaries; the latest in the kernel log is "
            f"{_spoken_label(kernel_event.get('event_type'))}."
        )
    elif count:
        parts.append("It's been quiet.")

    updates = brief.get("pending_updates") or []
    if updates:
        parts.append(
            f"{len(updates)} update{'' if len(updates) == 1 else 's'} can wait until morning."
        )
    parts.append("Sleep well.")
    return " ".join(parts)


def speak_privacy() -> str:
    """"Are you listening to me?" — the honest answer, said out loud.

    The most important sentence this system speaks, so every clause has to
    survive being quoted. Two disciplines govern it:

    1. It does NOT assert which listening mode is active. Push-to-talk —
       the blessed default — does not listen for a name at all, and a
       wake word may be any phrase the owner trained, so a flat "I listen
       for my name" would be a false privacy guarantee in the very setup
       this project recommends. Both modes are described conditionally,
       and both descriptions are true wherever they apply.
    2. It admits the wake-word residue (docs/voice_control.md, "What
       turning a wake word on means") rather than claiming a purity the
       design doesn't have.

    Composed from the contract, never from runtime state, so no setting
    can make it say something kinder than the truth.
    """
    return (
        "That depends on how you set me up, and both answers are short. If you "
        "talk to me by pressing the button, I hear you only while you're "
        "holding it — the rest of the time there's nothing running. If you "
        "turned on a wake word, I'm listening for that one phrase and nothing "
        "else; and if something false-wakes me, a television say, the few "
        "seconds after it are read here on this hub and thrown away. Either "
        "way, I don't record you, and nothing I hear is stored or sent "
        "anywhere — once I've answered, it's gone. As for the Canaries around "
        "the house: they report what happened, never who — no faces, no plate "
        "numbers, no footage leaving home. That isn't a setting I have. It's "
        "code that was never written."
    )


def speak_help() -> str:
    """"What can I ask you?" — discoverability, and the honest limit."""
    return (
        "Ask me what's up for the short version of everything. Or get specific: "
        "is the fleet OK, what was the last event, is anything offline, how's a "
        "particular Canary doing, what Canaries do I have — and if you're ever "
        "wondering, are you listening. Say goodnight and I'll tell you who's on "
        "watch. One thing I can't do: I only answer questions. I can't arm, "
        "disarm, unlock, or open anything, because a spoken word carries no "
        "signature."
    )
