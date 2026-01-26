# Cat Litter Box Witness Demo (XIAO ESP32C3 + Grove Vision AI V2)

This demo shows a **privacy-preserving cat litter box witness** that stays inside the
Privacy Witness Kernel (PWK) contract. The device emits **event-only** claims and
never exports raw frames. Evidence capture, if needed, is handled by the local PWK
vault under break-glass conditions.

## Architecture (Conforming)

**Edge device (XIAO ESP32C3 + Grove Vision AI V2):**

- Runs a cat/no-cat classifier on the Grove Vision AI V2 module.
- Detects transitions (cat present ↔ absent).
- Emits **one JSON event** per transition to the host over USB serial.
- Emits **no raw frames** and **no extra metadata**.

**Host (SecuraCV / witnessd):**

- Reads newline-delimited JSON from the device.
- Uses `grove_vision2_ingest` to append events to the sealed log.
- Rejects any payload with extra fields (conformance alarm).

This keeps raw media inside the kernel boundary and produces only contract-compliant
claims.

## Event contract (strict)

Grove Vision 2 ingest accepts **only** these fields:

- `event_type`
- `time_bucket`
- `zone_id`
- `confidence`

No device IDs, timestamps, session IDs, or free-form fields are allowed. A
conforming litter box event looks like:

```json
{
  "event_type": "boundary_crossing_object_small",
  "time_bucket": { "start_epoch_s": 1730000400, "size_s": 600 },
  "zone_id": "zone:litterbox",
  "confidence": 0.82
}
```

Notes:

- `event_type` must be one of the **allowed vocabulary** values.
- `time_bucket` is coarse by design (e.g., 10 minutes).
- `zone_id` is a **local** identifier only (no location data).

## Host ingest (SecuraCV)

Run the ingest binary and pipe serial JSON into it:

```bash
# Example for a USB serial device on Linux.
# Replace /dev/ttyACM0 with your actual device path.
# The device must output one JSON object per line.

stty -F /dev/ttyACM0 115200
stdbuf -oL cat /dev/ttyACM0 | \
  DEVICE_KEY_SEED="local-demo-seed" \
  cargo run --release --bin grove_vision2_ingest
```

Any non-conforming payloads are rejected and logged as conformance alarms.

## Smoke Test

Use this to confirm ingestion and conformance enforcement without changing
device firmware.

### Known-good event (append to DB)

**Option A: manual pipe (no device needed)**

```bash
printf '%s\n' '{"event_type":"boundary_crossing_object_small","time_bucket":{"start_epoch_s":1730000400,"size_s":600},"zone_id":"zone:litterbox","confidence":0.82}' | \
  DEVICE_KEY_SEED="local-demo-seed" \
  cargo run --release --bin grove_vision2_ingest
```

**Option B: device serial (device running, ingest already started)**

```bash
printf '%s\n' '{"event_type":"boundary_crossing_object_small","time_bucket":{"start_epoch_s":1730000400,"size_s":600},"zone_id":"zone:litterbox","confidence":0.82}' > /dev/ttyACM0
```

You should see an ingest log line on stdout/stderr confirming an accepted event.
Confirm the new entry in the DB with `sqlite3` or `log_verify`, for example:

```bash
sqlite3 witness.db "select id, created_at from sealed_events order by id desc limit 1;"
cargo run --release --bin log_verify -- --db witness.db
```

### Invalid payload (conformance rejection)

Send an event with an extra field (should be rejected and logged as a conformance alarm):

```bash
printf '%s\n' '{"event_type":"boundary_crossing_object_small","time_bucket":{"start_epoch_s":1730000400,"size_s":600},"zone_id":"zone:litterbox","confidence":0.82,"device_id":"forbidden"}' | \
  DEVICE_KEY_SEED="local-demo-seed" \
  cargo run --release --bin grove_vision2_ingest
```

Look for conformance rejection in stdout/stderr. You can also confirm the alarm
with `sqlite3` (table `conformance_alarms`) or via the `log_verify` warnings.

