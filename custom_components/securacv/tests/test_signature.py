"""Unit tests for signature.verify_*.

Round-trips through Python's `cryptography` Ed25519 implementation:
  - Build a canonical string identical to what firmware emits.
  - Sign with a known privkey.
  - Verify via signature.verify_chain / verify_event / verify_counts.

The canonical-string fixture vectors below are byte-identical to the
ones the firmware host test (test_device_signature.cpp) asserts on.
If firmware drifts, this test will fail to produce a matching sig
because the canonical bytes won't line up.
"""

import base64

from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
)

from . import conftest  # noqa: F401
from .conftest import run

from ..device_trust import TrustStore
from ..signature import (
    build_chain_canonical,
    build_counts_canonical,
    build_event_canonical,
    build_sense_event_canonical,
    verify_chain,
    verify_counts,
    verify_event,
    verify_sense_event,
)
from homeassistant.core import HomeAssistant


def _make_keypair():
    """Deterministic Ed25519 keypair from a zero seed — same shape
    HA's `cryptography` and firmware's Arduino Ed25519 lib produce,
    even though the seed differs in production."""
    priv = Ed25519PrivateKey.from_private_bytes(b"\x42" * 32)
    pub = priv.public_key()
    pub_bytes = pub.public_bytes_raw()
    return priv, pub_bytes


