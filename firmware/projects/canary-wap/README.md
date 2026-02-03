# Canary WAP

Wireless Access Point witness device with secure, privacy-preserving telemetry.

## Features

- **Witness Chain**: Ed25519 signed, hash-chained records with tamper evidence
- **GPS Tracking**: Location-aware witnessing with time coarsening for privacy
- **SD Storage**: Append-only witness record and health log storage
- **WiFi AP**: Local access point for monitoring and configuration
- **HTTP API**: REST API for status, logs, and record export
- **Web UI**: Interactive dashboard for device monitoring
- **Mesh Network**: Opera protocol peer-to-peer secure networking
- **Bluetooth**: BLE pairing and configuration
- **RF Presence**: Privacy-preserving nearby device detection

## Hardware Requirements

- **Board**: Seeed Studio XIAO ESP32S3 Sense
- **GPS**: L76K GNSS module (optional but recommended)
- **Storage**: microSD card (FAT32 formatted)

## Quick Start

1. **Install PlatformIO**:
   ```bash
   pip install platformio
   ```

2. **Create secrets file**:
   ```bash
   make secrets
   # Edit secrets/secrets.h with your credentials
   ```

3. **Build and upload**:
   ```bash
   make upload
   ```

4. **Connect to device**:
   - WiFi SSID: `SecuraCV-XXXX` (last 4 chars of MAC)
   - Password: `witness2026`
   - Open: http://192.168.4.1

## Build Configurations

| Configuration | Command | Description |
|---------------|---------|-------------|
| Default | `make build` | Full-featured WAP |
| Mobile | `make build-mobile` | Power-optimized |
| Debug | `make build-debug` | Verbose logging |

## Project Structure

```
canary-wap/
├── platformio.ini      # PlatformIO configuration
├── Makefile            # Build convenience targets
├── README.md           # This file
├── src/
│   └── main.cpp        # Application entry point
├── include/
│   └── app.h           # Application headers
└── secrets/
    ├── secrets.example.h
    └── secrets.h       # Your credentials (gitignored)
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | Device status JSON |
| `/api/health` | GET | Health metrics |
| `/api/config` | GET | Current configuration |
| `/api/config` | POST | Update configuration |
| `/api/logs` | GET | Health logs |
| `/api/logs/ack` | POST | Acknowledge logs |
| `/api/witness/export` | GET | Export witness records |
| `/api/peek/start` | GET | Start camera preview |
| `/api/peek/stop` | GET | Stop camera preview |

## Security Properties

- Unique device identity from hardware RNG
- Monotonic sequence numbers (persist across reboots)
- Hash chain with domain separation (tamper-evident)
- Ed25519 signatures on every record
- Time coarsening (5-second buckets) for privacy
- No MAC address storage in RF presence
- Session token rotation every 4 hours

## Privacy Guarantees

- No raw video transmission
- No persistent device fingerprinting
- Only aggregate RF presence counts
- Coarsened timestamps
- Local-first data storage

## Architecture

This project follows the SecuraCV firmware architecture:

- **Board definitions**: `../../boards/xiao-esp32s3-sense/`
- **Configuration**: `../../configs/canary-wap/default/`
- **Common modules**: `../../common/`
- **Build environments**: `../../envs/platformio/`

See `../../ARCHITECTURE.md` for detailed architecture documentation.

## License

See repository LICENSE file.
