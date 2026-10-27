"""Diagnostics — what a publicly shared dump actually contains.

Two layers are held here. Nothing raw is copied in: a Canary's status
payload (IP, AP address, the name the owner gave it) is projected to
firmware / hardware / online, the latest witness event to type / bucket /
attestation (never the zone label), the digest to counts, the kernel URL
to scheme and port. Then ``async_redact_data`` sweeps the result with
``TO_REDACT``, so a secret that reached the dump through a path this file
did not foresee is still replaced. A Canary's key fingerprint keeps none of
its digits (device-details finding P19, ledger F392): a per-dump salted
correlation id stands in for it. The old regression (a fingerprint
"truncated" to its own full length) is a special case of that rule.
"""

from __future__ import annotations

import json
import re
import types

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import device_registry, make_entry, make_runtime, run

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
from .. import _async_device_status_received, _async_record_health_firmware  # noqa: E402
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


def _publish(hass: HomeAssistant, entry, device_id: str, topic: str, body) -> None:
    """A Canary's publish through the integration's own handler: the status
    handler for status, the firmware half of the health subscription for
    health (the trust store here is a stand-in, so the pin is not driven)."""
    message = types.SimpleNamespace(
        topic=f"securacv/{device_id}/{topic}",
        payload=body if isinstance(body, str) else json.dumps(body),
    )
    if topic == "status":
        _async_device_status_received(hass, entry)(message)
    else:
        _async_record_health_firmware(hass, entry, message)


