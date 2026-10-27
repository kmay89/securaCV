"""Diagnostics: what a publicly shared dump may contain.

A diagnostics file gets attached to GitHub issues, so nothing in it may be
an identifier the owner did not mean to publish (THREAT_MODEL.md principle
1: never in exports, logs, diagnostics). Two layers enforce that:

  1. nothing raw is ever copied in — a Canary's status payload (which
     carries its IP, AP address and the name the owner gave it) is
     projected to firmware / hardware / online, the latest witness event to
     event type / time bucket / attestation, evidence exports to a count;
  2. ``async_redact_data`` then sweeps the result for every key in
     ``TO_REDACT`` (tokens, the pairing token id, the kernel URL's host,
     keys, hashes, entry ids, network and location identifiers, and the
     keys a Canary's answers use for its hardware, chip, module and radio
     identifiers), so a value that reaches the dump through a path this
     file did not foresee is still replaced.

Both layers cover what this function returns. Home Assistant adds every
Repairs issue it holds for the integration to the downloaded file itself,
outside them (ledger F399): the ones the owner ignored and those of the
integration's other entries too, each with the time it was raised. An
entry-scoped issue id ends in the entry id, and a kernel key-mismatch issue
carries the kernel's URL and the key it served.

A Canary's key fingerprint carries none of its digits into the dump. Until
the canary-wap stops serving it, the whole fingerprint is readable by anyone
in radio range (its Bluetooth Device Information serial), and the last four
digits are on air for good in its Bluetooth name and beacons, so any part of
it in a posted dump lets a stranger nearby match the device. Each pinned
Canary gets a correlation id instead: a keyed hash of its fingerprint under
a salt drawn for this dump and never written down, spelled in letters that
are not hex digits. Two Canaries pinned to one key share an id within a
dump; nothing links an id to the key, or one dump to another. The kernel's
own key fingerprint (eight hex characters, the kernel_fingerprint sensor's
state) is unchanged.
"""
from __future__ import annotations

import hashlib
import hmac
import secrets
from collections import Counter
from collections.abc import Callable, Iterable
from dataclasses import asdict
from typing import Any
from urllib.parse import urlsplit

from homeassistant.components.diagnostics import async_redact_data
from homeassistant.const import CONF_TOKEN, CONF_URL
from homeassistant.core import HomeAssistant
from homeassistant.helpers import device_registry as dr

from .const import (
    CONF_AUTH_MODE,
    CONF_ENABLE_MQTT,
    CONF_KERNEL_DEVICE_ID,
    CONF_KERNEL_VERIFYING_KEY,
    CONF_MQTT_PREFIX,
    CONF_MQTT_TOPIC_PREFIX,
    CONF_SETUP_MODE,
    DOMAIN,
)
from . import PLATFORMS
from .data import SecuraCVConfigEntry, runtime_for
from .entity import canary_registry_facts
from .kernel_trust import kernel_key_fingerprint, normalize_kernel_key
from .mqtt_payload import valid_device_id

# Every key whose value is a secret, a credential, a stable identifier, a
# network address or a location. Applied recursively by async_redact_data.
TO_REDACT = {
    CONF_TOKEN,
    "token_file",
    "token_id",
    "kernel_token_id",
    "pairing_code",
    "host",
    "hostname",
    "url",
    "device_id",
    "kernel_device_id",
    "verifying_key",
    "kernel_verifying_key",
    "kernel_previous_verifying_keys",
    "public_key",
    "pubkey_hex",
    "latest_hash",
    "entry_hash",
    "entry_id",
    "unique_id",
    "nonce",
    "claim_nonce",
    "ip",
    "ap_ip",
    "sta_ip",
    "mac",
    "bssid",
    "ssid",
    "device_name",
    "name",
    "latitude",
    "longitude",
    "seed",
    "sig",
    # The identifier keys a Canary's answers use (device-details finding P18,
    # ledger F391): the salted hardware token, the mDNS host name, a serial
    # number, chip and module ids, and Bluetooth addresses (the radio's own,
    # a connection's, a peer's). Named before any projection of those
    # answers reaches a dump.
    "hw_token",
    "mdns_host",
    "serial_number",
    "chip_id",
    "module_id",
    "local_address",
    "address",
    "peer_address",
}

# How many characters of the kernel id and of a Canary id the dump keeps.
SHOWN_ID_CHARS = 8

