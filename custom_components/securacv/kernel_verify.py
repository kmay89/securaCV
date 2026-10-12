"""The integration's OWN chain walk: what earns the word "Verified" (D10).

"Verified" means an Ed25519 signature checked against a pinned key — nothing
looser (AGENTS.md rule 4). The kernel's ``POST /verify`` report is the kernel
checking itself with its own key; a replaced kernel that keeps the old
public key in ``/api/identity`` and answers ``ok: true`` would pass that
check. So the integration walks the sealed log itself: it fetches
``GET /api/sealed-log?from_seq=<n>`` (the entries exactly as stored, each
with its ``seq``), recomputes every ``entry_hash = SHA256(prev_hash ||
payload)``, checks every ``prev_hash`` link, and verifies every signature
over ``SHA256(le32(len(domain)) || domain || entry_hash)`` with the key it
pinned at pairing. The math is a line-for-line port of
``viewer/verify_core.js`` (``hashEntry``, ``domainSeparatedHash``,
``verifyLinearChain``) and ``src/api/mod.rs``'s ``walk_with_kernel_hashing``;
``tests/fixtures/envelope/sealed_log_document_vector.json`` pins all three to
the same verdicts.

The walk is incremental: the newest verified row (``seq`` + ``entry_hash``)
is the cursor, persisted in a Store, and the next fetch asks for everything
after it. Continuity is proven by the first served entry's ``prev_hash``
equaling the cursor's hash (the standard transparency-log posture). When the
kernel has re-anchored the served tail past the cursor (a checkpoint or its
size cap) the entries still verify and the gap is recorded as the kernel's
continuity claim, never as this walk's proof. A chain that was rewritten at
the cursor, or a head that moved backwards, is a failed walk.

What the walk produces, beyond pass/fail, is the covered-entry index: the
``seq`` and the ``(event_type, zone_id, time_bucket_start)`` of every entry
it verified, bounded by the kernel's retention. An event row is "Verified ✓"
iff the walk verified ITS entry — by ``seq`` when the row carries one (C5),
by the key only under the rule ``event_verification`` states; otherwise it
is "Sealed ✓" (kernel-signed, not yet walked); a failed walk renders every
row "Failed ⚠"; a served key that differs from the pin is a key change —
Repairs ``tofu_mismatch`` and ``trust_reason: kernel_key_mismatch`` — never
"Failed".

Triggers: the Verify now button, the ``securacv.verify`` action, a 15-minute
backstop, 60 s after new entries go idle, and (a walk only, never a
verdict) the bridge's ``witness/chain_problem`` clearing. The crypto runs in
the executor.
"""
from __future__ import annotations

import asyncio
import hashlib
import json
import logging
import struct
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from typing import Any, Callable

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric import ed25519

from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.dispatcher import async_dispatcher_connect, async_dispatcher_send
from homeassistant.helpers.event import async_call_later, async_track_time_interval
from homeassistant.helpers.storage import Store

from .api import SecuraCVApi, SecuraCVApiError
from .const import (
    CONF_KERNEL_DEVICE_ID,
    DOMAIN,
    TIME_BUCKET_SECONDS,
    TRUST_REASON_HA_VERIFIED_CHAIN,
    TRUST_REASON_HA_VERIFY_FAILED,
    TRUST_REASON_KERNEL_KEY_MISMATCH,
    TRUST_REASON_KERNEL_SEALED,
    VERIFICATION_EVENT_CHAIN_FAILED,
    VERIFICATION_EVENT_CHAIN_VERIFIED,
    VERIFICATION_FAILED,
    VERIFICATION_SEALED,
    VERIFICATION_VERIFIED,
    VERIFY_BACKSTOP_INTERVAL_SECONDS,
    VERIFY_COVERED_DEFAULT_RETENTION_SECONDS,
    VERIFY_COVERED_MAX_ENTRIES,
    VERIFY_IDLE_DELAY_SECONDS,
    VERIFY_TRIGGER_BRIDGE,
    VERIFY_TRIGGER_BUTTON,
    VERIFY_TRIGGER_IDLE,
    VERIFY_TRIGGER_SCHEDULED,
    VERIFY_ERROR_KEY_CHANGED,
    VERIFY_TRIGGER_SERVICE,
    event_type_metadata,
    normalize_attestation,
    normalize_event_type,
)
from .coordinator import (
    DATA_DIGEST,
    SecuraCVKernelCoordinator,
    bucket_start,
    chain_problem_signal,
    event_bucket_start,
    verification_event_signal,
    wire_bucket_size,
    witness_event_signal,
)
from .data import SecuraCVConfigEntry, VerificationState, runtime_for
from .kernel_trust import normalize_kernel_key

_LOGGER = logging.getLogger(__name__)

# The sealed-log entry signing domain (src/crypto/signatures.rs
# DOMAIN_SEALED_LOG_ENTRY; viewer/verify_core.js V1_MANIFEST.signature_domains).
DOMAIN_SEALED_LOG_ENTRY = "securacv:pwk:sealed-log-entry:v2"
# The chain's genesis anchor: 32 zero bytes.
GENESIS_HASH = bytes(32)

# The persisted cursor.
STORAGE_VERSION = 1
STORAGE_SCHEMA_VERSION = 1

