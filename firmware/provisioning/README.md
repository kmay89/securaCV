# SecuraCV Canary — Hardware Provisioning Kit

Tools for secure hardware provisioning of SecuraCV Canary devices.

## Quick Start (Phase 1 - Development)

```bash
# 1. Verify device state
python verify_device.py --port /dev/ttyACM0 --expect-virgin

# 2. Flash development firmware (uses main platformio.ini)
cd ../canary && pio run -e dev -t upload

# 3. Device auto-provisions keys on first boot
# Check serial output for device fingerprint
```

## Phase 2 (not usable yet: do not run it on a board you want to keep)

**WARNING:** Phase 2 operations are IRREVERSIBLE, and as tooled today they
turn on neither Secure Boot nor flash encryption. `provision_canary.sh
--phase 2` writes the app in plaintext, burns the flash-encryption key,
`SECURE_BOOT_EN` with no Secure Boot key digest, and the JTAG fuses, but no
`SPI_BOOT_CRYPT_CNT`; its own `verify_device.py --expect-locked` then fails
and stops the run. The board ends with flash encryption off and Secure Boot
enabled with nothing to verify against, and what the ROM then does has not
been run on a bench: treat it as possibly unbootable. Uncommenting the
Secure Boot and flash-encryption lines in `sdkconfig.defaults.secure` does
not change this: that file is inert under `framework = arduino`
(`platformio_secure.ini`, "KNOWN INERT"). And neither protection, once the
kit can turn it on, covers NVS, where the identity key and the saved
passwords stay plaintext. See `docs/secure_provisioning.md`, Step 3.

What the dry run shows, which is the only Phase 2 step to run today:

```bash
# 1. Generate the keys (Phase 2 burns the flash-encryption key; the
#    signing key is checked for and not yet used)
./generate_keys.sh

# 2. Preview what Phase 2 would burn
./provision_canary.sh --port /dev/ttyACM0 --phase 2 --dry-run
```

## Files

| File | Purpose |
|------|---------|
| `generate_keys.sh` | Generate RSA-3072 + XTS-AES keys with entropy validation |
| `verify_device.py` | Read/verify security eFuses |
| `provision_canary.sh` | Provisioning orchestration (its Phase 2 turns on neither protection yet) |
| `create_manifest.py` | Fleet device management |
| `sdkconfig.defaults.secure` | Staged ESP-IDF configuration (Secure Boot and flash encryption commented out; inert under `framework = arduino`) |
| `platformio_secure.ini` | The secure PlatformIO environment (compile-only; it refuses the identity key on every board) |
| `partitions_secure.csv` | Partition table with `encrypted` flags on `nvs_keys` and `spiffs` (never on `nvs`) |

## Security Architecture

- **Phase 1 (Current):** Development mode, no eFuse burning
- **Phase 2 (target, not reached):** Secure Boot v2, flash encryption and JTAG disabled. As tooled it reaches only the last: it burns `SECURE_BOOT_EN` with no key digest and no flash-encryption counter (above)

See `docs/secure_provisioning.md` for full documentation.

## Important Notes

- Keys in `keys/` are gitignored - back them up securely!
- Run Phase 2 only with `--dry-run` today (Phase 2 above); Phase 1 burns nothing
- The secure env compiles no Bluetooth code (its `-DFEATURE_BLE=0` flags; eliminates CVE-2025-27840)
- This kit does NOT affect Arduino WAP test files
