"""The one defensive gate every MQTT callback shares.

Everything on the broker is untrusted input: a hostile or misconfigured
publisher must never be able to balloon memory, AttributeError a @callback
out of existence, or mint a device out of a topic segment. The three
helpers here are what every handler in the package calls first; they used
to live in ``__init__.py`` and are re-exported from there unchanged.
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
