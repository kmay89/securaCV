# Firmware Configurations

Configuration sets and feature flags for firmware targets.

## Configuration Structure

Each config is a directory `configs/<app-id>/<config-id>/` holding a
`config.h` (feature flags + behavior settings) and usually a `README.md`.
Four families exist today:

```
configs/
├── canary-display/     # dash, dash7, nightlight, nightstand,
│                       # nightstand7, touch169, watch
├── canary-sense/       # default, wellbeing
├── canary-sentinel/    # door, window, hallway, mailbox-lite, perimeter-demo
└── canary-vision/      # default
```

The Canary WAP has no directory here. Its sketch takes every flag from its
own `build_config.h` (the `BUILD_PROFILE_*` blocks, in
[`../projects/canary-wap/arduino/canary_wap/`](../projects/canary-wap/arduino/canary_wap/)),
and the Lab's generators read that header directly.

## Available Configurations

| App ID | Config ID | Description |
|--------|-----------|-------------|
| canary-display | dash | 4.3" dashboard glass |
| canary-display | dash7 | 7" big-glass dashboard |
| canary-display | nightlight | C3 1.47" pocket nightlight |
| canary-display | nightstand | 1.47" portrait bedside glass |
| canary-display | nightstand7 | 7" bedside glass |
| canary-display | touch169 | 240×280 touch portrait |
| canary-display | watch | Glance puck |
| canary-sense | default | Presence-only sensing |
| canary-sense | wellbeing | Presence + vitals |
| canary-sentinel | door | Standard-tier door preset |
| canary-sentinel | window | Standard-tier window preset |
| canary-sentinel | hallway | Standard-tier corridor preset |
| canary-sentinel | mailbox-lite | LITE tier (no radar, no CSI) |
| canary-sentinel | perimeter-demo | HEAVY tier, the rigged demo |
| canary-vision | default | Vision AI presence detection |

Every config here is wired into PlatformIO builds via `-I` paths in
[`../envs/platformio/`](../envs/platformio/), and
`firmware/scripts/check_config_feature_flags.py` requires each config's
`FEATURE_*` flags to be read by its project's sources. A directory no build
compiles fails that check unless it is listed, with the reason, in the
script's `SKIPPED_PROJECTS` (empty today). The Lab's generators read the
`FEATURE_*` lines (`canary-local/tools/gen_enclosures.py`, into
`canary-local/devices/workshop.json`) and the device types
(`canary-local/tools/figures/gen_figures.mjs`) from these files; for the
WAP they read the sketch instead — its flags from `build_config.h`, and the
device type it puts on the fleet's wire from `canary_wap.ino`'s mDNS `dt`
record and `csi_mqtt.cpp`'s status topic.

## Configuration Rules

1. **No board-specific data**: Configs contain only feature flags and
   behavior settings, not pin mappings.

2. **No code**: Configs are data only (`#define` statements).

3. **Inheritance**: Specialized configs can include a base config and
   override specific values.

4. **Local overrides**: Projects can create `config_local.h` to override
   settings without modifying the tracked config files.

5. **Breaking changes are logged**: renaming or removing a flag, or
   changing a default in a behavior-visible way, requires a dated entry in
   [../CONFIG_CHANGES.md](../CONFIG_CHANGES.md) **and** updating every
   shipped config the change breaks, in the same PR. CI builds every
   config that a PlatformIO env consumes on every PR, so a missed one
   fails loudly.

6. **Capability mismatches fail the build**: feature flags are
   cross-checked against the board's `HAS_*` capability flags by
   [../common/core/feature_sanity.h](../common/core/feature_sanity.h).
   Enabling a feature the board can't carry produces a compile-time
   `#error` naming the flag — not a runtime mystery.

## Creating a New Configuration

1. Create directory: `configs/<app-id>/<config-id>/`
2. Create `config.h` with feature flags and settings
3. Create `README.md` documenting the configuration
4. Reference in build environment

## Feature Flag Naming

Use `FEATURE_` prefix for on/off toggles:

```cpp
#define FEATURE_SD_STORAGE      1   // Enable SD card storage
#define FEATURE_WIFI_AP         1   // Enable WiFi Access Point
#define FEATURE_MESH_NETWORK    0   // Disable mesh networking
```

Use `CONFIG_` prefix for values:

```cpp
#define CONFIG_RECORD_INTERVAL_MS   1000
#define CONFIG_AP_CHANNEL           1
```

(Never define a password value in a config — AP credentials are derived
per device at runtime; a shared compile-time password is a hard CI failure
in `regression_check.sh`.)

Use `DEBUG_` prefix for debug flags:

```cpp
#define DEBUG_NMEA              0   // Print NMEA sentences
#define DEBUG_HTTP              0   // Print HTTP details
```
