# Evidence Envelope (Normative)

Status: v1 (proposed). This document defines the **canonical, versioned, self-verifying
evidence envelope** — the single interchange format for accessing, reviewing, exporting,
and independently verifying the Privacy Witness Kernel's chains. It supersedes both the
Rust `ExportBundle` (which it now *wraps*) and the canary-vision `securacv-witness-chain-v1`
envelope (which it retires).

The guiding rule: **the rules travel with the data.** A reader that has only a bundle —
no network, no vendor, no source tree — must be able to fully interpret and verify it,
and must never apply newer semantics to an older bundle (Invariant VI).

This spec is the shared contract for two verifiers that MUST agree byte-for-byte:
the Rust verifier (`src/verify.rs` / `envelope_verify`) and the offline JS verifier
(`viewer/verify_core.js`). Cross-language parity is enforced by fixtures in
`tests/fixtures/envelope/`.

---

## 1. Top-level shape

```
EvidenceEnvelope {
  envelope_format: "securacv-evidence-envelope",   // fixed; NEVER reused for a different layout
  envelope_version: 1,                              // integer, monotonic; bump for any incompatible change
  manifest:   EvidenceManifest,                     // §2 — self-describing rules
  provenance: Provenance,                           // §3 — keys + ruleset/kernel identity
  ledgers:    Ledgers,                              // §4 — the four chains, verbatim
  artifact:   ExportArtifact,                       // §5 — the coarse, human-readable event view
  gaps:       Gaps,                                 // §6 — explicit absence (gap != redaction)
  disclosure: DisclosureManifest,                   // §7 — what is included / provably withheld
  export_receipt_entry: ExportReceiptEntry,         // §8 — the signed receipt binding the artifact
  whole_envelope_digest: "<hex64>"                  // §9 — SHA-256 over the canonical digest input
}
```

## 2. `manifest` — self-describing rules

The manifest makes a bundle self-interpreting. A v1 bundle carries v1's own rules; a future
viewer reading it applies *those* rules, never its own.

```
EvidenceManifest {
  permitted_fields: [string],         // mirror of spec/event_contract.md §2 "MUST contain"
  forbidden_fields: [string],         // mirror of §2 "MUST NOT": raw_media, precise_timestamp,
                                      //   absolute_location, stable_identifier, free_text
  time_granularity: { min_bucket_s: 300, default_bucket_s: 600 },
  hash_rule: "SHA256(prev_hash || payload_json_utf8_bytes)",   // sealed ledgers; §4.1
  canonicalization: "securacv-cjson-v1",                       // envelope-level digests; §9
  signature_domains: {                // verbatim from src/crypto/signatures.rs
    sealed_log_entry: "securacv:pwk:sealed-log-entry:v2",
    checkpoint:       "securacv:pwk:sealed-log-checkpoint:v2",
    break_glass:      "securacv:pwk:break-glass-receipt:v2",
    export_receipt:   "securacv:pwk:export-receipt:v2"
  },
  signature_schemes: { classical: "ed25519", pq: "mldsa44" }
}
```

`forbidden_fields` is also a machine-checkable conformance assertion: a verifier MAY assert
that no forbidden field name appears anywhere in `artifact` or `ledgers`.

## 3. `provenance`

```
Provenance {
  kernel_version: string,
  ruleset_id: string,
  ruleset_hash: "<hex64>",
  device_public_key: "<hex64>",       // Ed25519 verifying key
  pq_public_key: "<hex>" | null       // ML-DSA-44 public key, if the device has one
}
```

Stable device/owner identifiers (device serial, owner name, MAC, firmware build) MUST NOT
appear (Invariant II/III). `kernel_version`/`ruleset_id` are capability identifiers, not
device identity.

## 4. `ledgers` — the four chains, carried verbatim

```
Ledgers {
  sealed_events:        LedgerSummary<SealedEntry>,
  break_glass_receipts: LedgerSummary<BreakGlassEntry>,
  export_receipts:      LedgerSummary<ExportEntry>,
  checkpoints:          { latest: CheckpointView | null }
}

LedgerSummary<E> { head_hash: "<hex64>" | null, count: int, entries: [E] }

SealedEntry / ExportEntry {
  payload_json: string,               // the EXACT bytes that were hashed at seal time
  prev_hash:  "<hex64>",
  entry_hash: "<hex64>",
  signatures: SignatureSet            // { ed25519_scheme, ed25519_signature[], pq_signature? }
}
BreakGlassEntry = SealedEntry + { approvals_json: string }
CheckpointView { chain_head_hash: "<hex64>", cutoff_event_id: int, signatures: SignatureSet }
```

