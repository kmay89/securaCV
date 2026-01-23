# Witness Kernel

Core library and tools for the Privacy Witness Kernel (PWK).

## Documentation

* Canonical specifications:
  * `spec/invariants.md`
  * `spec/event_contract.md`
  * `spec/threat_model.md`
  * `kernel/architecture.md`
* Other docs should link to the canonical specifications rather than duplicate them.
* See `kernel/architecture.md` for the canonical vault confidentiality policy and ruleset identifier guidance.
* Contribution guidance is in `CONTRIBUTING.md`.
* Security policy is in `SECURITY.md`.
* Release notes are in `CHANGELOG.md`.

## Device public key location

The device Ed25519 **verifying key** is stored locally in the witness database at
`device_metadata.public_key` (row `id = 1`). External verification tools like
`log_verify` read this public key from the database by default, or accept an
explicit key via `--public-key` / `--public-key-file` as documented in
`log_verify_README.md`.

## Event export

Use the sequential export tool to write a local artifact with coarse time buckets
and batched events (no precise timestamps or identity selectors).

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin export_events -- --db-path witness.db --output witness_export.json
```

`export_events` emits a single JSON artifact with batched buckets, applying
default jitter and batching unless overridden by CLI flags.

## RTSP ingestion (GStreamer)

`witnessd` uses GStreamer to decode RTSP streams in-memory. Configure an RTSP URL
in `RtspConfig` and the kernel will produce `RawFrame` values without writing
frames to disk. Time coarsening and non-invertible feature hashing happen at
capture time, and `RtspSource::is_healthy()` reports stream health.

GStreamer support is gated behind the `rtsp-gstreamer` feature and requires
system GStreamer dependencies at runtime for real RTSP streams. The `stub://`
scheme keeps the synthetic source for tests and local development.
