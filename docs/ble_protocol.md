# SecuraCV Canary — BLE Protocol Specification

Status: Draft v0.1
Last Updated: 2026-02-18

## 1. Overview

The SecuraCV Canary uses Bluetooth Low Energy (BLE) for three distinct subsystems:

- **Opera** — BLE server/advertising for device presence and GATT service
- **Chirp** — Connectionless broadcast alerts between Canary devices
- **Nearby** — BLE scanner for discovering other Canaries and measuring proximity

All BLE functionality is gated behind the `FEATURE_BLE` compile flag. When disabled, the firmware compiles identically to the BLE-free version with zero dead code.

## 2. Hardware Requirements

- **Board**: Seeed Studio XIAO ESP32S3 (Sense variant)
- **BLE Stack**: NimBLE-Arduino (lighter than bluedroid, ~60% less RAM)
- **Antenna**: Dedicated WiFi/BT IPEX antenna connector — **must be installed for BLE to work**
- **Coexistence**: WiFi AP and BLE share 2.4GHz radio; WiFi takes priority

## 3. UUID Registry

| UUID | Type | Description |
|------|------|-------------|
| `a1b2c3d4-e5f6-7890-abcd-ef0123456001` | Service | SecuraCV Canary BLE Service |
| `a1b2c3d4-e5f6-7890-abcd-ef0123456002` | Characteristic | Device Info (READ) |
| `a1b2c3d4-e5f6-7890-abcd-ef0123456003` | Characteristic | Witness Status (READ) |
| `a1b2c3d4-e5f6-7890-abcd-ef0123456004` | Characteristic | Command (WRITE) |

## 4. Opera — BLE Advertising & GATT Service

### 4.1 Device Name

Format: `SCV-XXXX` where `XXXX` is the last 4 hex characters of the Ed25519 public key fingerprint hash.

The fingerprint is derived from: `SHA256("securacv:pubkey:fingerprint" || pubkey)[0:8]`

### 4.2 Advertising

- Interval: 500ms
- Contains: Service UUID, device name
- Scan response enabled

### 4.3 GATT Characteristics

**Device Info** (READ):
```json
{
  "id": "A3F7B2C1...",
  "fw": "2.1.0",
  "type": "canary",
  "uptime": 12345,
  "chain_height": 42
}
```

**Witness Status** (READ):
```json
{
  "chain_height": 42,
  "chain_head": "4f8a...",
  "verified": true
}
```

**Command** (WRITE):
- `"STATUS"` — Triggers a heartbeat chirp
- `"EXPORT"` — Future: trigger witness export

## 5. Chirp — Broadcast Alert Protocol

### 5.1 Packet Format

Chirp uses BLE manufacturer-specific advertising data (no connection required).

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-1 | 2 | Company ID | `0xFFFF` (testing) — little-endian, added by NimBLE |
| 2 | 1 | Chirp Type | Message type enum |
| 3-6 | 4 | Timestamp | Coarse hour bucket, **boot-relative** (`millis / 3600000` — hours since the device booted, not epoch time), big-endian. Receivers may only compare buckets from the same sender across a short window; the value carries no wall-clock meaning. |
| 7-14 | 8 | Chain Hash | First 8 bytes of witness chain head hash |
| 15-16 | 2 | Device ID | 2-byte prefix from pubkey fingerprint |

Total manufacturer data: 17 bytes (fits within BLE advertising payload limit of 31 bytes).

### 5.2 Chirp Types

| Value | Name | Description |
|-------|------|-------------|
| 0x01 | ALERT | Manual or sensor-triggered alert |
| 0x02 | HEARTBEAT | Periodic "I'm alive" (every 5 min) |
| 0x03 | TAMPER | Tamper event detected |
| 0x04 | WITNESS | New witness record created |
| 0x05 | BOOT | Device just booted |

### 5.3 Rate Limiting

- Minimum interval: 10 seconds between chirps
- Heartbeat: automatic every 5 minutes
- Chirp broadcast duration: 2 seconds per chirp

