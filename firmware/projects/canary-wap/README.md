# SecuraCV Canary WAP

Privacy-preserving witness device with GPS, SD storage, and wireless access point.

```
╔════════════════════════════════════════════════════════════════════╗
║  SecuraCV Canary WAP - Witness Device Firmware                     ║
║  Ed25519 Signed • Hash-Chained • Privacy-Preserving                ║
╚════════════════════════════════════════════════════════════════════╝
```

## Quick Start

**Flash it from your browser → [the Lab's start page](https://kmay89.github.io/securaCV/canary-local/start.html)**
(or the desktop Flasher; [`docs/browser_flasher.md`](../../../docs/browser_flasher.md)) —
the released image, one click, no toolchain. The rest of this section is
**building from source** for makers.

### 1. Hardware Required

| Component | Description |
|-----------|-------------|
| **XIAO ESP32-S3 Sense** | Main board with camera |
| **L76K GPS Module** | GNSS receiver (optional) |
| **microSD Card** | 4GB+ FAT32 formatted |
| **USB-C Cable** | For programming |

### 2. Choose Your Build Path

<details>
<summary><strong>Option A: PlatformIO (Recommended)</strong></summary>

**Prerequisites:** [VS Code](https://code.visualstudio.com/) + [PlatformIO Extension](https://platformio.org/install/ide?install=vscode)

```bash
# Clone and navigate to project
cd firmware/projects/canary-wap

# Run setup wizard
./setup.sh

# Build and upload
make upload

# Monitor serial output
make monitor
```

**That's it!** The device will start with a WiFi access point.

</details>

<details>
<summary><strong>Option B: Arduino IDE</strong></summary>

**Prerequisites:** [Arduino IDE 2.x](https://www.arduino.cc/en/software)

1. **Install Board Support:**
   - Open Arduino IDE → File → Preferences
   - Add to "Additional Boards Manager URLs":
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Go to Tools → Board → Boards Manager
   - Search "esp32" and install **esp32 by Espressif Systems**

2. **Install Libraries** (Tools → Manage Libraries):
   - ArduinoJson by Benoit Blanchon (7.x)
   - Crypto by Rhys Weatherley
   - NimBLE-Arduino by h2zero (**2.3.8 or later** — the firmware uses the
     2.x callback API, 1.x will fail to compile and silently ship with
     Bluetooth disabled)

3. **Configure Board** (Tools menu):
   - Board: **ESP32S3 Dev Module**
   - USB CDC On Boot: **Enabled**
   - Flash Size: **8MB**
   - PSRAM: **OPI PSRAM**

4. **Open and Upload:**
   ```
   arduino/canary_wap/canary_wap.ino
   ```

</details>

> **One firmware, two toolchains.** Both paths build the *same* sources —
> `arduino/canary_wap/`. PlatformIO points its `src_dir` at the sketch, so
> `make build` (`pio run`) and the Arduino IDE produce identical firmware,
> including the full BLE stack. The build profile (FULL/DEV/MINIMAL) and
> hardware target default in `arduino/canary_wap/build_config.h`; PlatformIO
> overrides them per-env via `-DBUILD_PROFILE_*` / `-DHARDWARE_*` build flags.
> BLE requires NimBLE-Arduino 2.x — if it's missing the build now stops with a
> clear error instead of silently shipping with Bluetooth disabled.

### 3. Connect to Your Device

1. Connect to WiFi: **SecuraCV-wap-XXXX**
   > **Security:** The AP password is device-unique (`cv-` + 12 chars, derived
   > from the device's private key) and is printed on the serial console at
   > first boot. There is no shared default password; release builds fail
   > closed if provisioning is incomplete.
2. Open a browser at either of:
   - **http://canary.local** — single-device catch-all (first Canary on the
     network claims it)
   - **http://192.168.4.1** — numeric fallback that always works

   The first-boot setup wizard opens only at those two names; under any
   other name, the device's unique one included, `/` shows the static page
   that says to type `canary.local` (sweep F344). After setup, on your home
   network, the dashboard is at `canary.local` (when this unit holds that
   name) and at the device's unique mDNS hostname,
   **http://canary-\<name\>.local** once you name it (e.g.
   `canary-kitchen.local`). Until then it is `canary-` plus four lowercase
   hex digits, the first two bytes of its key fingerprint (e.g.
   `canary-7916.local`), not its device id's suffix; at the default log
   level the serial log names it on the
   `[INFO/NETWORK] mDNS started | canary-7916` line.
   > When a TLS certificate is available the dashboard is served over
   > **HTTPS (port 443)** and plain-HTTP requests are redirected; without a
   > cert the device runs HTTP-only, which is every stock build today (the
   > on-device certificate generator compiles out on every pinned core,
   > F15). See the
   > [Arduino build README](arduino/canary_wap/README.md) for details.
   > The **MQTT bridge** (`/mqtt` page) has its own encryption setting:
   > *Off* (plain) or *On, check the broker's certificate* (paste the
   > broker's CA in PEM form; port 8883). An "On" setting with no CA refuses
   > to connect and says why on the page and the serial log. Neither
   > fingerprint pinning nor an unverified "lab" socket is available on this
   > device: its esp_mqtt transport has no pin hook, and the pinned Arduino
   > core builds esp-tls without the option to skip verification. Per-variant
   > status in [`docs/FIRMWARE_VARIANT_AUDIT.md`](../../../docs/FIRMWARE_VARIANT_AUDIT.md).
3. View live device status on the web dashboard

For the full walkthrough — discovery, naming devices, the `canary.local`
catch-all, and Home Assistant/MQTT integration (the WAP publishes HA MQTT
auto-discovery via its CSI bridge) — see
[`docs/getting_started_canary.md`](../../../docs/getting_started_canary.md).

---

## Features

| Feature | Description |
|---------|-------------|
| **Ed25519 Signatures** | Every witness record is cryptographically signed |
| **Hash Chain** | Records are hash-chained for tamper evidence |
| **GPS Tracking** | Location with time coarsening for privacy |
| **SD Storage** | Append-only witness record storage |
| **WiFi AP** | Local web dashboard and API access |
| **Mesh Network** | Opera protocol for device-to-device communication |
| **Bluetooth** | BLE pairing and configuration |
| **Bluetooth setup door** | Improv Wi-Fi on the pairing server for a fresh unit (FULL profile, `ble_improv`): a phone's one tap hands over the home Wi-Fi over an encrypted Just Works link, then reads a one-time **claim ticket** it spends on the home LAN for the pairing receipt — the bearer token never rides Bluetooth. See [the setup door](#the-bluetooth-setup-door-improv-wi-fi) below |
| **RF Presence** | Privacy-preserving device detection |
| **Camera Peek** | Live MJPEG preview streaming |
| **Microphone** | PDM mic detects smoke (T3) / CO (T4) alarm cadences plus knock, doorbell, and glass-break; privacy-bounded (no audio stored), hard mute with witness-chain audit trail |

## The Bluetooth setup door (Improv Wi-Fi)

In the **FULL** profile (`FEATURE_IMPROV` = `FEATURE_BLUETOOTH && FEATURE_BLE`
in `build_config.h`; 0 in DEV and MINIMAL) a fresh WAP opens the open
**Improv Wi-Fi** door on the pairing channel's NimBLE server (`ble_improv.h`;
the bytes and the rules are `improv_core.h`, a staged byte-identical copy of
`firmware/common/network/improv_core.h` held by
`firmware/scripts/check_improv_sync.sh`). Opera's beacon stays the primary
advert; while the door is open it carries `FLEET_BEACON_FLAG_SETUP_OPEN` and
the name `WAP-AB12` (the last four hex of its key fingerprint — the same
four as its `SCV-XXXX` name, not its setup network's suffix, which is
spelled in the device id's no-confusion alphabet) and the scan response
carries the Improv service; when it shuts, Opera's own scan response
(the `SCV-XXXX` name and the SecuraCV service UUID) returns. A fresh unit
with nobody on its SoftAP brings the channel up **5 s** after boot instead
of the five-minute hold (`provisioning_logic::ble_fresh_unit_start_due`,
host-tested) so the door is on air quickly, and holds the BLE scanners
(Nearby, Scout) on that path — a scanning radio would fight the one link the
door needs — until the SoftAP drops after a join or the five-minute max hold
(`provisioning_logic::ap_only_for_discovery`, host-tested: a fresh unit's
AP-only boot state is not the standalone AP-only choice, so the 45 s settle
never releases them under a wizard phone's handshake); the heap guard keeps
the last word. The phone's picker is real here: `GET_WIFI_NETWORKS` streams
the sketch's scan cache — the same list the wizard's `GET /api/wifi/scan`
serves, behind one mutex, so the single radio is swept once for both.

**When the door is open.** Only while the unit has **no credentials stored**
(first boot, factory reset), for **30 minutes after power-on**
(`IMPROV_FIRST_BOOT_WINDOW_MS`; a power cycle re-arms it). A **software
restart does not** re-arm it — the first-boot wizard restarts a
never-provisioned unit every 15 idle minutes, which would otherwise have
re-opened the door forever: the spent time rides in RTC-noinit memory
(`provisioning_logic::door_window_record_valid`, host-tested; cleared after
a power-on, brownout, power-glitch, USB / JTAG-asserted or unknown reset,
and by **Forget Wi-Fi** or the 10 s BOOT hold on a unit that had
credentials, each of which re-arms the window, both the owner's acts: after
setup Forget takes the API token, so a home-network session cannot re-arm
it, F345), a phone at the door is a sign of life
for that wizard timer, and the restart waits while the door is open at all
(`provisioning_logic::wizard_restart_due`, host-tested: a person watching the
door's card on their phone has touched nothing yet) — so it cannot land
mid-provisioning or under the card. A WAP on its own
Wi-Fi offers nothing here — not even when that Wi-Fi is failing (the
dashboard, the SoftAP recovery portal with its printed key, and the BOOT-tap
receipt are the owner's paths; after setup a Wi-Fi change on the dashboard,
on the home network or on the `SecuraCV-wap-XXXX` recovery network, takes
the API token or one BOOT tap, so the recovery network's key alone opens the
dashboard and does not re-point the unit, F334; the bonded rescue service is
there too, but the iPhone app never reaches it on a WAP, and whether the
companion page's Bluetooth chooser lists a WAP waits on a bench, ledger A65). **There is no tap door on the WAP in this version:** the
BOOT button keeps the sketch's own gestures: a short press hands the
receipt to one asker for 30 s (click Use BOOT Tap Once first and press when
the page says to; a second address asking from 10 s before the tap closes
it, and a tap no page was asking for goes to the first address that asks
after it, sweep F342), a 2 s hold prints the identity on serial, and a 10 s hold is
the factory reset (`factory_reset_by_hold()`). The hold clears the saved
Wi-Fi networks, the broker row, the device name and the first-boot flag,
keeps the identity key and the API token (derived from the key, or the
one a flasher seeded), and restarts into first-boot setup when BOOT is
released. It clears the Wi-Fi through
the same `wifi_clear_credentials()` as Forget Wi-Fi, so on a unit that had
credentials it re-arms the door's first-boot window the same way. A join over the door
is held in RAM until WiFi proves it (`ble_improv_submit_join` /
`ble_improv_join_verdict`): a proven join is persisted then, exactly as the
wizard persists, and a failed or timed-out one is forgotten so the door
stays open for a retry — the wizard, the QR scan and the bonded rescue keep
their persist-first order, which the door cannot afford (one wrong
password would otherwise mark the unit configured and shut the door on the
next pass); a path that persisted credentials while a door join was in
flight owns them, and the door's later verdict changes nothing — its link
gets no claim and is told NotAuthorized. Bounds while open:
3 s between accepted credential writes, 10 attempts per open door, a
3-minute idle disconnect, ~20 s linger after the join.

**Security.** LE Secure Connections **Just Works**, no bond kept — the only
pairing a phone with no prior bond can open in one tap. The channel swaps
its security profile to that only while the door is open
(`bluetooth_channel::set_setup_door`; never while a Numeric Comparison is
pending or an authenticated / bonded link is up; restored when the door
shuts). The Improv command and result characteristics require an encrypted
link and the write handler checks it again; the key such a pairing yields is
*unauthenticated*, so the console, OTA, witness-export and bonded
provisioning characteristics (`READ_AUTHEN` / `WRITE_AUTHEN`) refuse it.

**The claim ticket — how the phone ends up paired.** The bearer token never
rides Bluetooth. Once the join the phone asked for succeeds, the WAP mints
16 random bytes (32 hex), readable **once** by the link that provisioned,
within **180 s**, from the SecuraCV companion service
(`8fc1cf00-b162-4401-9607-c8ac21383e90`, CLAIM `8fc1cf01-…`, READ encrypted)
as `{"device_id","claim","claim_url","tls_cert_fp","sta_ip","mdns_host","expires_in_s"}`.
The phone spends it on the home LAN — `GET /api/provisioning-receipt?claim=<hex>`
— and gets the same receipt the BOOT-tap route serves, `base_url` at the
`.local` name. (Every grant names the `.local` name once the WAP is on the
home Wi-Fi and the request did not arrive over its setup network, and so
does port 80's redirect to https on a unit that serves HTTPS;
`receipt_authority.h`, sweep F255. The dashboard at
`http://canary-<suffix>.local/settings` (`https://` once the WAP serves
HTTPS, which no stock build does yet, F15) then saves that receipt as
`canary-recovery-kit.json`, for the iPhone app's **Ready to pair** row:
offered after a BOOT-tap sign-in, and from **Settings**, **Device**,
**Recovery kit** when already signed in; sweep F257.) The route decides in
`provisioning_gate::receipt_decide`'s four-grant order: a `Host` that cannot
name the device is refused first, `403 {"error":"host"}`, in every mode
(sweep F344; `docs/design/magic_pairing.md` §3.7), then a valid bearer,
then the claim, then the BOOT tap; a wrong guess spends nothing, an expired
claim is burned, and a claim that serves leaves the tap unspent.
Two factors: the encrypted link that provisioned, and presence on the Wi-Fi
the WAP just joined. **The receipt the claim buys names the steady-state
address.** Once a certificate is stored, that is `https://<name>.local`
with the certificate's `tls_cert_fp` pin: the certificate is loaded on
every boot and only the HTTPS *server* waits for setup to end (the captive
portal needs plain HTTP), so the claim's `claim_url` is the http address
that answers now and the receipt is the one the phone keeps
(`tls_after_setup()` in the sketch). No stock build stores one today (the
on-device generator compiles out on every pinned core, F15), so the receipt
names `http://<name>.local` and an empty `tls_cert_fp`, and the WAP serves
plain HTTP after setup. The WAP then restarts once, ~10 s after the claim
is spent (on HTTPS ~15 s after "Paired"); the reboot is held while a claim
is outstanding (the claim promised 180 s; the grace alone ended at 120 s)
and follows the same runway after one expires. On a unit that serves
HTTPS, plain HTTP redirects to https at the address the request used (its
`Host`), never the setup network's.

Design, threat model and bench checklist: `docs/design/magic_pairing.md`.
**CI-compiled (also locally) and host-tested (`test_improv_core.cpp`,
`test_claim_ticket.cpp`, `test_provisioning_gate.cpp`), not bench-tested**
— the profile swap, the once-only claim read, the 5 s bring-up path, the
picker, the window across the wizard's restart and the reboot hold are the
open items.

## Build Configurations

| Config | Command | Description |
|--------|---------|-------------|
| `default` | `make build` | Full-featured, 1s record interval |
| `mobile` | `make build-mobile` | Power-optimized, 5s interval |
| `debug` | `make build-debug` | Verbose logging enabled |
| `usbdrive` | `pio run -e canary-wap-usbdrive` | Opt-in bench build, not a shipping profile: USB-OTG for a branded USB name and a read-only SD "evidence drive" ([`usb_drive_bench.md`](../../../docs/hardware/usb_drive_bench.md)) |

PR CI builds the `default` and `usbdrive` envs with PlatformIO
(`firmware/flavors.json`); `firmware.yml`'s Arduino CLI legs also compile
the sketch, the DEV profile `mobile` uses among them.

## Make Targets

```bash
# Setup
make setup              # Interactive setup wizard
make setup-platformio   # PlatformIO setup
make setup-arduino      # Arduino IDE setup
make secrets            # Create secrets.h template

# Build (PlatformIO)
make build              # Build default
make build-mobile       # Build power-optimized
make build-debug        # Build with debug logging
make build-all          # Build all configurations

# Upload & Monitor
make upload             # Build and flash to device
make monitor            # Serial monitor (115200 baud)
make run                # Upload + monitor

# Arduino CLI (alternative)
make arduino-build      # Compile with arduino-cli
make arduino-upload     # Upload with arduino-cli

# Maintenance
make clean              # Clean build artifacts
make lint               # Run static analysis
make info               # Show project info
make help               # Show all targets
```

## API Reference

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web dashboard |
| `/api/status` | GET | Device status JSON |
| `/api/system` | GET | System metrics (`FEATURE_SYS_MONITOR` builds) |
| `/api/diagnostics` | GET | Heap snapshot, SD health, degradation level, and the committed-event egress's counters as `csi_event_egress` (`FEATURE_SYS_MONITOR` builds) |
| `/api/config` | GET/POST | Configuration |
| `/api/logs` | GET | Log export |
| `/api/witness` | GET | The newest witness record (sequence, time bucket, type, chain hash); the signed page is `/api/v1/witness` |
| `/api/v1/witness?last=N` | GET | Newest N signed witness records with their chain-hash pre-image — the shared page contract the phone verifies (`spec/witness_api_v1.md`) |
| `/api/export` | POST | Answers the export bundle header (device id, firmware, chain position, record count, public key) inline, which the dashboard's Export saves as a JSON file; with a card in, also writes it to `/EXPORT/bundle_<ms>.json` and names that copy in `card_path` |
| `/api/peek/start` | POST | Start camera stream |
| `/api/peek/stop` | POST | Stop camera stream |
| `/api/audio/status` | GET | Mic state, mute info, detection counters |
| `/api/audio/mute` | POST | Hard mute/unmute (`{"muted":bool}`) |
| `/api/audio/selftest` | GET/POST | Guided alarm self-test (start/stop/progress) |
| `/api/audio/config` | GET/POST | Room-noise sensitivity (persisted) |
| `/api/audio/transitions` | GET | Recent sound on/off pattern (diagnostics) |
| `/api/fleet` | GET/OPTIONS | The coarse fleet roll-call (name, online, chain verdict); the one `/api/` row here that answers without a token ([`DISCOVERY.md`](../../../tvos/discovery/DISCOVERY.md)) |
| `/api/identify` | POST | Blink + chirp so you can find the unit (`duration_ms`, default ~15 s) |
| `/api/pairing-qr` | GET | The pairing receipt as a QR: device id, base URL and token, plus `tls_cert_fp` on a TLS-enabled device |
| `/api/mqtt/config` | GET/POST | Broker host, port, credentials (the password is write-only) and encryption (off, or CA-verified; see Connect above); a save that moves the broker while keeping the stored password takes the bearer or the password typed again (`403 password_needed` from a session alone) |
| `/api/settings` | GET/POST | Presence preset, sensitivity, pet mode, quiet hours, privacy ceiling, `filter_foreign` and the household time zone ([`csi_developer_api.md`](../../../docs/csi_developer_api.md)) |

The table is the common subset, not the whole surface: the sketch registers
many more routes, the mesh, vault, Wi-Fi, BLE, OTA, presence and CSI families
among them (the CSI ones are in
[`docs/csi_developer_api.md`](../../../docs/csi_developer_api.md)).

**Example:**
```bash
curl -H "Authorization: Bearer <api-token>" http://192.168.4.1/api/status
```

Every `/api/` route above except `/api/fleet` needs the device's API token,
as a bearer header or the dashboard's session cookie, except where the
bearer header alone is taken, because the landing page mints that session
for any host on the home network: `/api/pairing-qr` (it carries the token)
here, and among the routes not listed `POST /api/vault/key` (F335), `POST
/api/bluetooth/pair/confirm` and `POST /api/mesh/pair/confirm` (F346), `POST
/api/wifi/disconnect` and `/forget` in every mode and, after setup,
`/connect` and `/ap-only` (F334, F345; `POST /api/wifi/qr-scan` answers
`404` after setup), and `POST /api/reboot` and `POST /api/safe-mode/retry`
(F361). The
`/api/mqtt/config` row names the one save the session alone cannot make
there. The dashboard asks for the token at those actions: before Start
Pairing and the opera's Create/Join begin, and when any of the others is
refused (the Wi-Fi, vault key, reboot and pairing-QR cards say so
beforehand); paste it from the recovery kit, or use BOOT Tap Once.

```json
{
  "device_id": "canary-s3-4dC2",
  "firmware": "2.1.0",
  "uptime_sec": 3600,
  "chain_seq": 1234,
  "boot_count": 5,
  "sd_healthy": true,
  "crypto_healthy": true
}
```

## Security Properties

- **Unique Device Identity:** Hardware RNG generates unique Ed25519 keypair
- **Monotonic Sequences:** Sequence numbers persist across reboots
- **Hash Chain:** Each record references the previous (tamper-evident)
- **Time Coarsening:** ten-minute buckets prevent precise tracking
- **Crypto Self-Test:** Signature verification at boot and periodically
- **Domain Separation:** Hash chains use domain-separated hashing

## Privacy Guarantees

- **No Raw Video Storage:** Camera is for witness events only
- **No MAC Addresses Stored:** RF detection counts devices, not identities
- **Time Buckets:** Coarsened timestamps (ten-minute buckets in every build profile)
- **Local-First:** All data stays on device unless explicitly exported
- **Session Rotation:** the RF presence detector's ephemeral session tokens rotate every 4 hours. The API token does not rotate: the 10 s hold and Forget Wi-Fi keep it, an install or a full erase in a flasher replaces it, and a flasher's restore of a safety copy puts back the token that copy holds

## Project Structure

```
canary-wap/
├── arduino/
│   └── canary_wap/           # The firmware sources (single tree, both toolchains)
│       ├── canary_wap.ino    # Main sketch — PlatformIO's src_dir points here too
│       └── ...               # Modules (see arduino/canary_wap/README.md)
├── secrets/                  # Credentials (gitignored)
├── tests_host/               # Host-side unit tests
├── platformio.ini            # PlatformIO configuration (src_dir = arduino/canary_wap)
├── Makefile                  # Unified build system
├── setup.sh                  # Interactive setup wizard
└── README.md                 # This file
```

There is no separate `src/main.cpp` — PlatformIO builds the same
`arduino/canary_wap/` sketch tree that the Arduino IDE opens, so both
toolchains produce the same firmware.

## Architecture

This project follows the SecuraCV firmware architecture:

- **Board definitions**: `../../boards/xiao-esp32s3-sense/`
- **Configuration**: `arduino/canary_wap/build_config.h` (hardware target +
  build profile), where the WAP's profile flags live; a few module flags
  default in their own headers (`FEATURE_BEACON_CHANNEL` in
  `beacon_channel.h`, the BLE sub-modes in `ble_config.h`). There is no
  `configs/canary-wap/`; the Lab's Workshop and fleet figures read this
  header and `canary_wap.ino` directly.
- **Common modules**: `../../common/`
- **Build environments**: `../../envs/platformio/`

See `../../ARCHITECTURE.md` for detailed architecture documentation.

## Troubleshooting

<details>
<summary><strong>Device not detected on USB</strong></summary>

- Use a data-capable USB-C cable (not charge-only)
- Hold BOOT button while connecting
- Try a different USB port

</details>

<details>
<summary><strong>Build fails with missing libraries</strong></summary>

```bash
# Run setup again
make setup
```

</details>

<details>
<summary><strong>WiFi AP not appearing</strong></summary>

- Wait 10 seconds after boot
- Check serial monitor for errors

</details>

<details>
<summary><strong>SD card not mounting</strong></summary>

- Format as FAT32 (not exFAT)
- Use 32GB or smaller

</details>

## License

See repository LICENSE file.
