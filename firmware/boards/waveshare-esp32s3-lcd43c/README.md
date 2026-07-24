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
2. **Audio front end.** `AUDIO_PIN_I2S_*` are now **filled from the vendor's
   own pin-mapping table** (MCLK GPIO6, SCLK GPIO44, LRCK GPIO16, mic-data-in
   GPIO43 = the ES7210 ADC's serial-data-out; PA enable on CH422G EXIO4) — captured in
   [`board_facts.json`](../../../canary-local/devices/board_facts.json) and
   drift-locked against this map, so they're facts, not guesses. The
   **ES7210 register init is now written** (`es7210_init`), so the bench step
   is to *validate* it, not author it — turn the gain/OSR knobs if
   `MIC1 SNAP rms=0` in a live room. (The mic layer still refuses to start with any pin -1;
   the guard stays, it just isn't tripped anymore.)

## Power, the battery, and the side switch

Vendor-documented power scheme for this SKU: a **CS8501** charge/discharge
management chip charges a single-cell 3.7 V Li-ion pack at ~580 mA and
boosts it to 5 V when discharging, with three status LEDs (**PWR** power,
**CHG** charging, **DONE** charge complete). **The side switch is the
battery connect/disconnect — not a device power switch.** ON connects the
pack (charges over USB; takes over seamlessly when USB is lost), OFF
isolates it (neither charges nor discharges — the storage/shipping
position). USB/DC powers the board in *either* position, and Waveshare
documents CHG blinking + DONE lit as the normal "powered, nothing in the
battery path" pattern — not a fault.

Two honest lines for the firmware side:

- **Charging works today, in hardware.** Connect a pack, switch ON, and
  the CS8501 does everything with zero firmware involvement — including
  riding through a power cut.
- **The firmware is battery-blind.** `HAS_BATTERY 0` stands: no level
  read, no on-battery detection, no low-battery warning, no outage event.
  The demo-UI battery glyph in vendor listing photos is Waveshare's demo
  firmware, not this one. The sense path is the blocker: the series demo
  reads pack voltage on ADC1 ch3 (= GPIO4, which this map carries as
  touch INT) — a conflict only the schematic can arbitrate, so
  `BATTERY_PIN_ADC` ships `-1` (VERIFY) and no monitor gets written until
  the bench reads the real divider off the vendor schematic.

Sixty-second bench check of the above: on USB, flip the switch both ways —
the glass stays lit. Switch ON + charged pack, pull USB — stays lit
(that's the CS8501 boost path). Switch OFF, pull USB — dies. The follow-up
worth wanting is the 4.3B capability map's battery line, on this board:
*"cut the power, the Canary keeps witnessing"* — a schematic-confirmed
sense pin plus an on-battery/outage event would let the dash report the
power cut it survives. Staged, not started.

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
3. The I2S GPIOs are already filled from the vendor pin-mapping table
   (`pins/pins.h`; drift-locked to `board_facts.json`) and the **ES7210
   register init (`es7210_init`) is written**. Arm it and watch `MIC1`: a
   `SNAP rms` climbing off zero in a live room means capture works. If it
   stays at 0, turn the init's gain (regs 0x43/0x44) / OSR (reg 0x07) knobs —
   the sequence is a datasheet bring-up and those two are the usual culprits.
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