# Walk failure kinds — the same wire values the Rust verifier's VerifyFailure
# and verify_core.js's chainFail use, so a verdict reads the same everywhere.
FAIL_PREV_HASH = "prev_hash_mismatch"
FAIL_ENTRY_HASH = "entry_hash_mismatch"
FAIL_SIGNATURE = "signature_mismatch"
FAIL_MALFORMED = "malformed_entry"
FAIL_HEAD_REGRESSED = "head_regressed"
FAIL_HEAD_MISMATCH = "head_mismatch"
FAIL_KEY_MISMATCH = "key_mismatch"
FAIL_NO_KEY = "no_verifying_key"

# How the walk was anchored.
ANCHOR_GENESIS = "genesis"
ANCHOR_CHECKPOINT = "checkpoint"
ANCHOR_CONTINUITY = "continuity"
ANCHOR_REANCHORED = "reanchored"
ANCHOR_UNCHANGED = "unchanged"

MANUAL_TRIGGERS = (VERIFY_TRIGGER_BUTTON, VERIFY_TRIGGER_SERVICE)

CoveredKey = tuple[str, str, int]


# ── the math (viewer/verify_core.js, byte for byte) ─────────────────────


def le32(value: int) -> bytes:
    """Little-endian u32 — the domain-length prefix."""
    return struct.pack("<I", value)


def hash_entry(prev_hash: bytes, payload: str) -> bytes:
    """``SHA256(prev_hash || payload_json_utf8_bytes)`` (verify_core.js hashEntry)."""
    return hashlib.sha256(prev_hash + payload.encode("utf-8")).digest()


def domain_separated_hash(domain: str, entry_hash: bytes) -> bytes:
    """``SHA256(le32(len(domain)) || domain || entry_hash)``.

    verify_core.js domainSeparatedHash; src/crypto/signatures.rs
    domain_separated_hash.
    """
    domain_bytes = domain.encode("utf-8")
    return hashlib.sha256(le32(len(domain_bytes)) + domain_bytes + entry_hash).digest()


def verify_ed25519(public_key: bytes, signature: bytes, message: bytes) -> bool:
    """True iff `signature` is a valid Ed25519 signature by `public_key` over `message`."""
    try:
        ed25519.Ed25519PublicKey.from_public_bytes(public_key).verify(signature, message)
    except (InvalidSignature, ValueError):
        return False
    return True


# ── the served document ─────────────────────────────────────────────────


@dataclass(frozen=True)
class SealedEntry:
    """One sealed-log row as `GET /api/sealed-log` serves it, decoded."""

    seq: int | None
    payload: str
    prev_hash: bytes
    entry_hash: bytes
    signature: bytes


def _hex_bytes(value: Any, length: int) -> bytes | None:
    if not isinstance(value, str) or len(value) != 2 * length:
        return None
    try:
        return bytes.fromhex(value)
    except ValueError:
        return None


def entry_seq(raw: Any) -> int | None:
    """The row's sequence number: `seq` (the amended contract) else `id`."""
    if not isinstance(raw, dict):
        return None
    for key in ("seq", "id"):
        value = raw.get(key)
        if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
            return value
    return None


def parse_entry(raw: Any) -> SealedEntry | None:
    """Decode one served row; None when a field is not the shape the contract states."""
    if not isinstance(raw, dict):
        return None
    payload = raw.get("payload")
    if not isinstance(payload, str):
        return None
    prev_hash = _hex_bytes(raw.get("prev_hash"), 32)
    entry_hash = _hex_bytes(raw.get("entry_hash"), 32)
    signature = _hex_bytes(raw.get("signature"), 64)
    if prev_hash is None or entry_hash is None or signature is None:
        return None
    return SealedEntry(
        seq=entry_seq(raw),
        payload=payload,
        prev_hash=prev_hash,
        entry_hash=entry_hash,
        signature=signature,
    )


def covered_key_of_payload(payload: str) -> CoveredKey | None:
    """The (event_type, zone_id, bucket start) of a sealed event record.

    Heartbeats, failures and key rotations are not events and index nothing.
    The event type is folded to the dictionary's snake_case id, the same
    fold the event entities apply to the bridge's PascalCase.
    """
    try:
        data = json.loads(payload)
    except (json.JSONDecodeError, TypeError, ValueError):
        return None
    return covered_key_of_event(data)


def covered_key_of_event(event: Any) -> CoveredKey | None:
    """The covered-entry key of a C5 event payload or a sealed event record.

    The bucket is the canonical start (`coordinator.event_bucket_start`):
    a sealed-log record carries the true start and a wire row the kernel's
    export-jittered one, and the two must meet on one key.
    """
    if not isinstance(event, dict):
        return None
    record_type = event.get("record_type", "event")
    if record_type != "event":
        return None
    event_type = normalize_event_type(event.get("event_type"))
    zone_id = event.get("zone_id")
    bucket = event_bucket_start(event)
    if event_type is None or not isinstance(zone_id, str) or bucket is None:
        return None
    return (event_type, zone_id, bucket)


# ── the walk ────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class WalkFailure:
    """Where and why a walk stopped (the Rust VerifyFailure's wire values)."""

    kind: str
    index: int | None
    seq: int | None
    message: str


