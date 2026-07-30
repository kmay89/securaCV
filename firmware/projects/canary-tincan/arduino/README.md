# Tin Can bench bring-up — the day-one runbook

Flash this to **both** watches. It answers, in order, the five questions that
block everything else, and it ends with the product in miniature: **tap a
rhythm on one wrist, feel it on the other.**

> **This is a bench tool, not the product.** There is no encryption here. Bench
> frames use their own magic (`0x7B7C`) which the real Canary Link parser
> rejects outright, so a bench frame can never be mistaken for product traffic.
> The screen says `BENCH` for the same reason. Encryption arrives with the real
> firmware (`link_session.h`), not with this.

## Before you plug anything in

**Nothing here has been run on hardware.** I could not compile-test it either —
this container has no PlatformIO, no Arduino CLI, and no route to the package
registries. So treat the *driver glue* as a first draft: the panel constructor,
the touch register reads, and the haptic writes are transcribed from Waveshare's
own working examples and the DRV2605L datasheet, but they have never executed.

What *is* verified is the part that would be miserable to debug on a wrist: the
knock codec is the byte-identical host-tested header from the product tree
(CI diffs the copy), so if a rhythm crosses the room wrong, the bug is in the
radio or the motor — not in the codec.

## Arduino IDE setup

- **Board:** ESP32S3 Dev Module
- **USB CDC On Boot:** Enabled ← without this you get no serial output at all
- **Flash Size:** 32MB · **PSRAM:** OPI PSRAM
- **Partition Scheme:** 16M Flash (3MB APP/9.9MB FATFS)
- **Library:** *GFX Library for Arduino* **1.6.0 or newer** (moononournation).
  1.6.0 is where `Arduino_CO5300` lives, and it needs **esp32 core 3.x**.

That is the only third-party library. Touch, IMU, RTC, PMU and the haptic are
driven over raw `Wire` on purpose — every library skipped is a library that
cannot fail to compile at 9am.

## The five stages

### 1 — I²C bus (auto-advances after 6s)

Prints and displays every address on the bus, named. **Read the serial log for
this one**, it is the highest-information moment of the whole day.

It settles the touch-controller question outright: register `0xA0` on the touch
chip returns a device ID, and `0x03` means FT3168. That is the chip's own
answer, not a guess from a store page that claims CST9220.

| what you see | what it means |
|---|---|
| `0x38 0x34 0x51 0x6B` all present | the board is healthy |
| `touch device id 0x03` | FT3168 confirmed — `pins.h` is right |
| `touch device id` anything else | **stop and tell me** — `pins.h` needs updating and stage 3 will not work |
| `0x5A DRV2605L HAPTIC` present | the motor is wired; knocks will be *felt* |
| `haptic: MISSING` | expected if you have not fitted one yet — see below |

### 2 — Panel

A 2px frame at the extreme edge plus red/green/blue blocks. Two questions:

- **Are all four edges visible?** If the left or right edge is clipped or
  missing, the `col_offset1 = 22` in the sketch is wrong for your revision.
  That offset is not guessable — the CO5300's controller RAM is wider than the
  visible glass — so it is the first number to doubt if the image looks shifted.
- **Is the R/G/B order correct?** If it reads B/G/R, the panel wants a
  colour-order flag.

*Hold a finger for 2s to advance from here on.*

### 3 — Touch

A green dot should track your finger. If the dot is mirrored or the axes are
swapped, note which — it is a one-line fix, not a rewire.

### 4 — Peers

Power on the **second** watch. Each shows the other's id, RSSI, and `TAUT` /
`slack`. If they never see each other, both are on the same Wi-Fi channel by
default, so suspect the antenna or `esp_now_init` before the protocol.

### 5 — Knock — the one that matters

Tap a rhythm (up to 8 taps in 2 seconds). On release it goes to the other
watch, which replays it as pulses — same gaps, same order.

**This is the product.** If it lands and feels good, the Tin Can is real. Judge
it honestly:

- Does the far watch's rhythm *feel like* the one you tapped?
- Is a sharp double-tap distinguishable from two slow taps?
- Would a kid in another room notice it through a sleeve?

## If there is no haptic yet

Order a **DRV2605L breakout + an LRA** and wire it to the exposed I²C port
(SDA 15, SCL 14, address 0x5A). Until then stage 5 flashes a white bar instead
of buzzing, so you can still verify the *timing* — just not the feel.

Do not judge the concept without a motor. A knock you can only see is a
different and much worse product, and the entire design assumes that sensation
lands. **Bench an LRA against an ERM before anything else gets built on top of
it** — a mushy knock is a dead product, and that is a decision to make in week
one, not week six.

## When something fails

Please send me the **serial log** rather than a description — it prints the
full I²C scan, the touch device id, the ESP-NOW init result, this watch's id,
and every knock sent and received with its tap count and span. That is usually
enough to name the fault without guessing.

Likely first failures, in the order I would bet on them:

1. **Nothing on serial** → USB CDC On Boot is Disabled.
2. **`gfx->begin() FAILED`** → GFX library older than 1.6.0, or esp32 core 2.x.
3. **Blank screen but serial fine** → AMOLED brightness is a panel command, not
   a backlight pin; the panel may need an explicit brightness write on your
   revision.
4. **Touch reads nothing** → the device id in stage 1 was not `0x03`, so the
   register map differs.
5. **Haptic found but silent** → the DRV2605L real-time-playback path in
   `haptic_pulse()` is the untested part; an LRA also needs the auto-calibration
   the datasheet describes, which this sketch deliberately skips.

## What this is not

No strings, no tie ceremony, no Ring, no encryption, no persistence. Those come
with the real firmware, built on the host-tested cores already on `main`
(`firmware/common/link/`, `canary/tincan/`). This tool exists to make the next
step cheap — and to stop us building a product on top of a knock that turns out
to feel wrong.
