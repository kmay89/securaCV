# Bench Bring-Up — Make a Canary Chirp

> The fastest path from parts to a beeping board. This validates the three
> things the peripheral build plan is about — **audible chirp, status LED, and
> the button** — on a bare XIAO ESP32-S3 Sense, with the *minimum* shopping list
> and no enclosure, battery, or weatherproofing. For the full parts catalogue and
> wiring rationale, see the
> [Canary Peripheral Build Plan & BOM](./canary_peripheral_build_plan.md).
>
> 🖨️ **Have a 3D printer?** Two small prints make this hour easier: the
> [bench bring-up fixture](./enclosure/canary_bench_fixture.scad) holds the
> XIAO and every peripheral in labelled stations while you wire (with a
> sliding magnet for repeatable tamper tests), and the
> [fit coupon](./enclosure/canary_fit_coupon.scad) calibrates your printer
> for the [enclosures](./enclosure/) you'll print afterwards.

---

## The shortcut nobody tells you

Two of the three features need **zero extra parts** — they're already on the board:

| Feature | What you need | Why |
|---------|---------------|-----|
| 🔊 **Chirp** | **One passive buzzer** | The only feature that needs an external part. Driven on GPIO2 (`CHIRP_GPIO`) via `FEATURE_AUDIBLE_CHIRP`. |
| 💡 **LED** | **nothing** | The onboard `LED_BUILTIN` (GPIO21) is the chirp's *visual fallback* and the *identify* blink — test it via identify / visual-only mode (see below). |
| 🔘 **Button** | **nothing** | The onboard **BOOT** button (GPIO0) is already the presence/test gate (`boot_button_held()`). |

So a first test is really just **a board + a buzzer + a way to connect them.**

> The fancy **WS2812 RGB LED** and the **reed/Hall tamper** switch are *not wired
> into the canary-wap firmware yet* (see the Firmware-Support column in
> §5.1 of the build plan) — don't buy them for a first test; they won't do
> anything until firmware support is added.

---

## Order this

**Minimum to hear it chirp (~$40, or ~$25 if you have cables/breadboard/iron):**

| Qty | Item | MPN / spec | ~USD | Notes |
|----:|------|-----------|-----:|-------|
| 1 | Seeed **XIAO ESP32-S3 Sense** | `102010469` (Seeed p-5639) | 15 | Brains; camera, mic, LED, BOOT button on board |
| 1 | **Passive piezo buzzer** (assortment pack is fine) | any passive piezo, 1–3 kHz | 6 | The chirp. *Passive*, not a self-driving "active" buzzer |
| 1 | **microSD**, high-endurance 32 GB | SanDisk High Endurance | 9 | Firmware DEV/FULL storage; FAT32 |
| 1 | **USB-C data cable** | data-capable, not charge-only | 4 | Flash + power (you probably own one) |

**Hookup bits (skip if you have a parts bin):**

| Qty | Item | ~USD |
|----:|------|-----:|
| 1 | Solderless breadboard + jumper wires | 8 |
| 1 | Header pins for the XIAO (+ ~5 min soldering) | 2 |

**Don't buy yet:** WS2812 RGB, reed/Hall tamper, cap-touch pad, LiPo/battery,
enclosure, GORE vent, GPS. They're either not firmware-ready or not needed to
prove the chirp works. Add them once the basics sing — full list in the
[build plan](./canary_peripheral_build_plan.md).

---

## Wire it (60 seconds)

```
   XIAO ESP32-S3 Sense
   ┌───────────────┐
   │            D1  ├───────────► passive buzzer (+)
   │          (GPIO2)│
   │           GND  ├───────────► passive buzzer (−)
   └───────────────┘
```

- Buzzer between **D1 (GPIO2)** and **GND**. A passive piezo draws only a few mA
  and can be driven straight off the GPIO — no transistor needed.
- Nothing to wire for the LED or button — both are on the board.

---

## Flash & smoke-test

1. **Pick a build profile that includes the chirp.** In
   `firmware/projects/canary-wap/arduino/canary_wap/build_config.h`, select
   **`BUILD_PROFILE_DEV`** or **`BUILD_PROFILE_FULL`**.
   `FEATURE_AUDIBLE_CHIRP` is **on for DEV/FULL, off for MINIMAL** — a MINIMAL
   build stays silent by design.
2. **Build & upload** (PlatformIO or Arduino IDE) — see
   [`getting_started_canary.md`](../getting_started_canary.md) and
   [`esp32_s3_setup.md`](../esp32_s3_setup.md) for the toolchain.
3. **Test the chirp — listen on boot.** You should hear **`PATTERN_CONFIRM`** —
   two short 2 kHz beeps ("I'm here"). **Chirp confirmed.**
   > With a buzzer connected the LED does **not** blink during the confirm tones
   > — that's expected, not a failure. The onboard LED is the *no-buzzer
   > fallback* and the *identify* signal (next step), not a companion to every
   > tone.
4. **Test the LED.** Trigger the identify signal, which does a **triple LED-only
   blink** (`PATTERN_IDENTIFY`) regardless of the buzzer:
   ```
   POST http://canary.local/api/audible-chirp/play   {"pattern":"identify"}
   ```
   (or tap **Identify** in the dashboard). Alternatively, flip to visual-only so
   *every* pattern blinks the LED instead of sounding:
   ```
   POST http://canary.local/api/audible-chirp/config  {"visual_only": true}
   ```
   **LED confirmed.**
5. **Test the chirp on demand.** Fire the quick test route (replays confirm):
   ```
   POST http://canary.local/api/audible-chirp/test
   ```
   (If you set an API token, include it.)
6. **Test the button.** Hold **BOOT** to trigger the presence/beacon gate
   (`BOOT_BUTTON_HOLD_MS = 2000` ms, in `canary_wap.ino`) and confirm the
   firmware reacts.

---

## If it doesn't chirp

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| Total silence on boot | Built with **MINIMAL** | Rebuild with DEV or FULL (`FEATURE_AUDIBLE_CHIRP`) |
| Faint click only, no tone | **Active** buzzer fitted, or buzzer off-resonance | Use a **passive** piezo; confirm 1–3 kHz part |
| No beep **and** no LED on identify | Power / flash / cable | Use a **data** USB-C cable; check serial console at boot |
| LED never blinks but buzzer works | Looking for the LED during tones | Use the **identify** pattern or `visual_only:true` (step 4) — tones don't drive the LED |
| Tone but very quiet | Buzzer unbaffled on the bench | Normal — SPL improves with an enclosure vent later |
| `test` route 404 / 401 | Wrong profile or missing token | DEV/FULL enables the route; pass the API token if set |

---

## Next steps

Once the chirp works on the bench, graduate to the optional peripherals and the
indoor/outdoor builds in the
[Canary Peripheral Build Plan & BOM](./canary_peripheral_build_plan.md) — and read
the **battery safety notice (§6.5)** *before* you add any cell.
