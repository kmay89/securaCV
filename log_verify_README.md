# tools/log_verify

This directory documents the external verifier tool `log_verify`.

## Purpose
`log_verify` is an **external** checker that proves:
- the sealed event log is hash-chained (tamper-evident)
- the break-glass receipt log is hash-chained (tamper-evident)
- the export receipt log is hash-chained (tamper-evident)
- each entry is signed by the device key (Ed25519)
- checkpoints preserve verifiability across retention pruning

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

Warnings do not fail verification by default; pass `--strict` to exit
non-zero when any are present. Each warning is printed with a one-line
actionable hint (e.g. for missing heartbeats: check for PowerLoss/StorageFull
failure records near the gap before suspecting deletion).

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
| `untrusted_signer` | checkpoint signed by a key outside the device lineage | do not trust the checkpoint or this database's origin |

With `--json`, the same information is machine-readable: the report gains a
`failure` object (`{ledger, entry_id, kind, detail}`) next to the unchanged
`error` string. The HTTP API's `POST /verify` returns the same report.

## Future work
- Support multiple checkpoints and archived compacted segments.
