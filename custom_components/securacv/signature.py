"""Ed25519 signature verification for SecuraCV MQTT payloads.

The Canary firmware signs every chain / events / counts publish with
its on-device Ed25519 privkey over a canonical message (see
`firmware/projects/canary-wap/arduino/canary_wap/device_signature.cpp`
for the locked format). This module rebuilds the same canonical string
from the parsed JSON fields and verifies the b64url-encoded sig
against the device's pinned pubkey.

Why we hand-roll the canonical string instead of signing the JSON
directly: JSON canonicalization is fragile across snprintf and Python.
A field-separated canonical message is trivially deterministic on
both sides, and bumping `SCHEMA_V` is a clean evolution path if we
ever need to add fields.

Failure paths return a `TrustVerdict` (defined in device_trust.py)
so the caller has one type to branch on. We deliberately don't raise
on bad signatures — verification failure is normal-but-loud, not an
exception.
"""

from __future__ import annotations

import base64
import binascii
import logging
from typing import Any, Optional

# `cryptography` is a Home Assistant core dependency, pinned by HA itself, so
# it is deliberately NOT listed in manifest.json's `requirements`: an
# integration-level pin would fight HA's own on every core upgrade, and it is
# importable in every HA install this integration can load into. If HA ever
# drops it from core, this import is the loud failure that says so.
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric import ed25519

from .device_trust import TrustStore, TrustVerdict

_LOGGER = logging.getLogger(__name__)

SIG_PREFIX = "securacv-canary-sig"
SCHEMA_V = 1
ALG_NAME = "ed25519"


def _b64url_decode_nopad(data: str) -> bytes:
    """Inverse of firmware's `b64url_encode_nopad`. Re-adds the
    padding Python's `urlsafe_b64decode` requires before delegating."""
    pad = "=" * (-len(data) % 4)
    return base64.urlsafe_b64decode(data + pad)


def build_chain_canonical(device_id: str, length: int, latest_hash_hex: str) -> bytes:
    return f"{SIG_PREFIX}|v{SCHEMA_V}|chain|{device_id}|{length}|{latest_hash_hex}".encode(
        "utf-8"
    )


def build_event_canonical(
    device_id: str,
    event_id: int,
    state: str,
    category: str,
    privacy: str,
    motion: int,
    breath: int,
    bpm: int,
) -> bytes:
    return (
        f"{SIG_PREFIX}|v{SCHEMA_V}|event|{device_id}|{event_id}|{state}|"
        f"{category}|{privacy}|{motion}|{breath}|{bpm}"
    ).encode("utf-8")


def build_counts_canonical(device_id: str, total: int) -> bytes:
    return f"{SIG_PREFIX}|v{SCHEMA_V}|counts|{device_id}|{total}".encode("utf-8")


def build_sense_event_canonical(
    device_id: str,
    seq: int,
    event: str,
    presence: str,
    occupants: str,
    range_band: str,
    bucket_uptime_s: int,
) -> bytes:
    """canary-sense radar-witness event canonical (v1 `sense` kind).

    Locked against `firmware/common/identity/device_signature.cpp`
    (build_sense_canonical). Carries only the chokepoint's coarse
    vocabulary — event name, presence state, 0/1/2+ occupant bucket,
    near/mid/far range band, 10-minute uptime bucket."""
    return (
        f"{SIG_PREFIX}|v{SCHEMA_V}|sense|{device_id}|{seq}|{event}|"
        f"{presence}|{occupants}|{range_band}|{bucket_uptime_s}"
    ).encode("utf-8")


def _verify_raw(
    pubkey_bytes: bytes, canonical: bytes, sig_b64url: str
) -> bool:
    """Return True iff `sig_b64url` is a valid Ed25519 sig over
    `canonical` produced by the privkey matching `pubkey_bytes`.

    Any malformed input (bad b64, bad sig length, wrong pubkey type)
    counts as a verify failure — we never want a structurally invalid
    payload to be treated as "trusted just because verify didn't crash"."""
    try:
        sig = _b64url_decode_nopad(sig_b64url)
    except (ValueError, binascii.Error):
        return False
    if len(sig) != 64:
        return False
    try:
        key = ed25519.Ed25519PublicKey.from_public_bytes(pubkey_bytes)
        key.verify(sig, canonical)
        return True
    except (InvalidSignature, ValueError):
        return False