@dataclass
class WalkResult:
    """What one walk of a served document established."""

    ok: bool
    anchor: str
    verified: int = 0
    # The last verified row: the next cursor.
    last_seq: int | None = None
    last_hash: bytes | None = None
    # The document's declared head (`head.seq`), when it carries one.
    head_seq: int | None = None
    covered: list[CoveredKey] = field(default_factory=list)
    # The same entries by seq (entries served without one index by key only).
    covered_seqs: dict[int, CoveredKey] = field(default_factory=dict)
    failure: WalkFailure | None = None
    # Set when the served key is not the pinned key: a key change, not a
    # chain failure. `served_key` is what the kernel offered.
    served_key: str | None = None

    @property
    def key_mismatch(self) -> bool:
        """Whether the document was signed by a key other than the pin."""
        return self.failure is not None and self.failure.kind == FAIL_KEY_MISMATCH


@dataclass(frozen=True)
class Cursor:
    """The newest verified row, and how many rows the walks have verified."""

    seq: int
    entry_hash: bytes
    verified_entries: int


def walk_entries(
    entries: list[Any], verifying_key: bytes, expected_prev: bytes, anchor: str
) -> WalkResult:
    """verify_core.js verifyLinearChain, with signature checks on.

    Every entry in order: `prev_hash` must equal the running head, the
    recomputed hash must equal the stored `entry_hash`, and the Ed25519
    signature must verify over the domain-separated hash. The walk stops at
    the first entry that fails; what came before it stays verified.
    """
    result = WalkResult(ok=True, anchor=anchor)
    running = expected_prev
    for index, raw in enumerate(entries):
        entry = parse_entry(raw)
        if entry is None:
            result.ok = False
            result.failure = WalkFailure(
                FAIL_MALFORMED, index, entry_seq(raw), f"entry at index {index} is malformed"
            )
            return result
        if entry.prev_hash != running:
            result.ok = False
            result.failure = WalkFailure(
                FAIL_PREV_HASH,
                index,
                entry.seq,
                f"sealed_events integrity check failed at index {index}: prev_hash mismatch",
            )
            return result
        if hash_entry(running, entry.payload) != entry.entry_hash:
            result.ok = False
            result.failure = WalkFailure(
                FAIL_ENTRY_HASH,
                index,
                entry.seq,
                f"sealed_events integrity check failed at index {index}: entry_hash mismatch",
            )
            return result
        message = domain_separated_hash(DOMAIN_SEALED_LOG_ENTRY, entry.entry_hash)
        if not verify_ed25519(verifying_key, entry.signature, message):
            result.ok = False
            result.failure = WalkFailure(
                FAIL_SIGNATURE,
                index,
                entry.seq,
                f"sealed_events signature verification failed at index {index}",
            )
            return result
        running = entry.entry_hash
        result.verified += 1
        result.last_seq = entry.seq
        result.last_hash = entry.entry_hash
        key = covered_key_of_payload(entry.payload)
        if key is not None:
            result.covered.append(key)
            if entry.seq is not None:
                result.covered_seqs[entry.seq] = key
    return result


def _head_of(doc: dict[str, Any]) -> tuple[int | None, bytes | None]:
    head = doc.get("head")
    if not isinstance(head, dict):
        return None, None
    seq = head.get("seq")
    if isinstance(seq, bool) or not isinstance(seq, int):
        seq = None
    return seq, _hex_bytes(head.get("entry_hash"), 32)


