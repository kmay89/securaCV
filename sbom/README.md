# SBOM Generation

This directory documents the Software Bill of Materials (SBOM) generation process for SecuraCV.

## Overview

SBOMs are generated automatically in CI via the `.github/workflows/sbom.yml` workflow on every push to `main` and on pull requests. The output format is **CycloneDX 1.5+ JSON**.

## Generated SBOMs

| Ecosystem | File | Tool |
|-----------|------|------|
| Rust (kernel) | `sbom-rust.cdx.json` | `cargo-cyclonedx` |
| Node.js (device-api, SPA) | `sbom-node.cdx.json` | `@cyclonedx/cdxgen` |
| C/C++ (ESP32 firmware) | `sbom-firmware.cdx.json` | Manual (see below) |

## Firmware SBOM

The firmware SBOM is generated manually because the Arduino/ESP-IDF build uses vendored components that are not tracked by a standard package manager.

The **firmware version is read from source** (`FIRMWARE_VERSION` in `firmware/canary/include/canary_config.h`) at generation time, so the SBOM always names the version that actually shipped rather than a hardcoded literal.

The dev/release image builds `framework = arduino` on the **official espressif32 platform** (`firmware/canary/platformio.ini`), which packages **Arduino-ESP32 core 2.0.17 / ESP-IDF 4.4.7** — not bare ESP-IDF 5.1. The SBOM lists:

- **arduino-esp32** (2.0.17) — Arduino-ESP32 core (framework); bundles ESP-IDF 4.4.7
- **esp-idf** (4.4.7) — Espressif IoT Development Framework (bundled by the Arduino core)
- **FreeRTOS** (10.4.3) — Real-time OS kernel (bundled with ESP-IDF 4.4.7)
- **mbedtls** (2.28.3) — Cryptographic library (bundled with ESP-IDF 4.4.7, 2.28 LTS line)
- **cJSON** (1.7.15) — JSON parser
- **lwip** (2.1.2) — TCP/IP stack
- **esp-tls**, **nvs_flash**, **esp_https_ota** (4.4.7) — ESP-IDF components

When adding new ESP-IDF components or vendored C libraries, update the firmware SBOM template in `.github/workflows/sbom.yml`. The `[env:full]` build uses a different platform (pioarduino core 3.x / ESP-IDF 5.5.4); if it ever becomes the shipped image, the framework component versions above must move with it.

## Retrieving SBOMs

SBOMs are attached as build artifacts on each GitHub Actions run. To download:

1. Go to the **Actions** tab in the repository
2. Select the **SBOM Generation** workflow
3. Click the most recent run
4. Download the `sbom-artifacts` artifact

## Verification

To verify a CycloneDX SBOM:

```bash
# Install the CycloneDX CLI
npm install -g @cyclonedx/cyclonedx-cli

# Validate
cyclonedx validate --input-file sbom-rust.cdx.json --input-format json
```
