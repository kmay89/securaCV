# Canary Display — `nightlight` flavor

The **Canary Nightlight**: the Waveshare ESP32-C3-LCD-1.47 in the pocket
case, turned toward a kid's bedside. A big 7-segment clock over a soft lamp
wash, with a canary companion who visits on the household's rhythm — up
with you in the morning, a little song at midday, winding down in the
evening, asleep beside the clock after bedtime. Flash it in the Flasher,
join WiFi on the glass (SoftAP wizard + join QR), tune it from the iPhone
app — **zero other tools**.

One board wears this flavor:

| Board pins | MCU | Env | OTA product |
|---|---|---|---|
| `firmware/boards/waveshare-esp32c3-lcd147/pins/pins.h` | ESP32-C3, no PSRAM | `canary-display-nightlight-c3` | `securacv-canary-display-nightlight-c3` |

- Panel HAL: `firmware/projects/canary-display/src/hal/display_1in47.cpp`
  (`CD_FLAVOR_NIGHTSTAND` + the `TFT_USE_EXIO` expander path — CS/RST and
  the backlight PWM are I2C register writes on this board)
- Face: `firmware/projects/canary-display/src/ui/nightlight_ui.cpp`
  (`CD_NIGHTLIGHT` swaps `portrait_ui.cpp` out, the same way the
  Nightstand 7 swaps the dash poster)
- Visits model: `include/canary/care/nightlight.h` (pure, host-tested)
- Design: `docs/hardware/display_nightstand_line.md`

## The two personality departures (product decisions, not drift)

1. **The lamp defaults ON through quiet hours** (`CD_LANTERN_AUTO 1`). A
   nightlight that ships dark is a clock. The honest-night rule survives
   intact because this flavor never renders safety as light: the lamp is a
   look-engine scene (decor), the clock is information, and the lantern
   model's attention veto still extinguishes the lamp and takes the glass
   back the instant anything wants attention. Light never means "safe" —
   only "the lamp you asked for is on."
2. **Brightness is capped at 50% duty in the HAL** (`CD_BL_MAX_PCT 50`),
   enforced at the EXIO PWM register underneath every settings path — a
   heat budget for the closed PETG pocket case. A can't, not a won't.

## The lamp's looks

The scene ring is the shared look engine (`firmware/common/color`): the
warm **Lantern** orange (the shipped default), the full **Rainbow** sweep,
**Moonbeam** bright white, and the rest. Walk it with a BOOT double-press
(summon/dismiss) — the ring itself cycles from the app (`/api/set?k=lamp_scene`)
or the on-glass settings.

Differences from `nightstand`: no WS2812 (this board doesn't carry one — the
glass is the whole lamp), no comfort/weather lines, no proof QR, BLE
compiled out for the 4 MB OTA slot, presence-wake off (a bedside lamp must
not flare when someone walks past a hallway canary), and the 7-segment
clock face instead of the witness column. The fleet machinery stays: if
the household later adds real Canaries, the nightlight joins the fleet and
its companion's honesty is already wired.
