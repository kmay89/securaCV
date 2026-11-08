# tools/log_verify

This directory documents the external verifier tool `log_verify`.

## Purpose
`log_verify` is an **external** checker that proves:
- the sealed event log is hash-chained (tamper-evident)
- the break-glass receipt log is hash-chained (tamper-evident)
- the export receipt log is hash-chained (tamper-evident)
- each entry is signed by the device key (Ed25519)
- checkpoints preserve verifiability across retention pruning
- every RFC 3161 anchor still points at a state this database contains
  (structural; see "Anchors are part of the verdict" below)

This is not a convenience feature.
It is a core anti-erosion mechanism: integrity must be provable without trusting the runtime.

## CLI
The project provides `log_verify` as a cargo binary:

```bash
cargo run --bin log_verify -- --db witness.db
```

Options:
- `--db <path>`: path to SQLite DB (default `witness.db`)
- `--public-key <hex>`: hex-encoded device Ed25519 verifying key
- `--public-key-file <path>`: path to file containing the hex-encoded device public key
- `--json`: print the machine-readable `VerifyReport` instead of the human
  report (exit 1 on chain failure, 2 on `--strict` warnings)
- `--strict`: treat warnings as failures (exit 2) — timeline-audit warnings
  and anchor-row anomalies alike; the error counts them apart
  (`… ({t} timeline, {a} anchor)`)

If neither `--public-key` nor `--public-key-file` is provided, `log_verify` will read the
device public key from the local database metadata table (`device_metadata.public_key`).

## What it checks
1) If a checkpoint exists, verify its signature.
2) Iterate the remaining `sealed_events` in ascending id:
   - verify each `prev_hash` matches the running expected chain head
   - recompute `entry_hash = SHA256(prev_hash || payload_json)`
   - verify the Ed25519 signature over `entry_hash`
3) Report success/failure.

For break-glass receipts, `log_verify`:
1) Iterates `break_glass_receipts` in ascending id:
   - verify each `prev_hash` matches the running expected chain head
   - recompute `entry_hash = SHA256(prev_hash || payload_json)`
   - verify the Ed25519 signature over `entry_hash`
   - verify the approvals commitment matches the stored approvals JSON
2) Report success/failure.

For export receipts, `log_verify`:
1) Iterates `export_receipts` in ascending id:
   - verify each `prev_hash` matches the running expected chain head
   - recompute `entry_hash = SHA256(prev_hash || payload_json)`
   - verify the Ed25519 signature over `entry_hash`
2) Report success/failure.

For RFC 3161 anchors (when the database has a `tsa_anchors` table),
`log_verify` then walks every anchor row — see the next section.

## Anchors are part of the verdict (structural)

An anchor (`docs/timestamping.md`) is auxiliary evidence *about* a ledger: a
TSA countersigned "this ledger head existed at or before `genTime`". Since
this release `run_full_verify` — and so `log_verify`, witnessd's boot verify
and the API's `POST /verify` — checks that evidence **structurally** against
the ledgers, and a head-kind anchor whose state this database no longer
contains fails the verdict:

1. **Row shape.** Every column is read by its SQLite storage class and typed;
   a damaged row (`subject_hash` not a 32-byte BLOB, `created_bucket_start`
   not an INTEGER, …) is a **warning**, never a failure.
2. **Token imprint.** The stored token parses as an RFC 3161 token and its
   message imprint equals the hash the row claims. A token that does not
   parse, or covers a different hash, is a **warning**.
3. **Ledger membership (head kinds only).** `chain_head`,
   `export_receipt_head`, `break_glass_receipt_head` and `policy_head` rows
   must name a hash in the history of the ledger the row *declares*: a live
   row, or — chain head only — a retention checkpoint whose device signature
   verifies under a genesis-anchored lineage key. A hash in neither is an
   **`anchor_regression` failure**: rollback, truncation, or a different
   history than the one that was anchored. `digest` rows (export bundles,
   empty-ledger sentinels) assert no membership and stop at step 2.

Membership is keyed on signed evidence only. The unsigned `ledger_id` column
never turns a failure into a pass; it only sharpens the diagnosis text (the
anchored row at or below / newer than the retention cutoff, or no position
recorded). Every detail string carries the anchor row id and at most a
16-hex-character prefix of the hash — the report is published retained over
MQTT and carries no full hashes and no `genTime`.

The human report prints `anchors: N checked (structural; countersignatures:
log_anchor --db <db> verify --ca <tsa-ca.pem>)`; the JSON report gains
`anchors_checked` (omitted when the database has no anchors table, so a
report from such a database is byte-identical to before; on a failed report
it counts the rows checked before the failing one). Warnings about damaged
rows print under `=== Anchor Audit ===`, apart from the timeline audit.

**Structural only.** The TSA's CMS countersignature is *not* checked here:
the kernel never links an X.509 / CMS implementation, and the delegated
second implementation is `openssl ts -verify`, run by `log_anchor verify
--ca` (which image carries the `openssl` CLI: `docs/timestamping.md`). A
`valid` verdict says every anchor still points at a state this database
holds; it does not say any TSA vouched for it. Run both.

