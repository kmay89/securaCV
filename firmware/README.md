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
| **Canary Display** | ESP32-S3 / C6 / C3 glass: round watch, 4.3" dash, 7" dash7 / nightstand7, 1.47" sticks, Touch 1.69, AMOLED 2.41 (one manifest per product under [`devices/`](../devices/)) | Fleet status glass: MQTT subscribe, on-device Ed25519 chain verify, glance UI | [Get Started →](projects/canary-display/) |
| **Canary Sentinel** | XIAO ESP32-C3 / C6+MR60 (+S3 hub) | Multi-sensor fusion people detection (PIR + radar + WiFi/BLE + light), Lite/Standard/Heavy — **Phase 1a: compile-gated in CI, not released** (no flasher product, no published OTA channel) | [Get Started →](projects/canary-sentinel/) |

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
├── boards/         # one directory per board: pins/pins.h + README (registry: boards/boards.json)
├── common/         # board-agnostic modules; the module map is common/README.md
├── configs/        # per-product composition (feature flags, build profiles)
├── envs/           # PlatformIO .ini per product (envs/platformio/)
├── canary/         # the ACTIVE flagship tree (its own platformio.ini + lib/)
├── projects/       # product entry points; each one's lifecycle is VARIANT_POLICY.md
├── provisioning/   # production key + eFuse tooling (and the compile-only secure envs)
├── scripts/        # CI checks, release and OTA tooling
├── tests_host/     # host-side unit tests for the pure cores
├── examples/       # standalone samples (CSI, a module stub)
└── flavors.json    # every PlatformIO flavor env CI builds (ARCHITECTURE.md, "CI Flavor Manifest");
                    #   firmware.yml adds two compile-only steps (see "PR CI" below)
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

