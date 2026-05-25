# SecuraCV Scanner — Flipper Zero Application

BLE scanner for SecuraCV Canary devices. Discovers nearby Canaries and decodes
debug beacon data to display device health on the Flipper Zero screen.

## What it shows

- Device name and RSSI signal strength (with bar graph)
- Debug mode indicator (`DBG`)
- When a debug-mode device is selected:
  - Subsystem status flags (WiFi, BLE, Mesh, Chirp, GPS, SD, Crypto)
  - Mesh peer count and RF device count
  - Free heap and uptime
  - Witness chain height and verification status
  - Error code and tamper alert

## Requirements

- Flipper Zero with firmware 1.x or later
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
2. The Flipper scans for BLE devices with `SCV-` or `SecuraCV-` name prefixes
3. Use **Up/Down** to scroll through discovered devices
4. Press **OK** to view detailed health data (requires Canary to be in debug mode)
5. Press **Back** to return to list or exit

## Activating debug mode on a Canary

Hold the **BOOT** button on the Canary device for 3 seconds. The LED will
blink twice every 2 seconds to indicate debug mode is active. The BLE
device name changes from `SecuraCV-Canary` to `SCV-DBG-XXXX`.

## BLE scan API note

The Flipper Zero's BLE observer API varies across firmware versions. The app
includes a scan stub that should be connected to your firmware's BLE GAP
observer. See the comment in `securacv_scanner.c` for integration guidance.

## Protocol

See `securacv_protocol.h` for the full 20-byte debug beacon wire format and
the corresponding header in `firmware/common/bluetooth/ble_debug_beacon.h`.
