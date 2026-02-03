# PlatformIO Build Environments

PlatformIO configuration files for SecuraCV firmware targets.

## Files

| File | Description |
|------|-------------|
| `common.ini` | Shared settings across all environments |
| `canary-wap.ini` | Canary WAP environments |
| `canary-vision.ini` | Canary Vision environments |

## Environment Inheritance

```
common.ini
├── common_esp32s3 (ESP32-S3 settings)
│   └── canary-wap-default
│       ├── canary-wap-mobile
│       └── canary-wap-debug
└── common_esp32c3 (ESP32-C3 settings)
    └── canary-vision-default
        └── canary-vision-debug
```

## Using in Projects

Projects include these environment files in their `platformio.ini`:

```ini
[platformio]
extra_configs =
    ../../envs/platformio/common.ini
    ../../envs/platformio/canary-wap.ini

default_envs = canary-wap-default
```

## Build Flags

All environments automatically include:

- Board pin definitions from `boards/<board-id>/pins/`
- Configuration from `configs/<app-id>/<config-id>/`
- Common modules from `common/`

## Available Environments

### Canary WAP

```bash
# Default full-featured build
pio run -e canary-wap-default

# Mobile power-optimized build
pio run -e canary-wap-mobile

# Debug build with verbose logging
pio run -e canary-wap-debug
```

### Canary Vision

```bash
# Default build
pio run -e canary-vision-default

# Debug build
pio run -e canary-vision-debug
```
