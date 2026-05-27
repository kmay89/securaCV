# SecuraCV Canary Serial Monitor

Real-time serial debug monitor for SecuraCV Canary witness devices. Connects
over USB serial (UART), parses firmware log output, and displays a live TUI
dashboard with witness chain stats, device health, and colorized logs.

## Quick Start

```bash
# Install dependency
pip install -r requirements.txt

# Auto-detect and connect
python canary_monitor.py

# Or specify port explicitly
python canary_monitor.py --port /dev/ttyUSB0
python canary_monitor.py --port COM3          # Windows
```

## Features

- **Auto-detect** serial ports with known ESP32 USB-serial chips (CP2102, CH340,
  CH9102, FT232R, ESP32-S3 native USB)
- **Colorized log output** — INFO (green), WARN (yellow), ERROR (red), DEBUG (cyan)
- **Live dashboard** with:
  - Device identity, firmware version, uptime
  - Heap usage (free / minimum watermark)
  - Witness chain height and record count
  - Subsystem status (WiFi, GPS, SD, Camera, Mesh, BLE, etc.)
  - Battery voltage and state-of-charge
  - Recent witness chain events table
- **Raw mode** for piping to files or other tools
- **Graceful exit** with Ctrl+C and session summary

## Usage

```
usage: canary_monitor [-h] [--port PORT] [--baud BAUD] [--raw] [--list] [--version]

Options:
  --port, -p    Serial port (auto-detect if omitted)
  --baud, -b    Baud rate (default: 115200)
  --raw,  -r    Raw passthrough, no TUI dashboard
  --list, -l    List available serial ports and exit
  --version     Show version
```

## Supported Hardware

| USB-Serial Chip   | VID:PID     | Common Board                      |
|--------------------|-------------|-----------------------------------|
| CP2102 / CP2104    | 10C4:EA60   | Many ESP32 dev boards             |
| CH340              | 1A86:7523   | Budget ESP32 boards               |
| CH9102             | 1A86:55D4   | XIAO ESP32-S3                     |
| FT232R             | 0403:6001   | FTDI-based boards                 |
| FT231X             | 0403:6015   | Adafruit/SparkFun boards          |
| ESP32-S3 native    | 303A:1001   | USB-JTAG/Serial (no external IC)  |

## Example Output

### Dashboard Mode (default)

```
 SecuraCV Canary Monitor ─────────────────────────────────────────
  Device: canary-s3-a1b2  |  FW: 2.1.0  |  Uptime: 4h 23m 15s  |  State: stationary
  Heap: 142,816 (min: 98,304)  |  Records: 1,247  |  Chain seq: 1,248
  Subsystems: audio camera csi datamgmt diag gps mqtt policy power sd setup watchdog wifi
  GPS:OK | SD:OK | WiFi:OK | BAT:78%/3890mV | Health:95%
  Witness Chain:
    seq=1248   WITNESS        12s ago    state: stationary, fix: true
    seq=1247   WITNESS        13s ago    state: stationary, fix: true
    seq=1       BOOT_ATTEST    4h ago     Boot attestation record created
──────────────────────────────────────────────────────────────────
[INFO ] APP: SecuraCV Canary WAP v2.1.0 starting...
[  OK] Device ID: canary-s3-a1b2
[  OK] WiFi AP active
[  OK] SD card ready for witness records
[  OK] Boot attestation: seq=1
[  OK] Self-test: 95% health score
```

### Raw Mode

```bash
python canary_monitor.py --raw > canary_log.txt
```

Plain serial output is printed to stdout with no ANSI formatting,
suitable for logging to a file or piping to analysis tools.

## Log Format Reference

The Canary firmware emits log lines in two formats:

1. **Structured logs** (from `log.h` macros):
   ```
   [I] TAG: message      (INFO)
   [W] TAG: message      (WARN)
   [E] TAG: message      (ERROR)
   [D] TAG: message      (DEBUG)
   [V] TAG: message      (VERBOSE)
   ```

2. **Status messages** (from `Serial.printf`):
   ```
   [OK] Component initialized
   [..] Starting component...
   [WARN] Component degraded
   [ERR] Component failed
   [!!] Critical error
   ```

## Requirements

- Python 3.8+
- `pyserial` >= 3.5
- A terminal with ANSI color support (all modern terminals)

## License

Apache-2.0 — Copyright (c) 2026 ERRERlabs / Karl May
