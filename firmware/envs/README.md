# Build Environments

Toolchain and build target definitions for firmware builds.

## Environment Structure

```
envs/
└── platformio/
    ├── README.md
    ├── common.ini          # Shared PlatformIO settings
    ├── canary-wap.ini      # Canary WAP environments
    └── canary-vision.ini   # Canary Vision environments
```

## Available Environments

### Canary WAP

| Environment | Board | Config | Description |
|-------------|-------|--------|-------------|
| `canary-wap-default` | xiao-esp32s3-sense | `BUILD_PROFILE_FULL` | Full-featured WAP |
| `canary-wap-mobile` | xiao-esp32s3-sense | `BUILD_PROFILE_DEV` | The lighter DEV profile: no camera, mesh or Opera discovery (no power tuning of its own) |

The WAP's Config column is a build profile, not a `configs/` directory: the
sketch reads its flags from `projects/canary-wap/arduino/canary_wap/build_config.h`,
and `configs/canary-wap/` is on no include path (only the Lab's tooling reads it).

### Canary Vision

| Environment | Board | Config | Description |
|-------------|-------|--------|-------------|
| `canary-vision-default` | esp32-c3 | default | Standard vision |

## Environment Binding Rules

Each environment MUST specify:
1. **Board**: References `boards/<board-id>`
2. **Configuration**: References `configs/<app-id>/<config-id>`
3. **Toolchain**: Platform, framework, build flags

## Usage

From a project directory:

```bash
# Build specific environment
pio run -e canary-wap-default

# Upload
pio run -e canary-wap-default -t upload

# Monitor serial
pio device monitor -b 115200
```

## Creating a New Environment

1. Add to appropriate `.ini` file in `envs/platformio/`
2. Specify board, platform, framework
3. Add include paths for board and config
4. Reference from project's `platformio.ini`
