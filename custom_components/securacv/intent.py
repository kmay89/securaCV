"""Assist intents — read-only voice answers about the fleet.

Home Assistant's ``intent`` component discovers this platform and calls
``async_setup_intents``. The two handlers registered here answer the local
voice pipeline (docs/voice_control.md):

  - SecuracvFleetStatus: "is the fleet OK?"
  - SecuracvLastEvent:   "what was the last witness event?"

Both are queries. There are deliberately no action intents: voice may ask
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

from . import voice
from .const import DOMAIN

INTENT_FLEET_STATUS = "SecuracvFleetStatus"
INTENT_LAST_EVENT = "SecuracvLastEvent"
INTENT_WHATS_UP = "SecuracvWhatsUp"


async def async_setup_intents(hass: HomeAssistant) -> None:
    """Register the SecuraCV voice intents."""
    intent.async_register(hass, FleetStatusIntentHandler())
    intent.async_register(hass, LastEventIntentHandler())
    intent.async_register(hass, WhatsUpIntentHandler())


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
