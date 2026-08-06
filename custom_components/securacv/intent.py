"""Assist intents — read-only voice answers about the fleet.

Home Assistant's ``intent`` component discovers this platform and calls
``async_setup_intents``. The handlers registered here answer the local
voice pipeline (docs/voice_control.md), and are grouped by how people
actually ask:

  - the catch-up:   SecuracvWhatsUp        "what's up", "what did I miss"
  - the crisp ones: SecuracvFleetStatus    "is the fleet OK?"
                    SecuracvLastEvent      "what was the last event?"
                    SecuracvOfflineCheck   "is anything offline?"
                    SecuracvRoster         "what Canaries do I have?"
                    SecuracvDeviceCheck    "how's the gate Canary?"
  - the rituals:    SecuracvGoodnight      "goodnight"
  - about itself:   SecuracvPrivacy        "are you listening to me?"
                    SecuracvHelp           "what can I ask you?"

All are queries. There are deliberately no action intents: voice may ask
about the fleet but cannot arm, disarm, mute, or otherwise change the
security posture — a spoken word carries no signature, so those paths stay
on authenticated surfaces (AGENTS.md rule 1; the voice contract in
docs/research/whisper_local_voice.md §3.1). The sentences that trigger
these intents ship in docs/voice_sentences_en.yaml for the user to copy
into ``config/custom_sentences/en/``.

The answer-building logic lives in ``voice.py`` (pure, host-tested); this
file only snapshots hass.data and hands plain dicts over.
"""
from __future__ import annotations

import time
from typing import Any

from homeassistant.core import HomeAssistant
from homeassistant.helpers import intent

from . import voice, watches
from .const import DOMAIN

INTENT_FLEET_STATUS = "SecuracvFleetStatus"
INTENT_LAST_EVENT = "SecuracvLastEvent"
INTENT_WHATS_UP = "SecuracvWhatsUp"
INTENT_DEVICE_CHECK = "SecuracvDeviceCheck"
INTENT_OFFLINE_CHECK = "SecuracvOfflineCheck"
INTENT_ROSTER = "SecuracvRoster"
INTENT_GOODNIGHT = "SecuracvGoodnight"
INTENT_PRIVACY = "SecuracvPrivacy"
INTENT_HELP = "SecuracvHelp"
INTENT_START_WATCH = "SecuracvStartWatch"
INTENT_LIST_WATCHES = "SecuracvListWatches"


async def async_setup_intents(hass: HomeAssistant) -> None:
    """Register the SecuraCV voice intents."""
    intent.async_register(hass, FleetStatusIntentHandler())
    intent.async_register(hass, LastEventIntentHandler())
    intent.async_register(hass, WhatsUpIntentHandler())
    intent.async_register(hass, DeviceCheckIntentHandler())
    intent.async_register(hass, OfflineCheckIntentHandler())
    intent.async_register(hass, RosterIntentHandler())
    intent.async_register(hass, GoodnightIntentHandler())
    intent.async_register(hass, PrivacyIntentHandler())
    intent.async_register(hass, HelpIntentHandler())
    intent.async_register(hass, StartWatchIntentHandler())
    intent.async_register(hass, ListWatchesIntentHandler())


def _pending_updates(hass: HomeAssistant) -> list[str]:
    """Human names of updates the hub is waiting to install.

    Read from the ``update`` entities HA/Supervisor already maintain — an
    entity in state "on" has an update pending. The trailing " Update" that
    most friendly names carry is trimmed so speech doesn't say "the Core
    Update has an update". Defensive throughout: on any surprise, the
    casual answer simply doesn't mention updates.
    """
    names: list[str] = []
    try:
        for state in hass.states.async_all("update"):
            if state.state != "on":
                continue
            name = state.attributes.get("friendly_name") or state.entity_id
            if name.lower().endswith(" update"):
                name = name[: -len(" update")]
            names.append(name)
    except Exception:  # noqa: BLE001 - never let a listing break the answer
        return []
    return sorted(names)


def _snapshot(hass: HomeAssistant) -> list[dict[str, Any]]:
    """Plain-dict view of every config entry's runtime state.

    hass.data[DOMAIN] maps entry_id -> entry_data, plus a couple of
    domain-level flags (e.g. ``_frontend_registered``); only dicts that
    carry a ``devices`` slice are entries.
    """
    entries: list[dict[str, Any]] = []
    for entry_data in hass.data.get(DOMAIN, {}).values():
        if not isinstance(entry_data, dict) or "devices" not in entry_data:
            continue
        kernel: dict[str, Any] | None = None
        coordinator = entry_data.get("coordinator")
        if coordinator is not None:
            kernel = {
                "ok": bool(getattr(coordinator, "last_update_success", False)),
                "latest_event": (getattr(coordinator, "data", None) or {}).get(
                    "latest_event"
                ),
            }
        entries.append(
            {
                "devices": entry_data.get("devices", {}),
                "verify": entry_data.get("verify", {}),
                "kernel": kernel,
            }
        )
    return entries


