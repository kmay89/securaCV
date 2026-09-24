# SecuraCV Canary OTA Update System

> **This is a teaching sample, not the production OTA path.** The engine
> pioneered here was promoted to the shared library at
> **`firmware/common/ota/`** (2026-06) and extended there with Ed25519
> release-signature verification, certificate-bundle TLS, a URL transport
> policy, an NVS anti-rollback version floor, NVS-persisted settings, and
> install/pre-reboot hooks. Every released firmware tree consumes that
> shared engine: the canary (PIO), canary-wap (Arduino), canary-vision,
> canary-sense and every released canary-display flavor (canary-sentinel
> links it too, compile-gated and unreleased); see
> **`docs/firmware_ota.md`** for the end-to-end release + update flow.
>
> What stays useful here: a minimal, standalone ESP-IDF harness for
> studying the A/B + rollback mechanics in isolation, and
> `tools/mock_ota_server.py`, the reference local update server (it signs
> manifests via `firmware/scripts/ota_release.py`). What this harness
> deliberately does **not** do: verify release signatures — it checks
> SHA256 only, so it must never be pointed at untrusted update sources or
> used to ship devices. The Wi-Fi credentials in `sdkconfig.defaults` are
> the labeled `YOUR_WIFI_SSID` placeholders a demo user edits; nothing
> ships them.

Phase 1 implementation of the Over-The-Air (OTA) firmware update system for the SecuraCV Canary privacy witness device — kept as the standalone study/demo harness described in the note above.

## Overview

This project implements a secure OTA update engine using ESP-IDF's native APIs. It provides:

- **Manifest-based version checking** over HTTPS
- **SHA256 verification** of downloaded firmware
- **Dual-partition A/B scheme** for safe updates
- **Automatic rollback** if self-tests fail after OTA
- **Progress reporting** via callback interface

## Hardware

- **Board:** Seeed XIAO ESP32-S3 Sense
- **Flash:** 8MB
- **PSRAM:** 8MB (OctalSPI)

## Quick Start

### 1. Configure WiFi

Edit `sdkconfig.defaults` or use menuconfig:

```bash
# Option A: Edit sdkconfig.defaults directly
# Change CONFIG_ESP_WIFI_SSID and CONFIG_ESP_WIFI_PASSWORD

# Option B: Use menuconfig
pio run -t menuconfig
# Navigate to: Example Configuration -> WiFi SSID / Password
```

### 2. Build and Flash

```bash
# Build the project
pio run

# Build and upload to device
pio run -t upload

# Monitor serial output
pio device monitor
```

### 3. Test OTA Updates

```bash
# Navigate to tools directory
cd tools

# Initialize server (generates certificates)
python mock_ota_server.py init

# Build a new firmware version (edit version in securacv_ota.h first)
# Then generate manifest
python mock_ota_server.py generate ../.pio/build/dev/firmware.bin 1.1.0

# Start the mock server
python mock_ota_server.py serve
```

The device will check for updates on boot and download the new firmware if available.

## Project Structure

```
canary-ota/
├── CMakeLists.txt              # Root CMake file
├── platformio.ini              # PlatformIO configuration
├── partitions.csv              # Custom partition table
├── sdkconfig.defaults          # Default ESP-IDF configuration
├── README.md                   # This file
├── src/
│   ├── CMakeLists.txt
│   └── main.c                  # Demo application entry point
├── components/
│   ├── securacv_ota/           # OTA engine component
│   │   ├── CMakeLists.txt
│   │   ├── Kconfig             # Menuconfig options
│   │   ├── include/
│   │   │   └── securacv_ota.h  # Public API
│   │   └── securacv_ota.c      # Implementation
│   └── wifi_sta/               # WiFi station helper
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── wifi_sta.h
│       └── wifi_sta.c
├── tools/
│   └── mock_ota_server.py      # Local test server
└── test/                       # Unit tests (future)
```

## Partition Layout

The custom partition table supports OTA with rollback:

| Partition    | Type | Offset     | Size       | Purpose |
|-------------|------|------------|------------|---------|
| nvs         | data | 0x9000     | 24 KB      | Device identity, chain state |
| otadata     | data | 0xf000     | 8 KB       | Boot slot selector |
| phy_init    | data | 0x11000    | 4 KB       | WiFi calibration |
| factory     | app  | 0x20000    | 1.5 MB     | Recovery firmware |
| ota_0       | app  | 0x1A0000   | 1.5 MB     | Primary slot |
| ota_1       | app  | 0x320000   | 1.5 MB     | Secondary slot |
| witness_log | data | 0x4A0000   | 1.4 MB     | Tamper-evident log |

