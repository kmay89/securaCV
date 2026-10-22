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

**Important:** Secure Boot and Flash Encryption are **Phase 2 targets that no step of the
kit reaches yet**: as tooled, Phase 2 burns fuses but turns neither on, and may leave the
board unbootable ([Phase 2 Workflow, Step 3](#step-3-provision-device-dry-run-first)).
Phase 1 burns nothing, so it prepares devices without bricking them during development.

---

## Security Architecture Phases

### Phase 1: Development Mode (Current)

- Keys generated and stored in NVS
- Firmware unsigned (allows rapid iteration)
- Flash unencrypted (allows debugging)
- JTAG enabled (allows debugging)
- **Focus:** Functional validation without bricking risk

### Phase 2: Production Lockdown (Future)

The target. As tooled, `provision_canary.sh --phase 2` reaches only the JTAG line below:
it burns `SECURE_BOOT_EN` with no key digest and no flash-encryption counter (Step 3 of
the Phase 2 workflow).

- Secure Boot v2 with RSA-3072 signing
- XTS-AES-256 Flash Encryption
- Anti-rollback protection
- JTAG permanently disabled via eFuse
- NVS encryption — named in `sdkconfig.defaults.secure` but inert under
  `framework = arduino` (`platformio_secure.ini`, "KNOWN INERT"), so a Phase 2
  board's NVS (the identity key, the saved Wi-Fi and broker passwords, the
  bonds) stays plaintext until the arduino-as-IDF-component migration
- **Irreversible:** Once burned, device cannot return to dev mode

---

## What's in the Provisioning Kit

All tools are located in `firmware/provisioning/`:

| File | Purpose |
|------|---------|
| `generate_keys.sh` | One-time RSA-3072 signing key + XTS-AES flash encryption key generation with entropy checks and backup checklist |
| `verify_device.py` | Reads security-relevant eFuses via `espefuse.py`, checks virgin state pre-provisioning and lockdown post-provisioning, outputs JSON audit trail |
| `provision_canary.sh` | Orchestration: virgin verify → flash firmware (plaintext, unsigned) → burn the flash-encryption key → burn security eFuses → post-verify → generate manifest. Has `--dry-run` mode. As tooled it turns on neither Secure Boot nor flash encryption (Step 3 below) |
| `create_manifest.py` | Fleet management: list provisioned devices, search by MAC, generate summary reports |
| `sdkconfig.defaults.secure` | ESP-IDF Kconfig with BT disabled; Secure Boot v2 (with anti-rollback) and Flash Encryption (release mode) **prepared but not enabled** (commented out); NVS encryption named but inert under `framework = arduino`; JTAG is left to `provision_canary.sh`'s eFuse burn |
| `platformio_secure.ini` | PlatformIO environment config that wraps the security sdkconfig (`[env:secure]`, `[env:secure_ha]`; `firmware/canary/platformio.ini` includes it, and CI compiles both envs, compile-only) |
| `partitions_secure.csv` | Partition table with OTA A/B slots, an `nvs_keys` partition, and `encrypted` flags on the partitions flash encryption may cover — never on `nvs` |

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
#   keys/secure_boot_signing_key.pub  (not yet used: no step burns its digest)
#   keys/flash_encryption_key.bin     (KEEP SECRET - one per device batch)
#   keys/key_inventory.json           (tracking metadata)
```

**CRITICAL:** Back up `keys/` directory to offline storage before proceeding. The kit
does not use the signing key yet (no step burns its digest or signs with it, Step 3); it
is the key a Secure Boot set up from it would need for every later image.

### Step 2: Build the Secure Env's Firmware

```bash
# Use secure PlatformIO environment (from firmware/canary). The image is not
# Secure-Boot-signed: the signing lines in sdkconfig.defaults.secure are
# commented out, and that file is inert under framework = arduino.
pio run -e secure

# There is no ESP-IDF project for this tree yet (the arduino-as-IDF-component
# migration, firmware/ESP32S3_OPTIMIZATION_ROADMAP.md item 9), so no idf.py
# build reads sdkconfig.defaults.secure either.
```

### Step 3: Provision Device (Dry Run First!)

```bash
# ALWAYS dry-run first to preview what will happen
./provision_canary.sh --port /dev/ttyACM0 --phase 2 --dry-run

# Review output carefully, then execute for real:
./provision_canary.sh --port /dev/ttyACM0 --phase 2

# What the script does today, in order (provision_canary.sh):
# 1. Verify device is virgin
# 2. Flash the secure env's app image (or --firmware PATH) at 0x10000, in
#    plaintext: no bootloader, no Secure Boot signature, no --encrypt
# 3. Burn the flash encryption key into BLOCK_KEY0 (XTS_AES_256_KEY)
# 4. Burn SECURE_BOOT_EN, SOFT_DIS_JTAG (7), DIS_USB_JTAG, DIS_PAD_JTAG
#    and DIS_DOWNLOAD_MANUAL_ENCRYPT
# 5. Run verify_device.py --expect-locked, which fails and stops the run
#    (it expects SPI_BOOT_CRYPT_CNT odd, DIS_DOWNLOAD_MODE and
#    SECURE_BOOT_AGGRESSIVE_REVOKE, none of which step 4 burns)
# 6. Add the device to the fleet manifest (reached only with --skip-verify)
```

**What that leaves on the board.** Flash encryption is off after it, and
Secure Boot is enabled with nothing to verify against: neither protection
works. No step burns `SPI_BOOT_CRYPT_CNT`, so the chip neither
encrypts nor decrypts flash and the burned key goes unused; no step burns a
Secure Boot key digest or flashes a signed bootloader, so `SECURE_BOOT_EN`
is set with nothing to verify against (what the ROM then does has not been
run on a bench: treat the board as possibly unbootable). No step enables
anti-rollback (`SECURE_VERSION`) or disables ROM download mode. The RSA-3072
signing key `generate_keys.sh` makes is checked for and never used. JTAG is
off. The sdkconfig lines that would turn the two protections on are
commented out, and inert under `framework = arduino` anyway (Phase 2 above).

### Step 4: Verify Lockdown

```bash
python verify_device.py --port /dev/ttyACM0 --expect-locked

# Output: a JSON report of the security eFuses. After today's Phase 2 it
# fails: SPI_BOOT_CRYPT_CNT, DIS_DOWNLOAD_MODE and
# SECURE_BOOT_AGGRESSIVE_REVOKE are unburned (Step 3)
```

---

## Security eFuse Reference

The Phase 2 column is the lockdown's target. The script burns only the rows
marked "burned today" (Step 3); the rest are what a Phase 2 that enables
Secure Boot and flash encryption would set (`verify_device.py
--expect-locked` checks the ESP32-S3 counter and
`SECURE_BOOT_AGGRESSIVE_REVOKE` among them, which is why it fails today).
(`FLASH_CRYPT_CNT`, `JTAG_DISABLE` and
`FLASH_CRYPT_CONFIG` are the classic ESP32's names; on the ESP32-S3 the
counter is `SPI_BOOT_CRYPT_CNT` and JTAG is `DIS_PAD_JTAG` / `DIS_USB_JTAG`.)

| eFuse | Phase 1 (Dev) | Phase 2 (Prod) | Purpose |
|-------|---------------|----------------|---------|
| `SECURE_BOOT_EN` | 0 | 1 (burned today, with no key digest) | Enables Secure Boot v2 |
| `FLASH_CRYPT_CNT` | 0 | odd (1,3,5,7) (not burned today) | Enables flash encryption |
| `JTAG_DISABLE` | 0 | 1 (burned today, as `DIS_PAD_JTAG` and `DIS_USB_JTAG`) | Permanently disables JTAG |
| `FLASH_CRYPT_CONFIG` | 0 | 0xF (not burned today) | Full flash encryption |
| `SECURE_BOOT_AGGRESSIVE_REVOKE` | 0 | 1 (not burned today) | Aggressive key revocation |
| `SOFT_DIS_JTAG` | 0 | 7 (burned today) | Software JTAG disable (3-bit field) |

---

## Bluetooth Disabled by Design

The secure env compiles no Bluetooth code: its `-DFEATURE_BLE=0`,
`-DFEATURE_BLE_SCAN=0` and `-DFEATURE_BLE_STATUS=0` flags (`platformio_secure.ini`),
with the `lib_ignore` of the Bluetooth libraries in the `[env]` of
`firmware/canary/platformio.ini`, which it extends.
The `CONFIG_BT_ENABLED=n` in `sdkconfig.defaults.secure` does nothing under
`framework = arduino`, like the rest of that file. This is a deliberate
security decision:

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
nvs,       data, nvs,      0x9000,   0x5000,
otadata,   data, ota,      0xe000,   0x2000,
app0,      app,  ota_0,    0x10000,  0x1E0000,
app1,      app,  ota_1,    0x1F0000, 0x1E0000,
nvs_keys,  data, nvs_keys, 0x3D0000, 0x1000,   encrypted
spiffs,    data, spiffs,   0x3D1000, 0x2F000,  encrypted
```

Key differences:
- `encrypted` flag on `nvs_keys` and `spiffs` (app partitions are always
  encrypted once flash encryption is on)
- `nvs_keys` partition for NVS encryption key storage
- `nvs` is **not** flagged `encrypted`, on purpose. ESP-IDF's flash-encryption
  guide says the `nvs` partition cannot be encrypted, because the NVS library
  is not directly compatible with flash encryption; NVS is protected by NVS
  encryption instead (`CONFIG_NVS_ENCRYPTION`, keys in `nvs_keys`). With
  flash encryption on, IDF v4.3 and later refuse to open an NVS partition
  flagged `encrypted` (`ESP_ERR_NVS_WRONG_ENCRYPTION`), so the flag this table
  used to carry would have left NVS unreadable on a fused board (F42). Under
  `framework = arduino` there is no NVS encryption at all, so the identity key
  stays plaintext at rest and the device reports it as `key_at_rest`
  ([`design/hardware_root_of_trust.md`](design/hardware_root_of_trust.md)).

---

## OTA Updates with Secure Boot

No step of the kit sets Secure Boot up today (Step 3). Once one does:

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
the same signing key whose digest was burned to the device's eFuse (the kit
burns no digest today, Step 3).

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
