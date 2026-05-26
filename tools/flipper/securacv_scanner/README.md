# SecuraCV Scanner — Flipper Zero Application

BLE scanner for SecuraCV Canary devices. Discovers nearby Canaries via
the BLE GAP observer API and decodes debug beacon manufacturer data to
display device health on the Flipper Zero screen.

## Target firmware

Official Flipper Zero firmware **1.x** (stable release).

Uses `furi_hal_bt_start_observer()` for passive BLE scanning and the
`Bt` service for radio lifecycle management. While scanning, the
Flipper's phone app connection is temporarily disconnected and
automatically restored when the app exits.

## What it shows

**Scan list:**
- Device names matching `SCV-*` or `SecuraCV-*` prefixes
- RSSI signal strength with bar graph and dBm value
- `DBG` badge for devices in debug mode

**Detail view** (press OK on a debug-mode device):
- Subsystem status flags (WiFi, BLE, Mesh, Chirp, GPS, SD, Crypto)
- Mesh peer count and RF device count
- Free heap and uptime
- Witness chain height and verification status
- Error code and tamper alert

## Requirements

- Flipper Zero with official firmware 1.x
- [ufbt](https://github.com/flipperdevices/flipperzero-ufbt) (micro Flipper Build Tool)

## Build

```bash
cd tools/flipper/securacv_scanner
ufbt build
```

## Install

Connect your Flipper Zero via USB, then:

```bash
ufbt install
```

Or copy the `.fap` file from `dist/` to your Flipper's SD card under
`apps/Bluetooth/securacv_scanner.fap` using qFlipper or the file manager.

## Usage

1. Launch the app from **Apps > Bluetooth > SecuraCV Scanner**
2. The Flipper disconnects from the phone app and begins BLE scanning
3. SecuraCV devices appear as they are discovered (filtered by name prefix)
4. Use **Up/Down** to scroll through discovered devices
5. Press **OK** to view detailed health data (requires Canary debug mode)
6. Press **Back** to return to list or exit
7. On exit, the Flipper reconnects to the phone app automatically

## Activating debug mode on a Canary

Hold the **BOOT** button on the Canary device for 3 seconds. The LED will
blink twice every 2 seconds to indicate debug mode is active. The BLE
device name changes from `SecuraCV-Canary` to `SCV-DBG-XXXX`.

## How it works

1. Opens the `Bt` service and disconnects the phone app to free the radio
2. Starts the BLE GAP observer via `furi_hal_bt_start_observer()`
3. Observer callback fires for every BLE advertising packet received
4. Callback extracts device name from AD type 0x09 (Complete Local Name)
5. Filters for `SCV-*` / `SecuraCV-*` name prefixes
6. Extracts manufacturer-specific data (AD type 0xFF) if present
7. Sends matching devices to the main loop via `FuriMessageQueue`
8. Main loop updates device list, parses debug beacons, expires stale entries
9. On exit, stops observer and restarts phone app advertising

## Device expiry

Devices not seen for 15 seconds are automatically removed from the list.
A periodic tick timer (3 second interval) handles expiry and UI refresh.

## Protocol

See `securacv_protocol.h` for the full 20-byte debug beacon wire format and
the corresponding header in `firmware/common/bluetooth/ble_debug_beacon.h`.
