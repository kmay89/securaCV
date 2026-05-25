# Flipper Zero Debug Guide for SecuraCV Canary

## What your Flipper Zero can and cannot see

SecuraCV Canary devices operate exclusively in the **2.4 GHz ISM band**.
A stock Flipper Zero (no attachments) has one relevant radio:

| Flipper Radio | Frequency | Canary Signal | Verdict |
|---|---|---|---|
| **Bluetooth (built-in)** | 2.4 GHz | BLE advertisements | **YES** |
| Sub-GHz (CC1101) | 300-928 MHz | Nothing | No |
| NFC | 13.56 MHz | Nothing | No |
| 125 kHz RFID | 125 kHz | Nothing | No |
| Infrared | IR | Nothing | No |

**Cannot see** (without WiFi Dev Board): ESP-NOW mesh traffic, WiFi AP beacons,
Chirp/Beacon community alerts, CSI motion detection frames.

## Quick start: stock Flipper BLE scan

Even without the custom app, the built-in Bluetooth scanner is useful:

1. On your Flipper, go to **Bluetooth** (from main menu)
2. Scroll to **Scan** and press OK
3. Wait 5-10 seconds for devices to appear
4. Look for:
   - `SecuraCV-Canary` — normal mode (device is alive and advertising)
   - `SCV-DBG-XXXX` — debug mode (health data available in manufacturer data)
   - `SCV-XXXX` — Opera-mode name (pubkey fingerprint suffix)

### What to check

| Observation | Meaning |
|---|---|
| Device appears with strong RSSI (> -60 dBm) | Device is nearby and BLE antenna is working |
| Device appears with weak RSSI (< -85 dBm) | Device is far away or antenna issue |
| Device does not appear at all | BLE not active, antenna disconnected, or WiFi/BLE coexistence issue |
| Name shows `SCV-DBG-` prefix | Debug mode is active, manufacturer data contains health |
| Name shows `SecuraCV-Canary` | Normal mode — device is operating but not emitting debug data |

## Activating firmware debug mode

Debug mode enriches BLE advertisements with a 20-byte health payload that
any BLE scanner can read. Two ways to activate:

### Option 1: Runtime (button hold)

1. Locate the **BOOT** button on your XIAO ESP32-S3
2. Press and hold for **3 seconds**
3. The LED will blink **twice every 2 seconds** to confirm debug mode
4. BLE device name changes from `SecuraCV-Canary` to `SCV-DBG-XXXX`
5. To deactivate, reboot the device (press RESET or power cycle)

### Option 2: Compile-time

In `firmware/configs/canary-wap/default/config.h`, set:

```c
#define FEATURE_BLE_DEBUG  1
```

Then rebuild and flash:

```bash
pio run -e canary-wap -t upload
```

Debug mode will activate automatically on every boot.

## Using the SecuraCV Scanner FAP

The custom Flipper Zero app decodes debug beacons into a human-readable display.

### Install

```bash
cd tools/flipper/securacv_scanner
ufbt build
ufbt install   # via USB
```

Or copy `dist/securacv_scanner.fap` to your Flipper's SD card:
`SD Card/apps/Bluetooth/securacv_scanner.fap`

### Controls

| Key | Scan List | Detail View |
|---|---|---|
| Up/Down | Scroll devices | — |
| OK | Open device detail | — |
| Back | Exit app | Return to list |

### Detail view fields

When viewing a debug-mode device, you see:

```
SCV-DBG-A3F7            [||||]
------------------------------
W+ B+ M+ C+ G- S+ K+
Mesh:3 peers  RF:7 devs
Heap:142KB  Up:01:23:45
Chain:1042 V:OK E:OK
```

| Field | Meaning |
|---|---|
| `W+`/`W-` | WiFi AP active/inactive |
| `B+`/`B-` | BLE active/inactive |
| `M+`/`M-` | Mesh network active/inactive |
| `C+`/`C-` | Chirp channel active/inactive |
| `G+`/`G-` | GPS fix healthy/unhealthy |
| `S+`/`S-` | SD card healthy/unhealthy |
| `K+`/`K-` | Crypto subsystem healthy/unhealthy |
| `Mesh:N` | Number of active mesh peers (0-8) |
| `RF:N` | Aggregate nearby RF devices (privacy-safe count) |
| `Heap:NKB` | Free heap memory in kilobytes |
| `Up:HH:MM:SS` | Device uptime |
| `Chain:N` | Witness chain height (records created) |
| `V:OK/FAIL/PENDING` | Chain verification status |
| `E:OK/...` | Error code (OK, CRYPTO, SD CARD, MESH, WIFI, LOW HEAP, CHAIN, WATCHDOG) |
| `!! TAMPER !!` | Tamper event currently active |

## Debug beacon wire format reference