class _BriefIntentHandler(intent.IntentHandler):
    """Shared shape: snapshot -> voice.fleet_brief -> one spoken answer."""

    def _speak(self, brief: dict[str, Any]) -> str:
        raise NotImplementedError

    async def async_handle(self, intent_obj: intent.Intent) -> intent.IntentResponse:
        brief = voice.fleet_brief(_snapshot(intent_obj.hass), time.time())
        response = intent_obj.create_response()
        response.async_set_speech(self._speak(brief))
        return response


class FleetStatusIntentHandler(_BriefIntentHandler):
    """Answer 'is the fleet OK?' from local state only."""

    intent_type = INTENT_FLEET_STATUS
    description = "Spoken summary of Canary fleet health and signature trust"

    def _speak(self, brief: dict[str, Any]) -> str:
        return voice.speak_fleet_status(brief)


class LastEventIntentHandler(_BriefIntentHandler):
    """Answer 'what was the last witness event?' from local state only."""

    intent_type = INTENT_LAST_EVENT
    description = "Speak the most recent witness event's coarse label and time"

    def _speak(self, brief: dict[str, Any]) -> str:
        return voice.speak_last_event(brief)


def _weather(hass: HomeAssistant) -> dict[str, Any] | None:
    """Condition + temperature from the hub's first live weather entity.

    Defensive like _pending_updates: any surprise means the casual answer
    simply doesn't mention the weather.
    """
    try:
        for state in hass.states.async_all("weather"):
            if state.state in ("unknown", "unavailable", ""):
                continue
            temp = state.attributes.get("temperature")
            return {
                "condition": state.state,
                "temp": temp if isinstance(temp, (int, float)) else None,
            }
    except Exception:  # noqa: BLE001 - never let weather break the answer
        return None
    return None


class WhatsUpIntentHandler(intent.IntentHandler):
    """The casual one — 'what's up' gets one warm, honest reply.

    Unlike the crisp intents, this brief also carries the hub's pending
    updates and its weather entity's snapshot, so the answer can mention
    what's waiting on the owner and what it's like outside.
    """

    intent_type = INTENT_WHATS_UP
    description = (
        "Conversational fleet catch-up: alerts and attention items first, "
        "latest activity, health, weather, pending updates"
    )

    async def async_handle(self, intent_obj: intent.Intent) -> intent.IntentResponse:
        hass = intent_obj.hass
        brief = voice.fleet_brief(
            _snapshot(hass),
            time.time(),
            pending_updates=_pending_updates(hass),
            weather=_weather(hass),
        )
        response = intent_obj.create_response()
        response.async_set_speech(voice.speak_whats_up(brief))
        return response


class DeviceCheckIntentHandler(intent.IntentHandler):
    """'How's the gate Canary?' — one device, matched tolerantly by name."""

    intent_type = INTENT_DEVICE_CHECK
    description = "Report one Canary's online state, signature trust, and last event"

    async def async_handle(self, intent_obj: intent.Intent) -> intent.IntentResponse:
        spoken = ""
        slots = getattr(intent_obj, "slots", None)
        if isinstance(slots, dict):
            slot = slots.get("canary_name") or {}
            if isinstance(slot, dict):
                spoken = str(slot.get("value") or "")
            else:
                spoken = str(slot)
        brief = voice.fleet_brief(_snapshot(intent_obj.hass), time.time())
        response = intent_obj.create_response()
        response.async_set_speech(voice.speak_device_check(brief, spoken))
        return response


class OfflineCheckIntentHandler(_BriefIntentHandler):
    """'Is anything offline?' — the question with a yes-or-no shape."""

    intent_type = INTENT_OFFLINE_CHECK
    description = "Say which Canaries are not reporting as online, or that all are"

    def _speak(self, brief: dict[str, Any]) -> str:
        return voice.speak_offline_check(brief)


class RosterIntentHandler(_BriefIntentHandler):
    """'What Canaries do I have?' — the inventory, conversationally."""

    intent_type = INTENT_ROSTER
    description = "List the Canaries in the fleet and how many are online"

    def _speak(self, brief: dict[str, Any]) -> str:
        return voice.speak_roster(brief)


class GoodnightIntentHandler(intent.IntentHandler):
    """'Goodnight' — who is on watch tonight, and anything that would stop it."""

    intent_type = INTENT_GOODNIGHT
    description = "Bedtime check: who is watching tonight, plus anything pending"

    async def async_handle(self, intent_obj: intent.Intent) -> intent.IntentResponse:
        hass = intent_obj.hass
        brief = voice.fleet_brief(
            _snapshot(hass), time.time(), pending_updates=_pending_updates(hass)
        )
        response = intent_obj.create_response()
        response.async_set_speech(voice.speak_goodnight(brief))
        return response


