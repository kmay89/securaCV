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

from typing import Any

from .const import event_type_metadata

# Verify-dict reasons (async_record_verify) that mean "publishes without a
# checkable signature" rather than "signature failed".
_UNSIGNED_REASONS = ("unsigned", "no_pubkey")


def record_canary_event(
    devices: dict[str, Any],
    device_id: str,
    event_type: str | None,
    received_at: float,
) -> None:
    """Stash the newest event for a device where fleet_brief() can read it.

    ``devices`` is entry_data["devices"]; the device slot may not exist yet
    (an event can arrive before the first status publish), so create it.
    """
    if not device_id:
        return
    devices.setdefault(device_id, {})["last_event"] = {
        "event_type": event_type,
        "received_at": received_at,
    }


def fleet_brief(entries: list[dict[str, Any]], now: float) -> dict[str, Any]:
    """Reduce one or more config entries' runtime state to a fleet brief.

    Each entry dict carries:
      - "devices": entry_data["devices"] (MQTT status + stashed last_event)
      - "verify":  entry_data["verify"]  (per-device trust verdicts)
      - "kernel":  None when no kernel is configured, else
                   {"ok": bool, "latest_event": dict | None}
    """
    device_ids: list[str] = []
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
        "device_count": len(device_ids),
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


def speak_last_event(brief: dict[str, Any]) -> str:
    """The newest witness event, spoken with its coarse label and time."""
    canary = brief.get("canary_latest")
    if canary:
        label = event_type_metadata(canary.get("event_type"))["label"]
        when = ago_phrase(brief["now"] - canary["received_at"])
        return f"{label}, {when}, from Canary {canary['device_id']}."

    kernel_event = brief.get("kernel_latest_event")
    if kernel_event:
        label = event_type_metadata(kernel_event.get("event_type"))["label"]
        return f"{label} — the latest event in the kernel log."

    return "No witness events since the hub started listening."
