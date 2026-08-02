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
