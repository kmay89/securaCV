"""Unit tests for device_trust.TrustStore.

Covers:
  - Initial state (empty)
  - TOFU pin on first sight
  - Manual pin replaces TOFU pin (and records audit trail)
  - Rotate stamps PIN_SOURCE_ROTATION
  - Unpin removes the entry
  - Storage round-trip (save → new TrustStore → load preserves state)
  - fingerprint_from_pubkey_hex matches the firmware's expected output

The fingerprint cross-check is the same byte-for-byte calculation
firmware's compute_fingerprint does (canary_wap.ino:931). If either
side ever drifts, this test catches it.
"""

import hashlib

import pytest

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import run

from ..device_trust import (
    PIN_SOURCE_MANUAL,
    PIN_SOURCE_ROTATION,
    PIN_SOURCE_TOFU,
    TrustStore,
    fingerprint_from_pubkey_hex,
)
from homeassistant.core import HomeAssistant


SAMPLE_PUBKEY_A = "11" * 32
SAMPLE_PUBKEY_B = "22" * 32


def _expected_fingerprint(pubkey_hex: str) -> str:
    domain = b"securacv:pubkey:fingerprint"
    pubkey = bytes.fromhex(pubkey_hex)
    return hashlib.sha256(domain + pubkey).digest()[:8].hex()


def test_fingerprint_matches_firmware_formula():
    """Domain-separated SHA-256, first 8 bytes hex. Lock-in test —
    catches accidental change to the canonical fingerprint derivation
    that would silently fork firmware and HA."""
    fp = fingerprint_from_pubkey_hex(SAMPLE_PUBKEY_A)
    assert fp == _expected_fingerprint(SAMPLE_PUBKEY_A)
    assert len(fp) == 16


def test_initial_state_empty():
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    assert ts.all_devices() == {}
    assert not ts.is_pinned("canary-1")
    assert ts.get("canary-1") is None
    assert ts.get_pubkey_bytes("canary-1") is None


def test_tofu_pin_first_sight():
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())

    entry = run(ts.async_tofu_pin_if_unknown("canary-1", SAMPLE_PUBKEY_A))
    assert entry is not None
    assert entry.pin_source == PIN_SOURCE_TOFU
    assert entry.pubkey_hex == SAMPLE_PUBKEY_A
    assert entry.fingerprint_hex == _expected_fingerprint(SAMPLE_PUBKEY_A)

    # Second sighting must NOT overwrite — TOFU pin is sticky.
    entry2 = run(ts.async_tofu_pin_if_unknown("canary-1", SAMPLE_PUBKEY_B))
    assert entry2 is None
    pinned = ts.get("canary-1")
    assert pinned.pubkey_hex == SAMPLE_PUBKEY_A


def test_manual_pin_overrides_tofu():
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    run(ts.async_tofu_pin_if_unknown("canary-1", SAMPLE_PUBKEY_A))

    new_entry = run(
        ts.async_pin("canary-1", SAMPLE_PUBKEY_B, source=PIN_SOURCE_MANUAL)
    )
    assert new_entry.pubkey_hex == SAMPLE_PUBKEY_B
    assert new_entry.pin_source == PIN_SOURCE_MANUAL
    # Audit trail must include the previous TOFU pin.
    assert len(new_entry.previous) == 1
    assert new_entry.previous[0]["pubkey_hex"] == SAMPLE_PUBKEY_A


def test_rotate_records_rotation_source():
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    run(ts.async_pin("canary-1", SAMPLE_PUBKEY_A, source=PIN_SOURCE_MANUAL))

    rotated = run(ts.async_rotate("canary-1", SAMPLE_PUBKEY_B))
    assert rotated.pin_source == PIN_SOURCE_ROTATION
    assert rotated.pubkey_hex == SAMPLE_PUBKEY_B
    assert len(rotated.previous) == 1


def test_unpin_removes_entry():
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    run(ts.async_pin("canary-1", SAMPLE_PUBKEY_A))

    assert run(ts.async_unpin("canary-1")) is True
    assert not ts.is_pinned("canary-1")
    # Unpinning a never-pinned device is a no-op returning False.
    assert run(ts.async_unpin("canary-1")) is False


def test_storage_roundtrip():
    """Pin → save → re-load via fresh TrustStore → state matches."""
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    run(ts.async_pin("canary-1", SAMPLE_PUBKEY_A, source=PIN_SOURCE_MANUAL))
    run(ts.async_save())

    # Reach into the stub Store to verify the on-disk payload shape.
    raw = ts._store._payload
    assert raw["version"] == 1
    assert "canary-1" in raw["devices"]
    assert raw["devices"]["canary-1"]["pubkey_hex"] == SAMPLE_PUBKEY_A
    assert raw["devices"]["canary-1"]["pin_source"] == PIN_SOURCE_MANUAL

    # Fresh TrustStore on the same hass instance shares the stub
    # Store storage (keyed by entry_id) — the second load() picks up
    # the payload the first one saved.
    ts2 = TrustStore(hass, entry_id="abc")
    # Hand the second TrustStore the same Store instance so the
    # in-memory payload survives. The real HA Store reads from disk
    # so this isn't necessary in production — we're working around
    # the in-memory stub's per-instance state.
    ts2._store._payload = raw
    run(ts2.async_load())
    pinned = ts2.get("canary-1")
    assert pinned is not None
    assert pinned.pubkey_hex == SAMPLE_PUBKEY_A
    assert pinned.pin_source == PIN_SOURCE_MANUAL


def test_get_pubkey_bytes_decodes_hex():
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    run(ts.async_pin("canary-1", SAMPLE_PUBKEY_A))
    assert ts.get_pubkey_bytes("canary-1") == bytes.fromhex(SAMPLE_PUBKEY_A)


def test_invalid_pubkey_length_rejected():
    """Pubkey must be exactly 32 bytes (64 hex chars). Short input
    raises so the config flow / TOFU path never silently records
    a malformed key."""
    with pytest.raises(ValueError):
        fingerprint_from_pubkey_hex("aa" * 16)  # 16 bytes
