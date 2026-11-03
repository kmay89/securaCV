"""The one defensive gate every MQTT callback shares.

Everything on the broker is untrusted input: a hostile or misconfigured
publisher must never be able to balloon memory, AttributeError a @callback
out of existence, or mint a device out of a topic segment. The payload
helpers here are what every handler in the package calls first; they used
to live in ``__init__.py`` and are re-exported from there unchanged.
``broker_text`` bounds a single value the device registry takes from a
payload.
"""
from __future__ import annotations

import json
import re
from typing import Any

# Untrusted-broker hardening: cap how much of any single MQTT payload this
# integration will decode/parse. Real device publishes are well under 8 KiB;
# anything larger is malformed or hostile and gets dropped before json.loads.
MAX_MQTT_PAYLOAD_BYTES = 64 * 1024

# What a Canary actually puts in the device-id segment of its topics: the
# firmware's device_pseudonym / hostname alphabet. Anything else on the wildcard
# subscription is a hostile or misconfigured publisher, and before this gate it
# minted a device-registry entry plus up to ~20 entities per topic segment,
# unbounded (device_id was taken verbatim from the topic).
DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")

# The longest broker-supplied text the device registry takes (a name, a
# firmware version, a board): a device page is not a place for a hostile
# publisher's essay.
BROKER_TEXT_MAX_LENGTH = 64


def mqtt_payload_within_cap(payload: Any) -> bool:
    """True when a raw MQTT payload is within the untrusted-broker cap."""
    try:
        return len(payload) <= MAX_MQTT_PAYLOAD_BYTES
    except TypeError:
        return False


def valid_device_id(device_id: Any) -> bool:
    """True iff `device_id` is a topic segment a real Canary could publish."""
    # fullmatch, not match: `$` would accept a trailing newline.
    return isinstance(device_id, str) and DEVICE_ID_RE.fullmatch(device_id) is not None


def broker_text(value: Any, limit: int = BROKER_TEXT_MAX_LENGTH) -> str | None:
    """A broker-supplied value as bounded, printable text, or None.

    Only a str is text: a number, bool, list or object is None, never its
    ``str()``. Characters that neither print nor are whitespace (control
    characters among them) are dropped first, then runs of whitespace
    become one space and the ends are trimmed, so a dropped character
    leaves no space behind; the result is cut to ``limit`` characters, and
    empty is None.
    """
    if not isinstance(value, str):
        return None
    kept = "".join(ch for ch in value if ch.isprintable() or ch.isspace())
    text = " ".join(kept.split())
    return text[:limit].rstrip() or None


def parse_mqtt_json(payload: Any) -> dict[str, Any] | None:
    """Decode an MQTT payload into a JSON object, or None.

    Enforces the untrusted-broker size cap BEFORE decoding, tolerates bytes
    or str, and only ever returns a dict. Valid JSON that is a bare scalar
    or array returns None too — every handler indexes by key, and a hostile
    broker must never be able to AttributeError a @callback out of
    existence (payload "5" would make ``data.get(...)`` raise on the int).
    """
    if not mqtt_payload_within_cap(payload):
        return None
    if isinstance(payload, bytes):
        try:
            payload = payload.decode()
        except UnicodeDecodeError:
            return None
    try:
        data = json.loads(payload)
    except (json.JSONDecodeError, TypeError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def mqtt_payload_text(payload: Any) -> str | None:
    """A capped payload as text, or None when it is over the cap or not UTF-8.

    For the bare-word topics (``online``/``offline``, ``ON``/``OFF``,
    ``claimed``/``unclaimed``) that carry no JSON.
    """
    if not mqtt_payload_within_cap(payload):
        return None
    if isinstance(payload, bytes):
        try:
            return payload.decode()
        except UnicodeDecodeError:
            return None
    if isinstance(payload, str):
        return payload
    return None


def creating_payload(msg: Any) -> dict[str, Any] | None:
    """The body of the publish that creates an entity, to hand to it, or None.

    A discovery callback creates an entity on a device's first publish on a
    topic, and the entity subscribes only once it is added, after that
    delivery: on a topic that is not retained (events) it never sees the
    publish that made it unless it is handed the body (HA44; event.py's
    tamper entity is handed its own the same way). A delivery flagged
    retained is not handed over: the entity's own subscription is sent the
    broker's retained copy anyway.
    """
    if getattr(msg, "retain", False):
        return None
    return parse_mqtt_json(getattr(msg, "payload", None))


class CreatingEcho:
    """The creating publish's body: applied once, and its one echo recognized.

    ``take()`` hands the body over once (async_added_to_hass applies it after
    its subscribe) and drops it, so a second add of the same entity replays
    nothing. A broker that ALSO retained that publish (live, a broker
    forwards it with the retain flag cleared, so the discovery callback
    handed it over) sends the entity's fresh subscription its retained copy
    next. ``is_echo()`` is true for exactly that delivery, the first JSON
    object the entity reads after ``take()``, flagged retained and equal to
    the body shown; whatever that first one is, it ends the watch, so a
    later publish of the same body (a re-send, the retained copy after a
    reconnect) takes the ordinary path.
    """

    __slots__ = ("_pending", "_shown")

    def __init__(self, payload: dict[str, Any] | None) -> None:
        self._pending = payload
        self._shown: dict[str, Any] | None = None

    def take(self) -> dict[str, Any] | None:
        """The body to apply now, once; None after the first call."""
        payload, self._pending = self._pending, None
        self._shown = payload
        return payload

    def is_echo(self, msg: Any, data: dict[str, Any]) -> bool:
        """True for the subscription's retained copy of the body just applied."""
        shown, self._shown = self._shown, None
        return shown is not None and bool(getattr(msg, "retain", False)) and data == shown