# The correlation id's alphabet: sixteen letters, none of them a hex digit,
# so no digit of the fingerprint it stands for can appear in it.
_CORRELATION_ALPHABET = "ghjkmnpqrstvwxyz"
# Eight letters, 32 bits of the keyed hash: with ten pinned Canaries in one
# dump, two different keys share an id about once in 95 million dumps.
_CORRELATION_LETTERS = 8


def _truncated(value: Any, keep: int = SHOWN_ID_CHARS) -> str | None:
    """`value` cut to `keep` characters plus an ellipsis, or None."""
    if not isinstance(value, str) or not value:
        return None
    return value[:keep] + "…" if len(value) > keep else value


def _shown_ids(device_ids: Iterable[str]) -> dict[str, str]:
    """Each Canary id as the dump's row key: its first SHOWN_ID_CHARS characters.

    Ids that share those characters (two canary-wap units, `canary-s3-…`;
    a Sense and a Sentinel, `canary_s…`) each get a per-dump ordinal after
    them, numbered in the order given (the Canary records, then every other
    Canary with a device page under the entry, then any id only verification
    has seen: a verdict held for a Canary whose page was deleted), so no row
    replaces another and no further character of an id is shown.
    """
    ids = list(dict.fromkeys(device_ids))
    prefix = {device_id: _truncated(device_id) or device_id for device_id in ids}
    shared = Counter(prefix.values())
    seen: Counter[str] = Counter()
    shown: dict[str, str] = {}
    for device_id in ids:
        key = prefix[device_id]
        if shared[key] > 1:
            seen[key] += 1
            key = f"{key} #{seen[key]}"
        shown[device_id] = key
    return shown


def _canary_page_ids(hass: HomeAssistant, entry: SecuraCVConfigEntry) -> list[str]:
    """The id of every Canary the entry has a device page for.

    Read from the device registry, in the order it lists the entry's
    devices. A Canary's first publish on any topic the integration reads
    makes its page: the status handler writes it, the health handler does
    when the health reports a version, and the sensor, binary sensor and
    event platforms add entities, and so the page, for its status, health,
    counts, chain, events, tamper, transport, mesh and chirp. The runtime's
    records miss some of those (a health with no version, or a first mesh
    publish, records nothing), so the registry is what says which Canaries
    have a page. The kernel's page is not a Canary's.
    """
    prefix = "canary_"
    ids: list[str] = []
    for device in dr.async_entries_for_config_entry(dr.async_get(hass), entry.entry_id):
        for domain, identifier in sorted(device.identifiers):
            device_id = identifier[len(prefix):]
            if domain == DOMAIN and identifier.startswith(prefix) and valid_device_id(device_id):
                ids.append(device_id)
    return ids


