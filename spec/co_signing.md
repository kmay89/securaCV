# Event Co-Signing — Design Notes
Status: Draft v0.1
Intended Status: Design Note
Author: The Witness Project
Last Updated: 2026-02-02

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
endorsement_created_bucket: <iso-8601, 10-min>
endorser_domain_id: <utf8-string>
```

**Field definitions:**
- `log_id`: Identifier of the sealed log (device-local, non-global).
- `event_seq`: Sequential event index within the log.
- `event_hash`: Hash of the event record as stored in the sealed log.
- `ruleset_hash`: Ruleset identifier active at event creation time.
- `created_bucket`: Coarsened ISO-8601 timestamp for the event's creation,
  truncated to a 10-minute UTC boundary. Truncation is performed by flooring
  the minute component to the nearest multiple of 10 and zeroing seconds.

  **Example:** An event created at `2026-01-20T12:34:56Z` yields
  `created_bucket: 2026-01-20T12:30:00Z`. An event at `2026-01-20T12:09:59Z`
  yields `created_bucket: 2026-01-20T12:00:00Z`.

  This bucketing provides deterministic matching across endorsers while
  limiting timestamp precision for privacy.
- `endorsement_created_bucket`: Coarsened ISO-8601 timestamp for when the
  endorsement signature was created, truncated to a 10-minute UTC boundary
  using the same algorithm as `created_bucket`. This field captures when
  the endorser signed, distinct from when the original event was created.

  **Example:** An endorser signing at `2026-01-21T09:47:22Z` yields
  `endorsement_created_bucket: 2026-01-21T09:40:00Z`.
- `endorser_domain_id`: A local trust-store label identifying the
  administrative domain of the endorser. This is a UTF-8 string configured
  in the local trust store, used for threshold counting (see Section 4).
  Only one endorsement per `endorser_domain_id` counts toward a threshold.

  **Example:** `endorser_domain_id: acme-corp-legal` or
  `endorser_domain_id: external-auditor-pwc`.

**Notes:**
- All fields are ASCII, newline-delimited, and MUST appear in the order shown.
- No optional fields in v1; any future extensions must bump the version string.

## 4. Threshold Semantics (Distinct from Break-Glass Quorum)

Co-signing thresholds are *review thresholds* and must never be conflated with
break-glass quorum thresholds.

- A co-signing threshold defines how many independent endorsements are required
  to mark an event as “endorsed.”
- Endorsements counted toward a threshold MUST originate from independently
  controlled keys. **One endorsement per `endorser_domain_id` counts toward
  the threshold.** Multiple endorsements from the same principal or
  administrative domain MUST NOT be counted separately.
- An administrative domain is identified by the `endorser_domain_id` field,
  which is a local trust-store label configured by the deployment operator.
  Domain assignment is purely local configuration—**there is no CA inference,
  no remote directory lookup, and no automatic domain derivation from
  certificate fields.** The operator explicitly labels each trusted endorser
  key with a domain identifier in their local trust store.

  **Example configurations:**
  - Keys from the same organization's legal team: `endorser_domain_id: acme-legal`
  - Keys from an external audit firm: `endorser_domain_id: external-auditor-pwc`
  - Keys from a compliance department: `endorser_domain_id: compliance-internal`

  If two endorser keys share the same `endorser_domain_id`, only one
  endorsement from that domain counts toward the threshold. To ensure
  deterministic counting across implementations, the selected endorsement
  is the one with the lexicographically smallest `(endorsement_created_bucket,
  signature_bytes)` tuple. This ordering guarantees consistent threshold
  evaluation regardless of processing order.
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
  - Endorsement message (canonical form as above, including `endorsement_created_bucket`
    and `endorser_domain_id`)
  - Endorser public key identifier
  - Signature bytes

  Note: The `endorser_domain_id` used for threshold counting MUST be extracted
  from the signed endorsement message, not stored separately. This prevents
  inconsistencies between signed and unsigned copies.
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
