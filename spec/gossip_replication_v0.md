# Gossip Replication Protocol v0 (Event Claims Only)
Status: Draft v0.1
Intended Status: Normative
Last Updated: 2026-01-20

## 1. Purpose and Scope

This protocol defines **event-claim gossip replication** between witness-kernel peers. It propagates only signed, minimal event *claims* and never raw media, in alignment with the system privacy invariants. The protocol is designed to preserve auditability while avoiding any new data collection capability. It is **optional and off by default**, and **not required for v1** deployments.

**Explicit non-goal:** This protocol does **not** transport raw frames, audio, or identity-linked data. It only propagates signed claims about events that already exist in a local sealed log. See the trust and privacy boundaries in `spec/threat_model.md` and the evidence handling constraints in `docs/evidence_lifecycle.md`.

## 2. Data Model (Claim-Only)

Each replicated message carries a **SignedEventClaim** with the following fields:

- `event_hash`: Hash of the sealed-log event entry (content-addressed claim). No raw data.
- `signature`: Ed25519 signature over the **domain-separated** signing input defined below.
- `device_key_id`: Rotating identifier for the signing device key (see Privacy note).
- `time_bucket`: Integer time bucket index at 10-minute granularity (`floor(unix_time / 600)`).

No additional claim fields are allowed. In particular, **no raw media, no precise timestamps, and no identity data** are permitted. Receivers MUST reject any claim containing unknown or extra fields.

### `event_hash` Definition

`event_hash` MUST be the sealed-log entry hash computed as:

```
event_hash = SHA-256(prev_hash || payload_json_bytes)
```

where `prev_hash` is the previous sealed-log hash (32 bytes) and `payload_json_bytes` is the canonical JSON byte sequence (RFC 8785) stored in the sealed log entry. This is the same hash used by the kernel’s sealed-log chain (see `hash_entry` in the kernel log module).

### Canonical Signing Input (with Domain Separation)

To avoid cross-protocol signature confusion, the signing input MUST be:

```
SIGNING_INPUT =
  "securacv:gossip_claim:v0" ||
  event_hash ||
  time_bucket ||
  device_key_id
```

Encoding MUST be deterministic and specified by the wire format (Section 3). For signing, each component MUST be serialized to its canonical byte representation before concatenation (e.g., `event_hash` as 32 raw bytes, `time_bucket` as canonical CBOR-encoded uint, and `device_key_id` as its canonical CBOR byte string). Implementations MUST reject claims that do not match the canonical encoding.

### Privacy Note: `device_key_id` Rotation

`device_key_id` MUST NOT be a stable, long-lived identifier (e.g., not a hash of the device public key). It MUST be a **rotating key slot identifier** with a short validity window (e.g., daily or weekly rotation), and it MUST map to a device key via a local registry lookup. This reduces cross-mesh correlation while preserving authentication.

## 3. Wire Format and Message Types (v0)

### 3.1 Wire Format (Normative)

All messages MUST be encoded as **deterministic CBOR** with the following envelope:

```
{
  "protocol_version": 0,
  "message_type": "claim_announcement",
  "claim": {
    "event_hash": bstr(32),
    "time_bucket": uint,
    "device_key_id": bstr(<=16),
    "signature": bstr(64)
  }
}
```

CDDL (RFC 8610) form:

```
claim_announcement = {
  protocol_version: 0,
  message_type: "claim_announcement",
  claim: {
    event_hash: bstr .size 32,
    time_bucket: uint,
    device_key_id: bstr .size (1..16),
    signature: bstr .size 64
  }
}
```

Rules:
- CBOR encoding MUST use canonical (deterministic) ordering.
- `event_hash` is 32 bytes (SHA-256).
- `signature` is 64 bytes (Ed25519).
- `device_key_id` is a short rotating identifier (up to 16 bytes).
- Unknown **claim** fields MUST be rejected.
- `protocol_version` MUST be `0` for this spec; other versions MUST be ignored (not propagated).
- Unknown **message types** MUST be ignored (not propagated).

