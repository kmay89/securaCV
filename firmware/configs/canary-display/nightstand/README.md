# Canary Display — `nightstand` flavor

The **Canary Nightstand**: a Waveshare ESP32 1.47" board (ST7789 172×320
portrait IPS) with one onboard WS2812 addressable RGB LED. Made for a
nightstand or a desk — **color is its language**. The LED is an
across-the-room state beacon that breathes with the fleet; the tall glass
carries the living canary over a severity color wash, a vertical witness
column (worst floats to the top), and a glance line. No touch panel — the
BOOT button and the LED are the whole input/output surface beyond the glass.

Two boards share this flavor, each its own env + OTA product:

| Board pins | MCU | Env | OTA product |
|---|---|---|---|
| `firmware/boards/waveshare-esp32c6-lcd147/pins/pins.h` | ESP32-C6, no PSRAM | `canary-display-nightstand-c6` | `securacv-canary-display-nightstand-c6` |
| `firmware/boards/waveshare-esp32s3-lcd147/pins/pins.h` | ESP32-S3, 8 MB PSRAM | `canary-display-nightstand-s3` | `securacv-canary-display-nightstand-s3` |

Same look, two budgets: the C6 renders lean (single internal buffer,
dirty-region); the S3 has room to double-buffer and animate richly.

- Panel HAL: `firmware/projects/canary-display/src/hal/display_1in47.cpp` (`CD_FLAVOR_NIGHTSTAND`)
- Ambient LED: `firmware/projects/canary-display/src/hal/ambient_led.cpp`
- Portrait face: `firmware/projects/canary-display/src/ui/portrait_ui.cpp`
- Design + bring-up: `docs/hardware/display_nightstand_line.md`

Differences from `watch`: portrait 172×320 (not a 240 round), no touch, plus
the WS2812 ambient beacon and the honest dark-when-safe night behavior
(`FEATURE_NIGHT_BLACKOUT` seeded on). Shared modal surfaces (settings,
commissioning, onboarding, splash) render in the watch's small-portrait
style until they get a portrait-native polish pass.
