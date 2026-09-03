# SecuraCV Firmware

Privacy-preserving witness device firmware for ESP32 boards.

```
╔═══════════════════════════════════════════════════════════════════════════╗
║  SecuraCV Firmware - Privacy-Preserving Witness Devices                   ║
║  Multi-Board • Modular Architecture • Ed25519 Signed                      ║
╚═══════════════════════════════════════════════════════════════════════════╝
```

## Quick Start

Choose your project and board:

| Project | Board | Use Case | Quick Start |
|---------|-------|----------|-------------|
| **Canary WAP** | XIAO ESP32-S3 Sense | GPS tracking, SD storage, mesh network | [Get Started →](projects/canary-wap/) |
| **Canary Vision** | ESP32-C3 + Grove Vision AI | Person detection, Home Assistant | [Get Started →](projects/canary-vision/) |
| **Canary Sense** | XIAO ESP32-C6 + MR60BHA2 | Presence + breathing radar, Home Assistant | [Get Started →](projects/canary-sense/) |
| **Canary Display** | XIAO ESP32-S3 + Round Display (watch) / Waveshare 4.3" (dash) / 1.47" sticks (nightstand, nightlight) / 7" panels | Fleet status glass: MQTT subscribe, on-device Ed25519 chain verify, glance UI | [Get Started →](projects/canary-display/) |
| **Canary Sentinel** | XIAO ESP32-C3 / C6+MR60 (+S3 hub) | Multi-sensor fusion people detection (PIR + radar + WiFi/BLE + light), Lite/Standard/Heavy | [Get Started →](projects/canary-sentinel/) |

### Canary WAP (Recommended First Project)

```bash
cd firmware/projects/canary-wap

# Interactive setup (PlatformIO or Arduino IDE)
./setup.sh

# Build and upload
make upload

# Monitor output
make monitor
```

Connect to WiFi **SecuraCV-XXXX** → http://canary.local (or the numeric
fallback http://192.168.4.1)

> **Security Note:** The AP password is device-unique (derived from the
> device key) and printed on the serial console at first boot — there is
> no shared default. See `secrets/secrets.example.h` for optional overrides.

### Canary Vision

```bash
cd firmware/projects/canary-vision

# Create secrets file
make secrets
# Edit secrets/secrets.h with WiFi and MQTT credentials

# Build and upload (flashes the default env: canary-vision-default, ESP32-C3 DevKit)
make upload
```