Supported for Canary WAP and, through its generated parity sketch, Canary
Display (see [its README](projects/canary-display/README.md#arduino-ide-generated-parity-sketch)).
For the WAP, run the setup wizard:

```bash
cd firmware/projects/canary-wap
./setup.sh arduino
```

Then open `arduino/canary_wap/canary_wap.ino` in Arduino IDE.

### Make Targets

The sensor firmware projects — `canary-wap`, `canary-vision`, and
`canary-sense` — support these standard targets (`canary-display`,
`canary-sentinel` and `canary-ota` have no Makefile: use `pio run` there.
canary-display also builds in the Arduino IDE from its generated parity
sketch.):

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

See [FEATURES.md](FEATURES.md) for the complete feature audit matrix and [VARIANT_POLICY.md](VARIANT_POLICY.md) for each variant's lifecycle status. For **which partition table to flash for which deployment** (flash size × OTA × build profile: FULL + OTA fits the 8 MB `default_8MB` table, and a 16 MB board is for FULL plus a dedicated `witness_log` partition), see [PARTITIONS.md](PARTITIONS.md).

| Build Target | Location | Lifecycle | Notes |
|-------------|----------|-----------|-------|
| **PlatformIO (canary/)** | `canary/` | ACTIVE | Modular libraries, canonical onboarding UI/API |
| **Arduino IDE (canary-wap)** | `projects/canary-wap/arduino/canary_wap/` | COMPATIBILITY | Monolithic sketch; full WAP UX |
| **PlatformIO (canary-wap/)** | `projects/canary-wap/` | COMPATIBILITY | Uses common headers |
| **canary-vision** | `projects/canary-vision/` | SPECIALIZED | ESP32-C3 + Grove Vision AI + MQTT/HA |
| **canary-sense** | `projects/canary-sense/` | SPECIALIZED | XIAO ESP32-C6 + MR60BHA2 radar + MQTT/HA |
| **canary-display** | `projects/canary-display/` | SPECIALIZED | Fleet status glass across the display line: round watch, 4.3" dash (+ its feature envs and the 4.3B playground), 7" dash7 / nightstand7, 1.47" sticks (S3, C6, C3), Touch 1.69, AMOLED 2.41. Envs in `flavors.json`, one manifest per product in `devices/` |
| **canary-ota** | `projects/canary-ota/` | SPECIALIZED | Standalone ESP-IDF OTA teaching harness; the engine is `common/ota/`. Built by no CI workflow |
| **canary-sentinel** | `projects/canary-sentinel/` | SPECIALIZED | Doorway/window multi-sensor fusion. **Phase 1a: compile-gated in CI (`door`, `lite`), unreleased**; bench pending |
| **canary-tincan** | `projects/canary-tincan/` | SPECIALIZED | Kids' wrist Canary. Phase 0: pure cores host-tested in CI, no build env (its bench bring-up sketch is not compiled by CI) |
| **canary-companion** | `projects/canary-companion/` | SPECIALIZED | Night Watch + Pocket Canary. Phase 0: pure cores host-tested in CI, no build env |
| **canary-fence-guard** | `projects/canary-fence-guard/` | CONCEPT | Perimeter witness over LoRa. Research only; its stub `#error`s on purpose |
| **WAP Snapshot** | _(removed)_ | REMOVED | Frozen 2026-02-20, deleted 2026-05-29; history in git |

Lifecycle labels are [VARIANT_POLICY.md](VARIANT_POLICY.md)'s; where the two tables disagree, that one wins.

### PlatformIO Build Environments (canary/)

| Environment | Features | Use Case | PR CI |
|-------------|----------|----------|-------|
| `dev` | SD, WiFi, HTTP + self-signed HTTPS on 443 (port 80 redirects), Camera, OTA | Development iteration | built |
| `release` | as dev, size-optimized; HTTPS stays off until the size guard shows it fits the OTA slot | Production | built + OTA-slot guard |
| `full` | + Mesh, BLE, RF Presence, Chirp (pioarduino core 3, `default_8MB` table) | Full WAP parity | built |
| `dev_ha` | dev + MQTT (broker TLS: plain / CA / pin / lab, fail-closed) + HA Discovery | Home Assistant development | — |
| `release_ha` | release + the same MQTT + HA Discovery | The published, OTA-signed canary image | built + OTA-slot guard |
| `standalone` | release with MQTT forced off | Standalone WAP mode | — |
| `minimal` | Crypto + GPS only | Testing crypto/chain logic | — |
| `usb-onboard` | dev + USB-OTG HID help-launch + read-only SD drive | Opt-in; on-device (Phase 2) validation pending | — |
| `esp32cam` | classic-ESP32 reach port: camera peek, CSI, SPI SD, pull-OTA | AI-Thinker ESP32-CAM | built (compile-tested; USB install only, no OTA manifest) |
| `esp32-wroom` | classic ESP32: CSI presence witness, chain, pull-OTA | Generic WROOM-32 DevKit | built (same) |
| `freenove-s3` | S3 camera kit, SD off (1-bit SDMMC only) | Freenove FNK0085 | built (same) |
| `secure` / `secure_ha` | the provisioning kit's Tier 3/4 image, flash encryption required (`provisioning/platformio_secure.ini`) | The production eFuse path; nothing publishes it | compile-only |

"PR CI" is `firmware/flavors.json` `build_envs` (with the size guards its
`size_guards` name) plus `firmware.yml`'s two compile-only steps: `dev`
rebuilt with the tamper contact on, and the secure pair. A `—` env is not
compiled by any workflow.

## Fleet Management

Fleet management for **Canary WAP** devices lives in the Canary Vision
fleet app ([`canary-vision/`](../canary-vision/)), an SPA over an Express
reference server that mirrors the device API. It pairs with the zero-typing
BOOT-tap flow, shows fleet health (online/offline, events, uptime, signal), groups
devices by room, and offers per-device **Identify** (blink LED + chirp),
rename, logs, and witness-chain views. On desktop widths the dashboard lays
device cards out in a multi-column grid.

**Scope today:** the app's BOOT-tap pairing and Identify-over-HTTP apply to
`canary-wap` (which runs an HTTP server). `canary-vision` and `canary-sense`
are MQTT-only, advertise-only devices — they appear on the network via their
`_securacv._tcp` mDNS adverts and integrate through **Home Assistant** MQTT
auto-discovery, where each exposes its own **Identify** button (blinks the
device LED for 10 s). Full fleet-app pairing for these MQTT-only devices
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

The other fleet views (the Hub, the companion app on iPhone / iPad / Apple
Watch, the Witness Wall) are defined in
[`docs/GLOSSARY.md`](../docs/GLOSSARY.md#surfaces-the-things-a-person-actually-touches);
the Canary displays show the fleet on their own glass.

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
- MQTT broker link: plain by default; CA-verified or SHA-256-pinned TLS
  once provisioned, plus an opt-in unverified lab mode that warns on every
  connect (the WAP offers CA only; the nightstand-c6 image is plain-only).
  A TLS mode missing its CA or pin refuses to connect rather than falling
  back. Per product in
  [`docs/FIRMWARE_VARIANT_AUDIT.md`](../docs/FIRMWARE_VARIANT_AUDIT.md).
  Compile-tested by CI and host-tested; not yet bench-tested against a TLS
  broker.

**Privacy Guarantees:**
- No raw video storage
- Time coarsening (ten-minute buckets)
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
