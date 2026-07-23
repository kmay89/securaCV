# Waveshare ESP32-S3-Touch-LCD-4.3C — the mic-bearing Canary Dash

**The "AI voice" sibling of the Canary Dash panels — and the ONE display in
the family that physically carries microphones** (a dual-MIC array behind an
ES7210 ADC, with an ES8311 codec alongside). Commonly sold as the
**4.3C-BOX**: the board in a plastic case, mics listening through the
grille slots on the case top — worth knowing when you place it, and when
you point a smoke alarm's TEST horn at it. The vendor listing also names a
**PCF85063 RTC** (see the pin map's RTC section — same address as the
family's PCF8563, *different register map*, so the stock `FEATURE_RTC`
layer must stay off until a PCF85063 variant lands) and the BOX edition
exposes a **screw-terminal strip** whose functions are still to be read
off the schematic (unverified — declared nothing in the pin map). That makes it a **distinct
privacy surface**, handled the way Sense-Wellbeing is: its own board map,
its own build env (`canary-display-dash-mic`), its own OTA product
(`securacv-canary-display-dash-mic`) that never cross-installs with the
mic-free dashes, and a mic contract specified in
[`docs/hardware/display_mic_variant.md`](../../../docs/hardware/display_mic_variant.md):

- **Off by default.** The mics do nothing until armed from Settings →
  microphone (NVS-persisted, per-household choice).
- **Alarm patterns only.** When listening, the firmware computes envelope
  scalars and matches smoke-alarm T3 / CO-alarm T4 cadences — no samples
  kept, no spectra, no speech path, nothing recorded, nothing streamed.
- **You always know.** An amber ● MIC chip is on the glass exactly while
  the capture driver runs — the same function that starts the driver draws
  the chip, and the host-tested core forbids any listening-without-
  indicator state.
- **Hard mute.** Disarming uninstalls the I2S driver and releases the
  pins — the mute you can verify, not a software flag over a live stream.

## Status: compile-tested · mic front-end STAGED (bench pins needed)

Same honesty rule as every board here. Two VERIFY clusters gate real use:

1. **Panel (ST7701).** This map carries the 4.3's RGB pins/timings per the
   family sibling notes; the C's ST7701 controller may additionally need an
   init sequence, and its LCD_RST rides an extra CH422G bit (EXIO3 assumed
   — VERIFY). If the glass stays dark at bench: confirm the reset bit, then
   the init-sequence follow-up is the fix (Arduino_GFX carries ST7701
   support; wire it behind this board's macro).
2. **Audio front end.** `AUDIO_PIN_I2S_*` are **deliberately -1** — the
   vendor's public material doesn't land the audio GPIOs unambiguously, and
   a guessed microphone pin is the one guess this repo must never ship. The
   mic layer refuses to start while any pin is -1 (glass shows "mic: pins
   unset"; console says `[MIC] pins unset`). The mics are provably
   un-driven until you fill them in.

## Bench bring-up (the session this board is bought for)

1. `pio run -e canary-display-dash-mic -t upload` — boots the fleet face
   (panel VERIFY above; the mic layer runs headless either way, and the
   `MIC1` serial grammar reports its state).
   **If the glass stays dark, the board is not bricked and you are not
   blind:** the firmware keeps running — the USB serial console carries
   the full boot scene + `MIC1`/`DBG1` grammars, and once WiFi is up the
   device serves its own live web mirror (`http://<device-id>.local`) —
   a display with broken glass still mirrors, by design. Dark glass =
   the ST7701 init follow-up, worked with the mirror as your screen.
2. Open **debug mode** (Settings → modes → debug) → the I²C census page.
   The ES8311 (0x18) and ES7210 (0x40) should ACK — that confirms the
   codec silicon and the shared-bus wiring before any audio pin is touched.
   An ACK at **0x51** is the PCF85063 RTC saying hello (silicon confirmed;
   driver support is a separate follow-up — see the pin map's RTC note).
3. Read the I2S GPIOs (MCLK/SCLK/LRCK/SDIN) off the vendor wiki/schematic
   for your board revision and fill `AUDIO_PIN_I2S_*` in `pins/pins.h`.
4. Rebuild + flash. Settings → **microphone** → listening. The amber
   ● MIC chip must appear the same instant; `MIC1` SNAP lines start.
5. Hold a smoke alarm's TEST button near the board: the T3 cadence should
   raise `acoustic_smoke_alarm` on the glass (Alert). Disarm: the chip
   must vanish and `MIC1` must report the driver uninstalled.
6. File results per `firmware/HARDWARE.md` (tier promotion + retire the
   VERIFY notes), and update the mic doc's status ledger.
7. Then the second half: hand the unit to someone who didn't build it and
   run the [usability protocol](../../../docs/hardware/display_usability_protocol.md)
   — task H is this board's "is it listening?" comprehension battery, and
   its two safety-critical probes allow no assisted passes.

Vendor references: <https://www.waveshare.com/esp32-s3-touch-lcd-4.3c.htm>
and its wiki page (the schematic download is the pin truth).

Pin map: [`pins/pins.h`](pins/pins.h) — data only, VERIFY-tagged where the
vendor record is thin. Registered `compile-tested` in
[`../boards.json`](../boards.json).