def _correlation_ids() -> Callable[[Any], str | None]:
    """A per-dump correlation id for key fingerprints (finding P19, F392).

    The salt is 16 random bytes drawn for one dump and never written to it,
    so an id says only "the same key as another row of this dump". The
    fingerprint is case-folded first, so two spellings of one key share an
    id. None for an unpinned device.
    """
    salt = secrets.token_bytes(16)

    def correlation_id(fingerprint: Any) -> str | None:
        if not isinstance(fingerprint, str) or not fingerprint:
            return None
        digest = hmac.new(salt, fingerprint.lower().encode(), hashlib.sha256).digest()
        letters = "".join(
            _CORRELATION_ALPHABET[byte >> 4] + _CORRELATION_ALPHABET[byte & 0x0F]
            for byte in digest[: _CORRELATION_LETTERS // 2]
        )
        return f"{letters[:4]}-{letters[4:]}"

    return correlation_id


def _url_shape(url: Any) -> dict[str, Any]:
    """The scheme and port of the kernel URL; the host is an identifier."""
    if not isinstance(url, str) or not url:
        return {"configured": False}
    parts = urlsplit(url)
    return {"configured": True, "scheme": parts.scheme or None, "port": parts.port}


def _latest_event_shape(event: Any) -> dict[str, Any] | None:
    """The coarse claim only: type, bucket, attestation. Never the zone label."""
    if not isinstance(event, dict):
        return None
    bucket = event.get("time_bucket")
    if not isinstance(bucket, dict):
        start = event.get("time_bucket_start")
        size = event.get("time_bucket_size")
        bucket = {"start_epoch_s": start, "size_s": size} if start is not None else None
    return {
        "event_type": event.get("event_type"),
        "time_bucket": bucket,
        "attestation": event.get("attestation"),
    }


def _digest_shape(digest: Any) -> dict[str, Any] | None:
    """Counts only: zone labels are what the owner said, not for a dump."""
    if not isinstance(digest, dict):
        return None
    per_zone = digest.get("per_zone")
    event_types = digest.get("event_types")
    return {
        "total_events": digest.get("total_events"),
        "window_secs": digest.get("window_secs"),
        "retention_days": digest.get("retention_days"),
        "zone_count": len(per_zone) if isinstance(per_zone, (dict, list)) else None,
        "event_type_count": len(event_types) if isinstance(event_types, (dict, list)) else None,
        "generated_at_bucket_start": digest.get("generated_at_bucket_start"),
    }


def _frigate_link_shape(link: Any) -> dict[str, Any] | None:
    if not isinstance(link, dict):
        return None
    return {
        "connected": link.get("connected"),
        "mode": link.get("mode"),
        "messages_since_start": link.get("messages_since_start"),
        "last_message_bucket_start": link.get("last_message_bucket_start"),
    }


def _identity_shape(identity: Any, pinned_key: str | None) -> dict[str, Any] | None:
    if not isinstance(identity, dict):
        return None
    served = normalize_kernel_key(identity.get("verifying_key"))
    last_verify = identity.get("last_verify")
    return {
        "kernel_version": identity.get("kernel_version"),
        "ruleset_id": identity.get("ruleset_id"),
        "chain_length": identity.get("chain_length"),
        "paired_tokens": identity.get("paired_tokens"),
        "fleet_rollcall": identity.get("fleet_rollcall"),
        "served_key_matches_pin": (
            None if served is None or pinned_key is None else served == pinned_key
        ),
        "kernel_reported_last_verify": (
            {
                "at_bucket_start": last_verify.get("at"),
                "ok": last_verify.get("ok"),
                "identity_verified": last_verify.get("identity_verified"),
            }
            if isinstance(last_verify, dict)
            else None
        ),
    }


def _storage_shape(status: Any) -> dict[str, Any] | None:
    """The storage endurance numbers only; `source_device` is a device path."""
    if not isinstance(status, dict):
        return None
    storage = status.get("storage")
    if not isinstance(storage, dict):
        return None
    return {
        key: storage.get(key)
        for key in (
            "status",
            "free_pct",
            "wear_pct",
            "write_rate_bytes_per_day",
            "estimated_days_remaining",
            "replace_recommended",
        )
        if key in storage
    }


def _canary_shape(device: dict[str, Any], page: dict[str, str]) -> dict[str, Any]:
    """Firmware, hardware and liveness — never the raw status payload.

    Within a run the firmware and the board are the device page's own
    (``entity.canary_registry_facts``: the health topic's version, else the
    one the status last carried, and the board the status last named), and
    the type is the canonical one the status last named. Those facts are
    what the Canary has published since the entry was set up: a page kept
    from before a restart or a reload keeps the version and board the
    registry stored, and its row shows None for them until the Canary
    publishes them again. The status and health handlers recorded each as bounded broker
    text; nothing here re-reads a payload.
    """
    device_type = device.get("device_type")
    return {
        "online": device.get("online"),
        "firmware": page.get("sw_version"),
        "hardware": page.get("hw_version"),
        "device_type": device_type if isinstance(device_type, str) else None,
        "last_event_type": (device.get("last_event") or {}).get("event_type")
        if isinstance(device.get("last_event"), dict)
        else None,
    }


async def async_get_config_entry_diagnostics(
    hass: HomeAssistant, entry: SecuraCVConfigEntry
) -> dict[str, Any]:
    """Return diagnostics for a config entry."""
    runtime = runtime_for(entry)
    coordinator = runtime.coordinator if runtime else None
    data = coordinator.data if coordinator is not None and coordinator.data else {}

    pinned_key = normalize_kernel_key(entry.data.get(CONF_KERNEL_VERIFYING_KEY))
    kernel_info: dict[str, Any] = {
        # Keyed "api", not "url": the redaction sweep replaces any `url`
        # value wholesale, and this one is already only scheme and port.
        "api": _url_shape(entry.data.get(CONF_URL)),
        "auth_mode": entry.data.get(CONF_AUTH_MODE),
        # The prefix only (`pwk_` + four hex), under a key the sweep leaves alone.
        "kernel_device_id_prefix": _truncated(entry.data.get(CONF_KERNEL_DEVICE_ID)),
        "key_pinned": pinned_key is not None,
        "pinned_fingerprint": kernel_key_fingerprint(pinned_key) if pinned_key else None,
        "topic_prefix": entry.data.get(CONF_MQTT_TOPIC_PREFIX),
        "last_update_success": coordinator.last_update_success if coordinator else None,
        "available": coordinator.kernel_available if coordinator else None,
        "online_via_mqtt": coordinator.kernel_online if coordinator else None,
        "backstop_ok": coordinator.backstop_ok if coordinator else None,
        "backstop_bucket_start": data.get("backstop_bucket_start"),
        "last_push_bucket_start": data.get("last_push_bucket_start"),
        "key_mismatch": data.get("key_mismatch"),
        "latest_event": _latest_event_shape(data.get("latest_event")),
        "digest": _digest_shape(data.get("digest")),
        "chain_problem": data.get("chain_problem"),
        "claim_state": data.get("claim_state"),
        "claim_published": runtime.claim_published if runtime else False,
        "frigate_link": _frigate_link_shape(data.get("frigate_link")),
        "identity": _identity_shape(data.get("identity"), pinned_key),
        "storage_status": _storage_shape(data.get("status")),
    }

    # Canary ids are the owner's local pseudonyms; the dump keeps eight
    # characters of each (and an ordinal where two share them). A Canary's
    # key fingerprint keeps none of its digits: the module docstring says
    # why, and what stands in for it.
    devices_info: dict[str, Any] = {}
    trust_info: dict[str, Any] = {}
    correlation_id = _correlation_ids()
    # A row for every Canary the integration has a record of this run (from
    # its status, its availability or an event it published), then for every
    # other Canary with a device page under this entry, whichever topic made
    # the page and whether or not it has published since Home Assistant
    # started; what it has not published since is None.
    canary_ids: list[str] = []
    shown_ids: dict[str, str] = {}
    if runtime is not None:
        canary_ids = list(dict.fromkeys([*runtime.devices, *_canary_page_ids(hass, entry)]))
        shown_ids = _shown_ids([*canary_ids, *runtime.verify])
        for device_id in canary_ids:
            shown = shown_ids[device_id]
            device = runtime.devices.get(device_id)
            devices_info[shown] = _canary_shape(
                device if isinstance(device, dict) else {},
                canary_registry_facts(entry, device_id),
            )
            entry_trust = runtime.trust_store.get(device_id)
            if entry_trust:
                trust_info[shown] = {
                    "pinned": bool(entry_trust.fingerprint_hex),
                    "pin_source": getattr(entry_trust, "pin_source", "unknown"),
                    "key_correlation_id": correlation_id(entry_trust.fingerprint_hex),
                }

    verify_info: dict[str, Any] = {}
    if runtime is not None:
        for device_id, verdict in runtime.verify.items():
            verify_info[shown_ids[device_id]] = {
                "trusted": verdict.get("trusted", False),
                "reason": verdict.get("reason", "unknown"),
            }

    mqtt_info = {
        "enabled": entry.data.get(CONF_ENABLE_MQTT, False),
        "prefix": (runtime.mqtt_prefix if runtime else None) or entry.data.get(CONF_MQTT_PREFIX),
        "subscription_count": len(runtime.unsub_mqtt) if runtime else 0,
    }

    verification: dict[str, Any] | None = None
    if runtime is not None:
        verification = asdict(runtime.verification)
        # The covered set is (event type, zone, bucket) triples: zones are
        # owner-named; the count says how much the walk covered.
        verification["covered"] = len(runtime.verification.covered)
        verification.pop("head_hash", None)

    # Watches are domain-scoped (hass.data[DOMAIN]["watches"], persisted by
    # watch_runtime). Count only: a label is what the owner said aloud, and
    # diagnostics dumps get shared publicly.
    watch_bucket = hass.data.get(DOMAIN, {}).get("watches")
    watch_count = len(watch_bucket) if isinstance(watch_bucket, list) else 0

    exports = runtime.exports if runtime else {}
    export_rows = exports.get("exports") if isinstance(exports, dict) else None

    result = {
        "setup_mode": entry.data.get(CONF_SETUP_MODE, "unknown"),
        "entry_version": getattr(entry, "version", None),
        "kernel": kernel_info,
        "mqtt": mqtt_info,
        "canary_devices": devices_info,
        "canary_device_count": len(devices_info),
        "trust": trust_info,
        "verification": verify_info,
        "chain_verification": verification,
        "mismatch_notifications_sent": len(runtime.mismatch_notified) if runtime else 0,
        "watch_count": watch_count,
        "evidence_exports": {
            "count": len(export_rows) if isinstance(export_rows, list) else 0,
        },
        "platforms_loaded": [
            getattr(platform, "value", platform) for platform in PLATFORMS
        ],
    }
    return async_redact_data(result, TO_REDACT)
