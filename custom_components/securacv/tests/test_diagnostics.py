"""Diagnostics — what a publicly shared dump actually contains.

Two layers are held here. Nothing raw is copied in: a Canary's status
payload (IP, AP address, the name the owner gave it) is projected to
firmware / hardware / online, the latest witness event to type / bucket /
attestation (never the zone label), the digest to counts, the kernel URL
to scheme and port. Then ``async_redact_data`` sweeps the result with
``TO_REDACT``, so a secret that reached the dump through a path this file
did not foresee is still replaced. The old regression (a fingerprint
"truncated" to its own full length) stays pinned.
"""

from __future__ import annotations

import types

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import make_entry, make_runtime, run

from homeassistant.core import HomeAssistant  # noqa: E402  (the stub)

from ..const import (  # noqa: E402
    AUTH_MODE_PAIRED,
    CONF_AUTH_MODE,
    CONF_CLAIM_NONCE,
    CONF_ENABLE_MQTT,
    CONF_KERNEL_DEVICE_ID,
    CONF_KERNEL_TOKEN_ID,
    CONF_KERNEL_VERIFYING_KEY,
    CONF_MQTT_PREFIX,
    CONF_MQTT_TOPIC_PREFIX,
    CONF_SETUP_MODE,
    DOMAIN,
    SETUP_MODE_BOTH,
)
from ..diagnostics import TO_REDACT, async_get_config_entry_diagnostics  # noqa: E402
from ..kernel_trust import kernel_key_fingerprint  # noqa: E402

FULL_FP = "aabbccdd11223344"  # 16 hex chars, like the real Canary fingerprint
KERNEL_KEY = "ab" * 32
TOKEN = "f1" * 32
NONCE = "0123456789abcdef"
STATUS = (
    '{"online": true, "firmware_version": "2.4.16", "hardware": "esp32s3", '
    '"device_type": "canary-wap", "ip": "192.168.1.77", "ap_ip": "192.168.4.1", '
    '"device_name": "Front gate", "mac": "aa:bb:cc:dd:ee:ff"}'
)
LATEST_EVENT = {
    "event_type": "BoundaryCrossingObjectLarge",
    "zone_id": "zone:front_door",
    "zone": "front_door",
    "time_bucket": {"start_epoch_s": 1760140800, "size_s": 600},
    "confidence": 0.9,
    "attestation": "adapter",
    "device_id": "pwk_1a2b3c4d",
}
DIGEST = {
    "total_events": 12,
    "window_secs": 86400,
    "retention_days": 30,
    "per_zone": {"zone:front_door": 7, "zone:driveway": 5},
    "event_types": {"BoundaryCrossingObjectLarge": 12},
    "generated_at_bucket_start": 1760140200,
}
IDENTITY = {
    "device_id": "pwk_1a2b3c4d",
    "verifying_key": KERNEL_KEY,
    "kernel_version": "2.4.16",
    "chain_length": 1287,
    "latest_hash": "9c" * 32,
    "paired_tokens": 1,
    "fleet_rollcall": "self",
    "last_verify": {"at": 1760140200, "bucket_size": 600, "ok": True, "identity_verified": False},
}


def _entry_with_kernel(hass: HomeAssistant, fp: str | None = FULL_FP):
    trust_entry = types.SimpleNamespace(fingerprint_hex=fp, pin_source="tofu")
    trust_store = types.SimpleNamespace(get=lambda device_id: trust_entry)
    coordinator = types.SimpleNamespace(
        data={
            "latest_event": LATEST_EVENT,
            "digest": DIGEST,
            "identity": IDENTITY,
            "status": {
                "storage": {
                    "status": "good",
                    "free_pct": 71.0,
                    "wear_pct": 3,
                    "source_device": "/dev/mmcblk0p2",
                    "lifetime_bytes_written": 123456789,
                }
            },
            "chain_problem": False,
            "claim_state": "claimed",
            "frigate_link": {
                "connected": True,
                "mode": "events",
                "messages_since_start": 40,
                "last_message_bucket_start": 1760140800,
                "topic_prefix": "frigate",
            },
            "key_mismatch": False,
            "backstop_bucket_start": 1760140800,
            "last_push_bucket_start": 1760140800,
        },
        last_update_success=True,
        kernel_available=True,
        kernel_online=True,
        backstop_ok=True,
    )
    runtime = make_runtime(
        hass,
        trust_store=trust_store,
        coordinator=coordinator,
        devices={"canary01": {"status": STATUS, "online": True}},
        mqtt_prefix="securacv",
        unsub_mqtt=[lambda: None, lambda: None],
    )
    runtime.claim_published = True
    return make_entry(
        hass,
        runtime=runtime,
        data={
            "url": "http://192.168.1.20:8799",
            "token": TOKEN,
            CONF_AUTH_MODE: AUTH_MODE_PAIRED,
            CONF_KERNEL_TOKEN_ID: "0a1b2c3d",
            CONF_KERNEL_DEVICE_ID: "pwk_1a2b3c4d",
            CONF_KERNEL_VERIFYING_KEY: KERNEL_KEY,
            CONF_MQTT_TOPIC_PREFIX: "witness",
            CONF_CLAIM_NONCE: NONCE,
            CONF_ENABLE_MQTT: True,
            CONF_MQTT_PREFIX: "securacv",
            CONF_SETUP_MODE: SETUP_MODE_BOTH,
        },
    )


