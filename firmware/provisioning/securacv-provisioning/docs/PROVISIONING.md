# SecuraCV Canary — Device Provisioning & Hardening Guide

## ERRERlabs Security Provisioning System

> "The safest capabilities are the ones that don't exist."

This document describes the complete provisioning workflow for SecuraCV Canary
devices. The goal is to establish a hardware root of trust on each ESP32-S3,
verify supply chain integrity, lock down debug interfaces, and produce an
auditable device manifest for every unit that leaves the bench.

---
## Intent & Non-Goals

**Intent**
- Establish a hardware-backed chain of trust for Canary firmware and critical configuration
- Reduce attack surface by disabling debug and downgrade paths
- Produce an auditable provisioning manifest per device

**Non-Goals**
- Covert surveillance, stealth features, or owner-hostile behavior
- Bypassing lawful process or creating “anti-law” capabilities
- Guaranteeing anonymity (this is device integrity, not anonymity tooling)

## ⚠️ Safety Warning

Provisioning burns **irreversible eFuses**. A mistake can **permanently brick** devices or lock you out of updates.
Practice on sacrificial hardware first and treat keys like a root CA.

## Threat Model Summary

| Layer | What Lives Here | Can We Control It? | Risk Level |
|-------|----------------|-------------------|------------|
| Silicon / ROM | Espressif 1st-stage bootloader (immutable) | No — must trust vendor | Accepted risk |
| eFuses | One-time programmable security config | Yes — verify virgin, then burn our own | **Critical to verify** |
| Binary blobs | WiFi driver libs (proprietary) | No — compiled into ESP-IDF | Accepted risk (minimized) |
| Flash contents | Bootloader + App + Partitions | **Full control** | Fully mitigated |
| Debug interfaces | JTAG, UART download, USB-OTG | Yes — disabled via eFuse | Fully mitigated |

### What We Accept

- Espressif ROM code is a closed binary we cannot audit. This is true of every
  commercial MCU (TI, Nordic, STM32, NXP). We mitigate by sourcing from
  authorized distributors and verifying eFuse state.
- WiFi binary blobs are proprietary. We minimize exposure by running AP-only
  mode with no station scanning, no promiscuous mode, no Bluetooth.

### What We Eliminate

- **Bluetooth**: Not compiled. Not present. Zero BT binary blobs, zero HCI
  attack surface (eliminates CVE-2025-27840 class entirely).
- **JTAG**: Disabled via irreversible eFuse configuration via eFuse after provisioning.
- **UART download mode**: Switched to secure mode or permanently disabled.
- **Plaintext flash**: All flash contents encrypted with per-device unique key.
- **Unsigned firmware (ROM-enforced)**: Secure Boot v2 ensures only ERRERlabs-signed firmware
  can execute.

---

## Prerequisites

### Hardware
- ESP32-S3-WROOM-1 or ESP32-S3-WROOM-2 modules
- **Sourced from**: Mouser, DigiKey, or Espressif authorized distributors ONLY
- USB-C cable for flashing

### Software
- ESP-IDF v5.2+ (for Secure Boot v2 + Flash Encryption support)
- Python 3.8+
- `esptool`, `espefuse`, `espsecure` (bundled with ESP-IDF)
- PlatformIO (for firmware builds, wraps ESP-IDF)
- OpenSSL (for key generation)

### Environment
- Air-gapped or trusted workstation for key generation
- Secure storage for signing keys (hardware security module recommended for
  production volumes)

---

## Provisioning Stages

