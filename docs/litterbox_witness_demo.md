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

## Step-by-step demo setup (non-technical friendly)

This section walks you through **connecting, flashing, and running** the two boards
for the demo. Read it once end-to-end, then follow the steps in order.

### What you need

- **XIAO ESP32C3** (USB-C)
- **Grove Vision AI V2** (USB-C)
- **3x jumper wires** (RX, TX, GND)
- **USB-C data cables** for each board
- A computer with the Arduino IDE installed

> **SD card note:** this demo does **not** use an SD card. You do not need to
> insert or format one.

### 1) Install Arduino support

1. Install the **Arduino IDE** (from arduino.cc).
2. In the Arduino IDE, open **Boards Manager** and install:
   - **ESP32 by Espressif Systems** (for the XIAO ESP32C3)
3. Open **Library Manager** and install:
   - **Seeed Arduino SSCMA** (for Grove Vision AI V2)

### 2) Flash the Grove Vision AI V2 firmware (sensor board)

1. Plug the **Grove Vision AI V2** into USB.
2. Open the sketch:
   `examples/firmware/grove_vision_ai_v2_litterbox_firmware/grove_vision_ai_v2_litterbox_firmware.ino`
3. In **Tools → Board**, select the Grove Vision AI V2 board (or the closest
   Seeed/XIAO-compatible target your IDE lists).
4. In **Tools → Port**, pick the port for the Grove Vision AI V2.
5. Click **Upload**.

**If upload fails:** press **RESET** once, then try Upload again. If it still
fails, hold **BOOT**, tap **RESET**, then release **BOOT** and retry Upload.

### 3) Prepare Wi‑Fi credentials for the ESP32C3

1. In `examples/firmware/esp32c3_grove_vision_ai_v2_litterbox/`, copy
   `secrets.example.h` to `secrets.h`.
2. Open `secrets.h` and enter your Wi‑Fi **SSID** and **password**.

> **Wi‑Fi placement:** for reliable time sync, place the ESP32C3 within normal
> Wi‑Fi range of your access point (ideally the same room). Avoid metal enclosures
> or stacking the boards behind a PC tower.

### 4) Flash the XIAO ESP32C3 firmware (bridge board)

1. Plug the **XIAO ESP32C3** into USB.
2. Open the sketch:
   `examples/firmware/esp32c3_grove_vision_ai_v2_litterbox/esp32c3_grove_vision_ai_v2_litterbox.ino`
3. In **Tools → Board**, select **Seeed XIAO ESP32C3**.
4. In **Tools → Port**, choose the ESP32C3 port.
5. Click **Upload**.

**If upload fails:** hold **BOOT**, tap **RESET**, release **BOOT**, then retry
Upload. This forces the ESP32C3 into bootloader mode.

### 5) Wire the boards together (UART)

With both boards **unplugged**, connect the UART pins as follows:

- **ESP32C3 GPIO6 (RX)** ← **Grove Vision AI V2 TX**
- **ESP32C3 GPIO7 (TX)** → **Grove Vision AI V2 RX**
- **GND ↔ GND**

Then plug both boards back into USB power.

### 6) Confirm the device is running

1. Open **Tools → Serial Monitor** for the ESP32C3.
2. Set baud to **115200**.
3. You should see **one JSON line per event** when the cat/no‑cat state changes.

> **Important:** the ingest pipeline expects **only JSON lines**. The firmware
> must not print extra status logs on the same serial port or conformance checks
> will reject the stream.

### 6a) Checking Wi‑Fi status (IP address / signal strength)

To keep the event contract strict, the firmware **does not** print Wi‑Fi
diagnostics on the serial port. Use one of these non-invasive options instead:

- **Router admin page:** look for the ESP32C3 in the client list to see its
  **IP address** and **signal strength (RSSI)** if your router exposes it.
- **Phone Wi‑Fi analyzer apps:** verify signal strength near the installation
  spot before placing the device.

If Wi‑Fi or NTP is failing, the ESP32C3 will **not** emit events. Fix Wi‑Fi
coverage and power-cycle the board.

### 7) Run the host ingest

Once you see valid JSON events in the serial monitor, close it and run:

```bash
stty -F /dev/ttyACM0 115200
stdbuf -oL cat /dev/ttyACM0 | \
  DEVICE_KEY_SEED="local-demo-seed" \
  cargo run --release --bin grove_vision2_ingest
```

Replace `/dev/ttyACM0` with your actual device path.

### 8) Power and placement tips

- Keep both boards powered by USB throughout the demo.
- Place the Grove Vision AI V2 where it can see the litter box, with stable
  lighting and minimal reflections.
- Keep the ESP32C3 within **reliable Wi‑Fi range** so it can reach NTP and
  emit events.

### 9) Reset/boot quick guide

- **Normal restart:** press **RESET** once.
- **Bootloader mode (for flashing):** hold **BOOT**, tap **RESET**, then release
  **BOOT**.

## Why this is privacy-preserving

- **No raw media export**: the device never transmits frames.
- **No identity substrate**: there are no device IDs or identifiers in events.
- **Coarse time**: time is bucketed; no precise timestamps exist in the event.
- **Contract enforcement**: the ingest tool rejects extra metadata.

If you need evidence capture later, do it through the PWK vault and break-glass
workflow—not on the device.