## Verify Log Integrity

Run `log_verify` against the demo DB. It defaults to `witness.db`, but this uses
`WITNESS_DB_PATH` if you set it:

```bash
cargo run --release --bin log_verify -- --db "${WITNESS_DB_PATH:-witness.db}"
```

Expected success output (counts may vary):

```text
log_verify: checking witness.db

=== Sealed Events ===
checkpoint: none (genesis chain)
verified 1 event entries

=== Break-Glass Receipts ===
verified 0 receipt entries (0 granted, 0 denied)

=== Export Receipts ===
verified 0 export receipt entries
OK: all chains verified.
```

Troubleshooting: if you see an error like `device public key not found in database
(provide --public-key or --public-key-file if the database has no key)`, pass the
device public key explicitly (e.g., `--public-key <hex>` or `--public-key-file <path>`).

## Disconnect/Reconnect

`grove_vision2_ingest` exits on stdin EOF and does not attempt to reconnect to
serial devices. If the USB serial link drops, the pipeline will terminate and
the ingest process will stop.

Minimal restart procedure:

```bash
stty -F /dev/ttyACM0 115200
stdbuf -oL cat /dev/ttyACM0 | \
  DEVICE_KEY_SEED="local-demo-seed" \
  cargo run --release --bin grove_vision2_ingest
```

For unattended deployments, use a supervisor (systemd, runit, etc.) to restart
the pipeline on exit.

Expected behavior on link loss: when the USB serial link drops, `cat` receives
EOF, `grove_vision2_ingest` exits, and no events are ingested until you restart
the pipeline. When the link returns, the device may resume emitting events, but
the host side must be restarted to receive them.

## Firmware sketches

Two sketches are provided to keep the demo aligned with the PWK contract:

- **ESP32C3 bridge**: polls the Grove Vision AI V2 module and emits conforming
  JSON events (no extra fields).
- **Grove Vision AI V2**: a minimal AT responder pattern that returns a single
  `cat:<score>` line for `AT+INFER?`.

See:

- `examples/firmware/esp32c3_grove_vision_ai_v2_litterbox/esp32c3_grove_vision_ai_v2_litterbox.ino`
- `examples/firmware/grove_vision_ai_v2_litterbox_firmware/grove_vision_ai_v2_litterbox_firmware.ino`

**Time sync requirement (ESP32C3):** the bridge firmware requires Wi‑Fi to reach
an NTP server and set the system clock **before it will emit events**. If NTP
fails or Wi‑Fi is unavailable, you will see **no events**. Troubleshooting options
that stay within the event contract include: setting the time bucket locally in
firmware, or sourcing time from the host serial bridge and deriving buckets there
—without adding any new metadata fields.

### Wiring

Connect the XIAO ESP32C3 UART pins to the Grove Vision AI V2 UART pins as follows:

- **XIAO ESP32C3 GPIO6 (RX)** ← **Grove Vision AI V2 TX**
- **XIAO ESP32C3 GPIO7 (TX)** → **Grove Vision AI V2 RX**
- **GND ↔ GND** (common ground)

Both boards use **3.3V UART logic**. Do not connect 5V TTL UART signals directly; level
shift if your setup introduces 5V logic. The ESP32C3 sketch defines these pins as
`VISION_RX` and `VISION_TX` in
`examples/firmware/esp32c3_grove_vision_ai_v2_litterbox/esp32c3_grove_vision_ai_v2_litterbox.ino`
so you can reconcile the wiring with the code.

## Why this is privacy-preserving

- **No raw media export**: the device never transmits frames.
- **No identity substrate**: there are no device IDs or identifiers in events.
- **Coarse time**: time is bucketed; no precise timestamps exist in the event.
- **Contract enforcement**: the ingest tool rejects extra metadata.

If you need evidence capture later, do it through the PWK vault and break-glass
workflow—not on the device.
