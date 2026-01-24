# Witness Kernel

Core library and tools for the Privacy Witness Kernel (PWK).

## Quickstart: run the demo

```bash
cargo run --bin demo
```

This demo writes artifacts to `./demo_witness.db`, `./demo_out/export_bundle.json`, and the
vault directory at `./vault/envelopes` (unless you override `--out` / `--vault`). Verify the log
integrity with:

```bash
cargo run --bin log_verify -- --db demo_witness.db
```

### Tamper demo

```bash
cargo run --bin demo
cargo run --bin log_verify -- --db demo_witness.db
cargo run --bin export_verify -- --db demo_witness.db --bundle demo_out/export_bundle.json
printf "\n" >> demo_out/export_bundle.json
cargo run --bin export_verify -- --db demo_witness.db --bundle demo_out/export_bundle.json  # should FAIL
```

Next: for a real RTSP stream, see the RTSP ingestion section below.

## Documentation

* Canonical specifications:
  * `spec/invariants.md`
  * `spec/event_contract.md`
  * `spec/threat_model.md`
  * `kernel/architecture.md`
* Other docs should link to the canonical specifications rather than duplicate them.
* Host-compromise limitations are documented in `docs/root_paradox.md`.
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

## Break-glass policy management

The break-glass CLI stores the quorum policy in the kernel database so
`break_glass authorize` can evaluate approvals against a persistent trustee
roster. Manage the policy with `break_glass policy` subcommands:

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin break_glass -- policy set \
  --threshold 2 \
  --trustee alice:0123... \
  --trustee bob:4567... \
  --db witness.db

DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin break_glass -- policy show --db witness.db
```

Trustee entries use the format `id:HEX_PUBLIC_KEY`, where the public key is the
hex-encoded 32-byte Ed25519 verifying key.

## Event export

Use the sequential export tool to write a local artifact with coarse time buckets
and batched events (no precise timestamps or identity selectors).

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin export_events -- --db-path witness.db --output witness_export.json
```

`export_events` emits a single JSON artifact with batched buckets, applying
default jitter and batching unless overridden by CLI flags.

## Home Assistant Integration

Run the Privacy Witness Kernel as a Home Assistant add-on for easy camera integration:

1. Add the repository: `https://github.com/kmay89/securaCV`
2. Install "Privacy Witness Kernel" from the add-on store
3. Configure your cameras (auto-discovers from go2rtc/Frigate)
4. Enable MQTT Discovery for automatic sensor creation

### MQTT Discovery (Auto-Sensors)

PWK supports **Home Assistant MQTT Discovery** for automatic entity creation:

```yaml
# Add-on configuration
mqtt_publish:
  enabled: true
  host: "core-mosquitto"
```

This automatically creates these entities for each zone:
- `sensor.pwk_<zone>_events` - Event count (state_class: total_increasing)
- `binary_sensor.pwk_<zone>_motion` - Motion state (auto-off after 10 min)
- `sensor.pwk_last_event` - Most recent event with full attributes

Features:
- **QoS 1** for reliable message delivery
- **Last Will Testament** for availability tracking
- **Retained messages** for state persistence across HA restarts

### Frigate Integration

For users with Frigate NVR:

```yaml
mode: "frigate"
frigate:
  mqtt_host: "core-mosquitto"
  mqtt_username: ""  # Optional MQTT auth
  mqtt_password: ""
  min_confidence: 0.5
mqtt_publish:
  enabled: true  # Enable HA Discovery
```

See `docs/homeassistant_setup.md` for the full guide and `docs/frigate_integration.md` for Frigate-specific setup.

## RTSP Camera Setup

### Quick Start with Real Cameras

1. Install GStreamer dependencies:
   ```bash
   # Ubuntu/Debian
   sudo apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
       gstreamer1.0-plugins-good gstreamer1.0-plugins-bad libseccomp-dev
   ```

2. Build with RTSP support:
   ```bash
   cargo build --release --features rtsp-gstreamer
   ```

3. Configure your camera (create `witness.toml`):
   ```toml
   [rtsp]
   url = "rtsp://admin:password@192.168.1.100:554/stream1"
   target_fps = 10
   width = 640
   height = 480

   [zones]
   module_zone_id = "zone:front_door"
   ```

4. Run:
   ```bash
   export DEVICE_KEY_SEED=$(openssl rand -hex 32)
   WITNESS_CONFIG=witness.toml cargo run --release --features rtsp-gstreamer --bin witnessd
   ```

See `docs/rtsp_setup.md` for camera URL patterns, troubleshooting, and advanced configuration.

### RTSP Architecture

`witnessd` uses GStreamer to decode RTSP streams in-memory. Configure an RTSP URL
in `RtspConfig` and the kernel will produce `RawFrame` values without writing
frames to disk. Time coarsening and non-invertible feature hashing happen at
capture time, and `RtspSource::is_healthy()` reports stream health.

GStreamer support is gated behind the `rtsp-gstreamer` feature and requires
system GStreamer dependencies at runtime for real RTSP streams. The `stub://`
scheme keeps the synthetic source for tests and local development.

## Container deployment

See `docs/container.md` for building and running the containerized `witnessd`
artifact with RTSP configuration and Event API-only exposure.