def _entry_with_kernel(
    hass: HomeAssistant,
    fp: str | None = FULL_FP,
    devices: dict | None = None,
    fingerprints: dict[str, str] | None = None,
):
    """An entry with a kernel and, unless `devices` says otherwise, one
    Canary ("canary01") whose record its STATUS publish wrote."""
    def _trust(device_id: str):
        value = (fingerprints or {}).get(device_id, fp)
        return types.SimpleNamespace(fingerprint_hex=value, pin_source="tofu")

    trust_store = types.SimpleNamespace(get=_trust)
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
        devices=devices if devices is not None else {},
        mqtt_prefix="securacv",
        unsub_mqtt=[lambda: None, lambda: None],
    )
    runtime.claim_published = True
    entry = make_entry(
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
    if devices is None:
        _publish(hass, entry, "canary01", "status", STATUS)
    return entry


def _diag(hass, entry):
    hass.data.setdefault(DOMAIN, {})
    return run(async_get_config_entry_diagnostics(hass, entry))


# A correlation id: two groups of four letters, none of them a hex digit.
CORRELATION_ID = re.compile(r"[g-hj-km-np-tv-z]{4}-[g-hj-km-np-tv-z]{4}")


def _windows(value: str, width: int = 4) -> set[str]:
    """Every run of `width` characters of `value`, lowercased."""
    value = value.lower()
    return {value[i : i + width] for i in range(len(value) - width + 1)}


def test_no_digit_of_a_canary_fingerprint_reaches_the_dump() -> None:
    """P19 / F392: no digits, a per-dump salted correlation id instead.

    Four hex digits already pick one Canary out of the few in radio range
    (a canary-wap's Bluetooth name and the fleet beacon carry the last
    four), so no run of four of the
    fingerprint's characters may appear anywhere in the dump, in either
    case, and the field is no longer called a fingerprint.
    """
    hass = HomeAssistant()
    diag = _diag(hass, _entry_with_kernel(hass))
    row = diag["trust"]["canary01"]
    assert "fingerprint" not in row
    assert row["pinned"] is True
    assert CORRELATION_ID.fullmatch(row["key_correlation_id"]), row["key_correlation_id"]
    text = repr(diag).lower()
    for window in _windows(FULL_FP):
        assert window not in text, window


def test_the_correlation_id_correlates_within_one_dump_only() -> None:
    """Same key, same id in one dump (case-folded, as the store compares);
    another key, another id; the next dump of the same state, other ids."""
    shared = "aabbccdd11223344"
    other = "99887766deadbeef"
    devices = {name: {"status": STATUS, "online": True} for name in ("canaryA1", "canaryB2", "canaryC3")}
    fingerprints = {"canaryA1": shared, "canaryB2": shared.upper(), "canaryC3": other}

    hass = HomeAssistant()
    entry = _entry_with_kernel(hass, devices=devices, fingerprints=fingerprints)
    first = _diag(hass, entry)["trust"]
    second = _diag(hass, entry)["trust"]

    assert first["canaryA1"]["key_correlation_id"] == first["canaryB2"]["key_correlation_id"]
    assert first["canaryA1"]["key_correlation_id"] != first["canaryC3"]["key_correlation_id"]
    # A fresh salt per dump: nothing links one posted dump to another, and
    # nobody holding a fingerprint can compute its id.
    assert first["canaryA1"]["key_correlation_id"] != second["canaryA1"]["key_correlation_id"]
    assert first["canaryC3"]["key_correlation_id"] != second["canaryC3"]["key_correlation_id"]
    text = repr(first).lower() + repr(second).lower()
    for fingerprint in (shared, other):
        for window in _windows(fingerprint):
            assert window not in text, window


def test_every_canary_keeps_its_own_row() -> None:
    """Rows are keyed by eight characters of the Canary id, and many ids share
    them: every canary-wap unit is `canary-s3-…`, and the Sense's id
    (`canary_sense_001`, every Sense config) and the Sentinel's
    (`canary_sentinel_001`) are both `canary_s…`. Each Canary keeps its own
    row in every section, with a per-dump ordinal and no ninth character of
    its id, so two units pinned to one key are seen to share a correlation
    id. The WAP ids are ones canary_wap.ino's recipe can print (the fourth
    character of the suffix is always 2), which
    scripts/tests/test_wap_name_examples.py holds every source line to."""
    hass = HomeAssistant()
    shared, sense_key, sentinel_key = "aabbccdd11223344", "99887766deadbeef", "5566778899aabbcc"
    fingerprints = {
        "canary-s3-aB72": shared,
        "canary-s3-xQ32": shared,
        "canary_sense_001": sense_key,
        "canary_sentinel_001": sentinel_key,
    }
    entry = _entry_with_kernel(hass, devices={}, fingerprints=fingerprints)
    for device_id in ("canary-s3-aB72", "canary-s3-xQ32", "canary_sense_001", "canary_sentinel_001", "canary01"):
        _publish(hass, entry, device_id, "status", STATUS)
    entry.runtime_data.verify["canary-s3-xQ32"] = {"trusted": True, "reason": "ok"}
    entry.runtime_data.verify["canary_sentinel_001"] = {"trusted": False, "reason": "unsigned"}

    diag = _diag(hass, entry)
    assert diag["canary_device_count"] == 5
    assert set(diag["canary_devices"]) == {
        "canary-s… #1", "canary-s… #2", "canary_s… #1", "canary_s… #2", "canary01",
    }
    assert set(diag["trust"]) == set(diag["canary_devices"])
    assert diag["verification"] == {
        "canary-s… #2": {"trusted": True, "reason": "ok"},
        "canary_s… #2": {"trusted": False, "reason": "unsigned"},
    }
    wap1, wap2, sense, sentinel = (
        diag["trust"][row]["key_correlation_id"]
        for row in ("canary-s… #1", "canary-s… #2", "canary_s… #1", "canary_s… #2")
    )
    assert wap1 == wap2
    assert len({wap1, sense, sentinel}) == 3
    text = repr(diag)
    for leaked in ("canary-s3", "canary_se", "aB72", "xQ32", "_001"):
        assert leaked not in text, leaked


def test_a_canary_only_verification_has_seen_has_a_row() -> None:
    """A retained chain or counts publish can be verified before the
    Canary's status arrives, so verification can name an id no device record
    has. It gets a row of its own, numbered after the device records when it
    shares their eight characters, and the dump does not fail on it."""
    hass = HomeAssistant()
    entry = _entry_with_kernel(hass, devices={})
    _publish(hass, entry, "canary-s3-aB72", "status", STATUS)
    _publish(hass, entry, "canary-s3-xQ32", "status", STATUS)
    entry.runtime_data.verify["chainonly01"] = {"trusted": True, "reason": "ok"}
    entry.runtime_data.verify["canary-s3-Kx22"] = {"trusted": False, "reason": "bad_signature"}

    diag = _diag(hass, entry)
    assert diag["verification"] == {
        "chainonl…": {"trusted": True, "reason": "ok"},
        "canary-s… #3": {"trusted": False, "reason": "bad_signature"},
    }
    assert set(diag["canary_devices"]) == {"canary-s… #1", "canary-s… #2"}
    assert "Kx22" not in repr(diag)


def test_a_canary_heard_only_on_health_has_a_row() -> None:
    """A health that reports a version makes the Canary's device page before
    any status does, so the dump gives it a row too: the page's version, and
    its trust row, numbered after the status records when it shares their
    eight characters. A health with no version makes no page and no row."""
    hass = HomeAssistant()
    fingerprints = {"canary-s3-aB72": "aabbccdd11223344", "canary-s3-xQ32": "99887766deadbeef"}
    entry = _entry_with_kernel(hass, devices={}, fingerprints=fingerprints)
    _publish(hass, entry, "canary-s3-aB72", "status", STATUS)
    _publish(hass, entry, "canary-s3-xQ32", "health", {"firmware_version": "2.4.16"})
    _publish(hass, entry, "health02", "health", {"free_heap": 120000})

    diag = _diag(hass, entry)
    registry = device_registry(hass)
    page = registry.async_get_device(identifiers={(DOMAIN, "canary_canary-s3-xQ32")})
    assert page is not None and page.sw_version == "2.4.16"
    assert registry.async_get_device(identifiers={(DOMAIN, "canary_health02")}) is None
    assert diag["canary_device_count"] == 2
    assert diag["canary_devices"]["canary-s… #2"] == {
        "online": None,
        "firmware": "2.4.16",
        "hardware": None,
        "device_type": None,
        "last_event_type": None,
    }
    assert diag["trust"]["canary-s… #2"]["pinned"] is True
    assert CORRELATION_ID.fullmatch(diag["trust"]["canary-s… #2"]["key_correlation_id"])
    assert set(diag["trust"]) == set(diag["canary_devices"])
    text = repr(diag)
    assert "xQ32" not in text and "health02" not in text
    for window in _windows(fingerprints["canary-s3-xQ32"]):
        assert window not in text.lower(), window


def test_the_dump_shows_what_the_device_page_shows() -> None:
    """The per-Canary firmware is the device page's: the health topic's
    version over the status's, kept through a last will that carries none.
    Board and type are bounded broker text, and a non-string is no value."""
    hass = HomeAssistant()
    entry = _entry_with_kernel(hass, devices={})
    status = {"online": True, "firmware_version": "2.4.15", "hardware": "h" * 300,
              "device_type": "Canary_Sense" + "x" * 300}
    _publish(hass, entry, "sense01", "status", status)
    _publish(hass, entry, "sense01", "health", {"firmware_version": "2.4.16"})
    _publish(hass, entry, "wap01", "status", {"online": True, "fw_version": "2.4.15", "device_type": "canary-wap"})
    _publish(hass, entry, "wap01", "status", {"online": False})
    _publish(hass, entry, "odd01", "status", {"online": True, "hardware": {"board": "x"}, "device_type": 7})

    rows = _diag(hass, entry)["canary_devices"]
    page = {d: device_registry(hass).async_get_device(identifiers={(DOMAIN, f"canary_{d}")})
            for d in ("sense01", "wap01", "odd01")}
    assert rows["sense01"]["firmware"] == page["sense01"].sw_version == "2.4.16"
    assert rows["sense01"]["hardware"] == page["sense01"].hw_version == "h" * 64
    assert rows["sense01"]["device_type"] == "canary-sense" + "x" * 52
    assert rows["wap01"]["firmware"] == page["wap01"].sw_version == "2.4.15"
    assert rows["wap01"]["online"] is False
    assert rows["wap01"]["device_type"] == "canary-wap"
    assert rows["odd01"]["firmware"] is None and rows["odd01"]["hardware"] is None
    assert rows["odd01"]["device_type"] is None


def test_diagnostics_survive_an_unpinned_device() -> None:
    hass = HomeAssistant()
    diag = _diag(hass, _entry_with_kernel(hass, fp=""))
    assert diag["trust"]["canary01"]["key_correlation_id"] is None
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
        # The identifier keys a Canary's answers use (P18, ledger F391).
        "hw_token",
        "mdns_host",
        "serial_number",
        "chip_id",
        "module_id",
        "local_address",
        "address",
        "peer_address",
    ):
        assert key in TO_REDACT, key


def test_an_identifier_key_is_replaced_wherever_it_lands() -> None:
    """The second net: a value that reaches the dump under one of the
    identifier keys, at any depth, comes out redacted."""
    from homeassistant.components.diagnostics import async_redact_data

    seeded = {
        "hw_token": "b7a1c2d3e4f5a6b7",
        "mdns_host": "canary-kitchen.local",
        "serial_number": "7FA319C0AABBCCDD",
        "chip_id": "0x00a1b2c3d4e5",
        "module_id": "1073741824",
        "bluetooth": {
            "local_address": "34:85:18:aa:bb:cd",
            "connection": {"address": "5c:aa:bb:cc:dd:ee"},
            "paired": [{"peer_address": "7e:11:22:33:44:55"}],
        },
    }
    text = repr(async_redact_data(seeded, TO_REDACT))
    for value in (
        "b7a1c2d3e4f5a6b7",
        "canary-kitchen.local",
        "7FA319C0AABBCCDD",
        "0x00a1b2c3d4e5",
        "1073741824",
        "34:85:18:aa:bb:cd",
        "5c:aa:bb:cc:dd:ee",
        "7e:11:22:33:44:55",
    ):
        assert value not in text, value
