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

Connect to WiFi **SecuraCV-XXXX** → http://192.168.4.1

> **Security Note:** The default AP password is for development only.
> See `secrets/secrets.example.h` for configuration.

### Canary Vision

```bash
cd firmware/projects/canary-vision

# Create secrets file
make secrets
# Edit secrets/secrets.h with WiFi and MQTT credentials

# Build and upload
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

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed composition rules and [VARIANT_POLICY.md](VARIANT_POLICY.md) for the lifecycle policy governing each variant.

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

---

## Build Systems

### PlatformIO (Recommended)

Install: [VS Code Extension](https://platformio.org/install/ide?install=vscode) or `pip install platformio`

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

All projects support these standard targets:

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
dashboard. It pairs devices with the zero-typing BOOT-tap flow, shows fleet
health (online/offline, events, uptime, signal), groups devices by room,
and offers per-device **Identify** (blink LED + chirp), rename, logs, and
witness-chain views. On desktop widths the dashboard lays device cards out
in a multi-column grid.

```bash
cd canary-vision && npm install && npm run dev
# open http://localhost:3000
```

Everything runs on your local network — no cloud dependencies. The former
standalone `fleet-manager.html` has been retired in favor of the app.

For the end-to-end multi-device wizard (naming devices, `canary.local`
catch-all behaviour, Identify), see
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

1. Create `boards/<board-id>/pins/pins.h` with pin definitions
2. Add capability flags (`HAS_CAMERA`, `HAS_SD_CARD`, etc.)
3. Create board section in `envs/platformio/common.ini`
4. Create project wrapper in `projects/<project-id>/`

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