## OTA Workflow

1. **Boot self-test** validates system after OTA
2. **Manifest check** fetches version info from server
3. **Version compare** determines if update needed
4. **Download** firmware over HTTPS
5. **Verify** SHA256 hash matches manifest
6. **Flash** to inactive OTA partition
7. **Reboot** into new firmware
8. **Validate** via self-test (rollback if fails)

## API Usage

```c
#include "securacv_ota.h"

// Initialize OTA
securacv_ota_config_t config = SECURACV_OTA_CONFIG_DEFAULT;
config.manifest_url = "https://example.com/manifest.json";
config.on_progress = my_progress_callback;
securacv_ota_init(&config);

// Run boot self-test
securacv_ota_boot_self_test();

// Check for updates and install
securacv_ota_check_and_install();
```

## Self-Test Registration

```c
bool my_custom_test(const char *name) {
    // Return true if test passes
    return check_something_important();
}

securacv_selftest_t test = {
    .name = "Custom validation",
    .fn = my_custom_test,
    .required = true,  // Rollback if fails
};
securacv_ota_register_selftest(&test);
```

## Build Environments

| Environment | Use Case | Features |
|------------|----------|----------|
| `dev`      | Development | Debug logging, cert skip option |
| `production` | Release | Optimized, full cert verification |
| `test`     | Unit testing | Unity test framework |

```bash
# Build for development
pio run -e dev

# Build for production
pio run -e production
```

## Manifest Format

```json
{
  "product": "securacv-canary",
  "version": "1.3.0",
  "min_version": "1.0.0",
  "url": "https://example.com/firmware/canary-1.3.0.bin",
  "sha256": "a1b2c3d4e5f6...",
  "size": 1048576,
  "release_notes": "Bug fixes and improvements",
  "release_url": "https://example.com/changelog"
}
```

## Security Considerations

### What this harness does
- HTTPS with TLS certificate verification
- SHA256 hash verification of firmware
- Automatic rollback on self-test failure

### What it deliberately does not do (and where that lives instead)
This harness stops at SHA256 — a checksum proves integrity, not origin.
The production posture lives in the shared engine and the release
tooling, not here:
- Ed25519 release-signature verification against the pinned release key —
  `firmware/common/ota/` (`ota_release_key.h`)
- Anti-rollback version floor persisted in NVS — `firmware/common/ota/`
- Certificate-bundle TLS + URL transport policy — `firmware/common/ota/`
- Key-at-rest protection is tracked as roadmap work in
  `firmware/ESP32S3_OPTIMIZATION_ROADMAP.md` (item 8), not in this demo

## Troubleshooting

### WiFi Connection Failed

Check credentials in sdkconfig:
```bash
pio run -t menuconfig
# Example Configuration -> WiFi SSID / Password
```

### OTA Check Fails with Network Error

1. Verify WiFi is connected
2. Check manifest URL is accessible
3. For local testing, use `SECURACV_OTA_SKIP_CERT_VERIFY=1`

### Rollback Triggered After Update

Check serial output for failed self-test name. Common causes:
- NVS partition not initialized
- Custom self-test function returned false

### SHA256 Mismatch

Regenerate manifest with the correct firmware binary:
```bash
cd tools
python mock_ota_server.py generate ../.pio/build/dev/firmware.bin 1.1.0
```

## Where the "next steps" landed

This list used to be an open checklist; every item shipped in the main
trees, not in this harness — it is closed here so the boxes cannot read
as pending work:

- 24-hour automatic update check timer — the canary tree's pull-OTA loop
  (`firmware/canary/src/main.cpp`, jittered schedule)
- MQTT Update entity for Home Assistant + OTA progress via MQTT —
  `firmware/canary/lib/securacv_mqtt/` (`FEATURE_OTA_PULL`: update state,
  Install button, auto-update switch, republished on reconnect)
- Web update surface — the display tree's `ota_web` and the canary web UI
- Ed25519 firmware signatures — `firmware/common/ota/`
- Anti-rollback versioning — `firmware/common/ota/` (NVS version floor)

This harness itself is frozen at Phase 1 by design.

## License

Apache-2.0 (repository license) — see the repository root `LICENSE` file.

## Contributing

1. Follow the existing code style
2. Add tests for new functionality
3. Update documentation as needed
4. Submit PRs against the `main` branch
