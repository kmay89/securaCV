# Seeed XIAO Vision AI Setup Guide

This guide covers connecting a Seeed XIAO Vision AI device to the Privacy Witness Kernel
using the supported ESP32 ingestion backends.

## Supported ingestion modes

The ESP32 ingestion backend supports only the following modes:

- **HTTP MJPEG/JPEG** (`ingest-esp32` feature)
- **UDP RTP/JPEG** (`ingest-esp32` feature)

These are the same modes documented for the ESP32-S3 backend. No other transport or
codec is supported in this repository.

## Event-only mode (Grove Vision AI V2)

If you want a **strict event-only** pipeline (no raw frame transport), use the
`grove_vision2_ingest` binary with a device that emits contract-compliant JSON
over serial. See the cat litter box demo for a full example and firmware
sketches:

- [`docs/litterbox_witness_demo.md`](litterbox_witness_demo.md)

Serial disconnect handling for event-only mode:

- When ingesting via `cat ... | grove_vision2_ingest`, a USB disconnect produces
  EOF, the ingest process exits, and you must restart it after the device
  returns.
- When using `grove_vision2_ingest --serial-device /dev/ttyACM0`, the process
  stays up, logs the disconnect, and retries until the device reappears. Prefer
  `/dev/serial/by-id/...` if you want a stable device path across reconnects.

## Event-only ingest smoke test (JSON → DB → alarms)

This short walkthrough demonstrates: (a) the device emits contract-compliant JSON,
(b) `grove_vision2_ingest` appends events to the DB, and (c) conformance alarms are
recorded for invalid payloads. The commands below use a throwaway DB file and stdin
to keep the sequence self-contained.

### 1) Confirm the device emits contract-compliant JSON

With the device connected over USB (adjust the tty path and baud rate to match your
firmware), grab a single line of JSON:

```bash
stty -F /dev/ttyACM0 115200 raw -echo
timeout 2s cat /dev/ttyACM0 | head -n 1
```

Expected output (shape must match the contract; values will vary):

```
{"event_type":"boundary_crossing_object_large","time_bucket":{"start_epoch_s":1700000000,"size_s":600},"zone_id":"zone:entry","confidence":0.82}
```

### 2) Ingest a valid event and verify it landed in the DB

```bash
printf '%s\n' \
  '{"event_type":"boundary_crossing_object_large","time_bucket":{"start_epoch_s":1700000000,"size_s":600},"zone_id":"zone:entry","confidence":0.82}' \
  | DEVICE_KEY_SEED=devkey:demo cargo run --bin grove_vision2_ingest -- \
    --db-path demo_grove.db
```

Expected output:

```
INFO  grove_vision2_ingest > Grove Vision 2 event appended
```

Confirm a sealed event was written:

```bash
sqlite3 demo_grove.db "SELECT COUNT(*) FROM sealed_events;"
```

Expected output:

```
1
```

### 3) Send an invalid payload and confirm a conformance alarm

```bash
printf '%s\n' \
  '{"event_type":"boundary_crossing_object_small","time_bucket":{"start_epoch_s":1700000000,"size_s":600},"zone_id":"zone:entry","confidence":0.3,"extra":"nope"}' \
  | DEVICE_KEY_SEED=devkey:demo cargo run --bin grove_vision2_ingest -- \
    --db-path demo_grove.db
```

Expected output:

```
(no “event appended” line)
```

Now query the alarm table:

```bash
sqlite3 demo_grove.db \
  "SELECT code, message FROM conformance_alarms ORDER BY id DESC LIMIT 1;"
```

Expected output:

```
CONFORMANCE_GROVE_VISION2_INPUT_REJECT|conformance: grove_vision2 payload contains extra fields: extra
```

### 4) Verify log integrity for the demo DB

Run `log_verify` against the Arduino demo database path used above
(`demo_grove.db`):

```bash
cargo run --bin log_verify -- --db demo_grove.db
```

Expected output (counts may vary):

```
log_verify: checking demo_grove.db

=== Sealed Events ===
checkpoint: none (genesis chain)
verified 1 event entries

=== Break-Glass Receipts ===
verified 0 receipt entries (0 granted, 0 denied)

=== Export Receipts ===
verified 0 export receipt entries
OK: all chains verified.
```

Troubleshooting:

- If you see `device public key not found in database`, pass the device key with
  `--public-key <hex>` or `--public-key-file <path>`.
- If `demo_grove.db` does not exist, re-run the ingest step above with
  `--db-path demo_grove.db` to create it.

## Vendor resources

Use the vendor-provided resources for firmware and hardware references:

- Firmware repository: **https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2**
- Datasheet: **https://files.seeedstudio.com/wiki/grove-vision-ai-v2/HX6538_datasheet.pdf**

## Flash firmware and enable streaming

Follow the vendor instructions to flash the Seeed XIAO Vision AI firmware and enable
a camera stream endpoint. Two common approaches are supported:

### Arduino IDE

1. Install the Seeed board support package in Arduino IDE.
2. Open the vendor-provided sketch/project for the camera firmware.
3. Set Wi-Fi credentials and configure the camera stream mode.
4. Build and upload to the device.
5. Ensure the device exposes either:
   - an MJPEG stream endpoint (e.g., `http://<device-ip>:81/stream`), **or**
   - a JPEG snapshot endpoint (e.g., `http://<device-ip>/capture`).

### Seeed SDK

1. Install the Seeed SDK and required toolchain.
2. Configure the project for camera streaming over MJPEG or JPEG snapshot mode.
3. Build and flash the firmware to the device.
4. Confirm the HTTP endpoint is reachable at `/stream` (MJPEG) or `/capture` (JPEG).

If your firmware supports UDP RTP/JPEG, configure it to send RTP/JPEG (payload type 26)
packets to the host running `witnessd`.

## Minimal `witness.toml`

Use the ESP32 backend with the device stream URL and a conservative target FPS:

```toml
[ingest]
backend = "esp32"

[esp32]
url = "http://<device-ip>:81/stream"
target_fps = 10
```

## Invariant requirements (must-hold)

This repository enforces strict invariants for privacy preservation:

- **No raw-frame persistence** (frames never land on disk outside defined vault paths).
- **Time bucketing** (capture timestamps are coarsened).
- **Lossy feature hashing** (non-invertible, intentionally unstable hashes).

Review the authoritative constraints here:

- [`spec/invariants.md`](../spec/invariants.md)
- [`spec/threat_model.md`](../spec/threat_model.md)

## Troubleshooting

- **Common URLs**: try `http://<device-ip>:81/stream`, `http://<device-ip>/stream`, or
  `http://<device-ip>/capture` depending on firmware defaults.
- **Network/firewall**: ensure the device and host are on the same subnet and that
  any host firewall allows inbound connections to the MJPEG port (often `81`).
- **Validate the feed first**: open the stream/snapshot URL directly in a browser or
  with a tool like `curl` before starting `witnessd`.
- **RTP/JPEG**: if using UDP, confirm the host is listening on the configured port and
  that the firmware is sending RTP/JPEG payload type 26 to that address.
