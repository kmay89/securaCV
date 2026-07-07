# Canary Display — Bench Bring-Up Runbook

> Closes the display program's one open gate: **on-device hardware
> validation**. CI proves the two flavors compile and the fleet logic is
> host-tested; this runbook proves the same behavior on real glass, clears
> every `VERIFY` note the pin maps carry, and flips the features that ship
> disabled-until-fitted (`FEATURE_CHIME`). Companion to the sensor-side
> [v1 validation runbook](./v1_bench_validation_runbook.md) — the fleet-interop
> track here assumes at least one Canary WAP already passes *its* Track A.
>
> Status today: firmware is **compile/CI-verified, NOT bench-validated**
> (`main.cpp` DEV-STATUS banner; enclosures at the same v0.1-dev status).
> Each track below ends with the exact edits that retire that status.

Two tracks, one interop finale:

- **Track W — Canary Watch** (Seeed XIAO ESP32-S3 + Round Display for XIAO):
  round GC9A01 glass, CST816S touch, real PWM backlight.
- **Track D — Canary Dash** (Waveshare ESP32-S3-Touch-LCD-4.3B + case):
  800×480 RGB panel, GT911 5-point touch, CH422G expander (on/off backlight).
- **Track F — Fleet interop**: names/rooms, wellbeing, proof-QR, ack-sync,
  and the off-grid chirp demo — across both displays and a live Canary.

A pin map's `VERIFY` note flips to a plain comment (and this doc's box gets
ticked) **only after the matching step below passes on your board revision**.
Capture the artifact named at each track's end.

---

## Hardware (the ordered bench kit)

