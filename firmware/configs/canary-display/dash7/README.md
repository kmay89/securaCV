# Canary Display — `dash7` flavor

The **Canary Dash 7**: Waveshare ESP32-S3-Touch-LCD-7 (7" 800×480 IPS, GT911
5-point touch, CH422G expander). Electrically the 4.3" Canary Dash at 7" —
**same 800×480 canvas, same RGB HAL / GT911 / CH422G** — so it reuses the
Dash flavor (`CD_FLAVOR_DASH`) and the `dash_ui` layout wholesale. The glass
is simply larger: roomier touch targets, more whitespace, the same pixels.

The new capability is real **5-point multitouch** (the GT911 reports up to 5,
and the board pins carry `TOUCH_MAX_POINTS 5`); the full-report wiring and
gestures ride the shared dash HAL/UI.

- Board pins: `firmware/boards/waveshare-esp32s3-lcd7/pins/pins.h`
- Build env: `canary-display-dash7` (`firmware/envs/platformio/canary-display.ini`)
- OTA product: `securacv-canary-display-dash7`
- Design + bring-up: `docs/hardware/display_nightstand_line.md` §6

Differences from `dash`: board pins and OTA product only — same firmware,
bigger glass. The `CH422G` touch-reset must fire before the GT911 enumerates
(handled by `display_dash.cpp`); the 8 MB octal PSRAM is mandatory for the
768 KB framebuffer.
