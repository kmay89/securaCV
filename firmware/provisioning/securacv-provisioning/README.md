# SecuraCV Canary — Provisioning & Hardening

**ERRERlabs** · _The safest capabilities are the ones that don't exist._

Hardware root-of-trust provisioning system for SecuraCV Canary devices (ESP32-S3).
This repository content is designed to be **auditable, reproducible, and owner-respecting**.

---

## Intent & Non-Goals

**Intent**
- Make devices tamper-evident and resistant to silent modification
- Establish a hardware-backed chain of trust for firmware and critical config
- Reduce attack surface by **removing** capabilities (debug, downgrade paths, etc.)
- Produce an auditable per-device provisioning manifest

**Non-Goals**
- Covert surveillance or “always-on” monitoring
- Obscuring device behavior from the owner/operator
- Bypassing lawful process or “anti-law” capabilities
- Providing anonymity guarantees

---

## ⚠️ Safety Warning (Read This First)

Provisioning burns **irreversible eFuses**. If you provision the wrong unit, use the wrong keys,
or misconfigure secure boot / flash encryption, you can **permanently brick** a device.

**Rule of thumb:** Practice on sacrificial hardware first. Treat keys like you would treat a CA root key.

---

## Quick Start

```bash
# 0) Requirements
# - ESP-IDF installed and working
# - A trusted workstation (keys should never be generated on an untrusted machine)

# 1) Source ESP-IDF
. "$IDF_PATH/export.sh"

# 2) Generate keys (ONCE — on trusted workstation)
chmod +x scripts/*.sh
./scripts/generate_keys.sh

# 3) Verify a new device is untampered ("virgin" checks)
python3 scripts/verify_device.py --port /dev/ttyUSB0

# 4) Build your Canary firmware with sdkconfig.defaults.secure
#    (set SECURE_BOOT_SIGNING_KEY path in sdkconfig)

# 5) Provision device (irreversible!)
#    You MUST pass an explicit acknowledgement flag.
./scripts/provision_canary.sh --port /dev/ttyUSB0 --skip-build     --firmware-dir /path/to/build     --i-understand-this-is-irreversible

# 6) Verify provisioning
python3 scripts/verify_device.py --port /dev/ttyUSB0 --post-provision
```

---

## What This Locks Down (After Provisioning)

| Attack Surface | Status After Provisioning |
|---------------|--------------------------|
| Bluetooth | **Not compiled** (project policy: no BLE support in Canary builds) |
| JTAG (pad + USB) | **Disabled via irreversible eFuse configuration** |
| UART download | **Secure mode only** (ROM-enforced when configured) |
| Flash readout | **Encrypted** (XTS-AES; per-device keying via eFuses) |
| Unsigned firmware | **Rejected** by Secure Boot v2 (ROM-enforced) |
| Firmware downgrade | **Blocked** via anti-rollback eFuse counters **when versions are maintained correctly** |
| NVS data | **Encrypted** (when configured) |

---

## Known Trust Boundaries (We Say This Out Loud)

| Component | Trust Status |
|-----------|-------------|
| ESP32-S3 ROM code | Closed-source, immutable — vendor trust boundary |
| WiFi binary blobs | Closed-source — minimized (AP-only when used) |
| Espressif toolchain | Largely open-source; review recommended |
| Our firmware | Open-source, signed, encrypted |

---

## File Structure

```
securacv-provisioning/
├── scripts/
│   ├── generate_keys.sh      # Stage 0: Key generation
│   ├── verify_device.py      # Stage 1/4: Device verification
│   ├── provision_canary.sh   # Stage 2-4: Provisioning (irreversible)
│   └── create_manifest.py    # Stage 5: Fleet management
├── keys/                     # ⚠ NEVER COMMIT ⚠
├── logs/
│   └── manifests/            # Per-device audit trail
├── docs/
│   └── PROVISIONING.md       # Full documentation + threat model
├── sdkconfig.defaults.secure # ESP-IDF security config baseline
├── platformio_secure.ini     # PlatformIO secure environment
└── partitions_secure.csv     # Encrypted partition table
```

---

## Provisioning Flow

```
  [New ESP32-S3 from authorized distributor]
                  │
                  ▼
  ┌──────────────────────────────┐
  │ verify_device.py (virgin)    │──── FAIL ──→ QUARANTINE
  └──────────────────────────────┘
                  │ PASS
                  ▼
  ┌──────────────────────────────┐
  │ provision_canary.sh          │
  │  • Configure flash enc       │
  │  • Configure secure boot     │
  │  • Flash signed firmware     │
  │  • Burn security eFuses      │
  └──────────────────────────────┘
                  │
                  ▼
  ┌──────────────────────────────┐
  │ verify_device.py (post-prov) │──── FAIL ──→ INVESTIGATE
  └──────────────────────────────┘
                  │ PASS
                  ▼
  ┌──────────────────────────────┐
  │ Manifest logged              │
  │ Device ready for deployment  │
  └──────────────────────────────┘
```

---

## Updating Provisioned Devices

After provisioning, USB flashing may be restricted/locked depending on your eFuse configuration.
Updates should be treated as **OTA-only** in production:

1. Build new firmware, signed with the same signing key
2. Increment the secure version (`CONFIG_BOOTLOADER_APP_SECURE_VERSION` / app versioning)
3. Serve signed binary via HTTPS (or another authenticated transport)
4. Device verifies signature → decrypts → writes to inactive OTA slot
5. Anti-rollback prevents downgrade

---

## Legal, Warranty, and Licensing

- See `LEGAL.md` for safety, warranty, and responsible-use terms.
- Unless otherwise noted, this module is licensed under **Apache-2.0**. See `LICENSE` and `NOTICE`.

---

## Documentation

See `docs/PROVISIONING.md` for the full guide including threat model, emergency procedures,
and key-handling guidance.
