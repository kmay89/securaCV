# Firmware Configurations

Configuration sets and feature flags for firmware targets.

## Configuration Structure

Each config is a directory `configs/<app-id>/<config-id>/` holding a
`config.h` (feature flags + behavior settings) and usually a `README.md`.
Five families exist today:

```
configs/
├── canary-display/     # dash, dash7, nightlight, nightstand,
│                       # nightstand7, touch169, watch
├── canary-sense/       # default, wellbeing
├── canary-sentinel/    # door, window, hallway, mailbox-lite, perimeter-demo
├── canary-vision/      # default
└── canary-wap/         # default, mobile
```

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
| canary-wap | default | Full-featured WAP witness device |
| canary-wap | mobile | Power-optimized portable device |

The canary-display, canary-sense, canary-sentinel and canary-vision configs
are wired into PlatformIO builds via `-I` paths in
[`../envs/platformio/`](../envs/platformio/). The two canary-wap configs are
**not compiled by any build** — the WAP sketch's flags live in its
`build_config.h` — but they are still consumed: the workshop-page generator
(`canary-local/tools/gen_enclosures.py`) parses their `FEATURE_*` lines into
`canary-local/devices/workshop.json`, which CI drift-gates.

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
   config that a PlatformIO env consumes on every PR, so a missed one in
   those families fails loudly. The canary-wap configs are the exception —
   no build compiles them (see above), so a breaking change there is caught
   only by the workshop.json drift gate, and only if it moves a `FEATURE_*`
   line.

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