| Track | Board | Extras |
|---|---|---|
| W | Seeed **XIAO ESP32-S3** seated in the **Round Display for XIAO** (1.28" 240×240) | USB-C **data** cable; optional passive piezo (1–3 kHz) for the chime step |
| D | **Waveshare ESP32-S3-Touch-LCD-4.3B** (800×480, GT911, CH422G) + plastic case | USB-C **data** cable; 5 V/1 A+ supply (RGB panel + backlight draw more than a laptop port likes); optional passive piezo |
| F | both of the above + **≥1 Canary WAP** already passing its Track A | HA + Mosquitto broker on the LAN |

> The 4.3**C** "AI Voice" variant (mic array, different expander wiring) is a
> **sibling, not a drop-in** — if that's what you have, treat every Dash
> `VERIFY` as *unconfirmed* and re-check the CH422G bit map against its wiki
> page before trusting touch-reset/backlight.

Toolchain: [`esp32_s3_setup.md`](../esp32_s3_setup.md). Both flavors build
from `firmware/envs/platformio/canary-display.ini`:

```bash
cd firmware/projects/canary-display
cp secrets/secrets.example.h secrets/secrets.h    # WiFi + optional CD_TZ + broker
pio run -e canary-display-watch -t upload          # Track W
pio run -e canary-display-dash  -t upload          # Track D
pio device monitor -b 115200
```

Set `CD_TZ` in `secrets/secrets.h` (e.g. `#define CD_TZ "EST5EDT,M3.2.0,M11.1.0"`)
before validating quiet hours, or the night floor keys off UTC.

---

## Track W — Canary Watch

**Goal:** the round glass renders the fleet, touch pages/acks, and the
backlight dims to a bedroom-safe floor — clearing the three watch `VERIFY`
lines (`TFT_PIN_BL 43`, `TOUCH_PIN_INT 44`, `BUZZER_PIN 1`).

### W1. Flash & boot

1. Upload `canary-display-watch`; open serial at boot.
2. Watch the boot banner (`main.cpp` `boot_kv`) enumerate the panel/pins.

**Pass:** clean boot; `trust` NVS opens (health log); no watchdog reset over
10 min idle (`CD_WATCHDOG_TIMEOUT_SEC = 30`). **Artifact:** serial boot log.

### W2. Panel — GC9A01 (`TFT_PIN_*`)

The Round Display ties the GC9A01 reset to board reset (`TFT_PIN_RST -1`),
SPI at 40 MHz. Confirm geometry and color order on the disc.

**Pass:** full 240×240 fills to the rim; the halo ring is circular (not
squashed/mirrored); Quiet-Glass colors read true (severity greens/ambers/reds
per `theme.cpp`), no torn frames at the ~10 fps tick (`CD_UI_FRAME_MS 100`).

### W3. Touch — CST816S (`VERIFY: TOUCH_PIN_INT 44`)

CST816S at `0x15` on the shared I2C bus (SDA D4/GPIO5, SCL D5/GPIO6). Some
revisions nap until first touch — the HAL already logs "CST816S quiet at
boot" and arms anyway.

1. **Tap** cycles glance pages (halo → per-device → events → proof).
2. **Long-press** (`CD_LONGPRESS_MS 900`) shows the ack ring and acknowledges.

**Pass:** taps register on the first touch after a cold boot (validates the
INT line on **GPIO44/D7**); pages advance in order; long-press quiets an
active alert and leaves a residual chip. **→ clears the `TOUCH_PIN_INT` VERIFY.**

### W4. Backlight PWM (`VERIFY: TFT_PIN_BL 43`)

LEDC ch0, 5 kHz, 8-bit on **GPIO43/D6**. Force each rung and eyeball it (or
meter the pin): Day `255` → Ambient `100` → Night `10`.

1. Tap to wake → full brightness for `CD_TOUCH_WAKE_MS` (15 s), then re-dim.
2. Enter quiet hours (set clock into 22:00–07:00 or shift `CD_QUIET_*`) → the
   near-dark red-shifted floor.

**Pass:** three visibly distinct levels; smooth PWM (no flicker/buzz); the
night floor is bedroom-safe (≤10 lux at pillow distance is the design target).
**→ clears the `TFT_PIN_BL` VERIFY.** **Artifact:** photo of the night floor.

### W5. Chime — optional piezo (`VERIFY: BUZZER_PIN 1`)

Only if you fit a passive piezo to **GPIO1/D0 ↔ GND** (D0 is free on the
Round Display stack). Set `FEATURE_CHIME 1` in `firmware/configs/canary-display/watch/config.h`
and re-flash (LEDC ch1 tone engine, `chime.cpp`).

**Pass:** Tier-1 (alert/tamper) = fast alternating 2.6/3.1 kHz, re-voices every
`CD_CHIME_REVOICE_MS` (30 s) until ack; Tier-2 (warn) = 3 slow 1.8 kHz pulses;
all-clear = falling two-tone — audibly distinct at 5 m. **→ clears `BUZZER_PIN`
VERIFY;** leave `FEATURE_CHIME 1` only if the piezo stays populated.

---

## Track D — Canary Dash

**Goal:** the 800×480 panel renders, GT911 touch hit-tests the cards, the
CH422G backlight toggles for night — clearing the Dash `VERIFY` lines
(RGB **panel timings**, **CH422G bit map**, `TOUCH_PIN_INT 4`, `BUZZER_PIN 6`).

### D1. Flash & boot

Upload `canary-display-dash`; power from a **5 V supply**, not a bare laptop
port (RGB + backlight inrush). Serial at boot.

**Pass:** clean boot; GT911 product-id read succeeds (the HAL retries once,
tries `0x5D` then `0x14`); no brownout resets. **Artifact:** serial boot log
incl. the GT911 id line.

### D2. Panel — RGB565 parallel (`VERIFY: panel timings`)

`display_dash.cpp` drives the 16-bit RGB bus from `LCD_PIN_*`; timings
(`LCD_PCLK_HZ 16 MHz`, HSYNC 4/8/8, VSYNC 4/16/16) are Waveshare demo
starting values.

**Pass:** stable full-screen image, no horizontal tearing, no color-channel
swap (R/G/B bit lanes correct), no shimmer/rolling. If it tears or wraps,
adjust porches/PCLK per your revision's Waveshare demo, then re-confirm.
**→ clears the panel-timing VERIFY** (write the final values into `pins.h`).

### D3. Touch — GT911 (`VERIFY: TOUCH_PIN_INT 4` + touch-reset via expander)

GT911 reset is driven through **CH422G EXIO1**, not a native GPIO
(`TOUCH_PIN_RST -1`); the HAL pulses INT (GPIO4) + EXIO1 during init to select
the I2C address.

1. Tap a witness card → proof sheet opens (`dash_ui_handle_tap` hit-test).
2. Tap outside → dismiss. Long-press → ack.

**Pass:** taps land on the **correct** card across the full 800×480 (validates
both INT on GPIO4 and the EXIO1 reset sequence); proof sheet shows the right
device. **→ clears `TOUCH_PIN_INT` + the EXIO1/touch-reset VERIFY.**

### D4. Backlight + expander (`VERIFY: CH422G bit map`)

CH422G is **write-only**, addresses `0x24` (sys) / `0x38` (out); backlight is
**EXIO2, on/off only** (no PWM). Night mode = dark theme + backlight OFF.

1. Daytime → backlight ON, dark-on-light Quiet Glass.
2. Quiet hours → dark theme **and** backlight OFF (`CD_BRIGHT_NIGHT 0`).
3. Tap in quiet hours → backlight ON for `CD_TOUCH_WAKE_MS` (20 s), then OFF.

**Pass:** the EXIO2 bit toggles the backlight both ways; no other EXIO line
(SD_CS/USB-sel) is disturbed when toggling it. **→ clears the CH422G-bit VERIFY.**
**Artifact:** photo of the quiet-hours dark state.

### D5. Chime — optional piezo (`VERIFY: BUZZER_PIN 6`)

If fitted to **GPIO6 ↔ GND** (free of the RGB/touch/expander nets), set
`FEATURE_CHIME 1` in the **dash** config and re-flash. Same pass criteria as
W5. **→ clears `BUZZER_PIN` VERIFY.**

---

## Track F — Fleet interop (the magic)

**Goal:** prove the wave-1/wave-2 features end-to-end on glass, with a real
Canary publishing. Prerequisite: W1–W4 and D1–D4 pass, ≥1 WAP on the broker.

### F1. Discovery & render

Point both displays at the broker (or let mDNS `_securacv._tcp` gossip find
it — pull the configured host and confirm rebind within `CD_BROKER_REDISCOVER_MS`).

**Pass:** the WAP appears as a card/halo within a publish cycle; link state
tracks Online→Stale (3 min)→Lost (10 min) if you mute the Canary; a dead
broker link is **bannered** ("showing last known state"), never blank-and-calm.

### F2. Verified badge & proof-on-glass

**Pass:** the badge reads **✓ Verified** only after on-device Ed25519 against
the TOFU pin (doctor a payload with the wrong key → **✕ Failed**, never a
silent ✓). Tap the card → QR; scan with a plain phone camera + the 20-line
python verifier → it checks against the health-topic pubkey, no app/cloud.

### F3. Names, rooms, wellbeing (wave 2, §8/§9)

Publish retained meta and (if a wellbeing build) a state field:

```bash
mosquitto_pub -r -t securacv/<device_id>/meta \
  -m '{"name":"Kitchen","room":"kitchen"}'
```

**Pass:** both displays switch from the device id to **Kitchen · kitchen**
on every surface (card, hero, event rows, proof title); clearing the retained
meta reverts to the id. A witness publishing `breathing_locked` shows
**breathing ✓/—** on its detail line; a non-bool value is ignored (no false
health signal); a witness that never publishes it shows no wellbeing line.

### F4. Household ack-sync (§2) + heartbeat (§4)

**Pass:** long-press-ack on the **Dash** quiets the **Watch** within ~1 s
(`securacv/fleet/ack`), and vice-versa; a fresh alert re-arms both. When every
witness is Ok/Verified and it's daytime, the once-a-minute heartbeat swell
fires; it is **absent** the moment anything is stale/unverified or at night.

### F5. Off-grid chirp — the demo (§6)

With a chirping Canary in BLE range and both displays idle:

1. **Unplug the router / kill the broker.** Displays banner the dead link.
2. Trigger a tamper on the Canary.

**Pass:** the tamper reaches the bedside Watch **in <10 s over BLE**, labelled
**"tamper (chirp)"** (coarser trust — never "verified"); a known Canary files
under its real card by fingerprint **suffix** (not a `SCV-XXXX` twin); restore
the broker → the scan stops and MQTT resumes as the richer channel.
**Artifact:** video of the router unplugged and the puck still alarming.

---

## Sign-off — retire the DEV status

When a track passes on your board revision:

1. **Pin maps** — turn each cleared `VERIFY` note into a plain "confirmed on
   rev X" comment in
   `firmware/boards/xiao-esp32s3-round/pins/pins.h` /
   `firmware/boards/waveshare-esp32s3-lcd43/pins/pins.h`
   (backlight/touch-int/buzzer lines; Dash panel timings + CH422G bits).
2. **Chime** — leave `FEATURE_CHIME 1` in a flavor's `config.h` **only** if the
   piezo is populated on that build; otherwise keep it 0 (honest: no silent
   dead pad).
3. **DEV banner** — once both flavors clear W1–W4 / D1–D4 and Track F, drop the
   `⚠️ DEV STATUS` block in `firmware/projects/canary-display/src/main.cpp` and
   the matching note atop each `README.md` / pins header.
4. **Docs** — flip the display's row in the hardware status table and note the
   validated board revision + date. Attach the artifacts named above.

Exit criteria: both flavors pass their track, Track F passes end-to-end
(including the router-unplugged chirp demo), and every `VERIFY` note is either
cleared or annotated with the value that worked on your revision.
