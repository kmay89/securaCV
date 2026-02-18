# Canary WAP (Snapshot)

This directory captures a **frozen snapshot** of the existing Arduino sketch so
we can iterate toward the firmware architecture without breaking the current
working demo. The files under `snapshot/` are intentionally **not wired into any
build system**; they serve as a reference baseline only.

## Quick Start — Fastest Builds

### 1. Arduino IDE Settings (one-time)

Open Arduino IDE and configure for faster builds:

```
File > Preferences:
  [x] Enable "Aggressively cache compiled core"
  Set "Compiler warnings" to "None" (optional, saves a few seconds)
```

### 2. Select Build Profile

Edit `snapshot/canary_wap/build_config.h` and uncomment ONE profile:

| Profile | Build Time | Features |
|---------|-----------|----------|
| `BUILD_PROFILE_MINIMAL` | ~45s | Crypto + GPS only (fastest iteration) |
| `BUILD_PROFILE_DEV` | ~90s | + WiFi + HTTP + SD (web UI testing) |
| `BUILD_PROFILE_FULL` | ~150s | + Camera + Mesh + BLE (production) |

```cpp
// In build_config.h, uncomment your choice:
#define BUILD_PROFILE_MINIMAL   // <-- Fastest for testing
// #define BUILD_PROFILE_DEV
// #define BUILD_PROFILE_FULL
```

### 3. Build Tips

- **Don't close Arduino IDE** between builds (cache stays warm)
- **Use Upload directly** instead of Verify then Upload (saves one compile)
- **Stay on one profile** during a testing session
- **First build is slow** (~3-5 min), subsequent builds use cache (~45-150s)

### Board Settings

```
Board: "XIAO_ESP32S3"
USB CDC On Boot: "Enabled"
PSRAM: "OPI PSRAM"
Partition Scheme: "Huge APP (3MB No OTA)"
```

---

## BLE Discovery (Opera/Chirp/Nearby)

The Canary firmware includes an optional BLE Discovery subsystem controlled by the
`FEATURE_BLE` compile flag (enabled in FULL profile). It adds three BLE subsystems:

- **Opera**: BLE server/advertising — Canary broadcasts its presence via custom GATT service
- **Chirp**: Connectionless broadcast alerts between Canary devices
- **Nearby**: BLE scanner that discovers other Canaries and tracks proximity (RSSI)

**New files**: `ble_config.h`, `ble_opera.h`, `ble_chirp.h`, `ble_nearby.h`, `ble_manager.h`

**API endpoints**: `GET /api/ble/status`, `GET /api/nearby`, `POST /api/chirp/send`

**Library dependency**: NimBLE-Arduino by h2zero (install via Arduino Library Manager)

**Antenna requirement**: The XIAO ESP32S3 has a dedicated IPEX BLE antenna connector — it
must be installed for BLE to work. If absent, the firmware gracefully degrades without BLE.

**Security note**: BLE can be compiled out entirely by setting `FEATURE_BLE=0`. When disabled,
all BLE binary blobs are removed from the build, reducing the trust surface.

See `docs/ble_protocol.md` for the full protocol specification.

---

## What belongs here

- The raw Arduino sketch files as-is (e.g., `*.ino`, `*.h`).
- No secrets, credentials, or environment-specific settings.
- No build config integration (that belongs under `envs/` and `projects/` once we
  conform to the firmware architecture).

## How to update this snapshot

1. Unzip or copy the working sketch into `snapshot/canary_wap/`.
2. Preserve file names and layout so diffs remain clear.
3. Keep this snapshot stable while we refactor into `common/`, `boards/`,
   `configs/`, and a proper project wrapper.

## Next step (planned)

We will translate this snapshot into a conforming firmware layout by extracting
board-agnostic logic into `firmware/common/`, pin maps into `firmware/boards/`,
and configuration into `firmware/configs/`, then wiring it through a proper
`envs/` target.
