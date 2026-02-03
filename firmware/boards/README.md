# Board Definitions

Pin maps and board-specific wiring live here.

## Supported Boards

| Board ID | MCU | Description |
|----------|-----|-------------|
| `xiao-esp32s3-sense` | ESP32-S3 | Seeed XIAO with camera, mic, SD slot |
| `esp32-c3` | ESP32-C3 | Generic C3 dev board for Vision AI |

## Directory Structure

Each board directory follows this pattern:

```
boards/
  <board-id>/
    README.md           # Board metadata and constraints
    pins/
      pins.h            # Main pin definitions
      camera.h          # Camera pin config (if applicable)
      ...               # Other peripheral-specific pin files
    variants/           # Optional board revisions
```

## Adding a New Board

1. Create a new directory: `boards/<board-id>/`
2. Add `README.md` with board specs and constraints
3. Create `pins/pins.h` with all pin definitions
4. Add capability flags (`HAS_CAMERA`, `HAS_BLE`, etc.)
5. Update this README with the new board entry

## Pin Definition Rules

- Pin files must ONLY contain `#define` statements
- No logic, functions, or variables in pin files
- Use `-1` for pins that are not connected
- Include capability flags for conditional compilation
- Document any pin conflicts or boot strapping requirements

## Board Capability Flags

Standard capability flags (define in each `pins.h`):

```cpp
#define HAS_CAMERA          0/1
#define HAS_MICROPHONE      0/1
#define HAS_SD_CARD         0/1
#define HAS_PSRAM           0/1
#define HAS_USB_CDC         0/1
#define HAS_WIFI            0/1
#define HAS_BLE             0/1
#define HAS_GNSS_UART       0/1
#define HAS_TAMPER_INPUT    0/1
#define HAS_VISION_AI       0/1
```