def _diag(hass, entry):
    hass.data.setdefault(DOMAIN, {})
    return run(async_get_config_entry_diagnostics(hass, entry))


def test_fingerprint_is_actually_truncated() -> None:
    hass = HomeAssistant()
    diag = _diag(hass, _entry_with_kernel(hass))
    shown = diag["trust"]["canary01"]["fingerprint"]
    assert shown.endswith("…")
    # Really shorter than the identifier it stands for — not the old no-op.
    assert len(shown.rstrip("…")) < len(FULL_FP)
    assert shown == FULL_FP[:8] + "…"
    # And the full fingerprint appears nowhere in the dump.
    assert FULL_FP not in repr(diag)


def test_diagnostics_survive_an_unpinned_device() -> None:
    hass = HomeAssistant()
    diag = _diag(hass, _entry_with_kernel(hass, fp=""))
    assert diag["trust"]["canary01"]["fingerprint"] is None
    assert diag["trust"]["canary01"]["pinned"] is False


def test_nothing_secret_or_identifying_survives() -> None:
    """The seeded secrets: token, token id, nonce, the key, the URL's host,
    the zone label, the Canary's addresses and name, the chain head."""
    hass = HomeAssistant()
    text = repr(_diag(hass, _entry_with_kernel(hass)))
    for secret in (
        TOKEN,
        "0a1b2c3d",
        NONCE,
        KERNEL_KEY,
        "192.168.1.20",
        "192.168.1.77",
        "192.168.4.1",
        "aa:bb:cc:dd:ee:ff",
        "Front gate",
        "front_door",
        "zone:",
        "driveway",
        "9c" * 32,
        "pwk_1a2b3c4d",
        "/dev/mmcblk0p2",
    ):
        assert secret not in text, secret


def test_the_kernel_section_is_a_projection() -> None:
    hass = HomeAssistant()
    diag = _diag(hass, _entry_with_kernel(hass))
    kernel = diag["kernel"]
    assert kernel["api"] == {"configured": True, "scheme": "http", "port": 8799}
    assert kernel["auth_mode"] == AUTH_MODE_PAIRED
    assert kernel["kernel_device_id_prefix"] == "pwk_1a2b…"
    assert kernel["key_pinned"] is True
    assert kernel["pinned_fingerprint"] == kernel_key_fingerprint(KERNEL_KEY)
    assert kernel["available"] is True and kernel["online_via_mqtt"] is True
    assert kernel["latest_event"] == {
        "event_type": "BoundaryCrossingObjectLarge",
        "time_bucket": {"start_epoch_s": 1760140800, "size_s": 600},
        "attestation": "adapter",
    }
    assert kernel["digest"] == {
        "total_events": 12,
        "window_secs": 86400,
        "retention_days": 30,
        "zone_count": 2,
        "event_type_count": 1,
        "generated_at_bucket_start": 1760140200,
    }
    assert kernel["identity"]["served_key_matches_pin"] is True
    assert kernel["identity"]["kernel_reported_last_verify"] == {
        "at_bucket_start": 1760140200,
        "ok": True,
        "identity_verified": False,
    }
    assert kernel["frigate_link"] == {
        "connected": True,
        "mode": "events",
        "messages_since_start": 40,
        "last_message_bucket_start": 1760140800,
    }
    assert kernel["claim_state"] == "claimed" and kernel["claim_published"] is True
    assert kernel["chain_problem"] is False
    # The storage report is projected to its endurance numbers: the block
    # device path and the raw counters stay out.
    assert kernel["storage_status"] == {"status": "good", "free_pct": 71.0, "wear_pct": 3}


def test_canary_status_is_projected_not_copied() -> None:
    hass = HomeAssistant()
    diag = _diag(hass, _entry_with_kernel(hass))
    assert diag["canary_devices"] == {
        "canary01": {
            "online": True,
            "firmware": "2.4.16",
            "hardware": "esp32s3",
            "device_type": "canary-wap",
            "last_event_type": None,
        }
    }
    assert diag["canary_device_count"] == 1
    assert diag["mqtt"] == {"enabled": True, "prefix": "securacv", "subscription_count": 2}
    assert diag["evidence_exports"] == {"count": 0}
    assert diag["platforms_loaded"] == ["sensor", "binary_sensor", "event", "button"]
    assert diag["chain_verification"]["ok"] is None
    assert diag["chain_verification"]["covered"] == 0


def test_an_entry_without_a_runtime_still_produces_a_dump() -> None:
    """An entry that failed to load has no runtime; diagnostics are the one
    place that must still answer."""
    hass = HomeAssistant()
    entry = make_entry(data={CONF_SETUP_MODE: "mqtt", CONF_ENABLE_MQTT: True})
    diag = _diag(hass, entry)
    assert diag["kernel"]["api"] == {"configured": False}
    assert diag["kernel"]["available"] is None
    assert diag["canary_devices"] == {}


def test_the_redaction_set_names_every_identifier_class() -> None:
    for key in (
        "token",
        "token_file",
        "kernel_token_id",
        "pairing_code",
        "host",
        "url",
        "device_id",
        "kernel_device_id",
        "verifying_key",
        "kernel_verifying_key",
        "public_key",
        "latest_hash",
        "entry_id",
        "unique_id",
        "claim_nonce",
        "ip",
        "ap_ip",
        "mac",
        "bssid",
        "ssid",
        "hostname",
        "device_name",
        "latitude",
        "longitude",
    ):
        assert key in TO_REDACT, key
