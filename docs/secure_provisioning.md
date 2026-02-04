# SecuraCV Canary — Secure Hardware Provisioning Guide

Status: Normative
Intended Status: Informative
Last Updated: 2026-02-04

## Overview

This document describes the hardware security provisioning workflow for SecuraCV Canary
devices. The provisioning kit provides tools for:

- Cryptographic key generation with entropy validation
- Device identity provisioning (Ed25519 signing keys)
- eFuse verification and security configuration
- Fleet management and device manifests
- **Future:** Secure Boot v2 and Flash Encryption

**Important:** Secure Boot and Flash Encryption are **Phase 2 features** (not yet enabled).
The current provisioning workflow prepares devices for future lockdown without bricking
them during development.

---

## Security Architecture Phases

### Phase 1: Development Mode (Current)

- Keys generated and stored in NVS
- Firmware unsigned (allows rapid iteration)
- Flash unencrypted (allows debugging)
- JTAG enabled (allows debugging)
- **Focus:** Functional validation without bricking risk

### Phase 2: Production Lockdown (Future)

- Secure Boot v2 with RSA-3072 signing
- XTS-AES-256 Flash Encryption
- Anti-rollback protection
- JTAG permanently disabled via eFuse
- NVS encryption enabled
- **Irreversible:** Once burned, device cannot return to dev mode

---

## What's in the Provisioning Kit

All tools are located in `firmware/provisioning/`:

| File | Purpose |
|------|---------|
| `generate_keys.sh` | One-time RSA-3072 signing key + XTS-AES flash encryption key generation with entropy checks and backup checklist |
| `verify_device.py` | Reads security-relevant eFuses via `espefuse.py`, checks virgin state pre-provisioning and lockdown post-provisioning, outputs JSON audit trail |
| `provision_canary.sh` | Full orchestration: virgin verify → burn keys → flash signed firmware → burn security eFuses → post-verify → generate manifest. Has `--dry-run` mode |
| `create_manifest.py` | Fleet management: list provisioned devices, search by MAC, generate summary reports |
| `sdkconfig.defaults.secure` | ESP-IDF Kconfig with BT disabled, Secure Boot v2 **prepared but not enabled**, Flash Encryption release mode, NVS encryption, anti-rollback, JTAG off |
| `platformio_secure.ini` | PlatformIO environment config that wraps the security sdkconfig |
| `partitions_secure.csv` | Partition table with OTA A/B slots and encrypted flags on sensitive partitions |

---

## Prerequisites

### Host Machine

```bash
# Python 3.8+ with ESP tools
pip install esptool espefuse

# OpenSSL for key generation
openssl version  # Must be 1.1.1+

# Optional: Hardware RNG for key generation
# On Linux, ensure /dev/hwrng or /dev/random available
```

### Hardware

- ESP32-S3 (XIAO ESP32S3 Sense or compatible)
- USB-C cable for flashing
- SD card (for device key backup during provisioning)

---

## Phase 1 Workflow (Current - Development)

### Step 1: Verify Virgin Device State

```bash
cd firmware/provisioning

# Check device hasn't been previously provisioned
python verify_device.py --port /dev/ttyACM0 --expect-virgin

# Output: JSON report showing all security eFuses are unset
```

### Step 2: Flash Development Firmware

```bash
# Use standard PlatformIO build (no security features)
cd ../canary
pio run -e dev -t upload
```

### Step 3: Device Self-Provisions Keys

On first boot, the firmware automatically:
1. Generates Ed25519 keypair from hardware RNG (`esp_fill_random`)
2. Stores keypair in NVS
3. Derives device fingerprint from public key
4. Creates boot attestation record

### Step 4: Verify Device Identity

```bash
# Read device identity via dashboard API
curl http://192.168.4.1/api/manifest

# Or via serial monitor
pio device monitor  # Shows device fingerprint on boot
```

### Step 5: Register Device in Fleet Manifest

```bash
python create_manifest.py add \
  --mac AA:BB:CC:DD:EE:FF \
  --fingerprint "abc123..." \
  --location "Office Front Door" \
  --notes "Dev unit #1"
```

---

## Phase 2 Workflow (Future - Production)

**WARNING:** Phase 2 operations are IRREVERSIBLE. Once security eFuses are burned,
the device cannot be returned to development mode.

### Step 1: Generate Signing Keys (One-Time)

```bash
cd firmware/provisioning

# Generate RSA-3072 signing key with entropy validation
./generate_keys.sh

# This creates:
#   keys/secure_boot_signing_key.pem  (KEEP SECRET - offline backup required)
#   keys/secure_boot_signing_key.pub  (embedded in firmware)
#   keys/flash_encryption_key.bin     (KEEP SECRET - one per device batch)
#   keys/key_inventory.json           (tracking metadata)
```

**CRITICAL:** Back up `keys/` directory to offline storage before proceeding.
Loss of signing key = bricked devices on next OTA.

### Step 2: Build Signed Firmware

```bash
# Use secure PlatformIO environment
pio run -e secure

# Or with ESP-IDF directly
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults.secure" build
```

