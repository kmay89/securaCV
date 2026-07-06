# Canary Display — `watch` flavor

The **Canary Watch Station**: XIAO ESP32-S3 + Seeed Round Display for XIAO
(1.28" 240×240 GC9A01, CST816S touch). A bedside/desk glance puck rendering
the fleet as a witness ring — one arc per Canary, center glyph for the
worst-severity state, tap to page, long-press to acknowledge.

- Board pins: `firmware/boards/xiao-esp32s3-round/pins/pins.h`
- Enclosure: `docs/hardware/enclosure/canary_watch_station.scad`
- UX spec: `docs/hardware/display_ux_design.md`
- Build env: `canary-display-watch` (`firmware/envs/platformio/canary-display.ini`)

Differences from `dash`: 8-witness cap (ring legibility), PWM backlight
night dimming, battery-capable hardware.