### 5.4 Mode Switching

Chirp temporarily overrides Opera advertising:
1. Stop Opera advertising
2. Set manufacturer data with chirp payload
3. Broadcast for 2 seconds
4. Restore Opera service advertising

## 6. Nearby — Device Discovery

### 6.1 Scan Parameters

- Scan interval: 100 (in 0.625ms units)
- Scan window: 99 (in 0.625ms units)
- Scan duration: 5 seconds
- Scan period: every 30 seconds
- Active scan by default (passive for stealth mode)

### 6.2 Device Identification

Nearby devices are classified as:
- **Canary**: Advertises the SecuraCV service UUID or sends valid chirp manufacturer data
- **Non-Canary**: All other BLE devices (counted only, never individually tracked)

### 6.3 Tracked Data (Canary devices only)

| Field | Description |
|-------|-------------|
| Device ID Prefix | 4-char hex prefix from BLE name or chirp data |
| RSSI History | Last 10 signal strength readings |
| Last Seen | Timestamp of most recent detection |
| Last Chirp Type | Most recent chirp type from this device |
| Chain Hash Prefix | 8-byte prefix of their chain head hash |

### 6.4 Expiry

Devices not seen for 120 seconds are marked inactive and removed.

## 7. RSSI Signal Strength Interpretation

| RSSI Range | Quality | Approximate Distance |
|------------|---------|---------------------|
| -30 to -50 dBm | Excellent | Within a few meters |
| -50 to -70 dBm | Good | Same room, ~5-15m |
| -70 to -85 dBm | Fair | Adjacent room, ~15-30m |
| -85 to -100 dBm | Weak | Far, >30m |

Note: RSSI-to-distance mapping is approximate and varies significantly by environment (walls, interference, antenna orientation).

## 8. Privacy Constraints

| Constraint | Enforcement |
|------------|-------------|
| No MAC addresses stored to SD | MAC used for local correlation only, never persisted |
| No MAC addresses in API responses | Only device ID prefix (from pubkey hash) exposed |
| Non-Canary devices counted only | `non_canary_device_count` field, no individual entries |
| Device ID from pubkey hash | Not hardware MAC — changes with key rotation |
| Time coarsened in chirps | Boot-relative hour buckets — even coarser than the PWK's 10-minute wall-clock buckets, and anchored to nothing an observer can correlate with a clock |
| Chain hash truncated | 8-byte prefix — proves integrity, doesn't reveal content |

## 9. API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/ble/status` | GET | BLE subsystem status (Opera/Chirp/Nearby) |
| `/api/nearby` | GET | List of nearby Canary devices with RSSI |
| `/api/chirp/send` | POST | Trigger a manual chirp broadcast |

## 10. Interoperability

### Discovering Canaries with Third-Party Tools

Using **nRF Connect** or **LightBlue** on a smartphone:

1. Scan for BLE devices
2. Look for devices named `SCV-XXXX`
3. Connect and explore the service UUID `a1b2c3d4-e5f6-7890-abcd-ef0123456001`
4. Read the Device Info characteristic for device status
5. Write `"STATUS"` to the Command characteristic to trigger a chirp

### Identifying Chirp Broadcasts

1. Scan for manufacturer-specific data with company ID `0xFFFF`
2. Parse the 15-byte payload per the format in section 5.1
3. Chirp type byte at offset 2 identifies the message type

## 11. Thread Safety

The Nearby scanner runs on a dedicated FreeRTOS task (core 0). HTTP API handlers run on the httpd task. Both access the shared `nearbyCanaries[]` array, which is protected by a `SemaphoreHandle_t` mutex. The mutex is acquired before any read or write and released immediately after — never held during I/O operations.

## 12. Graceful Degradation

If NimBLE initialization fails (no antenna, hardware fault):
- `ble_available` is set to `false`
- All BLE functions check this flag and return no-ops
- Firmware continues operating with WiFi AP, GPS, camera, and witness chain
- Dashboard shows "BLE Unavailable" status
- No watchdog resets from BLE failures