def _b64url_nopad(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode("ascii")


def _pin(ts: TrustStore, device_id: str, pub_bytes: bytes) -> None:
    run(ts.async_pin(device_id, pub_bytes.hex(), source="manual"))


# ─── canonical bytes match firmware reference ─────────────────────────

def test_chain_canonical_matches_firmware_reference():
    """Same vector test_device_signature.cpp asserts on — locks the
    two implementations together."""
    hash_bytes = bytes(range(32))
    canonical = build_chain_canonical("abc123", 42, hash_bytes.hex())
    assert canonical == (
        b"securacv-canary-sig|v1|chain|abc123|42|"
        b"000102030405060708090a0b0c0d0e0f"
        b"101112131415161718191a1b1c1d1e1f"
    )


def test_event_canonical_matches_firmware_reference():
    canonical = build_event_canonical(
        "abc123", 7, "active", "event", "p1", 180, 120, 15
    )
    assert canonical == (
        b"securacv-canary-sig|v1|event|abc123|7|active|event|p1|180|120|15"
    )


def test_counts_canonical_matches_firmware_reference():
    canonical = build_counts_canonical("abc123", 12345)
    assert canonical == b"securacv-canary-sig|v1|counts|abc123|12345"


# ─── verify_chain ─────────────────────────────────────────────────────

def test_sense_canonical_matches_firmware_reference():
    """Byte-identical to the firmware host test vector
    (firmware/tests_host/test_device_signature_common.cpp)."""
    got = build_sense_event_canonical(
        "sense01", 3, "presence_detected", "present", "1", "near", 1200
    )
    assert got == (
        b"securacv-canary-sig|v1|sense|sense01|3|"
        b"presence_detected|present|1|near|1200"
    )


def test_verify_sense_event_happy_path():
    priv, pub = _make_keypair()
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    _pin(ts, "sense01", pub)

    canonical = build_sense_event_canonical(
        "sense01", 3, "presence_detected", "present", "1", "near", 1200
    )
    sig = _b64url_nopad(priv.sign(canonical))
    payload = {
        "v": 1,
        "event": "presence_detected",
        "seq": 3,
        "bucket_uptime_s": 1200,
        "presence": "present",
        "occupants": "1",
        "range": "near",
        "signed": True,
        "alg": "ed25519",
        "fp": ts.get("sense01").fingerprint_hex,
        "sig": sig,
    }
    verdict = verify_sense_event(ts, "sense01", payload)
    assert verdict.trusted is True


def test_verify_sense_event_vision_shaped_payload():
    """canary-vision signs the SAME locked sense canonical (its optical
    presence/occupants fit; range is honestly 'unknown'), so its events
    verify through verify_sense_event with zero HA-side changes. The
    extra vision-only fields (device_type, reason, ts_ms, bbox…) are
    outside the canonical and must not disturb verification."""
    priv, pub = _make_keypair()
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    _pin(ts, "vision01", pub)

    canonical = build_sense_event_canonical(
        "vision01", 12, "dwell_started", "present", "1", "unknown", 600
    )
    sig = _b64url_nopad(priv.sign(canonical))
    payload = {
        "device_id": "vision01",
        "device_type": "vision",
        "event": "dwell_started",
        "reason": "dwell",
        "seq": 12,
        "bucket_uptime_s": 600,
        "presence": "present",
        "occupants": "1",
        "range": "unknown",
        "signed": True,
        "ts_ms": 654321,
        "presence_ms": 12000,
        "dwell_ms": 5000,
        "confidence": 87,
        "v": 1,
        "alg": "ed25519",
        "fp": ts.get("vision01").fingerprint_hex,
        "sig": sig,
    }
    verdict = verify_sense_event(ts, "vision01", payload)
    assert verdict.trusted is True
    assert verdict.reason == "ok"


def test_verify_sense_event_missing_required_field():
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    payload = {
        "event": "presence_detected",
        "seq": 3,
        # presence, occupants, range, bucket_uptime_s missing
    }
    verdict = verify_sense_event(ts, "sense01", payload)
    assert verdict.trusted is False
    assert verdict.reason == "unsigned"


def test_verify_sense_event_tampered_payload():
    """Flipping the occupant bucket after signing must fail verification —
    the canonical no longer matches the signed bytes."""
    priv, pub = _make_keypair()
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    _pin(ts, "sense01", pub)

    canonical = build_sense_event_canonical(
        "sense01", 3, "presence_detected", "present", "1", "near", 1200
    )
    sig = _b64url_nopad(priv.sign(canonical))
    payload = {
        "v": 1,
        "event": "presence_detected",
        "seq": 3,
        "bucket_uptime_s": 1200,
        "presence": "present",
        "occupants": "2+",  # tampered after signing
        "range": "near",
        "alg": "ed25519",
        "fp": ts.get("sense01").fingerprint_hex,
        "sig": sig,
    }
    verdict = verify_sense_event(ts, "sense01", payload)
    assert verdict.trusted is False
    assert verdict.reason == "mismatch"


def test_verify_chain_happy_path():
    priv, pub = _make_keypair()
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    _pin(ts, "canary-1", pub)

    canonical = build_chain_canonical("canary-1", 42, "ff" * 32)
    sig = _b64url_nopad(priv.sign(canonical))
    payload = {
        "v": 1,
        "length": 42,
        "latest_hash": "ff" * 32,
        "alg": "ed25519",
        "fp": ts.get("canary-1").fingerprint_hex,
        "sig": sig,
    }
    verdict = verify_chain(ts, "canary-1", payload)
    assert verdict.trusted is True
    assert verdict.reason == "ok"


def test_verify_chain_unsigned_payload():
    """Old firmware (no sig fields) → reason='unsigned', trusted=False."""
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    payload = {"length": 1, "latest_hash": "00" * 32}
    verdict = verify_chain(ts, "canary-1", payload)
    assert verdict.trusted is False
    assert verdict.reason == "unsigned"


def test_verify_chain_no_pin_yet():
    """Device hasn't been pinned and the payload IS signed → caller
    should treat as 'no_pubkey' (TOFU path runs separately)."""
    priv, pub = _make_keypair()
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())

    canonical = build_chain_canonical("canary-1", 1, "00" * 32)
    sig = _b64url_nopad(priv.sign(canonical))
    payload = {
        "v": 1,
        "length": 1,
        "latest_hash": "00" * 32,
        "alg": "ed25519",
        "fp": "0011223344556677",
        "sig": sig,
    }
    verdict = verify_chain(ts, "canary-1", payload)
    assert verdict.trusted is False
    assert verdict.reason == "no_pubkey"


def test_verify_chain_fingerprint_mismatch():
    """Two different keys produce two different fingerprints. Pinning
    one and verifying with the other surfaces reason='mismatch' so
    the caller fires a persistent_notification."""
    priv_a, pub_a = _make_keypair()
    other = Ed25519PrivateKey.from_private_bytes(b"\x99" * 32)
    pub_b = other.public_key().public_bytes_raw()

    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    _pin(ts, "canary-1", pub_a)

    canonical = build_chain_canonical("canary-1", 5, "ab" * 32)
    sig = _b64url_nopad(other.sign(canonical))
    from ..device_trust import fingerprint_from_pubkey_hex

    payload = {
        "v": 1,
        "length": 5,
        "latest_hash": "ab" * 32,
        "alg": "ed25519",
        "fp": fingerprint_from_pubkey_hex(pub_b.hex()),
        "sig": sig,
    }
    verdict = verify_chain(ts, "canary-1", payload)
    assert verdict.trusted is False
    assert verdict.reason == "mismatch"