### Step 3: Provision Device (Dry Run First!)

```bash
# ALWAYS dry-run first to preview what will happen
./provision_canary.sh --port /dev/ttyACM0 --dry-run

# Review output carefully, then execute for real:
./provision_canary.sh --port /dev/ttyACM0

# This will:
# 1. Verify device is virgin
# 2. Flash signed firmware
# 3. Burn flash encryption key (if not already burned)
# 4. Enable Secure Boot v2
# 5. Disable JTAG
# 6. Enable anti-rollback
# 7. Verify all eFuses are correctly set
# 8. Generate device manifest entry
```

### Step 4: Verify Lockdown

```bash
python verify_device.py --port /dev/ttyACM0 --expect-locked

# Output: JSON showing all security eFuses are correctly burned
```

---

## Security eFuse Reference

| eFuse | Phase 1 (Dev) | Phase 2 (Prod) | Purpose |
|-------|---------------|----------------|---------|
| `SECURE_BOOT_EN` | 0 | 1 | Enables Secure Boot v2 |
| `FLASH_CRYPT_CNT` | 0 | odd (1,3,5,7) | Enables flash encryption |
| `JTAG_DISABLE` | 0 | 1 | Permanently disables JTAG |
| `FLASH_CRYPT_CONFIG` | 0 | 0xF | Full flash encryption |
| `SECURE_BOOT_AGGRESSIVE_REVOKE` | 0 | 1 | Aggressive key revocation |
| `SOFT_DIS_JTAG` | 0 | 1 | Software JTAG disable |

---

## Bluetooth Disabled by Design

The secure configuration disables Bluetooth at compile time (`CONFIG_BT_ENABLED=n`).
This is a deliberate security decision:

- **No proprietary Bluetooth binary blobs** in firmware
- **Eliminates CVE-2025-27840** class of attacks (hidden HCI commands)
- **Reduces attack surface** significantly
- WiFi blobs remain (unavoidable on commercial MCUs), but AP-only mode minimizes exposure

If Bluetooth is required for a specific deployment, use the `full` build environment
and accept the increased attack surface.

---

## Partition Table Layout

### Development (`partitions_ota.csv`)

```
# Name,    Type, SubType,  Offset,   Size,     Flags
nvs,       data, nvs,      0x9000,   0x5000,
otadata,   data, ota,      0xe000,   0x2000,
app0,      app,  ota_0,    0x10000,  0x1E0000,
app1,      app,  ota_1,    0x1F0000, 0x1E0000,
spiffs,    data, spiffs,   0x3D0000, 0x30000,
```

### Production (`partitions_secure.csv`)

```
# Name,    Type, SubType,  Offset,   Size,     Flags
nvs,       data, nvs,      0x9000,   0x5000,   encrypted
otadata,   data, ota,      0xe000,   0x2000,
app0,      app,  ota_0,    0x10000,  0x1E0000,
app1,      app,  ota_1,    0x1F0000, 0x1E0000,
nvs_keys,  data, nvs_keys, 0x3D0000, 0x1000,   encrypted
spiffs,    data, spiffs,   0x3D1000, 0x2F000,  encrypted
```

Key differences:
- `encrypted` flag on sensitive partitions
- `nvs_keys` partition for NVS encryption key storage

---

## OTA Updates with Secure Boot

Once Secure Boot is enabled:

1. All OTA firmware must be signed with the same RSA-3072 key
2. Anti-rollback prevents downgrading to older versions
3. Failed signature verification = OTA rejected (device continues on current firmware)

**Key rotation is possible** but requires careful planning:
- New key must be signed by old key
- Burn new key digest to eFuse
- Devices will accept firmware signed by either key
- Eventually revoke old key via eFuse

---

## Fleet Management

### List All Provisioned Devices

```bash
python create_manifest.py list
```

### Search by MAC or Fingerprint

```bash
python create_manifest.py search --mac AA:BB:CC
python create_manifest.py search --fingerprint abc123
```

### Generate Fleet Report

```bash
python create_manifest.py report --output fleet_report.json
```

### Export for Backup

```bash
python create_manifest.py export --output fleet_backup.json
```

---

## Troubleshooting

### "Device not in virgin state"

The device has been previously provisioned or has non-default eFuse values.
For Phase 1 development, this is usually fine. For Phase 2 production,
you need a fresh device.

### "Entropy check failed"

The host machine's random number generator may not have enough entropy.
Wait a few minutes, or use a hardware RNG if available.

### "Signature verification failed" (after Secure Boot)

The firmware was not signed with the correct key. Ensure you're using
the same signing key that was burned to the device's eFuse.

### "OTA rejected"

Either signature verification failed, or anti-rollback blocked a downgrade.
Check firmware version numbers and signing key.

---

## References

- ESP-IDF Security Guide: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/
- ESP32-S3 eFuse Reference: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/efuse.html
- Secure Boot v2: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/secure-boot-v2.html
- Flash Encryption: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/flash-encryption.html
- SecuraCV Threat Model: `spec/threat_model.md`
- SecuraCV Invariants: `spec/invariants.md`
