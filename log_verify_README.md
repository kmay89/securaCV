# tools/log_verify

This directory documents the external verifier tool `log_verify`.

## Purpose
`log_verify` is an **external** checker that proves:
- the sealed event log is hash-chained (tamper-evident)
- each entry is signed by the device key (MVP placeholder signing)
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
- `--device-key-seed <seed>`: MVP only; must match witnessd seed (default `devkey:mvp`)

## What it checks
1) If a checkpoint exists, verify its signature.
2) Iterate the remaining `sealed_events` in ascending id:
   - verify each `prev_hash` matches the running expected chain head
   - recompute `entry_hash = SHA256(prev_hash || payload_json)`
   - recompute `signature = SHA256(device_key || entry_hash)` (MVP placeholder)
3) Report success/failure.

## Future work
- Replace MVP signing with Ed25519 signatures and a real public-key verifier.
- Support multiple checkpoints and archived compacted segments.
- Provide a JSON report suitable for audits and city procurement verification.
