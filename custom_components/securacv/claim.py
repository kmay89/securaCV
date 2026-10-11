"""The retained claim: "this kernel is managed by the integration" (C3).

While the integration is loaded for a kernel, the kernel's MQTT bridge would
otherwise keep advertising its own ``pwk_*`` entities through MQTT discovery
beside the integration's — one physical thing twice. The claim is a retained
message on ``<prefix>/claim/<kernel_device_id>``; the bridge withdraws its
discovery configs (empty retained payloads) while a claim fresher than 24 h
is present, and republishes them when the claim is cleared.

The payload carries a random per-entry nonce, never the entry id (a stable
per-installation identifier has no place in shared telemetry), and the
bucket start it was claimed at (coarse time). It is republished on every
load and hourly, so a claim can never outlive the integration by more than
the bridge's 24-hour freshness window — and it is withdrawn ONLY from
``async_remove_entry``. A reload or restart must not flip the bridge's
entities out and in.
"""
from __future__ import annotations

import json
import logging
import secrets
import time
from datetime import datetime, timedelta
from typing import Any, Callable

from homeassistant.components import mqtt
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.event import async_track_time_interval

from .const import (
    CONF_CLAIM_NONCE,
    CONF_KERNEL_DEVICE_ID,
    CONF_MQTT_TOPIC_PREFIX,
    DEFAULT_MQTT_TOPIC_PREFIX,
    KERNEL_TOPIC_CLAIM,
    TIME_BUCKET_SECONDS,
)
from .data import SecuraCVConfigEntry

_LOGGER = logging.getLogger(__name__)

CLAIM_SCHEMA_VERSION = 1
# Hourly: the bridge treats a claim older than 24 h as cleared.
CLAIM_REPUBLISH_INTERVAL = timedelta(hours=1)
CLAIM_QOS = 1
CLAIM_RETAIN = True
# 16 hex, random per entry.
CLAIM_NONCE_BYTES = 8


def new_claim_nonce() -> str:
    """A fresh per-entry nonce (16 hex)."""
    return secrets.token_hex(CLAIM_NONCE_BYTES)


def bucket_start(now: float | None = None) -> int:
    """The start of the 10-minute bucket `now` falls in."""
    stamp = time.time() if now is None else now
    return int(stamp // TIME_BUCKET_SECONDS) * TIME_BUCKET_SECONDS


def claim_topic(prefix: str, device_id: str) -> str:
    """``<prefix>/claim/<kernel_device_id>``."""
    return f"{prefix}/{KERNEL_TOPIC_CLAIM}/{device_id}"


def claim_payload(nonce: str, now: float | None = None) -> str:
    """The claim document, compact JSON."""
    return json.dumps(
        {
            "v": CLAIM_SCHEMA_VERSION,
            "claimed": True,
            "nonce": nonce,
            "claimed_bucket_start": bucket_start(now),
        },
        separators=(",", ":"),
    )


def claim_identity(entry: SecuraCVConfigEntry) -> tuple[str, str, str] | None:
    """(prefix, kernel device id, nonce) when this entry may claim a kernel.

    None until the entry knows its kernel's device id — a claim for an id
    the integration has not read over the authenticated channel is
    forbidden — or when no nonce was ever assigned (an entry that predates
    the claim and has not been migrated).
    """
    device_id = entry.data.get(CONF_KERNEL_DEVICE_ID)
    nonce = entry.data.get(CONF_CLAIM_NONCE)
    if not device_id or not nonce:
        return None
    prefix = entry.data.get(CONF_MQTT_TOPIC_PREFIX) or DEFAULT_MQTT_TOPIC_PREFIX
    return str(prefix), str(device_id), str(nonce)


async def async_publish_claim(hass: HomeAssistant, entry: SecuraCVConfigEntry) -> bool:
    """Publish (or refresh) the retained claim. False when the entry cannot claim."""
    identity = claim_identity(entry)
    if identity is None:
        return False
    prefix, device_id, nonce = identity
    await mqtt.async_publish(
        hass,
        claim_topic(prefix, device_id),
        claim_payload(nonce),
        qos=CLAIM_QOS,
        retain=CLAIM_RETAIN,
    )
    return True


async def async_withdraw_claim(hass: HomeAssistant, prefix: str, device_id: str) -> None:
    """Clear the retained claim: an empty retained payload on the same topic."""
    await mqtt.async_publish(
        hass,
        claim_topic(prefix, device_id),
        b"",
        qos=CLAIM_QOS,
        retain=CLAIM_RETAIN,
    )


@callback
def async_schedule_claim_republish(
    hass: HomeAssistant, entry: SecuraCVConfigEntry
) -> Callable[[], None]:
    """Republish the claim hourly; returns the cancel callable."""

    async def _republish(_now: datetime) -> None:
        try:
            await async_publish_claim(hass, entry)
        except Exception:  # noqa: BLE001 - a missed republish is retried next hour
            _LOGGER.debug("claim republish failed", exc_info=True)

    return async_track_time_interval(hass, _republish, CLAIM_REPUBLISH_INTERVAL)


def claim_document(entry: SecuraCVConfigEntry) -> dict[str, Any] | None:
    """What this entry's claim says (for diagnostics; the nonce is redacted there)."""
    identity = claim_identity(entry)
    if identity is None:
        return None
    prefix, device_id, nonce = identity
    return {"topic": claim_topic(prefix, device_id), "nonce": nonce}