def verify_document(
    doc: dict[str, Any], pinned_key_hex: str, cursor: Cursor | None
) -> WalkResult:
    """Walk a served sealed-log document against the pinned key.

    Pure and blocking (run it in the executor). With a cursor the walk
    continues from the cursor's hash; without one it starts at the
    document's anchor (`checkpoint_head`) or genesis.
    """
    pinned = normalize_kernel_key(pinned_key_hex)
    served = normalize_kernel_key(doc.get("verifying_key"))
    if pinned is None or served is None:
        return WalkResult(
            ok=False,
            anchor=ANCHOR_GENESIS,
            failure=WalkFailure(FAIL_NO_KEY, None, None, "the document names no verifying key"),
            served_key=served,
        )
    if served != pinned:
        return WalkResult(
            ok=False,
            anchor=ANCHOR_GENESIS,
            failure=WalkFailure(
                FAIL_KEY_MISMATCH, None, None, "the sealed log is signed by a key other than the pin"
            ),
            served_key=served,
        )
    key_bytes = bytes.fromhex(pinned)
    raw_entries = doc.get("entries")
    entries: list[Any] = list(raw_entries) if isinstance(raw_entries, list) else []
    anchor_hash = _hex_bytes(doc.get("checkpoint_head"), 32)
    head_seq, head_hash = _head_of(doc)

    if cursor is None:
        start = anchor_hash if anchor_hash is not None else GENESIS_HASH
        result = walk_entries(
            entries, key_bytes, start, ANCHOR_CHECKPOINT if anchor_hash else ANCHOR_GENESIS
        )
    else:
        # A kernel that ignores `from_seq` serves the whole tail; rows at or
        # below the cursor were verified on earlier walks.
        newer = [raw for raw in entries if (seq := entry_seq(raw)) is None or seq > cursor.seq]
        if not newer:
            result = WalkResult(
                ok=True,
                anchor=ANCHOR_UNCHANGED,
                last_seq=cursor.seq,
                last_hash=cursor.entry_hash,
            )
            if head_seq is not None and head_seq < cursor.seq:
                result.ok = False
                result.failure = WalkFailure(
                    FAIL_HEAD_REGRESSED,
                    None,
                    head_seq,
                    f"the chain head (seq {head_seq}) is behind the last verified entry "
                    f"(seq {cursor.seq}): the sealed log was replaced or restored",
                )
            elif (
                head_seq == cursor.seq
                and head_hash is not None
                and head_hash != cursor.entry_hash
            ):
                result.ok = False
                result.failure = WalkFailure(
                    FAIL_HEAD_MISMATCH,
                    None,
                    head_seq,
                    f"the chain head at seq {head_seq} no longer matches the verified entry",
                )
        else:
            first = parse_entry(newer[0])
            if first is None:
                result = WalkResult(
                    ok=False,
                    anchor=ANCHOR_CONTINUITY,
                    failure=WalkFailure(
                        FAIL_MALFORMED, 0, entry_seq(newer[0]), "entry at index 0 is malformed"
                    ),
                )
            elif first.prev_hash == cursor.entry_hash:
                result = walk_entries(newer, key_bytes, cursor.entry_hash, ANCHOR_CONTINUITY)
            elif anchor_hash is not None and first.prev_hash == anchor_hash:
                # The kernel re-anchored past the cursor (a checkpoint, or its
                # size cap). Everything served still verifies; continuity
                # across the gap is the kernel's claim.
                result = walk_entries(newer, key_bytes, anchor_hash, ANCHOR_REANCHORED)
            else:
                result = WalkResult(
                    ok=False,
                    anchor=ANCHOR_CONTINUITY,
                    failure=WalkFailure(
                        FAIL_PREV_HASH,
                        0,
                        first.seq,
                        f"the entry after seq {cursor.seq} no longer links to the verified "
                        "head: the sealed log was rewritten at the cursor",
                    ),
                )
    # The declared head must be the row the walk ended on, when it was served.
    if (
        result.ok
        and head_seq is not None
        and head_hash is not None
        and result.last_seq is not None
        and head_seq == result.last_seq
        and head_hash != result.last_hash
    ):
        result.ok = False
        result.failure = WalkFailure(
            FAIL_HEAD_MISMATCH,
            None,
            head_seq,
            f"the declared chain head at seq {head_seq} does not match the verified entry",
        )
    result.head_seq = head_seq if head_seq is not None else result.last_seq
    return result


# ── the D10 attribute rule ──────────────────────────────────────────────


def event_verification(
    state: VerificationState | None, event: dict[str, Any] | None
) -> tuple[str, str]:
    """(verification, trust_reason) for one kernel event row.

    `verified` iff the last successful walk verified THE ROW'S OWN entry;
    `failed` iff the last walk failed; a key mismatch wins over everything
    (Repairs tofu_mismatch is open); otherwise `sealed`.

    Which entry is the row's: its `seq` (C5 — every bridge payload and
    kernel view since the amended contract carries the sealed-log row id),
    and the walk must have verified that seq with the row's own key. The
    key alone — (event_type, zone_id, bucket) — is not an identity: a dog
    and a cat in one zone in one bucket are two entries with one key, and
    the second must not read Verified on the first's signature. A row with
    no `seq` is therefore Verified only when its key is in the index AND
    the walk that built the index began after the row's bucket closed
    (`verified_bucket_start` is the bucket the walk started in) AND that
    walk reached the kernel's declared head: every entry of that bucket
    existed when the walk ran and was walked, so whichever entry the row
    is, it was verified. A key-only row in the open bucket stays Sealed
    until the next walk after the bucket turns. (Continuity across a
    re-anchor gap stays the kernel's claim, as the walk already states.)
    """
    if state is None:
        return VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED
    if state.key_mismatch:
        # A key change is not a walk failure (D10: Repairs tofu_mismatch,
        # never Failed): the row stays Sealed, and the reason says why the
        # badge cannot be more than that until a person confirms the key.
        return VERIFICATION_SEALED, TRUST_REASON_KERNEL_KEY_MISMATCH
    if state.ok is False:
        return VERIFICATION_FAILED, TRUST_REASON_HA_VERIFY_FAILED
    key = covered_key_of_event(event)
    if key is None or not isinstance(event, dict):
        return VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED
    seq = event.get("seq")
    if isinstance(seq, int) and not isinstance(seq, bool):
        if state.covered_seqs.get(seq) == key:
            return VERIFICATION_VERIFIED, TRUST_REASON_HA_VERIFIED_CHAIN
        return VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED
    if key in state.covered and _walk_closed_over(state, key, event):
        return VERIFICATION_VERIFIED, TRUST_REASON_HA_VERIFIED_CHAIN
    return VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED


def _walk_closed_over(state: VerificationState, key: CoveredKey, event: dict[str, Any]) -> bool:
    """Whether the last walk began after the row's bucket closed and reached
    the kernel's declared head — the condition under which a key-only row
    (no `seq`) is known to have been walked whichever entry it is."""
    if state.verified_bucket_start is None or state.verified_seq is None:
        return False
    if state.head_seq is None or state.head_seq != state.verified_seq:
        return False
    size = wire_bucket_size(event)
    if isinstance(size, bool) or not isinstance(size, int) or size <= 0:
        size = TIME_BUCKET_SECONDS
    return key[2] + size <= state.verified_bucket_start


def chain_trust_reason(state: VerificationState | None) -> str | None:
    """The chain_valid sensor's `trust_reason`, the same vocabulary the
    event rows use: `kernel_key_mismatch` while the served key is not the
    pin (whatever the last walk said), `ha_verified_chain` / `ha_verify_failed`
    for a walk that passed / failed, None before the first walk."""
    if state is None:
        return None
    if state.key_mismatch:
        return TRUST_REASON_KERNEL_KEY_MISMATCH
    if state.ok is True:
        return TRUST_REASON_HA_VERIFIED_CHAIN
    if state.ok is False:
        return TRUST_REASON_HA_VERIFY_FAILED
    return None


def _time_bucket_of(event: dict[str, Any]) -> dict[str, Any] | None:
    bucket = event.get("time_bucket")
    if isinstance(bucket, dict):
        return {"start_epoch_s": bucket.get("start_epoch_s"), "size_s": bucket.get("size_s")}
    start = event.get("time_bucket_start")
    if start is None:
        return None
    return {"start_epoch_s": start, "size_s": event.get("time_bucket_size", TIME_BUCKET_SECONDS)}


def witness_event_attributes(
    event: dict[str, Any],
    state: VerificationState | None,
    *,
    kernel_device_id: str | None,
    kernel_reported_chain_ok: bool | None,
    kernel_verified_bucket_start: int | None,
) -> dict[str, Any]:
    """The C5 attribute table for a kernel event row, exactly.

    Shared by the last-event sensor and the witness event entities so the
    card reads one shape. `verification`/`verified`/`trust_reason` are the
    integration's (D10); the kernel's own last verify rides along as
    `kernel_reported_chain_ok` / `kernel_verified_bucket_start`, never as
    `verified`.
    """
    zone_id = event.get("zone_id")
    zone = None
    if isinstance(zone_id, str):
        zone = zone_id[len("zone:"):] if zone_id.startswith("zone:") else zone_id
    verification, trust_reason = event_verification(state, event)
    attrs: dict[str, Any] = {
        "zone_id": zone_id,
        "zone": zone,
        "time_bucket": _time_bucket_of(event),
        "confidence": event.get("confidence"),
        "attestation": normalize_attestation(event.get("attestation")),
        "friendly_event": event_type_metadata(event.get("event_type"))["label"],
        "kernel_version": event.get("kernel_version"),
        "ruleset_id": event.get("ruleset_id"),
        "kernel_device_id": event.get("device_id") or kernel_device_id,
        "verification": verification,
        "verified": verification == VERIFICATION_VERIFIED,
        "trust_reason": trust_reason,
        "verified_bucket_start": (
            state.verified_bucket_start
            if state is not None and verification == VERIFICATION_VERIFIED
            else None
        ),
        # Every sealed event is kernel-signed; whether the integration checked
        # that signature is what `verified` says.
        "signed": True,
        "kernel_reported_chain_ok": (
            event.get("verified_ok")
            if isinstance(event.get("verified_ok"), bool)
            else kernel_reported_chain_ok
        ),
        "kernel_verified_bucket_start": (
            event.get("verified_bucket_start")
            if isinstance(event.get("verified_bucket_start"), int)
            else event.get("verified_at")
            if isinstance(event.get("verified_at"), int)
            else kernel_verified_bucket_start
        ),
    }
    seq = event.get("seq")
    if isinstance(seq, int) and not isinstance(seq, bool):
        attrs["seq"] = seq
    return attrs


# ── the verifier ────────────────────────────────────────────────────────


def storage_key(entry_id: str) -> str:
    """The Store key holding one entry's cursor and covered index."""
    return f"{DOMAIN}.verify_{entry_id}"


async def async_remove_verify_store(hass: HomeAssistant, entry_id: str) -> None:
    """Delete the entry's persisted cursor and covered index (the entry is being removed).

    The key carries the entry id, so nothing could ever read the file back;
    leaving it would keep a map of which zones saw events in which buckets
    on disk for an installation that no longer exists.
    """
    store: Store[dict[str, Any]] = Store(hass, STORAGE_VERSION, storage_key(entry_id))
    await store.async_remove()


def _prune_covered(
    covered: set[CoveredKey], *, now: float, retention_seconds: int
) -> set[CoveredKey]:
    """Drop keys older than retention; cap the index at the newest N."""
    floor = int(now) - retention_seconds
    kept = {key for key in covered if key[2] >= floor}
    if len(kept) > VERIFY_COVERED_MAX_ENTRIES:
        newest = sorted(kept, key=lambda key: key[2], reverse=True)[:VERIFY_COVERED_MAX_ENTRIES]
        kept = set(newest)
    return kept