### 3.2 `ClaimAnnouncement` (v0)

The only v0 message type is `claim_announcement`, which advertises possession of a signed event claim.

**Note:** No message in this protocol contains raw media or references to raw media locations. v0 is **push-only**; request/response is reserved for a future version and MUST preserve non-queryability if ever added.

## 4. Peer Authentication

Peers authenticate using existing **device Ed25519 keys**. The transport MUST:

1. Validate the peer’s device key against the local device key registry (which maps rotating `device_key_id` values to active device keys). Registry provisioning and synchronization are out of scope for this document and MUST be managed via a separate, secure process.
2. Require a proof-of-possession (e.g., a signed nonce) during session establishment.
3. Bind the authenticated device identity to the connection context.

Authentication is **device-to-device**, not user-to-user. Trust assumptions and adversary model are defined in `spec/threat_model.md`.

## 5. Validation Rules

A received `SignedEventClaim` is accepted only if:

1. `signature` verifies against the device public key referenced by `device_key_id`, using the domain-separated signing input in Section 2.
2. `time_bucket` is an integer bucket index at 10-minute granularity.
3. `event_hash` is 32 bytes and matches the sealed-log hash format.
4. The claim does not include any additional fields beyond those listed in Section 2.

Claims with unknown or expired `device_key_id` values MUST be rejected and MUST NOT be propagated.

Claims failing any rule MUST be rejected and MUST NOT be propagated.

## 6. Replay, Duplication, and TTL

### 6.1 Duplicate Suppression
Peers MUST de-duplicate claims by `(event_hash, device_key_id, time_bucket)`.

Suggested storage:
- A fixed-size LRU or time-bucketed cache of recently seen claims.
- De-duplication index keyed by the tuple above.

Implementations MUST bound de-duplication state in both time and storage to preserve metadata minimization.

### 6.2 Replay Handling
Replayed claims (same tuple) are safe to ignore after validation. Receivers SHOULD:

- Drop duplicates without side effects.
- Avoid re-propagating duplicates to minimize amplification.

### 6.3 TTL Behavior (Bucket Distance)

TTL is defined in **bucket distance**, not wall-clock dates:

- `local_bucket = floor(local_unix_time / 600)`
- A claim is accepted iff `abs(local_bucket - time_bucket) <= MAX_BUCKET_SKEW`.
- Default `MAX_BUCKET_SKEW = 144` (24 hours).

Claims outside the window MUST be rejected and MUST NOT be re-propagated. Implementations SHOULD record a local audit note when rejecting due to skew (without propagating the claim).

## 7. Propagation Policy

- Peers SHOULD implement a fan-out limit to avoid uncontrolled flooding.
- Peers MUST NOT repackage, enrich, or transform claims.
- Peers MUST NOT add any new identifiers or timestamps beyond the defined fields.

Non-normative guidance: a default fan-out of 3–5 peers balances redundancy against amplification.

## 8. Auditability Boundary

This protocol transmits **only signed event claims**, never raw evidence. It preserves the audit boundary described in `docs/evidence_lifecycle.md` and the adversary constraints in `spec/threat_model.md`.

Claims remain subject to local sealed-log verification; gossip is **advisory**, not authoritative. A claim’s signature proves that a device asserted it, not that the claim is true or that raw media exists.

## 9. Security and Privacy Considerations

- **No raw export:** This protocol MUST NOT be used to move raw frames or media.
- **No identity substrate:** Device keys identify devices only; they must not encode user identity.
- **Metadata minimization:** Time buckets are coarse; no precise timestamps are shared.
- **Non-queryability:** Gossip does not enable bulk search or indexing of events.

Future versions MAY further reduce correlation by introducing an **ephemeral gossip signing key** (rotated frequently and attested by the device key). Such a change MUST preserve claim-only semantics and MUST NOT introduce new identifiers or query surfaces.

Any implementation that extends beyond these constraints violates the kernel invariants.