20-byte manufacturer-specific BLE advertising data:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0-1 | 2 | Company ID | `0xFFFF` (little-endian, BLE SIG testing) |
| 2 | 1 | Magic | `0x5C` |
| 3 | 1 | Version | `0x01` |
| 4 | 1 | Subsystem flags | Bitfield (see above) |
| 5 | 1 | Mesh peer count | 0-8 |
| 6 | 1 | RF device count | Aggregate count |
| 7-8 | 2 | Free heap (KB) | Big-endian |
| 9-12 | 4 | Uptime (seconds) | Big-endian |
| 13-14 | 2 | Chain height | Big-endian |
| 15 | 1 | Chain verify | 0=ok, 1=fail, 0xFF=pending |
| 16-17 | 2 | Reserved | 0x0000 |
| 18 | 1 | Error code | See error enum |
| 19 | 1 | Checksum | XOR of bytes 2-18 |

## Troubleshooting

### "I don't see any devices"

1. **Check the antenna.** The XIAO ESP32-S3 Sense has an IPEX antenna
   connector — BLE will not work without the antenna physically connected.
2. **WiFi/BLE coexistence.** WiFi AP takes priority over BLE on the shared
   2.4 GHz radio. If WiFi is heavily loaded (multiple connected clients,
   active HTTP transfers), BLE advertising may be delayed or suppressed.
3. **Range.** BLE TX power defaults to 0 dBm. Effective range is ~10-30m
   indoors depending on walls and interference.
4. **Check `FEATURE_BLUETOOTH`.** In `config.h`, ensure
   `FEATURE_BLUETOOTH` is set to `1`.
5. **Serial debug.** Connect via USB serial at 115200 baud and look for
   `"Bluetooth started"` in the boot log. If you see
   `"BLE Unavailable"`, the NimBLE stack failed to initialize.

### "I see the device but no debug data"

Debug mode is not active. Either:
- Hold the BOOT button for 3 seconds, or
- Rebuild with `FEATURE_BLE_DEBUG 1`

### "Debug data looks wrong / checksum fails"

- Ensure firmware and Flipper app use the same protocol version (`0x01`)
- Check that the device has been running long enough for health data
  to populate (wait ~60 seconds after boot)

### "Device appears and disappears"

Normal during WiFi/BLE coexistence. The BLE stack pauses advertising
during WiFi channel switches and heavy traffic. The device should
reappear within a few seconds.

## Alternative tools for the full signal stack

Your Flipper covers BLE only. For comprehensive signal debugging:

### ESP-NOW mesh traffic (channels 1, 6, 11)

**Wireshark + ESP32 sniffer:**
Flash a second ESP32 with the [esp32-wifi-sniffer](https://github.com/espressif/esp-idf/tree/master/examples/wifi/sniffer)
example. Set it to monitor channels 1, 6, and 11 in sequence. Pipe output
to Wireshark over serial. ESP-NOW frames appear as 802.11 action frames
with Espressif OUI `18:FE:34`.

**What to look for:**
- Heartbeat frames every 30 seconds between peers
- Beacon state transitions (arrived/departed)
- Chirp messages with magic byte `0xC4`

### WiFi AP verification

**WiFi Analyzer** (Android) or **AirPort Utility** (iOS):
Scan for SSIDs matching `SecuraCV-*` on channel 1 (2412 MHz).
Verify the AP is visible and check signal strength.

**What to look for:**
- SSID present on expected channel
- Signal strength consistent with placement
- No channel overlap with neighboring APs

### BLE deep inspection

**nRF Connect** (Android/iOS):
Superior to Flipper for BLE work — shows full GATT service tree,
advertising data hex dump, connection parameters, and real-time
RSSI graphing.

1. Scan and find `SCV-*` or `SecuraCV-*`
2. Tap "RAW" to see manufacturer-specific data bytes
3. Connect to browse GATT services and characteristics
4. Read Device Info characteristic for JSON status
5. Write `"STATUS"` to Command characteristic to trigger chirp

### Flipper Zero + WiFi Dev Board

If you later get the **WiFi Dev Board** (ESP32-S2 with Marauder firmware):
- Scan for WiFi APs to verify `SecuraCV-*` SSID
- Sniff WiFi traffic on channels 1/6/11
- Detect ESP-NOW frames (Marauder's packet monitor mode)

This turns the Flipper into a much more capable tool for this project.

## Signal reference

| Signal | Protocol | Frequency | Channel | Tool |
|---|---|---|---|---|
| BLE advertisements | BLE 4.2 | 2402/2426/2480 MHz | 37/38/39 | Flipper Zero, nRF Connect |
| BLE debug beacon | BLE manuf. data | 2402/2426/2480 MHz | 37/38/39 | Flipper Zero + FAP, nRF Connect |
| WiFi AP beacons | 802.11n | 2412 MHz | 1 | WiFi Analyzer, Marauder |
| ESP-NOW mesh | 802.11 action | 2412/2437/2462 MHz | 1/6/11 | ESP32 sniffer + Wireshark |
| Chirp broadcast | ESP-NOW + custom | 2412/2437/2462 MHz | 1/6/11 | ESP32 sniffer + Wireshark |
| Beacon alerts | ESP-NOW + custom | 2412/2437/2462 MHz | 1/6/11 | ESP32 sniffer + Wireshark |
| WiFi CSI probes | 802.11 | 2412-2462 MHz | 1-11 | ESP32 CSI tool |
| BLE presence scan | BLE 4.2 | 2.4 GHz | All | Passive (Canary is receiver) |