### 4.1 Hashing of sealed entries (Tier 1 — verbatim bytes)
Each ledger entry is hashed as `SHA256(prev_hash || payload_json_utf8_bytes)` and signed via
domain-separated `SHA256(le32(len(domain)) || domain || entry_hash)` then Ed25519 (+ optional
ML-DSA-44). The envelope ships the **literal `payload_json` string** that was hashed at seal
time — never a re-serialization. This makes Rust and JS verification byte-identical with zero
canonicalization risk for already-sealed data.

Chain linkage: the first `sealed_events` entry links to `checkpoints.latest.chain_head_hash`
if a checkpoint exists, else to genesis (32 zero bytes). `break_glass_receipts` and
`export_receipts` always start at genesis.

## 5. `artifact` — the coarse, human-readable view

The existing `ExportArtifact` (`src/lib.rs`): `{ batches[].buckets[].{time_bucket, events[],
failures[]}, max_events_per_batch, jitter_s, jitter_step_s }`. Events carry only contract-approved
fields (event_type, coarse `time_bucket`, local `zone_id`, `confidence`, kernel/ruleset ids).
This is the readable evidence; the ledgers are the proof.

## 6. `gaps` — explicit absence

```
Gaps {
  failure_count: int,                 // count of Failure records across the artifact
  checkpoint_cutoff_event_id: int | null,   // events <= this id were pruned under a signed checkpoint
}
```
Per `docs/evidence_lifecycle.md`: no data is stored for periods with no qualifying events; a
gap means "the kernel produced nothing," NOT hidden data. This is distinct from a **redaction**
(§7), which is data that existed and was deliberately withheld by the discloser.

## 7. `disclosure` — included vs. provably withheld

```
DisclosureManifest {
  profile: "full" | "legal" | "insurance" | "dispute" | "at_risk",
  disclosed_window: { start_bucket_s: int, end_bucket_s: int } | null,
  break_glass_included: bool,
  redactions: [ { ledger: string, omitted_count: int, omitted_entry_hashes: ["<hex64>"] } ]
}
```

Redaction MUST be **provable, not silent**: when entries are withheld, their `entry_hash`
values (32-byte commitments, zero content leak) are listed so a verifier can confirm the chain
still links across the omission. A viewer renders three distinct states: **present / redacted /
gap**. (v1 implements `profile: "full"` with no redactions; the selective-disclosure profiles
are layered on in a later phase without changing this shape.)

The disclosure manifest is inside the digested envelope (§9), so the chosen profile and the
redaction set are themselves tamper-evident.

## 8. `export_receipt_entry`

The existing signed `ExportReceiptEntry` (`{ receipt, prev_hash, entry_hash, signatures }`).
`receipt.artifact_hash` MUST equal `SHA256(serde_json_bytes(artifact))` (the artifact digest the
device committed to at export time). This is verified end-to-end.

The receipt carries two OPTIONAL trailing fields (added after v1 shipped; both are part of the
signed receipt bytes when present):

- `auth_mode`: how the export was authorized — `"break_glass"` (trustee quorum),
  `"self_export"` (owner, authenticated by possession of the device key seed), or `"api"`
  (local capability-token API). Lets a verifier distinguish owner-authorized disclosure from
  quorum disclosure.
- `window`: `{ start_epoch_s, end_epoch_s }`, the half-open, bucket-aligned time window the
  export was restricted to. Both bounds MUST be multiples of the 600 s bucket size, so the
  window discloses nothing finer than the buckets themselves.

Compatibility rule: a receipt with neither field is a **legacy receipt** and remains valid
forever (pinned by the `valid_envelope_legacy.json` fixture in both verifiers). Verifiers MUST
serialize these fields only when present and in this order, after `artifact_hash` — the receipt
entry hash is computed over the exact serialized bytes, so field order and absence semantics are
part of the signed format. Verifiers predating these fields will reject bundles that carry them
(re-serialization drops the unknown fields and the entry hash no longer matches); verify such
bundles with a current verifier.

## 9. `whole_envelope_digest` — Tier 2, canonical

The single fingerprint a custodian cites (e.g. in court). Computed as:

`whole_envelope_digest = SHA256( CJSON( digest_input ) )`

where `digest_input` is a JSON object containing every envelope field **except**
`whole_envelope_digest` itself, with the `artifact` replaced by its hex `artifact_hash`
(= `export_receipt_entry.receipt.artifact_hash`). The artifact is bound by reference, so the
digest input contains **no floating-point numbers** — eliminating cross-language number-format
drift (the `confidence: f32` hazard).

