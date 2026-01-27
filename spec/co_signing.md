# Event Co-Signing — Design Notes
Status: Draft v0.1
Intended Status: Design Note
Author: The Witness Project
Last Updated: 2026-01-20

## 1. Purpose

This document specifies the co-signing (endorsement) model for event hashes in
SecuraCV. Co-signing provides independent attestations that an event log entry
was reviewed and endorsed, without altering or expanding the kernel’s break-glass
authorization model. Break-glass quorum rules remain the only path to raw media
access (see Invariant V in `spec/invariants.md`).

## 2. Non-Goals

- **No raw media exchange.** Co-signing only covers event hashes and associated
  metadata. Endorsers never receive raw media, frames, or vault contents.
- **No break-glass substitution.** Co-signing thresholds are distinct from
  break-glass quorum rules, and must not be reused as an authorization mechanism
  for vault access.

## 3. Event Hash Endorsement Message Format

Endorsements are signed messages that bind an endorser identity to a specific
sealed event record. The message format is canonicalized and stable to prevent
ambiguous signing.

### 3.1 Canonical Message

```
PWK/Endorsement/v1
log_id: <hex>
event_seq: <u64>
event_hash: <hex>
ruleset_hash: <hex>
created_bucket: <iso-8601, 10-min>
```

**Field definitions:**
- `log_id`: Identifier of the sealed log (device-local, non-global).
- `event_seq`: Sequential event index within the log.
- `event_hash`: Hash of the event record as stored in the sealed log.
- `ruleset_hash`: Ruleset identifier active at event creation time.
- `created_bucket`: Coarsened ISO-8601 timestamp for the event's creation,
  truncated to a 10-minute boundary (e.g., `2026-01-20T12:34:56Z` becomes
  `2026-01-20T12:30:00Z`).

**Notes:**
- All fields are ASCII, newline-delimited, and MUST appear in the order shown.
- No optional fields in v1; any future extensions must bump the version string.

## 4. Threshold Semantics (Distinct from Break-Glass Quorum)

Co-signing thresholds are *review thresholds* and must never be conflated with
break-glass quorum thresholds.

- A co-signing threshold defines how many independent endorsements are required
  to mark an event as “endorsed.”
- Endorsements counted toward a threshold MUST originate from independently
  controlled keys. Multiple endorsements from the same principal or
  administrative domain MUST NOT be counted separately. An administrative
  domain is a deployment-defined trust grouping (e.g., keys issued under the
  same org-scoped CA, keys bound to the same operator account, or keys labeled
  with the same domain identifier in local policy configuration).
- Endorsements MAY be collected over time and remain valid as long as the event
  record is unchanged.
- Endorsement thresholds do **not** authorize evidence vault access or any
  release of raw media. Break-glass remains the sole authorization mechanism
  for raw media access, with its own quorum requirements (see Invariant V in
  `spec/invariants.md`).
- Policies may vary per deployment (e.g., 2-of-5 reviewers) but must be recorded
  alongside the policy identifier used for the evaluation.
- Endorsements are non-transitive. An endorsement issued for one sealed log MUST
  NOT be interpreted as endorsement of the same event hash appearing in a
  different log or deployment.

## 5. Storage Rules for Co-Signatures

Co-signatures are stored *adjacent to* the event record in the sealed log, but
remain logically distinct from the event itself.

- **Append-only:** Co-signatures are appended as separate records referencing a
  specific event hash and sequence number.
- **No mutation:** Existing event records MUST NOT be rewritten or re-hashed when
  co-signatures are added.
- **Binding:** Each co-signature record stores:
  - Endorsement message (canonical form as above)
  - Endorser public key identifier
  - Signature bytes
  - Endorsement creation timestamp bucket (coarsened, time of signature)
- **Indexing:** The log may maintain an internal index mapping event hashes to
  endorsement records, but must remain local and non-queryable externally.
- **Retention:** Co-signatures follow the same retention policy as the event
  records they reference, and are purged together.

## 6. Security Considerations

- Co-signing does not increase the kernel’s access to raw media.
- Endorser identities should be local or organization-scoped, not global.
- Any UI must avoid implying that endorsement is equivalent to evidence access
  or break-glass approval.
- User interfaces MUST NOT present endorsed events as “verified,” “confirmed,”
  or “validated.” Endorsement indicates review, not ground truth.
- Endorsement messages MUST be bound to the sealed log context (e.g., by
  `log_id` and `event_seq`) and MUST be rejected if replayed for a different
  event or log.

## 7. Future Extensions

- Multi-scheme endorsements (e.g., Ed25519 and post-quantum) require a new
  message version and explicit scheme identifiers.
- Additional context fields must be versioned to avoid signature ambiguity.