def _extract_envelope(payload: dict[str, Any]) -> Optional[tuple[str, str, str]]:
    """Pull (sig, fp, alg) from the envelope, or None if any are missing.

    `v` is checked separately so we can distinguish "unsigned (v0/missing)"
    from "wrong-version signed" — the former is a graceful-degradation
    case (old firmware), the latter is a misconfiguration we want to log."""
    sig = payload.get("sig")
    fp = payload.get("fp")
    alg = payload.get("alg")
    if not sig or not fp or not alg:
        return None
    if not isinstance(sig, str) or not isinstance(fp, str) or not isinstance(alg, str):
        return None
    return sig, fp, alg


def _verify_with_kind(
    trust_store: TrustStore,
    device_id: str,
    payload: dict[str, Any],
    canonical: bytes,
) -> TrustVerdict:
    """Common path for the three publish kinds. Returns a TrustVerdict
    that the caller stamps onto entity attributes."""
    envelope = _extract_envelope(payload)
    if envelope is None:
        return TrustVerdict(
            trusted=False,
            reason="unsigned",
            detail="Payload missing sig/fp/alg fields",
        )
    sig_b64, fp, alg = envelope
    if alg != ALG_NAME:
        return TrustVerdict(
            trusted=False,
            reason="mismatch",
            received_fingerprint=fp,
            detail=f"Unknown alg '{alg}', expected '{ALG_NAME}'",
        )
    v = payload.get("v")
    if v is not None and v != SCHEMA_V:
        return TrustVerdict(
            trusted=False,
            reason="mismatch",
            received_fingerprint=fp,
            detail=f"Schema version {v} != expected {SCHEMA_V}",
        )

    pubkey_bytes = trust_store.get_pubkey_bytes(device_id)
    pinned = trust_store.get(device_id)

    if pubkey_bytes is None:
        # No pin yet. Caller (handle_*) will TOFU-pin via a separate
        # round-trip to /api/device/enroll, or wait for the next publish
        # — we don't synthesize a pubkey from the sig.
        return TrustVerdict(
            trusted=False,
            reason="no_pubkey",
            received_fingerprint=fp,
            detail="No pinned pubkey for this device_id",
        )

    if pinned is not None and pinned.fingerprint_hex != fp:
        # The signature MIGHT still be valid for the new key, but the
        # fingerprint pinned in our store doesn't match — that's the
        # mismatch case. We don't even attempt the verify because the
        # pubkey we'd verify against is the old one.
        return TrustVerdict(
            trusted=False,
            reason="mismatch",
            pinned_fingerprint=pinned.fingerprint_hex,
            received_fingerprint=fp,
            detail="Fingerprint changed without rotation",
        )

    if _verify_raw(pubkey_bytes, canonical, sig_b64):
        return TrustVerdict(
            trusted=True,
            reason="ok",
            pinned_fingerprint=fp,
            received_fingerprint=fp,
        )

    # Fingerprint matches but the sig doesn't verify. Either the privkey
    # has changed (unannounced rotation) or the payload was tampered
    # with in flight. We can't tell which from here.
    return TrustVerdict(
        trusted=False,
        reason="mismatch",
        pinned_fingerprint=fp,
        received_fingerprint=fp,
        detail="Signature failed to verify against pinned pubkey",
    )


