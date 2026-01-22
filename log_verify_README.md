# tools/log_verify

This directory documents the external verifier tool `log_verify`.

## Purpose
`log_verify` is an **external** checker that proves:
- the sealed event log is hash-chained (tamper-evident)
- the break-glass receipt log is hash-chained (tamper-evident)
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

## Future work
- Support key rotation and explicit public-key rollover rules.
- Support multiple checkpoints and archived compacted segments.
- Provide a JSON report suitable for audits and city procurement verification.