**Legacy prune versus rollback.** A build older than the one that keeps
anchored chain heads as checkpoints pruned them for good. Such a head now
fails the verdict, with the diagnosis naming the anchored row as at or
below the retention cutoff; `log_anchor relabel --id N --subject digest`
records that membership is no longer asserted (it refuses a head newer than
the cutoff, any head when no checkpoint exists, and every receipt-ledger
and policy head — those are rollback or truncation, not a legacy prune).
Never delete an anchor row to clear a finding: that destroys evidence.

## Timeline audit (warnings)

Hash-chain verification proves interior integrity but cannot see **tail
truncation**: deleting the newest N rows leaves a chain that still verifies.
After the chains pass, `log_verify` runs a timeline audit over the (now
trusted) records and prints warnings for:

- **Stale tail** — the last lifecycle record is `start` but the newest record
  is more than two buckets old: possible tail truncation, crash, or a daemon
  stopped without a shutdown record.
- **Missing heartbeats** — buckets inside a `start`..`shutdown_clean` segment
  with no heartbeat record (the daemon seals one per 10-minute bucket):
  possible mid-chain deletion. Skipped on databases that predate heartbeats.
- **`created_at` regressions** — softened to a note when a `ClockSkew`
  failure record covers the jump (the daemon witnessed it itself).
- **Checkpoint anomalies** — back-dated or future-dated checkpoint
  timestamps.

Warnings do not fail verification by default; pass `--strict` to exit 2
when any are present. Each warning is printed with a one-line actionable
hint (e.g. for missing heartbeats: check for PowerLoss/StorageFull failure
records near the gap before suspecting deletion).

Anchor-row anomalies (a malformed `tsa_anchors` row, a stored token that does
not parse or does not cover the hash its row claims) are warnings too, printed
under their own `=== Anchor Audit ===` heading because they are a different
finding: a damaged auxiliary row, not a gap in the record. `--strict`
promotes both kinds and its error counts them apart: `verification passed
but --strict treats 3 warning(s) as failure (1 timeline, 2 anchor)`.

## Diagnosing failures

When verification fails, `log_verify` prints a diagnosis block before exiting:
**where** the chain broke (ledger + entry id), **what kind** of check failed,
the likely causes in plain language, and concrete next steps. Example:

```
=== Diagnosis ===
Where:           sealed event log at entry id 42
What this means: the stored payload at this entry no longer matches the hash
                 that was sealed into the chain when it was written.
Likely causes:   the row was edited in place; disk corruption; a partial
                 restore mixed rows from different database generations.
Next steps:      do NOT edit the database to "fix" it. Check disk health,
                 compare against backups, and run with --verbose to see how
                 far the chain verifies.
Error: verification FAILED: integrity check failed at id 42: computed_hash=…
```

Failure kinds and the gist of their guidance:

| Kind | Meaning | First thing to try |
|------|---------|--------------------|
| `prev_hash_mismatch` | entry doesn't link to its predecessor — rows deleted/reordered, or DB older than its checkpoint | compare against backups; `--verbose` shows the last good entry |
| `entry_hash_mismatch` | stored payload no longer matches its sealed hash | check disk health; never edit the DB |
| `signature_mismatch` | hashes fine, signature doesn't verify under the expected key | retry with `--device-key-seed`, or drop `--public-key` |
| `key_rotation_invalid` | a device-key rotation record failed validation | treat later entries as unattributed; see `docs/db_key_rotation.md` |
| `checkpoint_invalid` | retention checkpoint signature/shape failed | retry with the device seed; if it still fails, pruned history is unverifiable |
| `approvals_commitment_mismatch` | break-glass approvals altered after the fact | compare against trustees' own records |
| `policy_violation` | receipt doesn't satisfy the quorum policy (or no policy configured) | restore the original policy configuration |
| `untrusted_signer` | checkpoint or high-water-mark signed by a key outside the device lineage | do not trust the checkpoint or this database's origin |
| `high_water_regression` | the live log is behind the signed external high-water-mark (`--high-water-mark`): tail truncation, rollback or wipe | recover the newer database; never re-sign a shorter chain |
| `anchor_regression` (ledger `anchors`, `entry_id` = the `tsa_anchors` row id) | a head-kind anchor covers a hash that is not in its declared ledger's history, or that survives only in a checkpoint whose signature does not verify: rollback, truncation, or a different history than the one anchored | `log_anchor verify` for the per-anchor picture; `relabel` only for a head at or below the cutoff of a database pruned by an older build; never delete the row |

With `--json`, the same information is machine-readable: the report gains a
`failure` object (`{ledger, entry_id, kind, detail}`) next to the unchanged
`error` string. `ledger` is one of `sealed_events`, `break_glass_receipts`,
`export_receipts`, `checkpoint`, `key_lineage`, `high_water_mark`, `anchors`. The HTTP API's `POST /verify` returns the same report.

## Future work
- Support multiple checkpoints and archived compacted segments.
