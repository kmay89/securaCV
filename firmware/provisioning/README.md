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

## Production Provisioning (Phase 2 - Future)

**WARNING:** Phase 2 operations are IRREVERSIBLE.

```bash
# 1. Generate signing keys (one-time)
./generate_keys.sh

# 2. Preview provisioning (ALWAYS do this first!)
./provision_canary.sh --port /dev/ttyACM0 --phase 2 --dry-run

# 3. Execute production provisioning
./provision_canary.sh --port /dev/ttyACM0 --phase 2
```

## Files

| File | Purpose |
|------|---------|
| `generate_keys.sh` | Generate RSA-3072 + XTS-AES keys with entropy validation |
| `verify_device.py` | Read/verify security eFuses |
| `provision_canary.sh` | Full provisioning orchestration |
| `create_manifest.py` | Fleet device management |
| `sdkconfig.defaults.secure` | Secure ESP-IDF configuration |
| `platformio_secure.ini` | Secure PlatformIO environment |
| `partitions_secure.csv` | Partition table with encryption flags |

## Security Architecture

- **Phase 1 (Current):** Development mode, no eFuse burning
- **Phase 2 (Future):** Production lockdown with Secure Boot v2, Flash Encryption, JTAG disabled

See `docs/secure_provisioning.md` for full documentation.

## Important Notes

- Keys in `keys/` are gitignored - back them up securely!
- Always use `--dry-run` before production provisioning
- Bluetooth is disabled by design (eliminates CVE-2025-27840)
- This kit does NOT affect Arduino WAP test files