```
┌─────────────────────────────────────────────────────────┐
│  STAGE 0: Key Generation (once per product line)        │
│  Generate Secure Boot signing key + Flash Encryption    │
│  key. Store securely. Never leaves provisioning host.   │
├─────────────────────────────────────────────────────────┤
│  STAGE 1: Device Identity & Virgin Verification         │
│  Read chip ID, MAC, eFuse state. Verify all eFuses are  │
│  in factory-default state. ABORT if any anomaly.        │
├─────────────────────────────────────────────────────────┤
│  STAGE 2: Build Firmware (release mode)                 │
│  Compile Canary firmware with Secure Boot v2 + Flash    │
│  Encryption in RELEASE mode. BT disabled. JTAG off.     │
├─────────────────────────────────────────────────────────┤
│  STAGE 3: Flash & Provision                             │
│  Burn keys to eFuse blocks. Flash signed + encrypted    │
│  firmware. Enable security eFuses. This is irreversible.│
├─────────────────────────────────────────────────────────┤
│  STAGE 4: Post-Provisioning Verification                │
│  Verify Secure Boot active, Flash Encryption active,    │
│  JTAG disabled, UART DL mode secure. Test boot cycle.   │
├─────────────────────────────────────────────────────────┤
│  STAGE 5: Device Manifest & Audit Log                   │
│  Record device identity, provisioning timestamp, eFuse  │
│  summary, firmware hash, operator ID. Sign manifest.    │
└─────────────────────────────────────────────────────────┘
```

---

## Security eFuse Map (ESP32-S3)

After provisioning, these eFuses should be in the following state:

| eFuse | Expected Value | Purpose |
|-------|---------------|---------|
| `SECURE_BOOT_EN` | 1 | Enables Secure Boot v2 |
| `SPI_BOOT_CRYPT_CNT` | 0b111 (7) | Enables flash encryption (release) |
| `DIS_DOWNLOAD_MANUAL_ENCRYPT` | 1 | Disables manual encryption in DL mode |
| `DIS_PAD_JTAG` | 1 | Disables JTAG via pads |
| `DIS_USB_JTAG` | 1 | Disables JTAG via USB |
| `ENABLE_SECURITY_DOWNLOAD` | 1 | Restricts UART download to secure mode |
| `KEY_PURPOSE_0` | XTS_AES_256_KEY_1 or XTS_AES_128_KEY | Flash encryption key |
| `KEY_PURPOSE_1` | SECURE_BOOT_DIGEST0 | Secure boot public key digest |
| `BLOCK_KEY0` | [write-protected, read-protected] | Flash encryption key storage |
| `BLOCK_KEY1` | [write-protected] | Secure boot digest storage |

---

## Emergency Procedures

### Provisioning Failure Mid-Burn
If power is lost during eFuse burning, the device may be in a partially
provisioned state. Run `verify_device.py` to assess. If Secure Boot is enabled
but firmware is not properly flashed, the device is bricked. This is by design —
a partially provisioned device should not boot.

### Key Compromise
If the Secure Boot signing key is compromised:
1. Immediately stop all provisioning.
2. Generate new key pair.
3. All previously provisioned devices remain secure (their eFuses contain the
   OLD key digest — attacker cannot re-flash them either).
4. New devices are provisioned with the new key.
5. OTA updates for existing devices require the OLD key (ESP32-S3 supports up
   to 3 key slots for key rotation).

### Suspected Supply Chain Tampering
If `verify_device.py` detects non-virgin eFuses on a "new" device:
1. **DO NOT PROVISION.** The device is compromised or used.
2. Log the device MAC and chip ID.
3. Photograph the module and packaging.
4. Contact the supplier.
5. Quarantine the entire batch.

---

## File Manifest

```
securacv-provisioning/
├── scripts/
│   ├── generate_keys.sh          # Stage 0: One-time key generation
│   ├── verify_device.py          # Stage 1: eFuse virgin verification
│   ├── provision_canary.sh       # Stage 2-4: Main provisioning orchestrator
│   └── create_manifest.py        # Stage 5: Audit log generation
├── keys/                         # NEVER COMMIT TO VERSION CONTROL
│   ├── .gitignore
│   ├── secure_boot_signing_key.pem
│   └── flash_encryption_key.bin
├── logs/                         # Device provisioning audit trail
│   └── manifests/
├── firmware/                     # Built firmware artifacts
│   └── .gitkeep
├── sdkconfig.defaults.secure     # ESP-IDF security Kconfig overrides
└── docs/
    └── PROVISIONING.md           # This file
```

---

## Legal & Responsible Use

See `LEGAL.md` for warranty disclaimers, responsible-use expectations, and safety notes.