def _prune_covered_seqs(
    covered_seqs: dict[int, CoveredKey], *, now: float, retention_seconds: int
) -> dict[int, CoveredKey]:
    """The seq index under the same retention bound and cap (newest seqs win)."""
    floor = int(now) - retention_seconds
    kept = {seq: key for seq, key in covered_seqs.items() if key[2] >= floor}
    if len(kept) > VERIFY_COVERED_MAX_ENTRIES:
        newest = sorted(kept, reverse=True)[:VERIFY_COVERED_MAX_ENTRIES]
        kept = {seq: kept[seq] for seq in newest}
    return kept


def _covered_rows(state: VerificationState) -> list[list[Any]]:
    """The persisted index: `[event_type, zone_id, bucket_start, seq | None]`
    per entry, oldest bucket first — one row per seq, plus a seq-less row
    for a key no seq names (an entry served without one)."""
    rows: list[list[Any]] = [[*key, seq] for seq, key in state.covered_seqs.items()]
    keyed = set(state.covered_seqs.values())
    rows.extend([*key, None] for key in state.covered if key not in keyed)
    rows.sort(key=lambda row: (row[2], row[3] is None, row[3] or 0, row[0], row[1]))
    return rows


class KernelVerifier:
    """Runs the walk on its triggers and keeps ``runtime.verification`` true."""

    def __init__(
        self,
        hass: HomeAssistant,
        entry: SecuraCVConfigEntry,
        coordinator: SecuraCVKernelCoordinator,
        api: SecuraCVApi,
    ) -> None:
        """Bind to the entry's coordinator (the pin, the push feed) and API client."""
        self.hass = hass
        self.entry = entry
        self.coordinator = coordinator
        self.api = api
        self._store: Store[dict[str, Any]] = Store(hass, STORAGE_VERSION, storage_key(entry.entry_id))
        self._cursor: Cursor | None = None
        self._lock = asyncio.Lock()
        self._unsubs: list[Callable[[], None]] = []
        self._idle_cancel: Callable[[], None] | None = None
        # A failed walk whose cause is a replaced log (the head moved back):
        # only a person's Verify now re-anchors the cursor after that.
        self._regressed = False
        # The state when the runtime is gone (a walk finishing after unload).
        self._detached_state = VerificationState()

    # ── state ────────────────────────────────────────────────────────

    @property
    def state(self) -> VerificationState:
        """The entry's verification state (``runtime.verification``)."""
        runtime = runtime_for(self.entry)
        return runtime.verification if runtime is not None else self._detached_state

    @property
    def cursor(self) -> Cursor | None:
        """The persisted cursor (None before the first successful walk)."""
        return self._cursor

    # ── lifecycle ────────────────────────────────────────────────────

    async def async_start(self) -> None:
        """Load the cursor, arm the triggers, schedule the first walk."""
        await self._async_load()
        self._unsubs.append(
            async_track_time_interval(
                self.hass,
                self._scheduled_tick,
                timedelta(seconds=VERIFY_BACKSTOP_INTERVAL_SECONDS),
            )
        )
        self._unsubs.append(
            async_dispatcher_connect(
                self.hass, witness_event_signal(self.entry.entry_id), self._on_new_entry
            )
        )
        self._unsubs.append(
            async_dispatcher_connect(
                self.hass, chain_problem_signal(self.entry.entry_id), self._on_chain_problem
            )
        )
        self._spawn(self.async_run(VERIFY_TRIGGER_SCHEDULED))

    @callback
    def _spawn(self, coro: Any) -> None:
        """Run a walk as a background task bound to the entry.

        A background task never holds Home Assistant's startup for a slow
        kernel and is canceled with the entry; a core (or a stub) without
        the entry-bound form falls back to a plain task.
        """
        create = getattr(self.entry, "async_create_background_task", None)
        if create is not None:
            create(self.hass, coro, name=f"{DOMAIN} chain walk {self.entry.entry_id}")
        else:
            self.hass.async_create_task(coro)

    @callback
    def async_stop(self) -> None:
        """Release the triggers (unload; idempotent)."""
        for unsub in self._unsubs:
            unsub()
        self._unsubs.clear()
        self._cancel_idle()

    async def _async_load(self) -> None:
        data = await self._store.async_load()
        if not isinstance(data, dict) or data.get("v") != STORAGE_SCHEMA_VERSION:
            return
        seq = data.get("verified_seq")
        entry_hash = _hex_bytes(data.get("verified_hash"), 32)
        verified_entries = data.get("verified_entries", 0)
        if isinstance(seq, int) and not isinstance(seq, bool) and entry_hash is not None:
            self._cursor = Cursor(
                seq=seq,
                entry_hash=entry_hash,
                verified_entries=int(verified_entries) if isinstance(verified_entries, int) else 0,
            )
        covered: set[CoveredKey] = set()
        covered_seqs: dict[int, CoveredKey] = {}
        for row in data.get("covered") or []:
            # [event_type, zone_id, bucket_start, seq] — a store written
            # before the seq index has three-element rows (key only).
            if (
                isinstance(row, list)
                and len(row) in (3, 4)
                and isinstance(row[0], str)
                and isinstance(row[1], str)
                and isinstance(row[2], int)
                and not isinstance(row[2], bool)
            ):
                key: CoveredKey = (row[0], row[1], row[2])
                covered.add(key)
                seq = row[3] if len(row) == 4 else None
                if isinstance(seq, int) and not isinstance(seq, bool):
                    covered_seqs[seq] = key
        state = self.state
        now = time.time()
        state.covered = _prune_covered(
            covered, now=now, retention_seconds=self._retention_seconds()
        )
        state.covered_seqs = _prune_covered_seqs(
            covered_seqs, now=now, retention_seconds=self._retention_seconds()
        )
        if self._cursor is not None:
            state.verified_seq = self._cursor.seq
            state.verified_entries = self._cursor.verified_entries
            state.head_hash = self._cursor.entry_hash.hex()

    def _stored(self) -> dict[str, Any]:
        state = self.state
        return {
            "v": STORAGE_SCHEMA_VERSION,
            "verified_seq": self._cursor.seq if self._cursor else None,
            "verified_hash": self._cursor.entry_hash.hex() if self._cursor else None,
            "verified_entries": self._cursor.verified_entries if self._cursor else 0,
            "covered": _covered_rows(state),
        }

    def _retention_seconds(self) -> int:
        """The kernel's retention from its digest, else the default bound."""
        data = self.coordinator.data if isinstance(self.coordinator.data, dict) else {}
        digest = data.get(DATA_DIGEST)
        if isinstance(digest, dict):
            days = digest.get("retention_days")
            if isinstance(days, int) and not isinstance(days, bool) and days > 0:
                return days * 86400
        return VERIFY_COVERED_DEFAULT_RETENTION_SECONDS

    # ── triggers ─────────────────────────────────────────────────────

    @callback
    def _scheduled_tick(self, _now: datetime) -> None:
        self._spawn(self.async_run(VERIFY_TRIGGER_SCHEDULED))

    @callback
    def _on_new_entry(self, _event: Any) -> None:
        """A new entry arrived: walk 60 s after they stop arriving."""
        self.async_arm_idle_walk()

    @callback
    def async_arm_idle_walk(self) -> None:
        """(Re)arm the idle timer."""
        self._cancel_idle()
        self._idle_cancel = async_call_later(
            self.hass, VERIFY_IDLE_DELAY_SECONDS, self._idle_fire
        )

    @callback
    def _cancel_idle(self) -> None:
        if self._idle_cancel is not None:
            self._idle_cancel()
            self._idle_cancel = None

    @callback
    def _idle_fire(self, _now: datetime) -> None:
        self._idle_cancel = None
        self._spawn(self.async_run(VERIFY_TRIGGER_IDLE))

    @callback
    def _on_chain_problem(self, problem: bool) -> None:
        """The bridge's own chain report moved.

        A problem the kernel reports about itself is surfaced at once as a
        verification event (trigger `bridge`, `kernel_reported: true`) — a
        self-reported break is actionable. A clear is never a verdict: it
        arms a walk, and the walk says whether the chain verifies.
        """
        if problem:
            self._fire(
                VERIFICATION_EVENT_CHAIN_FAILED,
                trigger=VERIFY_TRIGGER_BRIDGE,
                kernel_reported=True,
                chain_valid=False,
                verdict="the kernel reports its own chain check failed",
            )
            return
        self.async_arm_idle_walk()

    async def async_run(self, trigger: str) -> None:
        """A walk that never raises (the scheduled and idle triggers)."""
        try:
            await self.async_verify(trigger)
        except SecuraCVApiError as err:
            _LOGGER.debug("chain walk (%s) could not reach the kernel: %s", trigger, err)
        except Exception:  # noqa: BLE001 - a background walk must never take the loop down
            _LOGGER.exception("chain walk (%s) failed unexpectedly", trigger)

    # ── the walk ─────────────────────────────────────────────────────

    async def async_verify(self, trigger: str) -> VerificationState:
        """Run one walk now; raises the API errors for a caller that asked."""
        async with self._lock:
            state = self.state
            state.verifying = True
            state.last_trigger = trigger
            self.coordinator.async_update_listeners()
            try:
                return await self._async_walk(trigger)
            finally:
                state.verifying = False
                self.coordinator.async_update_listeners()

    async def _async_walk(self, trigger: str) -> VerificationState:
        state = self.state
        pinned = self.coordinator.pinned_key
        if pinned is None:
            # Nothing to verify against yet: the pin comes from the first
            # authenticated identity fetch. Not a failure.
            state.error = "no witness key pinned yet"
            return state
        if self.coordinator.key_mismatch:
            state.key_mismatch = True
            state.error = VERIFY_ERROR_KEY_CHANGED
            return state
        cursor = self._cursor
        if cursor is not None and self._regressed and trigger in MANUAL_TRIGGERS:
            # A person asked, after the head moved backwards: re-anchor at
            # the document's own anchor and walk everything served again.
            cursor = None
        # The walk's bucket is the one the FETCH began in: a key-only row's
        # Verified rests on its bucket having closed before this instant
        # (`event_verification`), so the stamp is taken before the kernel
        # answers, never after.
        bucket = bucket_start(time.time())
        doc = await self.api.async_get_sealed_log(
            from_seq=cursor.seq if cursor is not None else None
        )
        result = await self.hass.async_add_executor_job(verify_document, doc, pinned, cursor)
        now = time.time()

        if result.key_mismatch:
            # The served key is not the pin: the coordinator owns the Repairs
            # issue (and the key-changed event); the walk records nothing.
            if result.served_key is not None:
                self.coordinator.async_note_served_key(result.served_key)
            state.key_mismatch = True
            state.error = VERIFY_ERROR_KEY_CHANGED
            self.coordinator.async_update_listeners()
            return state

        state.head_seq = result.head_seq
        if result.ok:
            if result.anchor in (ANCHOR_CONTINUITY, ANCHOR_UNCHANGED) and cursor is not None:
                total = cursor.verified_entries + result.verified
            else:
                total = result.verified
            if result.last_seq is not None and result.last_hash is not None:
                self._cursor = Cursor(
                    seq=result.last_seq, entry_hash=result.last_hash, verified_entries=total
                )
            self._regressed = False
            state.ok = True
            state.error = None
            state.failed_seq = None
            state.verified_seq = self._cursor.seq if self._cursor else None
            state.verified_entries = total
            state.verified_bucket_start = bucket
            state.verified_bucket_size = TIME_BUCKET_SECONDS
            state.head_hash = self._cursor.entry_hash.hex() if self._cursor else None
            state.walk_entries = result.verified
            state.anchor_moved = result.anchor == ANCHOR_REANCHORED
            covered = set(state.covered)
            covered.update(result.covered)
            state.covered = _prune_covered(
                covered, now=now, retention_seconds=self._retention_seconds()
            )
            covered_seqs = dict(state.covered_seqs)
            covered_seqs.update(result.covered_seqs)
            state.covered_seqs = _prune_covered_seqs(
                covered_seqs, now=now, retention_seconds=self._retention_seconds()
            )
            self._store.async_delay_save(self._stored, 5)
            if result.anchor == ANCHOR_REANCHORED:
                _LOGGER.info(
                    "The witness kernel re-anchored its sealed log past the last verified "
                    "entry; %d newer entries verified, continuity across the gap is the "
                    "kernel's claim",
                    result.verified,
                )
            verdict = (
                f"{result.verified} new entries verified against the pinned key"
                if result.verified
                else "chain intact; no new entries since the last walk"
            )
            self._fire(
                VERIFICATION_EVENT_CHAIN_VERIFIED,
                trigger=trigger,
                chain_valid=True,
                verdict=verdict,
                events_verified=result.verified,
                verified_seq=state.verified_seq,
                verified_bucket_start=bucket,
                verified_bucket_size=TIME_BUCKET_SECONDS,
            )
        else:
            failure = result.failure
            state.ok = False
            state.error = failure.message if failure else "verification failed"
            state.failed_seq = failure.seq if failure else None
            state.walk_entries = result.verified
            self._regressed = failure is not None and failure.kind == FAIL_HEAD_REGRESSED
            _LOGGER.warning(
                "Witness chain verification FAILED (%s): %s", trigger, state.error
            )
            self._fire(
                VERIFICATION_EVENT_CHAIN_FAILED,
                trigger=trigger,
                chain_valid=False,
                verdict=state.error,
                events_verified=result.verified,
                verified_seq=state.verified_seq,
                failed_seq=state.failed_seq,
                error=failure.kind if failure else None,
            )
        self.coordinator.async_update_listeners()
        return state

    @callback
    def _fire(self, event_type: str, **attributes: Any) -> None:
        async_dispatcher_send(
            self.hass, verification_event_signal(self.entry.entry_id), event_type, attributes
        )


