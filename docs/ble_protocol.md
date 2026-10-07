# SecuraCV Canary — BLE Protocol Specification

Status: Draft v0.2
Last Updated: 2026-10-07

## 1. Overview

The SecuraCV Canary uses Bluetooth Low Energy (BLE) for three distinct subsystems:

- **Opera** — BLE server/advertising for device presence and GATT service
- **Chirp** — Connectionless broadcast alerts between Canary devices
- **Nearby** — BLE scanner for discovering other Canaries and measuring proximity

and, since 2026-10, one door: the **Bluetooth setup door** (section 13) — the open Improv Wi-Fi standard, through which a phone hands a brand-new Canary its Wi-Fi. Built, host-tested, not yet bench-tested.

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
| `00467768-6228-2272-4663-277478268000` | Service | Improv Wi-Fi (the open standard's UUID, not ours) — the setup door, section 13 |
| `00467768-6228-2272-4663-277478268001` | Characteristic | Improv current state (READ, NOTIFY; plain) |
| `00467768-6228-2272-4663-277478268002` | Characteristic | Improv error state (READ, NOTIFY; plain) |
| `00467768-6228-2272-4663-277478268003` | Characteristic | Improv RPC command (WRITE; **encrypted link required**) |
| `00467768-6228-2272-4663-277478268004` | Characteristic | Improv RPC result (READ, NOTIFY; **encrypted link required**) |
| `00467768-6228-2272-4663-277478268005` | Characteristic | Improv capabilities (READ; plain) |
| `0x4677` | 16-bit service data | Improv's advert service data: `[state][capabilities][0 0 0 0]`, 6 bytes, in the scan response while the door is open |
| `8fc1cf00-b162-4401-9607-c8ac21383e90` | Service | SecuraCV companion service (WAP only; outside the Improv service on purpose) |
| `8fc1cf01-b162-4401-9607-c8ac21383e90` | Characteristic | CLAIM (READ; encrypted link required): the claim ticket, readable once by the link that provisioned, within 180 s of the join |

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
- The setup door (section 13) stays a no-op too; the `SecuraCV-XXXX` setup network is the way in

## 13. The setup door (Improv Wi-Fi over BLE)

The canonical design is [`design/magic_pairing.md`](design/magic_pairing.md); the wire and the door rules are the header of `firmware/common/network/improv_core.h`. This section is the radio-level summary: what the WAP puts on air and how its pairing channel's security changes while the door is open. Nothing here has been bench-tested.

### 13.1 What it is

The open [Improv Wi-Fi](https://www.improv-wifi.com/) standard — the one Home Assistant, ESPHome and the improv-wifi web SDK speak — on the pairing channel's NimBLE server (Sense and Vision run it on the beacon's stack through `common/network/improv_ble`). A phone connects, asks `GET_WIFI_NETWORKS`, writes `WIFI_SETTINGS` (`[ssid_len][ssid][pass_len][pass]`, checksummed), and reads the device's own verdict on the state and error characteristics. Capability bits: identify `0x01`, device info `0x02`, scan Wi-Fi `0x04`, hostname `0x08`.

### 13.2 When the door is open — the rule

From `improv_core.h`: the door is open for a device NOBODY OWNS YET, and for an owner's own tap — never for a device that has an owner and a bad day.

- **No credentials stored** (first boot, factory reset): open for a first-boot window of **30 minutes** after power-on (`IMPROV_FIRST_BOOT_WINDOW_MS`; a power cycle or a factory reset re-arms it; compile-time 0 = tap-only).
- **Credentials stored:** shut, whatever the network is doing. A saved network that stopped working raises the SoftAP recovery portal (a door with a key printed on the unit) — it does **not** open this one.
- **A short BOOT tap** (Sense and Vision; the WAP has no tap door in this version) opens it for **60 s**.
- Bounds while open: accepted `WIFI_SETTINGS` writes at least **3 s** apart; **10 attempts** per open door, then it shuts until a tap or a power cycle; a connected client silent for **3 minutes** is dropped; ~**20 s** linger after a successful join, then back to the beacon.

### 13.3 The advert set swap

The fleet presence beacon (section 5's manufacturer-data format, type `0x10`) stays the **primary** advert at all times, so a display's passive roster scan keeps hearing the device either way.

| | Door shut | Door open |
|---|---|---|
| Primary | the fleet beacon | the fleet beacon with **`FLEET_BEACON_FLAG_SETUP_OPEN`** (bit 5, `0x20`), the flags AD and a short name `WAP-AB12` (Sense: `Sense-AB12`, Vision: `Vision-AB12`) — the same four hex characters as the device's `SecuraCV-AB12` setup network |
| Connectable | WAP: yes, the pairing channel as before. Sense / Vision: **no** | yes |
| Scan response | WAP: Opera's own (the `SCV-XXXX` name + the SecuraCV service UUID). Sense / Vision: none | the Improv service UUID + the 6-byte `0x4677` service data |

On the WAP, Opera composes both sets (`ble_improv::compose_scan_response` fills the open-door scan response); the swap is a stop → set → start of advertising, and Opera's own scan response returns when the door shuts. iOS, Android and Chrome each merge the scan response into the advertisement a service filter matches, so a scan filtered on the Improv UUID still finds the device.

### 13.4 The security-profile swap (WAP)

The pairing channel's steady profile is the authenticated one: Numeric Comparison, bonds kept, and every console / OTA / witness-export / bonded-provisioning characteristic marked `READ_AUTHEN` / `WRITE_AUTHEN`. The setup door needs the only pairing a phone with no prior bond can open in one tap — **LE Secure Connections Just Works, no bond kept** — so:

- `bluetooth_channel::set_setup_door` swaps the channel's security profile to Just Works **only while the door is open**, and **never while a Numeric Comparison is pending or an authenticated / bonded link is up**; it is restored when the door shuts.
- The Improv command and result characteristics require an encrypted link by their properties, **and the write handler checks the link's encryption again** before parsing a byte. The state, error and capability bytes stay plain, so a standard client reads "awaiting authorization" without pairing.
- The key a Just Works pairing yields is **unauthenticated**, and every `*_AUTHEN` characteristic refuses it: the console, OTA, the witness export and the bonded provisioning service are out of reach of a link that only came through the door.

### 13.5 The claim ticket (WAP) — the token never rides Bluetooth

The bearer token lasts; a Just Works link is encrypted but not authenticated; nothing that lasts belongs on it. So once the join the phone asked for succeeds, the WAP mints **16 random bytes (32 hex)** and lets **the link that provisioned read them once**, within **180 s**, from the companion service's CLAIM characteristic (`8fc1cf01`, READ, encrypted) as JSON:

```json
{"device_id":"…","claim":"<32 hex>","claim_url":"…","tls_cert_fp":"…","sta_ip":"…","mdns_host":"…","expires_in_s":180}
```

The phone spends it **on the home LAN** — `GET /api/provisioning-receipt?claim=<hex>` at the device's `.local` name — and gets the same receipt the BOOT-tap route serves (`device_id`, `base_url` at the `.local` name, `token`, `tls_cert_fp` where TLS is on). The route decides in this order and no other: a valid bearer → the claim → the BOOT tap (`provisioning_gate::receipt_decide`'s four grants; its first question, a foreign `Host`, is the flagship's `auth_gate`'s — the WAP does not ask it on this route today). A wrong guess **burns** the claim; an expired claim is burned too; a claim that serves leaves the BOOT tap unspent. Two factors: the encrypted link that provisioned, **and** presence on the Wi-Fi the device just joined — a phone that was only near the device holds a string that opens nothing from the street.

### 13.6 Discovering it with third-party tools

While a door is open, nRF Connect / LightBlue see the `<Family>-XXXX` name and, in the scan response, the Improv service with its `0x4677` data (`[0x02][caps]…` = Authorized). Reading `…8001` answers `0x02`; after the door shuts, `0x01` (awaiting authorization). A write to `…8003` without pairing is refused; with the Just Works pairing it is accepted only while the door is open. The Home Assistant companion app and the improv-wifi web SDK can provision Wi-Fi through it; they get Wi-Fi only, no SecuraCV pairing (the companion service is ours). An ESPHome Bluetooth proxy cannot complete the Just Works pairing — expected, untested.

### 13.7 Where

`firmware/common/network/improv_core.h` (pure; `tests_host/test_improv_core.cpp`), `improv_ble.{h,cpp}` (NimBLE glue, both majors), `common/network/claim_ticket.h` (`test_claim_ticket.cpp`), `common/io/short_tap.h` (`test_short_tap.cpp`), `common/network/provisioning_gate.h` (`test_provisioning_gate.cpp`), the WAP's `ble_improv.h` and `build_config.h` (`FEATURE_IMPROV` = `FEATURE_BLUETOOTH && FEATURE_BLE`, the FULL profile), the Sense's and Vision's `include/canary/config.h` (`FEATURE_IMPROV`). The flagship `firmware/canary` build is a follow-up; the displays keep their glass QR.
