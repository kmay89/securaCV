# Dev Playground — Waveshare ESP32-S3-Touch-LCD-4.3B bench mode

A **safe, guided peripheral test bench** built into the `canary-display`
firmware: flash the playground build onto the Waveshare
**ESP32-S3-Touch-LCD-4.3B** and the glass becomes a bench instrument —
one card per peripheral "station", live values, on-screen wiring
instructions, and an always-visible **pin tracker** showing every used vs
open line. It exists so anyone can try candidate SecuraCV peripherals
(doorbell button, intrusion sensors, light sensor, capacitive touch
through printed shells, time-of-flight, laser beam-gap…) against our
firmware **without being able to hurt the board or the fleet**.

## Two ways in: the bench build, or "dev mode" from a fleet unit

There are two builds that reach this bench, both dash-flavor + 4.3B only
(`feature_sanity` enforces the board contract):

- **`canary-display-playground`** (`-DFEATURE_PLAYGROUND=1`) — boots straight
  into the bench, *instead of* the fleet face. No WiFi/MQTT/OTA is ever
  initialized. This is the dedicated bench flash.
- **`canary-display-dash-b`** (`-DFEATURE_DEVMODE=1`) — a normal **fleet**
  witness on the 4.3B that *also* carries the bench. It boots the fleet face;
  open **Settings → "dev mode" → enter** and it latches an NVS flag and reboots
  into the exact same network-silent bench. **Long-press the glass for 3 s** to
  clear the latch and reboot back to the fleet. So one binary is both a witness
  and a bench/test device, and the bench UI never coexists with a live network
  stack — entering it is a local, on-glass, confirm-gated reboot, nothing more.

Both share the same `canary::playground` code and the `PG1` serial protocol
below; `FEATURE_DEVMODE` only adds the Settings doorway and the exit.

Vendor documentation (the authoritative board references):

- Wiki: <https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B>
- Docs platform: <https://docs.waveshare.com/ESP32-S3-Touch-LCD-4.3B>
- Product page: <https://www.waveshare.com/esp32-s3-touch-lcd-4.3b.htm>
- In-repo pin truth: [`firmware/boards/waveshare-esp32s3-lcd43b/`](../../firmware/boards/waveshare-esp32s3-lcd43b/)

> ⚠️ **Status: compile-tested.** The 4.3B pin map is transcribed from
> Waveshare's wiki/demo sources and carries VERIFY flags (RS485
> orientation, DO latch polarity, DI read-mode blip, VOUT default).
> Running the playground smoke tests below **is** the bench validation
> that retires those flags.

## Why this is safe

The 4.3B is the one board in the family with **no raw GPIO broken out**.
Every terminal is isolated, buffered, or bused:

| Surface | Protection |
|---------|-----------|
| DI0 / DI1 inputs | Optocoupled, 5–36 V, referenced to DI COM — galvanically separated from the ESP32 |
| DO0 / DO1 outputs | Optocoupled open-drain, ≤450 mA sink, external supply carries the load |
| I2C header | 3.3 V logic bus shared with onboard touch/expander; worst miswire kills a sensor, not the S3 |
| RS485 / CAN | Transceiver-buffered differential pairs |
| Power | 6–36 V wide-input regulator (silk says `6~36V`; product page says 7–36 V — stay ≥7 V) |

Firmware adds its own rails:

- **No network.** The playground build never initializes WiFi, MQTT,
  discovery, provisioning, or OTA — a bench unit cannot join the fleet,
  phone home, or take an update. (`main.cpp` branches before any of it.)
- **Bounded outputs.** DO channels drive only on explicit on-glass taps;
  pulses are 1.5 s and latches self-release after 30 s. Boot state =
  everything released.
- **Compile-time contract.** `FEATURE_PLAYGROUND` refuses to build on any
  board without isolated IO (`feature_sanity.h`), so the mode physically
  cannot land on a board where "safe wiring surface" isn't true.

**Bench rules:** low-voltage DC only (5–24 V recommended, 36 V absolute
max on DI; never mains), wire with supplies off, and check the VOUT
resistor option (5 V default) before powering a 3.3 V-only sensor.

## Building / flashing

**PlatformIO** (canonical):

```bash
cd firmware/projects/canary-display
pio run -e canary-display-playground -t upload
pio device monitor -b 115200          # PG1 serial telemetry
```

