"""Fleet voice briefs — the pure logic behind the Assist intents.

This module is the host-testable half of the hub's local voice support
(``intent.py`` is the thin Home Assistant-facing half). It turns the
integration's runtime state — the per-entry ``devices`` and ``verify``
dicts plus the kernel coordinator's latest data — into short, honest
spoken answers for the two read-only intents:

  - "is the fleet OK?"        -> speak_fleet_status()
  - "what was the last event" -> speak_last_event()

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
    """
    device_ids: list[str] = []
    online: list[str] = []
    verified: list[str] = []
    unsigned: list[str] = []
    mismatched: list[str] = []
    unknown: list[str] = []
    canary_latest: dict[str, Any] | None = None
    kernel_configured = False
    kernel_ok: bool | None = None
    kernel_latest_event: dict[str, Any] | None = None

    for entry in entries:
        devices = entry.get("devices") or {}
        verify = entry.get("verify") or {}
        for device_id in sorted(devices):
            device_ids.append(device_id)
            if _status_online(devices[device_id].get("status")):
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
        "device_count": len(device_ids),
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