def test_verify_chain_tampered_payload():
    """Pubkey + fingerprint match the pin, but the payload was edited
    in flight — the sig doesn't verify. reason='mismatch'."""
    priv, pub = _make_keypair()
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    _pin(ts, "canary-1", pub)

    canonical_real = build_chain_canonical("canary-1", 7, "dd" * 32)
    sig = _b64url_nopad(priv.sign(canonical_real))
    payload = {
        "v": 1,
        "length": 8,  # ← tampered: real signed length was 7
        "latest_hash": "dd" * 32,
        "alg": "ed25519",
        "fp": ts.get("canary-1").fingerprint_hex,
        "sig": sig,
    }
    verdict = verify_chain(ts, "canary-1", payload)
    assert verdict.trusted is False
    assert verdict.reason == "mismatch"


# ─── verify_event ─────────────────────────────────────────────────────

def test_verify_event_happy_path():
    priv, pub = _make_keypair()
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    _pin(ts, "canary-1", pub)

    canonical = build_event_canonical(
        "canary-1", 7, "active", "event", "p1", 180, 120, 15
    )
    sig = _b64url_nopad(priv.sign(canonical))
    payload = {
        "v": 1,
        "event_id": 7,
        "state": "active",
        "category": "event",
        "privacy": "p1",
        "motion": 180,
        "breathing": 120,
        "bpm": 15,
        "alg": "ed25519",
        "fp": ts.get("canary-1").fingerprint_hex,
        "sig": sig,
    }
    verdict = verify_event(ts, "canary-1", payload)
    assert verdict.trusted is True


def test_verify_event_missing_required_field():
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    payload = {
        "event_id": 7,
        "state": "active",
        # category, privacy, motion, breathing, bpm missing
    }
    verdict = verify_event(ts, "canary-1", payload)
    assert verdict.trusted is False
    assert verdict.reason == "unsigned"


def test_verify_event_non_numeric_scalar_returns_unsigned():
    """A malformed payload — `motion='oops'` — must NOT raise out of
    the verify path. Pre-Codex-#447-review the unguarded int() would
    let ValueError escape the MQTT @callback and the message wouldn't
    update entity state. Now it returns reason='unsigned' so the
    entity continues updating, just marked unverified."""
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    payload = {
        "v": 1,
        "event_id": 7, "state": "active", "category": "event", "privacy": "p1",
        "motion": "oops",  # ← non-numeric, would have raised
        "breathing": 0, "bpm": 0,
        "alg": "ed25519", "fp": "0" * 16, "sig": "x" * 86,
    }
    verdict = verify_event(ts, "canary-1", payload)
    assert verdict.trusted is False
    assert verdict.reason == "unsigned"


def test_verify_chain_non_numeric_length_returns_unsigned():
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    verdict = verify_chain(
        ts, "canary-1",
        {"v": 1, "length": "abc", "latest_hash": "ff" * 32,
         "alg": "ed25519", "fp": "0" * 16, "sig": "x" * 86},
    )
    assert verdict.trusted is False
    assert verdict.reason == "unsigned"


# ─── verify_counts ────────────────────────────────────────────────────

def test_verify_counts_happy_path():
    priv, pub = _make_keypair()
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    _pin(ts, "canary-1", pub)

    canonical = build_counts_canonical("canary-1", 99)
    sig = _b64url_nopad(priv.sign(canonical))
    payload = {
        "v": 1,
        "total": 99,
        "alg": "ed25519",
        "fp": ts.get("canary-1").fingerprint_hex,
        "sig": sig,
    }
    verdict = verify_counts(ts, "canary-1", payload)
    assert verdict.trusted is True


def test_verify_counts_wrong_schema_version():
    """A `v=2` payload with everything else valid must still fail —
    forward-compat handling is to fall through to mismatch so
    deployments can't be silently upgraded by a hostile peer."""
    priv, pub = _make_keypair()
    hass = HomeAssistant()
    ts = TrustStore(hass, entry_id="abc")
    run(ts.async_load())
    _pin(ts, "canary-1", pub)

    canonical = build_counts_canonical("canary-1", 99)
    sig = _b64url_nopad(priv.sign(canonical))
    payload = {
        "v": 2,  # ← future version we don't know how to verify
        "total": 99,
        "alg": "ed25519",
        "fp": ts.get("canary-1").fingerprint_hex,
        "sig": sig,
    }
    verdict = verify_counts(ts, "canary-1", payload)
    assert verdict.trusted is False
    assert verdict.reason == "mismatch"