> **Flashing a XIAO kit?** `make upload` targets the **ESP32-C3 DevKit** env
> (`canary-vision-default`), whose I2C pins differ from the XIAO boards.
> Select the env explicitly for XIAO hosts:
>
> ```bash
> pio run -e canary-vision-xiao-c3 -t upload   # Seeed XIAO ESP32-C3
> pio run -e canary-vision-xiao-s3 -t upload   # Seeed XIAO ESP32-S3 (Vision AI V2 Kit)
> ```
>
> See the [board table in the project README](projects/canary-vision/README.md#supported-host-boards)
> for the per-board I2C pin assignments.

### Canary Sense

```bash
cd firmware/projects/canary-sense

# Create secrets file
make secrets
# Edit secrets/secrets.h with WiFi and MQTT credentials

# Build and upload (default env: canary-sense-default, presence-only)
make upload
```

---

## Architecture

The firmware uses a **modular, multi-board architecture** where common code is shared across projects:

```
firmware/
├── boards/                 # Hardware pin definitions
│   ├── xiao-esp32s3-sense/   # XIAO ESP32-S3 (WAP)
│   └── esp32-c3/             # ESP32-C3 (Vision)
├── common/                 # Shared modules (board-agnostic)
│   ├── core/                 # Types, logging, ring buffers
│   ├── hal/                  # Hardware abstraction layer
│   ├── witness/              # Witness chain (Ed25519)
│   ├── gnss/                 # GPS parsing
│   ├── storage/              # NVS + SD storage
│   ├── network/              # Mesh networking
│   ├── bluetooth/            # BLE management
│   ├── rf_presence/          # RF detection
│   ├── web/                  # HTTP server + Web UI
│   ├── camera/               # Camera management
│   └── encoding/             # CBOR encoding
├── configs/                # Product configurations
│   ├── canary-wap/           # WAP configs (default, mobile)
│   └── canary-vision/        # Vision configs
├── envs/                   # Build environments
│   └── platformio/           # PlatformIO .ini files
└── projects/               # Product entry points
    ├── canary-wap/           # WAP project (PlatformIO + Arduino)
    └── canary-vision/        # Vision project
```

**Key rule:** Composition happens only in `envs/` and `projects/`. Common modules never import board or config files.

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed composition rules, [VARIANT_POLICY.md](VARIANT_POLICY.md) for the lifecycle policy governing each variant, and [HARDWARE.md](HARDWARE.md) for board support tiers and the community hardware contribution path.

---

## Projects

### Canary WAP

**Hardware:** XIAO ESP32-S3 Sense + L76K GPS + microSD

**Features:**
- Ed25519 signed witness records with hash chaining
- GPS location with time coarsening for privacy
- SD card append-only storage
- WiFi AP with Web dashboard
- HTTP REST API
- Opera mesh networking
- Bluetooth pairing
- RF presence detection
- Camera peek streaming

**Build Configurations:**

| Config | Use Case |
|--------|----------|
| `default` | Full-featured, 1s record interval |
| `mobile` | Power-optimized, 5s interval |
| `debug` | Verbose logging |

**Build Options:**
- **PlatformIO:** `make build` or `pio run`
- **Arduino IDE:** Open `arduino/canary_wap/canary_wap.ino`

### Canary Vision

**Hardware:** ESP32-C3 + Grove Vision AI V2

**Features:**
- Person detection using SSCMA
- Presence/dwelling state machine
- MQTT publishing to Home Assistant
- Auto-discovery integration

**MQTT Topics:**
- `securacv/<device_id>/events` - Detection events
- `securacv/<device_id>/state` - Current state
- `securacv/<device_id>/status` - Device status

### Canary Sense

**Hardware:** XIAO ESP32-C6 + Seeed MR60BHA2 60GHz mmWave radar kit (+ BH1750 lux)

**Features:**
- Radar presence with 0/1/2+ occupant bucket and near/mid/far range band
- Optional P1-gated wellbeing vitals (breathing lock; BPM in the opt-in wellbeing build only)
- Ed25519 signed witness chain (NVS-persisted, wap-schema chain/health topics)
- MQTT publishing with Home Assistant MQTT auto-discovery
- Signed pull-OTA with HA `update` entity
- mDNS fleet advert (`_securacv._tcp` with the canonical TXT schema)
- Identify button in Home Assistant (blinks the LED for 10 s)

**Build Configurations:**

| Config | Use Case |
|--------|----------|
| `canary-sense-default` | Presence-only (vitals compiled out) |
| `canary-sense-wellbeing` | Adds the P1-gated vitals lock (`-DCANARY_SENSE_VITALS`) |
| `canary-sense-debug` | Verbose ESP-IDF logging |

> New mDNS/identify features are compile/CI-verified; hardware bench
> validation on the C6 kit is still pending — see the
> [project README](projects/canary-sense/README.md) bench checklist.

---

## Build Systems

### PlatformIO (Recommended)

Install: [VS Code Extension](https://platformio.org/install/ide?install=vscode) or `pip install platformio`

Which `espressif32` / pioarduino platform each env builds on — and the one
file that pins it, `envs/platformio/platforms.ini` — is in
[PLATFORMS.md](PLATFORMS.md).

```bash
cd firmware/projects/canary-wap
pio run              # Build
pio run -t upload    # Upload
pio device monitor   # Monitor
```

### Arduino IDE

Supported for Canary WAP. Run setup wizard:

```bash
cd firmware/projects/canary-wap
./setup.sh arduino
```

Then open `arduino/canary_wap/canary_wap.ino` in Arduino IDE.

### Make Targets

The sensor firmware projects — `canary-wap`, `canary-vision`, and
`canary-sense` — support these standard targets (`canary-display` and
`canary-ota` are PlatformIO-only for now: use `pio run` there):

```bash
make build       # Build firmware
make upload      # Build and flash
make monitor     # Serial monitor
make run         # Upload + monitor
make secrets     # Create secrets template
make clean       # Clean build
make help        # Show all targets
```

---

## Build Targets & Feature Parity

See [FEATURES.md](FEATURES.md) for the complete feature audit matrix and [VARIANT_POLICY.md](VARIANT_POLICY.md) for each variant's lifecycle status. For **which partition table to flash for which deployment** (flash size × OTA × build profile — and why FULL + OTA needs a 16 MB board), see [PARTITIONS.md](PARTITIONS.md).

| Build Target | Location | Lifecycle | Notes |
|-------------|----------|-----------|-------|
| **PlatformIO (canary/)** | `canary/` | ACTIVE | Modular libraries, canonical onboarding UI/API |
| **Arduino IDE (canary-wap)** | `projects/canary-wap/arduino/canary_wap/` | COMPATIBILITY | Monolithic sketch; full WAP UX |
| **PlatformIO (canary-wap/)** | `projects/canary-wap/` | COMPATIBILITY | Uses common headers |
| **canary-vision** | `projects/canary-vision/` | SPECIALIZED | ESP32-C3 + Grove Vision AI + MQTT/HA |
| **canary-sense** | `projects/canary-sense/` | SPECIALIZED | XIAO ESP32-C6 + MR60BHA2 radar + MQTT/HA |
| **canary-display** | `projects/canary-display/` | SPECIALIZED | Fleet status displays: watch puck (round) + 4.3" dash |
| **canary-ota** | `projects/canary-ota/` | SPECIALIZED | OTA A/B subsystem |
| **WAP Snapshot** | _(removed)_ | REMOVED | Frozen 2026-02-20, deleted 2026-05-29; history in git |

### PlatformIO Build Environments (canary/)

| Environment | Features | Use Case |
|-------------|----------|----------|
| `dev` | SD, WiFi, HTTP, Camera, OTA | Development iteration |
| `release` | Same as dev, optimized | Production |
| `full` | + Mesh, BLE, RF Presence, Chirp | Full WAP parity |
| `dev_ha` | dev + MQTT + HA Discovery | Home Assistant integration |
| `release_ha` | release + MQTT + HA Discovery | Production HA deployment |
| `standalone` | release - MQTT | Standalone WAP mode |
| `minimal` | Crypto + GPS only | Testing crypto/chain logic |

## Fleet Management

Fleet management lives in the **Canary Vision** companion app
([`canary-vision/`](../canary-vision/)) — the single supported multi-device
dashboard. For **Canary WAP** devices it pairs with the zero-typing BOOT-tap
flow, shows fleet health (online/offline, events, uptime, signal), groups
devices by room, and offers per-device **Identify** (blink LED + chirp),
rename, logs, and witness-chain views. On desktop widths the dashboard lays
device cards out in a multi-column grid.

**Scope today:** the app's BOOT-tap pairing and Identify-over-HTTP apply to
`canary-wap` (which runs an HTTP server). `canary-vision` and `canary-sense`
are MQTT-only, advertise-only devices — they appear on the network via their
`_securacv._tcp` mDNS adverts and integrate through **Home Assistant** MQTT
auto-discovery, where each exposes its own **Identify** button (blinks the
device LED for 10 s). Full companion-app pairing for these MQTT-only devices
is on the roadmap — see
[`docs/onboarding_unified_wizard.md`](../docs/onboarding_unified_wizard.md).

```bash
cd canary-vision && npm install && npm run dev
# open http://localhost:3000
```

Everything runs on your local network — no cloud dependencies. The former
standalone `fleet-manager.html` has been retired in favor of the app.

For the end-to-end multi-device wizard (naming devices, `canary.local`
catch-all behavior, Identify), see
[`docs/onboarding_multiple_canaries.md`](../docs/onboarding_multiple_canaries.md).

---

## Provisioning

Production device provisioning tools are in `provisioning/`:

```bash
# Generate signing keys
./provisioning/generate_keys.sh

# Verify virgin device
python3 provisioning/verify_device.py --port /dev/ttyACM0

# Full provisioning workflow
./provisioning/provision_canary.sh --port /dev/ttyACM0

# Dry run (preview without burning eFuses)
./provisioning/provision_canary.sh --dry-run --port /dev/ttyACM0
```

See [provisioning/README.md](provisioning/README.md) for complete guide.

---

## Adding a New Board

See **[PORTING.md](PORTING.md)** for the full bring-up guide — board
directory, pin/capability rules, build env, and the registry entries CI
checks. **[HARDWARE.md](HARDWARE.md)** defines the support tiers
(verified / community / compile-tested) and how a port earns them; the
machine-readable board list is [`boards/boards.json`](boards/boards.json).

---

## Secrets

Never commit secrets. Each project has a `secrets/` directory:

```bash
make secrets  # Creates secrets.h template
```

Edit `secrets/secrets.h` with your credentials. The `.gitignore` prevents commits.

---

## Security & Privacy

**Security Properties:**
- Hardware RNG for device identity
- Ed25519 signatures on all records
- Hash-chained records for tamper evidence
- Monotonic sequence numbers
- Crypto self-test at boot

**Privacy Guarantees:**
- No raw video storage
- Time coarsening (5s buckets)
- No MAC address logging
- Local-first data storage
- Session token rotation

---

## Troubleshooting

**USB not detected:**
- Use data-capable USB-C cable
- Hold BOOT button while connecting

**Build fails:**
- Run `make setup` or `./setup.sh`
- Check PlatformIO version: `pio --version`

**WiFi AP not appearing:**
- Wait 10s after boot
- Check serial monitor for errors

---

## License

See repository LICENSE file.
