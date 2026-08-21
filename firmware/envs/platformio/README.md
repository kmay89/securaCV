# PlatformIO Build Environments

PlatformIO configuration files for SecuraCV firmware targets.

## Files

| File | Environments | Description |
|------|:---:|-------------|
| `common.ini` | — | Shared bases (`common`, `common_esp32s3`, `common_esp32c3`) every env extends |
| `canary-display.ini` | 21 | Canary Display flavors (watch, dash + option probes, dash7, nightstand-s3/-c6, nightstand7, touch169, nightlight-c3, playground, debug) |
| `canary-sense.ini` | 3 | Canary Sense (default, wellbeing, debug) |
| `canary-sentinel.ini` | 5 | Canary Sentinel presets (door, window, hallway, demo-head, lite) |
| `canary-vision.ini` | 5 | Canary Vision (default, debug, xiao-c3, c3-super-mini, xiao-s3) |
| `canary-wap.ini` | 4 | Canary WAP (default, mobile, debug, usbdrive) |

The env lists above are counts, not a registry — **the `.ini` files are the
source of truth.** To see every env with its board and flags, read the ini
itself or run `pio project config` in the consuming project. (A hand-drawn
inheritance tree used to live here; it drifted to covering half the files
and was removed rather than re-drawn.)

## Using in Projects

Projects include these environment files in their `platformio.ini`:

```ini
[platformio]
extra_configs =
    ../../envs/platformio/common.ini
    ../../envs/platformio/canary-wap.ini

default_envs = canary-wap-default
```

Build any env from its project directory:

```bash
pio run -e canary-display-watch     # from projects/canary-display/
pio run -e canary-wap-default       # from projects/canary-wap/
```

## Build Flags

All environments extend `common.ini` and pull shared modules from
`firmware/common/`. Board pin definitions come from `boards/<board-id>/pins/`
via per-env `-I` flags.

Configuration handling differs by family:

- **canary-display, canary-sense, canary-sentinel, canary-vision** — each env
  adds `-I ../../configs/<app-id>/<config-id>/` so the matching `config.h`
  from `firmware/configs/` is on the include path.
- **canary-wap** — does *not* include anything from `configs/`; its feature
  selection lives in the sketch's own `build_config.h` profiles
  (`-DBUILD_PROFILE_DEV` etc. in `canary-wap.ini`).