**Arduino IDE / CLI** (arduino-esp32 **core 3.x only** — the Waveshare
vendor board entries don't exist in the 2.0.x line):

```bash
cd firmware/projects/canary-display
./setup.sh arduino playground         # stages flavor_local.h + pins
arduino-cli compile --profile playground
```

IDE users: open the staged sketch, pick Tools → Board →
**Waveshare ESP32-S3-Touch-LCD-4.3B** (the board macro auto-selects the
dash flavor and 4.3B pin map; `setup.sh arduino playground` supplies the
`FEATURE_PLAYGROUND` define). Selecting the plain 4.3, or the generic
"ESP32S3 Dev Module" with a playground override, fails the build with an
actionable error — that's the arduino rules working, not breaking.

## The stations

Each station is a card on the glass; tap it for full wiring instructions
and its controls. All DI wiring shares one external DC supply whose `+`
lands on **DI COM**.

| Station | Terminal | Peripheral candidates | What you get |
|---------|----------|----------------------|--------------|
| **Doorbell** | DI0 | Any momentary button (dry contact) or 5–36 V wet output | Press counter, live state, and the **ding link**: each press pulses DO0 |
| **Intrusion** | DI1 | PIR module (relay/OC out), reed + magnet, **laser break-beam receiver** | Trip counter + held-ms (beam-gap timing) |
| **Chime out** | DO0 | Mechanical/electronic chime, LED bar, relay coil (flyback diode!) | PULSE 1.5 s / LATCH 30 s, bounded always |
| **Strobe out** | DO1 | Strobe / siren candidate | Same controls, second channel |
| **Light** | I2C | **VEML7700** (0x10, preferred) or BH1750 **strapped to 0x5C** | Live lux; hot-plug |
| **ToF range** | I2C | **VL53L0X** (0x29) | Live mm + trip threshold (50/100/200/400 mm) with counter — the laser-gap prototype |
| **Cap touch** | I2C | **MPR121** 12-pad (0x5A) | Electrode bitmap + 4 sensitivity presets for the shell-thickness test |
| **I2C census** | bus | anything | Live scan every 3 s with reserved-address warnings |

### Capacitive-touch shell-thickness test (printed coupons)

Goal: pick the enclosure wall thickness over the hidden doorbell/touch
pad. Print flat coupons at 1 / 2 / 3 / 4 mm (same material + infill as
the enclosure — see `docs/hardware/enclosure/` for the fit-coupon
pattern).

1. Stick a foil or PCB pad electrode to MPR121 electrode 0.
2. Tape a coupon flat over the electrode (no air gap — air is the enemy,
   it dominates the thickness effect).
3. Start at **contact** sensitivity; touch through the coupon; step the
   preset up (`2mm shell` → `4mm shell` → `max gain`) until touches
   register reliably.
4. Now hover *near* the pad without touching: if it triggers, you've
   over-sensitized — step back down.
5. Record `(material, infill, thickness) → weakest reliable preset` in
   the bench log. That table is the enclosure design input.

### Laser beam-gap testing, two ways

- **Break-beam pair** (emitter + receiver module with digital output):
  wire the receiver like an intrusion sensor on DI1. `held_ms` on the
  card is your gap timing; counts are crossings.
- **ToF as reflective gap sensor**: point the VL53L0X across the opening
  and set the trip threshold under the far-wall distance. Anything
  entering the gap trips it — no second module to align.

## Comms standard (`PG1`)

Every playground build speaks one line protocol on the USB-CDC console
(115200). Grammar (version-tagged so future modes can extend it):

```
PG1 HELLO board=<board-id> fw=<version>          # once at boot
PG1 <millis> EVT <station> <key>=<value>...      # on every edge/trip/action
PG1 <millis> SNAP <key>=<value>...               # 1 Hz full snapshot
```

Stations: `doorbell intrusion chime strobe light tof captouch`. Snapshot
keys: `di0 di1 do0 do1 lux tof_mm tof_ok pads preset i2c`. Values are
plain `k=v`, no quoting; a line is one record — trivially parseable by a
`grep`/`awk` bench log or a future desktop harness.

**Future-proofing (documented now, implemented when needed):** when a
playground station graduates to a real product feature, its telemetry
moves onto the fleet bus as `securacv/<device>/playground/<station>`
with the same `k=v` payload semantics — the PG1 grammar is the wire
contract either way.

## <a name="pin-tracker"></a>Pin tracker — used vs open, fully loaded

The canonical budget for the board, mirrored live on the playground's
left panel (the glass renders this table with live states — it is
"always monitored" by construction). Legend: ✅ used · 🟢 open (free for
new work) · 🟡 reserved (wired, firmware not using it yet) · ⚠️ shared /
verify · ⛔ not available.

### ESP32-S3 native GPIO (every pin accounted for)

| GPIO | Net on 4.3B | Status | Notes |
|------|-------------|--------|-------|
| 0 | LCD G3 | ✅ | strap pin — panel data by design |
| 1 | LCD R3 | ✅ | |
| 2 | LCD R4 | ✅ | |
| 3 | LCD VSYNC | ✅ | strap pin |
| 4 | Touch INT (GT911) | ✅ | |
| 5 | LCD DE | ✅ | |
| 6 | *not routed* | ⛔ | the plain 4.3's piezo pad doesn't exist here |
| 7 | LCD PCLK | ✅ | |
| 8 | I2C SDA | ⚠️ | shared: GT911 + CH422G + sensor header |
| 9 | I2C SCL | ⚠️ | shared: same bus |
| 10 | LCD B7 | ✅ | |
| 11 | SD MOSI | 🟡 | TF slot, unused in v0.1 (VERIFY) |
| 12 | SD SCK | 🟡 | |
| 13 | SD MISO | 🟡 | |
| 14 | LCD B3 | ✅ | |
| 15 | CAN TX | 🟢 | dedicated transceiver — open for a CAN wave |
| 16 | CAN RX | 🟢 | |
| 17 | LCD B6 | ✅ | |
| 18 | LCD B5 | ✅ | |
| 19 | USB D− | ✅ | native CDC console/flash |
| 20 | USB D+ | ✅ | |
| 21 | LCD G7 | ✅ | |
| 38 | LCD B4 | ✅ | |
| 39 | LCD G2 | ✅ | |
| 40 | LCD R7 | ✅ | |
| 41 | LCD R6 | ✅ | |
| 42 | LCD R5 | ✅ | |
| 43 | UART0 TX **and** RS485 | ⚠️ | console ↔ RS485 mutually exclusive (VERIFY orientation) |
| 44 | UART0 RX **and** RS485 | ⚠️ | |
| 45 | LCD G4 | ✅ | strap pin |
| 46 | LCD HSYNC | ✅ | strap pin |
| 47 | LCD G6 | ✅ | |
| 48 | LCD G5 | ✅ | |

**Free native GPIO: none.** That is the design, not a problem — expansion
happens on the buses (below), never by tacking wires onto the S3.

### CH422G expander lines

| Line | Net | Status | Notes |
|------|-----|--------|-------|
| EXIO0 | DI0 | ✅ | playground: doorbell |
| EXIO1 | Touch reset | ✅ | internal |
| EXIO2 | Backlight/DISP | ✅ | on/off only |
| EXIO3 | LCD reset | ✅ | internal |
| EXIO4 | SD_CS | 🟡 | with the SD SPI trio |
| EXIO5 | DI1 | ✅ | playground: intrusion / beam |
| EXIO6–7 | *unrouted* | ⛔ | |
| OD0 | DO0 | ✅ | playground: chime |
| OD1 | DO1 | ✅ | playground: strobe |
| OD2–3 | *unrouted* | ⛔ | |

### "Fully loaded" — the theoretical max, all at once

Every interface running simultaneously — the target the docs and comms
are standardized against, so nothing added later needs a re-plan:

| Interface | Fully-loaded occupant | Playground coverage today |
|-----------|----------------------|---------------------------|
| Panel + touch | 800x480 UI, 5-pt touch | ✅ the instrument face |
| DI0 + DI1 | doorbell + intrusion/beam | ✅ |
| DO0 + DO1 | chime + strobe | ✅ |
| I2C header | light + ToF + MPR121 concurrently (distinct addrs); **TCA9548A (0x70) fans out to 8 more buses** when candidates collide | ✅ (mux documented, not yet driven) |
| RS485 | Modbus RTU field sensors (wind/env/meters) — console moves to native USB first | 🟢 future wave |
| CAN | multi-device wired bus (fence-guard class) | 🟢 future wave |
| SD | bench log / witness cache | 🟡 future wave |

When a future wave claims a 🟢/🟡 row, update this table and the UI
tracker rows in `playground_ui.cpp` **in the same PR** — the table and
the glass must never disagree.

### I2C address landmines (shared bus!)

The CH422G is command-addressed; its "addresses" are functions, not a
device you can move. Never put an external device at: **0x23** (WR_OC —
BH1750's default!), **0x24** (WR_SET), **0x26** (RD_IO), **0x38** (WR_IO
— AHT20's default!), **0x5D/0x14** (GT911). The census card flags
violators live.

## Bench validation checklist (retires the VERIFY flags)

1. Boot: panel up, tracker renders, `PG1 HELLO` on the console.
2. DI0 button press → count increments, DO0 ding pulse fires, backlight
   does **not** flicker during DI polling (the mode-blip check).
3. DO0/DO1: pulse and latch each; confirm auto-release at 1.5 s / 30 s;
   note actual polarity vs `ISO_OUT_BIT_*` comments.
4. Light/ToF/MPR121: hot-plug each, confirm census attach lines, sane
   values, detach detection.
5. RS485 loopback per the Waveshare demo — record which GPIO is TX.
6. File the results in the board README's status section and promote the
   board tier in `boards.json` (per `firmware/HARDWARE.md`).
