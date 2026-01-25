# Seeed XIAO Vision AI Setup Guide

This guide covers connecting a Seeed XIAO Vision AI device to the Privacy Witness Kernel
using the supported ESP32 ingestion backends.

## Supported ingestion modes

The ESP32 ingestion backend supports only the following modes:

- **HTTP MJPEG/JPEG** (`ingest-esp32` feature)
- **UDP RTP/JPEG** (`ingest-esp32` feature)

These are the same modes documented for the ESP32-S3 backend. No other transport or
codec is supported in this repository.

## Vendor resources

Use the vendor-provided resources for firmware and hardware references:

- Firmware repository: **<USER-PROVIDED-FIRMWARE-REPO-URL>**
- Datasheet: **<USER-PROVIDED-DATASHEET-URL>**

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
