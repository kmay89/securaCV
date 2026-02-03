# Firmware Configurations

Configuration sets and feature flags for firmware targets.

## Configuration Structure

```
configs/
├── canary-wap/
│   ├── default/        # Standard WAP configuration
│   │   ├── config.h
│   │   └── README.md
│   └── mobile/         # Power-optimized mobile config
│       ├── config.h
│       └── README.md
└── canary-vision/
    └── default/        # Standard vision config
        ├── config.h
        └── README.md
```

## Available Configurations

| App ID | Config ID | Description |
|--------|-----------|-------------|
| canary-wap | default | Full-featured WAP witness device |
| canary-wap | mobile | Power-optimized portable device |
| canary-vision | default | Vision AI presence detection |

## Configuration Rules

1. **No board-specific data**: Configs contain only feature flags and
   behavior settings, not pin mappings.

2. **No code**: Configs are data only (`#define` statements).

3. **Inheritance**: Specialized configs can include a base config and
   override specific values.

4. **Local overrides**: Projects can create `config_local.h` to override
   settings without modifying the tracked config files.

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
#define CONFIG_AP_PASSWORD          "witness2026"
```

Use `DEBUG_` prefix for debug flags:

```cpp
#define DEBUG_NMEA              0   // Print NMEA sentences
#define DEBUG_HTTP              0   // Print HTTP details
```