class PrivacyIntentHandler(intent.IntentHandler):
    """'Are you listening to me?' — the honest answer, out loud.

    Deliberately answers from the contract rather than from runtime state:
    what it says is true of every configuration this project ships, so it
    cannot be made to say something reassuring that a setting has changed.
    """

    intent_type = INTENT_PRIVACY
    description = "Explain honestly what is and is not listening, recording, or watching"

    async def async_handle(self, intent_obj: intent.Intent) -> intent.IntentResponse:
        response = intent_obj.create_response()
        response.async_set_speech(voice.speak_privacy())
        return response


class HelpIntentHandler(intent.IntentHandler):
    """'What can I ask you?' — discoverability, and the honest limit."""

    intent_type = INTENT_HELP
    description = "List what the fleet voice can answer, and what it cannot do"

    async def async_handle(self, intent_obj: intent.Intent) -> intent.IntentResponse:
        response = intent_obj.create_response()
        response.async_set_speech(voice.speak_help())
        return response


# ── Watches ─────────────────────────────────────────────────────────────
# Design: docs/design/watches.md. Voice may START a watch but not end one
# early: starting only ever ADDS attention (a stray sentence from a
# television costs you a fortnight of being told slightly too much, and
# the watch expires on its own), while ending removes it — the silencing
# direction, which stays on authenticated surfaces for the same reason
# voice cannot mute an Alert. The rule underneath: voice may make you
# better informed, never less.
#
# Storage note: watches live in hass.data for now, so they do not yet
# survive a Home Assistant restart. Persistence is the next step in the
# design doc's status table, and is deliberately not claimed here.


def _watch_bucket(hass: HomeAssistant) -> list[dict[str, Any]]:
    """The list of live watches, created on first use."""
    domain_data = hass.data.setdefault(DOMAIN, {})
    bucket = domain_data.get("watches")
    if not isinstance(bucket, list):
        bucket = []
        domain_data["watches"] = bucket
    return bucket


def _slot_text(intent_obj: intent.Intent, name: str) -> str:
    slots = getattr(intent_obj, "slots", None)
    if not isinstance(slots, dict):
        return ""
    slot = slots.get(name) or {}
    if isinstance(slot, dict):
        return str(slot.get("value") or "")
    return str(slot)


class StartWatchIntentHandler(intent.IntentHandler):
    """'Keep an eye on the litter box for two weeks.'"""

    intent_type = INTENT_START_WATCH
    description = (
        "Start a bounded, self-expiring watch on something the fleet already "
        "senses. Adds attention only; it cannot arm, disarm, or unseal anything"
    )

    async def async_handle(self, intent_obj: intent.Intent) -> intent.IntentResponse:
        hass = intent_obj.hass
        subject_text = _slot_text(intent_obj, "watch_subject").strip()
        duration_text = _slot_text(intent_obj, "watch_duration")
        response = intent_obj.create_response()

        if not subject_text:
            response.async_set_speech(
                "Tell me what to keep an eye on, and for how long — "
                "something like: watch the litter box for two weeks."
            )
            return response

        now = time.time()
        bucket = _watch_bucket(hass)
        days = watches.parse_duration_days(duration_text)
        concern = watches.concern_from_text(subject_text)
        label = subject_text
        for filler in ("the ", "my "):
            if label.startswith(filler):
                label = label[len(filler):]
        label = "the " + label

        # Bind to a Canary if the words name one; otherwise the watch is
        # created against the spoken subject and the answer says plainly
        # that nothing is feeding it yet — never a silent no-op.
        brief = voice.fleet_brief(_snapshot(hass), now)
        device_id = voice.match_device(brief.get("device_ids") or [], subject_text)
        subject = (
            {"kind": "event", "ref": device_id}
            if device_id
            else {"kind": "unbound", "ref": subject_text}
        )

        watch = watches.make_watch(
            f"w{len(bucket) + 1}-{int(now)}", label, subject, now,
            days=days, concern=concern,
        )
        bucket.append(watch)

        speech = watches.speak_started(watch)
        if not device_id:
            speech += (
                " One thing to be straight about: nothing in the fleet is "
                "reporting that yet, so I have nothing to watch until "
                "something does."
            )
        response.async_set_speech(speech)
        return response


class ListWatchesIntentHandler(intent.IntentHandler):
    """'What am I watching?'"""

    intent_type = INTENT_LIST_WATCHES
    description = "List the watches currently running and how long each has left"

    async def async_handle(self, intent_obj: intent.Intent) -> intent.IntentResponse:
        response = intent_obj.create_response()
        response.async_set_speech(
            watches.speak_roster(_watch_bucket(intent_obj.hass), time.time())
        )
        return response
