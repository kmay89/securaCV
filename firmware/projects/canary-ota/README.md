# SecuraCV Canary OTA Update System

Phase 1 implementation of the Over-The-Air (OTA) firmware update system for the SecuraCV Canary privacy witness device.

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

### Phase 1 (Current)
- HTTPS with TLS certificate verification
- SHA256 hash verification of firmware
- Automatic rollback on self-test failure

### Phase 3 (Future)
- Ed25519 signature verification of firmware
- Certificate pinning
- Anti-rollback with eFuse version tracking
- NVS encryption for Ed25519 private key

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

Regenerate manifest with correct firmware binary:
```bash
python tools/mock_ota_server.py generate firmware.bin 1.1.0
```

## Next Steps (Phase 2+)

- [ ] 24-hour automatic update check timer
- [ ] MQTT Update entity for Home Assistant
- [ ] Web portal with update status page
- [ ] OTA progress via MQTT
- [ ] Ed25519 firmware signatures
- [ ] Anti-rollback versioning

## License

MIT License - See LICENSE file for details.

## Contributing

1. Follow the existing code style
2. Add tests for new functionality
3. Update documentation as needed
4. Submit PRs against the `main` branch
