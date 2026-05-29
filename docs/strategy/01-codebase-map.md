# 01 — Codebase Map

Goal: let a newcomer orient in ~10 minutes. SecuraCV is a multi-language monorepo. The
center of gravity is the Rust **Privacy Witness Kernel**; everything else ingests into it,
runs it, or displays its output.

## One paragraph

Cameras (IP/RTSP, USB, or ESP32 "Canary" devices) feed frames or detection metadata into
the kernel (`witnessd`). The kernel converts raw detections into privacy-preserving semantic
*claims* (e.g. `BoundaryCrossingObjectLarge`), strips identity and precise time, and appends
each claim to a hash-chained, Ed25519-signed, SQLCipher-encrypted log. Sensitive payloads are
sealed into encrypted vault envelopes that require multi-party "break-glass" to open. Home
Assistant surfaces the events, chain status, and device health as sensors and notifications.

## Data flow

```
Camera (RTSP / V4L2 / ESP32 / Frigate-MQTT)
   → witnessd  (in-memory frames → detect → contract enforcement)
   → hash-chained, Ed25519-signed, SQLCipher log   (+ sealed vault for sensitive payloads)
   → Home Assistant (sensors, verification, daily digest, pattern alerts)
   → your phone (HA Companion push)
```

## Component map

| Path | Purpose | Language / runtime | Entry point | How it runs |
|------|---------|--------------------|-------------|-------------|
| `src/` | Privacy Witness Kernel: log, crypto, vault, ingest, detect, API, break-glass | Rust 2021 (v0.5.0) | `src/lib.rs` + 8 bins in `src/bin/` | `cargo run --bin witnessd` |
| `src/bin/` | CLIs: `witnessd` (daemon), `log_verify`, `break_glass`, `export_events`, `frigate_bridge`, `event_mqtt_bridge`, `witness_api`, `demo` | Rust | each `*.rs` | `cargo run --bin <name>` |
| `firmware/` | ESP32 "Canary" device firmware (Vision, WAP, OTA) + shared `common/` | C/C++ (PlatformIO / Arduino) | `firmware/projects/*` | flash to ESP32 |
| `canary-vision/` | Device-hosted HTTP API + vanilla-JS SPA (incl. the event timeline UI) | Node.js / Express + JS | `canary-vision/` | runs on device / Node |
| `custom_components/securacv/` | Home Assistant integration (sensors, config flow) | Python 3 | `__init__.py` | HACS / copy to HA |
| `privacy_witness_kernel/` | Home Assistant add-on wrapping `witnessd` + setup wizard | Docker / shell | `run.sh`, `wizard/` | HA add-on store |
| `spec/` | Normative specs: invariants, event contract, threat model, break-glass, channels | Markdown | `spec/invariants.md` | read first |
| `kernel/` | Canonical architecture docs (`architecture.md`, `rf_presence_architecture.md`) | Markdown | — | read |
| `docs/` | Setup + philosophy guides (RTSP, V4L2, ESP32, HA, Frigate, why-witnessing) | Markdown | — | read |
| `integrations/ha_frigate_mqtt/` | Worked Frigate + HA MQTT example & verify script | YAML / shell | `verify_pipeline.sh` | example |
| `homeassistant/` | Example automations + Lovelace dashboard | YAML | — | template |
| `tests/` | Rust integration tests (mqtt e2e, hardening, frigate, tract) | Rust | — | `cargo test` |
| `tools/` | Dev tools (artwork, serial-monitor, flipper) | mixed | — | dev only |
| `sbom/` | SBOM generation process doc (CI-generated, none checked in) | Markdown | — | reference |
| `brands/` (incl. `brands/submission/`) | Logo assets + HA brands-repo submission package | PNG / docs | — | release/distribution |

## Where to start, by role

- **Kernel / Rust developer**: `spec/invariants.md` → `spec/event_contract.md` →
  `kernel/architecture.md` → `src/lib.rs` → `src/bin/witnessd.rs`. Run `cargo run --bin demo`
  then `cargo run --bin log_verify -- --db demo_witness.db` to see the witness + tamper proof.
- **Firmware developer**: `firmware/README.md` → `firmware/projects/canary-vision/` (Vision)
  or `firmware/projects/canary-wap/` (WAP). Shared code in `firmware/common/`.
- **Home Assistant user**: root `README.md` install → `docs/homeassistant_setup.md` →
  `docs/frigate_integration.md`. The add-on lives in `privacy_witness_kernel/`.
- **Anyone evaluating the idea**: `docs/why_witnessing_matters.md` then `spec/threat_model.md`.

## The seven invariants (the spine of the whole system)

Everything is constrained by `spec/invariants.md`, enforced in code, not policy:
no raw export, no identity substrate, metadata minimization (coarse time + zone IDs),
local ownership, break-glass by quorum, no retroactive expansion, non-queryable.
Read these before proposing any feature — `CONTRIBUTING.md` rejects changes that weaken them.
