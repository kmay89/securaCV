# Canary Display — `dash` flavor

The **Canary Dash**: Waveshare ESP32-S3-Touch-LCD-4.3 (800×480 IPS, GT911
5-point touch, CH422G IO expander). A front-door/kitchen dashboard rendering
a witness card grid plus an event timeline.

- Board pins: `firmware/boards/waveshare-esp32s3-lcd43/pins/pins.h`
- Enclosure: `docs/hardware/enclosure/canary_dash_display.scad`
- UX spec: `docs/hardware/display_ux_design.md`
- Build env: `canary-display-dash` (`firmware/envs/platformio/canary-display.ini`)

Differences from `watch`: 16-witness cap, event timeline column, and a
hardware constraint worth knowing — the backlight runs through the CH422G
expander and is **on/off only** (no PWM), so night mode is dark-theme +
scheduled backlight-off with touch-to-wake, not dimming.