def verify_chain(
    trust_store: TrustStore, device_id: str, payload: dict[str, Any]
) -> TrustVerdict:
    length = payload.get("length")
    latest_hash = payload.get("latest_hash")
    if length is None or latest_hash is None:
        return TrustVerdict(
            trusted=False, reason="unsigned", detail="Chain payload missing fields"
        )
    # Cast under guard — a malformed/tampered payload (e.g. length="abc") would
    # raise ValueError out of int() and escape the @callback, blocking the
    # entity from updating on legitimate later publishes. The whole point of
    # the verify path is to return a verdict, never to throw.
    try:
        length_int = int(length)
    except (TypeError, ValueError):
        return TrustVerdict(
            trusted=False, reason="unsigned",
            detail=f"Chain payload `length` is not an integer: {length!r}",
        )
    canonical = build_chain_canonical(device_id, length_int, str(latest_hash))
    return _verify_with_kind(trust_store, device_id, payload, canonical)


def verify_event(
    trust_store: TrustStore, device_id: str, payload: dict[str, Any]
) -> TrustVerdict:
    """Verify an event publish. Required fields on the canonical:
    event_id, state, category, privacy, motion, breathing, bpm. If any
    are missing we treat the payload as unsigned (graceful) rather than
    a mismatch."""
    required = ("event_id", "state", "category", "privacy", "motion", "breathing", "bpm")
    if not all(k in payload for k in required):
        return TrustVerdict(
            trusted=False,
            reason="unsigned",
            detail=f"Event payload missing required fields: {required}",
        )
    # Same guarding rationale as verify_chain — never raise out of the
    # verify path. Numeric fields that arrive as strings or other junk
    # are treated as unsigned, which keeps entity state updating while
    # surfacing the issue in the verdict's detail string.
    try:
        canonical = build_event_canonical(
            device_id=device_id,
            event_id=int(payload["event_id"]),
            state=str(payload["state"]),
            category=str(payload["category"]),
            privacy=str(payload["privacy"]),
            motion=int(payload["motion"]),
            breath=int(payload["breathing"]),
            bpm=int(payload["bpm"]),
        )
    except (TypeError, ValueError) as err:
        return TrustVerdict(
            trusted=False, reason="unsigned",
            detail=f"Event payload has non-numeric scalar field: {err}",
        )
    return _verify_with_kind(trust_store, device_id, payload, canonical)


def verify_sense_event(
    trust_store: TrustStore, device_id: str, payload: dict[str, Any]
) -> TrustVerdict:
    """Verify a canary-sense radar-witness event publish. Required fields
    on the canonical: seq, event, presence, occupants, range,
    bucket_uptime_s. Missing fields degrade to unsigned (graceful),
    same policy as the CSI event verifier."""
    required = ("seq", "event", "presence", "occupants", "range", "bucket_uptime_s")
    if not all(k in payload for k in required):
        return TrustVerdict(
            trusted=False,
            reason="unsigned",
            detail=f"Sense event payload missing required fields: {required}",
        )
    try:
        canonical = build_sense_event_canonical(
            device_id=device_id,
            seq=int(payload["seq"]),
            event=str(payload["event"]),
            presence=str(payload["presence"]),
            occupants=str(payload["occupants"]),
            range_band=str(payload["range"]),
            bucket_uptime_s=int(payload["bucket_uptime_s"]),
        )
    except (TypeError, ValueError) as err:
        return TrustVerdict(
            trusted=False, reason="unsigned",
            detail=f"Sense event payload has non-numeric scalar field: {err}",
        )
    return _verify_with_kind(trust_store, device_id, payload, canonical)


def verify_counts(
    trust_store: TrustStore, device_id: str, payload: dict[str, Any]
) -> TrustVerdict:
    total = payload.get("total")
    if total is None:
        return TrustVerdict(
            trusted=False, reason="unsigned", detail="Counts payload missing total"
        )
    try:
        total_int = int(total)
    except (TypeError, ValueError):
        return TrustVerdict(
            trusted=False, reason="unsigned",
            detail=f"Counts payload `total` is not an integer: {total!r}",
        )
    canonical = build_counts_canonical(device_id, total_int)
    return _verify_with_kind(trust_store, device_id, payload, canonical)