async def async_verify_entry(
    hass: HomeAssistant, entry: SecuraCVConfigEntry, *, trigger: str
) -> VerificationState:
    """Run the entry's verifier now (the button and the action).

    Raises ValueError when the entry has no kernel verifier, and the API
    errors when the kernel cannot be reached; the callers translate both.
    """
    runtime = runtime_for(entry)
    if runtime is None or runtime.verifier is None:
        raise ValueError("entry has no kernel verifier")
    return await runtime.verifier.async_verify(trigger)


def verification_response(state: VerificationState, kernel_device_id: str | None) -> dict[str, Any]:
    """The `securacv.verify` action's response (and the button's log line)."""
    if state.key_mismatch:
        verdict = VERIFY_ERROR_KEY_CHANGED
    elif state.ok is True:
        verdict = (
            f"{state.walk_entries} new entries verified against the pinned key"
            if state.walk_entries
            else "chain intact; no new entries since the last walk"
        )
    elif state.ok is False:
        verdict = state.error or "verification failed"
    else:
        verdict = state.error or "not verified yet"
    return {
        "chain_valid": state.ok,
        "verdict": verdict,
        "events_verified": state.walk_entries,
        "verified_entries": state.verified_entries,
        "verified_seq": state.verified_seq,
        "verified_bucket_start": state.verified_bucket_start,
        "verified_bucket_size": state.verified_bucket_size,
        "key_mismatch": state.key_mismatch,
        "error": state.error if state.ok is not True else None,
        "kernel_device_id": kernel_device_id,
    }


def kernel_device_id_of(entry: SecuraCVConfigEntry) -> str | None:
    """The entry's kernel device id, when known."""
    device_id = entry.data.get(CONF_KERNEL_DEVICE_ID)
    return str(device_id) if device_id else None
