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
     keys, hashes, entry ids, network and location identifiers), so a
     value that reaches the dump through a path this file did not foresee
     is still replaced.

Fingerprints are shown truncated (eight hex characters), enough to correlate
with what the kernel or a Canary prints, never the whole identifier.
"""
from __future__ import annotations

from dataclasses import asdict
from typing import Any
from urllib.parse import urlsplit

from homeassistant.components.diagnostics import async_redact_data
from homeassistant.const import CONF_TOKEN, CONF_URL
from homeassistant.core import HomeAssistant

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
from .kernel_trust import kernel_key_fingerprint, normalize_kernel_key
from .mqtt_payload import parse_mqtt_json

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
}

# How much of an identifier survives: the fingerprint rule.
SHOWN_HEX = 8


def _truncated(value: Any, keep: int = SHOWN_HEX) -> str | None:
    """`value` cut to `keep` characters plus an ellipsis, or None."""
    if not isinstance(value, str) or not value:
        return None
    return value[:keep] + "…" if len(value) > keep else value


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


def _canary_shape(device: dict[str, Any]) -> dict[str, Any]:
    """Firmware, hardware and liveness — never the raw status payload."""
    status = device.get("status")
    data = status if isinstance(status, dict) else (parse_mqtt_json(status) or {})
    return {
        "online": device.get("online"),
        "firmware": data.get("firmware_version") or data.get("fw_version"),
        "hardware": data.get("hardware") or data.get("board"),
        "device_type": data.get("device_type"),
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
        "kernel_device_id_prefix": _truncated(entry.data.get(CONF_KERNEL_DEVICE_ID), keep=8),
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
    # characters of each, enough to tell devices apart, like the fingerprints.
    devices_info: dict[str, Any] = {}
    trust_info: dict[str, Any] = {}
    if runtime is not None:
        for device_id, device in runtime.devices.items():
            shown = _truncated(device_id, keep=8) or device_id
            devices_info[shown] = _canary_shape(device if isinstance(device, dict) else {})
            entry_trust = runtime.trust_store.get(device_id)
            if entry_trust:
                trust_info[shown] = {
                    "pinned": bool(entry_trust.fingerprint_hex),
                    "pin_source": getattr(entry_trust, "pin_source", "unknown"),
                    # The full fingerprint is exactly 16 hex chars
                    # (device_trust: sha256 digest[:8].hex()), so truncate
                    # to half of it — diagnostics dumps get shared publicly,
                    # and 8 chars is plenty to correlate against the
                    # fingerprint the device shows (docs/device_trust.md)
                    # without republishing the whole identifier.
                    "fingerprint": _truncated(entry_trust.fingerprint_hex),
                }

    verify_info: dict[str, Any] = {}
    if runtime is not None:
        for device_id, verdict in runtime.verify.items():
            verify_info[_truncated(device_id, keep=8) or device_id] = {
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