`CJSON` ("securacv-cjson-v1") is the canonical serialization:
- Object keys sorted by UTF-16 code-unit order; no insignificant whitespace.
- Strings escaped per RFC 8785 §3.2.2.2, which mirrors ECMAScript `JSON.stringify`: the
  two-character escapes `\"` `\\` `\b` `\t` `\n` `\f` `\r` are used for those characters, and any
  other control character in U+0000–U+001F is emitted as `\uHHHH` (lowercase hex). (These short
  escapes are required, not forbidden — they are exactly what a JS verifier produces.)
- Only objects, arrays, strings, booleans, null, and **integers** are permitted; floating-point
  numbers are rejected at serialization time. Integers MUST be within the JavaScript safe-integer
  range (±(2^53−1)); out-of-range integers are rejected, since the JS verifier parses numbers as
  IEEE-754 doubles and would otherwise lose precision and diverge.

This is a deliberate subset of RFC 8785 that is trivially reproducible in JavaScript
(`JSON.stringify` with a recursive key-sorting replacer over integer/string values).

## 10. Versioning & compatibility ("no oops-we-forgot")

- `envelope_format` is fixed; an incompatible layout MUST bump `envelope_version` and SHOULD
  bump `canonicalization` / domain version suffixes as needed.
- A verifier that does not recognize `envelope_version` MUST refuse loudly rather than guess.
- Because the manifest is embedded and digested, old bundles remain fully interpretable forever
  using their own carried rules.

## 11. Verification algorithm (both verifiers MUST implement identically)

1. Reject unknown `envelope_format` / `envelope_version`.
2. Recompute `whole_envelope_digest` (§9) and compare.
2b. **Device-key lineage.** `provenance.device_public_key` is the key current at SEAL time; a
   device that rotated its signing key exports ledgers whose rows are signed by earlier keys,
   and the evidence for trusting those keys travels IN the envelope as `key_rotation` sealed
   records (internally tagged: `record_type: "key_rotation"`, with `prev_public_key`,
   `new_public_key`, the NEW key's `new_key_attestation`, and the PREV key's
   `prev_key_authorization`; binding message
   `domain_separated_hash(domain, SHA256(prev_public_key || new_public_key))` under domains
   `securacv:pwk:device-key-rotation:v1` / `…-authz:v1`). Verifiers reconstruct the lineage by
   walking those records **backward from the provenance anchor**: a rotation extends the lineage
   only if its `new_public_key` equals the currently-trusted key, its attestation verifies, and
   its authorization (when present — legacy records predate the field) verifies under the
   announced predecessor; a chaining rotation failing either check is a hard failure. A rotation
   that does not chain cannot extend the lineage — its row still needs a valid signature under
   the key active at its position, so a fabricated record breaks the walk. Legacy records
   (empty authorization) are anchored by their row's entry signature under the predecessor,
   which step 3 enforces because that row's assigned signer IS the predecessor.
   (Pinned cross-language by `valid_envelope_rotated.json`.)
3. For each ledger, walk entries in order: check `prev_hash` linkage (from checkpoint head or
   genesis), recompute `entry_hash = SHA256(prev_hash || payload_json)`, verify the SignatureSet
   over `domain_separated_hash(domain, entry_hash)` using the manifest's domain string.
   **Signer selection:** for `sealed_events` the signer of each row is exact — the lineage key
   active at that row, switching to the successor AFTER each chaining `key_rotation` row (the
   rotation record itself is signed by the retiring key); for the receipt ledgers, whose rows
   carry no ordering relative to the rotations, any validated lineage key is acceptable and a
   signature valid under none of them is a failure. PQ is
   verified when present and a PQ key is available; otherwise it is reported as "not checked"
   (mirrors `SignatureMode::Compat`), never silently passed. (The PQ key does not rotate.)
4. Checkpoint: verify its signature over `chain_head_hash` with the checkpoint domain, accepting
   any validated lineage key (the checkpoint is signed by the key current when it was written).
5. Break-glass: recompute `approvals_commitment` and confirm it matches each receipt; tally
   granted/denied.
6. Disclosure: confirm any `omitted_entry_hashes` link the chain across the omission.
7. Artifact: confirm `SHA256(serde_json_bytes(artifact)) == export_receipt_entry.receipt.artifact_hash`
   and verify the export-receipt entry signature.

Overall status: `ok` (all pass, no gaps/warnings) / `valid_with_warnings` (valid but gaps,
PQ-unchecked, or redactions present) / `compromised` (any chain, hash, or signature failure).
