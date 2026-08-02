# Waveshare ESP32-S3-Touch-AMOLED-2.06 (the wrist board)

One board, two Phase-0 projects. Both are host-tested cores with no build env
yet, which is why `used_by` is still `[]`.

- [`canary-tincan`](../../projects/canary-tincan/README.md) — **the Tin Can**: a
  kids' watch that ties a **string** to a sibling's watch and carries knocks,
  tugs and stamps across the house, with no voice, no location and no cloud.
  Design: [`docs/design/canary_tincan_kids_watch.md`](../../../docs/design/canary_tincan_kids_watch.md).
- [`canary-companion`](../../projects/canary-companion/README.md) — **the Night
  Watch** (a bedside clock that goes genuinely dark, because AMOLED lets it) and
  **the Pocket Canary** (a virtual pet with Tamagotchi's charm and without its
  guilt loop).
  Design: [`docs/design/canary_companion.md`](../../../docs/design/canary_companion.md).

Both share this pin map, both need the same haptic add-on (§2 below), and both
inherit the same pre-launch gate at the bottom of this file.

It is a watch *body*, not a watch *product*. See "Before anyone calls this a
kids' device" below — that section is the one that stops a launch.

## Hardware Specifications

- **MCU**: ESP32-S3R8 (Xtensa dual-core LX7, 240 MHz), Wi-Fi + BLE 5
- **Flash**: 32 MB · **PSRAM**: 8 MB octal
- **Display**: 2.06" **410 × 502 AMOLED**, CO5300 driver, 4-lane QSPI.
  True black costs no power, which is why the watch face is mostly black.
  **No backlight pin** — brightness is a panel command.
- **Touch**: **FT3168** @ 0x38 (see the discrepancy note below)
- **IMU**: QMI8658 6-axis @ 0x6B — steps, wake-on-raise, knock-by-wrist-tap
- **RTC**: PCF85063 @ 0x51 — the clock survives with the radio down
- **PMU**: AXP2101 @ 0x34 + Li-po (battery **not** included with the board)
- **Audio**: ES8311 codec @ 0x18, ES7210 AEC, dual digital mic array,
  PA enable on GPIO46
- **microSD**: SDMMC 1-bit
- **Breakout**: 1 × I²C, 1 × UART, USB pads

## Pin map provenance

Transcribed from the **vendor sample tree**
([waveshareteam/ESP32-S3-Touch-AMOLED-2.06](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06)) —
`examples/arduino/libraries/Mylibrary/pin_config.h` for the panel, touch, I²C
and SD lines, and `examples/arduino/08_ES8311/08_ES8311.ino` for the I2S pin
order (`setPins(41, 45, 40, 42, 16)`) and the PA-enable line. Not bench
validated. Verify against your board revision before trusting a pin.

### Re-verified against the live vendor tree — 2026-08-02

Every line of the transcription above was checked again, from source, against
`waveshareteam/ESP32-S3-Touch-AMOLED-2.06@main`. **Nothing had drifted:**

| Committed here | Vendor source | Match |
|---|---|---|
| QSPI 4/5/6/7, SCLK 11, CS 12, RST 8, 410 × 502 | `Mylibrary/pin_config.h` | ✅ |
| I²C SDA 15 / SCL 14, `TOUCH_PIN_INT` 38, `TOUCH_PIN_RST` 9 | same | ✅ |
| SDMMC CLK 2 / CMD 1 / D0 3 / CS 17 | same | ✅ |
| I2S 41 / 45 / 40 / 42 / 16, PA-enable **46** | `08_ES8311/08_ES8311.ino` | ✅ |
| AXP2101 PMU | `XPOWERS_CHIP_AXP2101` in `pin_config.h` | ✅ |

**Touch is settled as far as source can settle it.** Three separate sketches
(`01_HelloWorld`, `03_LVGL_PCF85063_simpleTime`, `06_LVGL_Arduino_v9`) construct
`Arduino_FT3x68(IIC_Bus, FT3168_DEVICE_ADDRESS, …)`. There is no CST9220 driver
anywhere in the tree. It is still the first thing to confirm on the bench — a
store page and a code tree disagreeing is exactly the situation where the *board
in your hand* is the only authority — but the evidence is now three sketches
deep, not one.

**The no-haptic finding is stronger than it was.** The vendor README's full
example index is AXP2101, LVGL v9, ESP-Brookesia, a motion block demo, a mic
spectrum analyser, a video player, GFX text, the PCF85063 RTC, the QMI8658 IMU,
AXP2101 ADC telemetry, SD, and ES8311 — **thirteen examples and not one of them
drives a motor.** No `pin_config.h` haptic pin, and no mention of vibration,
haptic, DRV2605, LRA or ERM anywhere in the README. This is now a negative
result from the primary source rather than an inference.

This is *source* verification, not bench validation. The tier stays
`compile-tested`.

## Three things that will bite you

### 1. The touch controller is FT3168, not CST9220

Waveshare's own store copy lists **CST9220**; every sketch in the vendor tree
drives an **FT3168** at `0x38` through `Arduino_DriveBus`. Vendor code beats
vendor marketing, so that's what `pins.h` says — but confirm it first at
bring-up. A wrong touch driver is a dead watch, not a degraded one.

### 2. There is no vibration motor

The board has no LRA/ERM and no haptic driver anywhere in the vendor tree. The
Tin Can's entire premise is a knock you *feel* — quiet, private, unnoticed by a
classroom — so a **DRV2605L + LRA on the exposed I²C port (0x5A)** is required
hardware, not an accessory.

`HAS_HAPTIC` is therefore **0**. The firmware probes for the driver at boot and
degrades honestly: with no motor a knock is seen (a full-screen flash) and,
where an output transducer is actually present, heard. It never silently
pretends to have buzzed.

### 3. The mics are present and deliberately unused

`HAS_MICROPHONE` is **0**. That flag describes what the *firmware may touch*,
not what is soldered down. The Tin Can carries no speech by construction — the
capture path is not compiled in, the same posture as
[`display_mic_variant.md`](../../../docs/hardware/display_mic_variant.md). The
I2S input pin is recorded in `pins.h` for completeness and must never be wired
to a capture path.

`HAS_SPEAKER` is 1 because the codec and the PA-enable line exist and the
vendor's own example plays PCM through them; whether a transducer is fitted
varies by revision, so the runtime probes rather than assumes.

## Power

A 410 × 502 panel plus a radio is not a two-week watch. Target is **a full
school day** (~7 a.m.–7 p.m.) on a ~500 mAh cell, charged nightly on a dock —
"charge it like a toothbrush." The panel is off most of the time
(wake-on-raise via the IMU), and radio duty is the real budget: ESP-NOW listen
windows stretch once every string has been slack for a while.

## Before anyone calls this a kids' device

A bare Li-po strapped to a child's wrist is the largest physical risk in the
whole design and no firmware decision touches it. Before the word "kids"
appears on a store listing, this board needs: CPSIA third-party testing and
ASTM F963 (plus small parts, 16 CFR 1500.18); a child-resistant battery
enclosure in the spirit of Reese's Law; a **breakaway strap**; and FCC/CE for
an intentional radiator. Until then it ships as a maker kit for the builder's
own household. Full argument in
[the design doc §3.3](../../../docs/design/canary_tincan_kids_watch.md).
