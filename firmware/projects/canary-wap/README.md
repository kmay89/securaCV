# SecuraCV Canary WAP

Privacy-preserving witness device with GPS, SD storage, and wireless access point.

```
╔════════════════════════════════════════════════════════════════════╗
║  SecuraCV Canary WAP - Witness Device Firmware                     ║
║  Ed25519 Signed • Hash-Chained • Privacy-Preserving                ║
╚════════════════════════════════════════════════════════════════════╝
```

## Quick Start (5 minutes)

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

1. Connect to WiFi: **SecuraCV-XXXX**
   > **Security:** The AP password is device-unique (`cv-` + 12 chars, derived
   > from the device's private key) and is printed on the serial console at
   > first boot. There is no shared default password; release builds fail
   > closed if provisioning is incomplete.
2. Open a browser at any of:
   - **http://canary.local** — single-device catch-all (first Canary on the
     network claims it)
   - **http://canary-\<name\>.local** — each device's unique mDNS hostname
     (e.g. `canary-kitchen.local` after you name it, or `canary-ab7k.local`)
   - **http://192.168.4.1** — numeric fallback that always works
   > When a TLS certificate is available the dashboard is served over
   > **HTTPS (port 443)** and plain-HTTP requests are redirected; without a
   > cert the device runs HTTP-only. See the
   > [Arduino build README](arduino/canary_wap/README.md) for details.
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
| **RF Presence** | Privacy-preserving device detection |
| **Camera Peek** | Live MJPEG preview streaming |
| **Microphone** | PDM mic detects smoke (T3) / CO (T4) alarm cadences plus knock, doorbell, and glass-break; privacy-bounded (no audio stored), hard mute with witness-chain audit trail |

## Build Configurations

| Config | Command | Description |
|--------|---------|-------------|
| `default` | `make build` | Full-featured, 1s record interval |
| `mobile` | `make build-mobile` | Power-optimized, 5s interval |
| `debug` | `make build-debug` | Verbose logging enabled |

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
| `/api/health` | GET | Health metrics |
| `/api/config` | GET/POST | Configuration |
| `/api/logs` | GET | Log export |
| `/api/witness/export` | GET | Export witness records |
| `/api/peek/start` | GET | Start camera stream |
| `/api/peek/stop` | GET | Stop camera stream |
| `/api/audio/status` | GET | Mic state, mute info, detection counters |
| `/api/audio/mute` | POST | Hard mute/unmute (`{"muted":bool}`) |
| `/api/audio/selftest` | GET/POST | Guided alarm self-test (start/stop/progress) |
| `/api/audio/config` | GET/POST | Room-noise sensitivity (persisted) |
| `/api/audio/transitions` | GET | Recent sound on/off pattern (diagnostics) |

**Example:**
```bash
curl http://192.168.4.1/api/status
```

```json
{
  "device_id": "canary-s3-AB7K",
  "firmware": "2.1.0",
  "uptime_sec": 3600,
  "sequence": 1234,
  "boot_count": 5,
  "sd_healthy": true,
  "crypto_healthy": true
}
```

## Security Properties

- **Unique Device Identity:** Hardware RNG generates unique Ed25519 keypair
- **Monotonic Sequences:** Sequence numbers persist across reboots
- **Hash Chain:** Each record references the previous (tamper-evident)
- **Time Coarsening:** 5-second buckets prevent precise tracking
- **Crypto Self-Test:** Signature verification at boot and periodically
- **Domain Separation:** Hash chains use domain-separated hashing

## Privacy Guarantees

- **No Raw Video Storage:** Camera is for witness events only
- **No MAC Addresses Stored:** RF detection counts devices, not identities
- **Time Buckets:** Coarsened timestamps (5s default, 10s mobile)
- **Local-First:** All data stays on device unless explicitly exported
- **Session Rotation:** Tokens rotate every 4 hours

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
- **Configuration**: `../../configs/canary-wap/default/`
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
