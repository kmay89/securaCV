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

The firmware SBOM is generated manually because the ESP-IDF build system uses vendored components that are not tracked by a standard package manager. The SBOM lists:

- **esp-idf** — Espressif IoT Development Framework
- **FreeRTOS** — Real-time OS kernel (bundled with ESP-IDF)
- **mbedtls** — Cryptographic library (bundled with ESP-IDF)
- **cJSON** — JSON parser
- **lwip** — TCP/IP stack
- **esp-tls**, **nvs_flash**, **esp_https_ota** — ESP-IDF components

When adding new ESP-IDF components or vendored C libraries, update the firmware SBOM template in `.github/workflows/sbom.yml`.

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
