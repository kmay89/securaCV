# 4.3B peripheral activation — bench checklist

The executable companion to [`board_capability_map_43b.md`](./board_capability_map_43b.md).
The map says **what** each latent capability is and **why** it matters; this doc
says **how to validate and activate it** on a real Waveshare ESP32-S3-Touch-LCD-4.3B.

Everything here is **compile/CI-verified but NOT yet bench-validated** — the
firmware carries `VERIFY` notes in
[`pins.h`](../../firmware/boards/waveshare-esp32s3-lcd43b/pins/pins.h) precisely
because the polarity/timing hasn't been proven on hardware. Each section ends
with the exact edit that **retires** that VERIFY note — including the honesty
correction to make if the silicon turns out to differ from what the pin map
assumes. Do these on a bench you can safely energize; several drive real
outputs.

> **Provenance reminder (do not lose this):** the dash has **no signing
> identity** — it verifies others' chains, it never mints its own. Anything it
> reports locally (a DI contact) is an **unsigned** local event. Nothing in this
> checklist changes that, and nothing should be relabeled as a "signed" or
> "witnessed" event. See the map's §2.

## 0. Prerequisites

- A 4.3B board, a **5–36 V DC** bench supply for the terminal block's isolated
  side, and USB-C for flashing/serial.
- Toolchain: `pio` (PlatformIO). Envs live in
  [`firmware/envs/platformio/canary-display.ini`](../../firmware/envs/platformio/canary-display.ini);
  the flavor set is in [`firmware/flavors.json`](../../firmware/flavors.json).
- A serial monitor at the console baud. The features below announce themselves
  with tagged lines (`[FIELD]`, `[RS485]`, `[CAN]`) — those lines are the
  pass signals.
- The safest way to exercise the isolated I/O and I²C sensors interactively is
  the **dev playground** (`-DFEATURE_PLAYGROUND=1`, or Settings → "dev mode" on
  a `dash-b` unit) — see [`dev_playground_43b.md`](./dev_playground_43b.md). Use
  it to sanity-check wiring before trusting the production runtime paths.

---

## 1. Field I/O — isolated DI/DO (ships in the runtime on the 4.3B)

Already active on the 4.3B (gated on `HAS_ISOLATED_IO`); this validates polarity.

**Build/flash:** `pio run -e canary-display-dash-b -t upload` (the 4.3B image;
boots the fleet face — `field_io_loop` runs every pass).

**Wire (supply OFF first):**
- External 5–24 V DC supply **+** → `DI COM`.
- A dry contact (door reed / PIR relay / tamper switch) between supply **−** and
  `DI0` (a monitored opening) and/or `DI1` (a tamper). `DI0` = `ISO_IN_BIT_DI0`
  (EXIO0), `DI1` = `ISO_IN_BIT_DI1` (EXIO5), read via `CH422G_ADDR_IN` (0x26).
- A load (siren/strobe/LED, add a flyback diode across a coil) between its own
  supply **+** and `DO0`; load **−** → the isolated `GND`. `DO0` = `ISO_OUT_BIT_DO0`
  (OD0), driven via `CH422G_ADDR_OC` (0x23).

**Pass:**
- Close/open the DI contact → serial shows `[FIELD] door_contact` (DI0) or
  `[FIELD] tamper_contact` (DI1), and the event appears on-glass. `tamper_contact`
  classifies as `Sev::Tamper`.
- With an unacked alert standing (a `tamper_contact`, or a real Tier-1 from a
  Canary on the broker) → `[FIELD] siren on (unacked alert)` and `DO0` conducts.
  Long-press to ack, or clear the alert → `[FIELD] siren off`. The siren
  self-releases after 5 min even if unacked (bounded controller).
- The input poll briefly flips the CH422G to input mode ~20×/s — **watch the
  backlight for flicker** through a few minutes of polling.

**Fail → fix (retire the VERIFY):**
- DI reads inverted (contact open logs active, closed logs clear): the input is
  wired the wrong sense, or the active-LOW assumption is wrong for your board
  revision. Flip the `a0`/`a1` sense in `src/io/field_io.cpp` and update the
  active-LOW note in `pins.h` §Isolated inputs.
- DO drives the wrong way (siren on when it should be off): flip the `sink`
  polarity in `expander_od_set` usage / the `pins.h` DO polarity VERIFY note.
- Backlight flickers at the poll rate: raise `POLL_MS` in `field_io.cpp`, or gate
  the direction-flip differently; record the safe rate in `pins.h`.

---

## 2. RS485 / Modbus RTU

**Build/flash:** `pio run -e canary-display-dash-rs485 -t upload` (sets
`-DFEATURE_RS485=1`). **Note:** RS485 shares GPIO43/44 with the CH343 USB-UART
console — keep logging on the native USB CDC; expect UART programming to fight a
live bus.

**Wire:** terminal `A`/`B` (RS485 A/B) to a Modbus RTU **slave** — a real meter,
or a USB-RS485 dongle running a slave sim answering **slave 1, holding register
0** at **9600 8N1** (`RS485_BAUD_DEFAULT`). Match A↔A / B↔B.

**Pass:** at boot, `[RS485] up on A/B (TX=44 RX=43) @ 9600 8N1`; then every ~5 s,
`[RS485] probe slave 1 reg 0 = <n> (0x....)`. A quiet bus logs
`[RS485] probe slave 1: no reply (nothing on the bus?)`.

**Fail → fix:** no reply with a known-good slave → **swap A/B** (or the
TX/RX orientation) and re-test; the `pins.h` RS485 orientation note is
`VERIFY`-tagged for exactly this. Confirm the far end is 9600 8N1 and shares a
ground reference. Once a register reads back correctly, retire the RS485
orientation VERIFY note and (optionally) raise the default baud.

---

## 3. CAN / TWAI

**Build/flash:** `pio run -e canary-display-dash-can -t upload` (sets
`-DFEATURE_CAN=1`, pulls `<driver/twai.h>`).

**Wire:** terminal `H`/`L` (CANH/CANL) onto a CAN bus at **500 kbit/s**
(`CAN_BITRATE_DEFAULT`) — another node or a USB-CAN adapter. The bus needs
**120 Ω termination at both ends**; the board's on-board terminator jumper is
**OFF by default** — enable it only if this board is a bus end.

**Pass:** at boot, `[CAN] up on H/L (TX=15 RX=16) @ 500000 bit/s`; received
frames log as `[CAN] id=0x... ext=0 rtr=0 dlc=N data=...`. Force enough TX error
(e.g. transmit with no other node/termination) and confirm self-heal:
`[CAN] bus-off - initiating recovery` → `[CAN] recovered - restarted`.

**Fail → fix:** no frames / immediate bus-off → check **termination** (both ends,
and/or the jumper), confirm the far end is 500 kbit/s, and verify H/L aren't
swapped. Retire the CAN timing/terminator VERIFY notes in `pins.h` once frames
flow cleanly; if your bus runs another standard rate, change `CAN_BITRATE_DEFAULT`.

---

## 4. Evidence vault (flash persistence)

Built and complete (`src/fleet/journal_store.cpp`), **compile-verified in CI**
by the `canary-display-dash-vault` env, held off in the default build by
`FEATURE_TIME_MACHINE_PERSIST 0` in
[`configs/canary-display/dash/config.h`](../../firmware/configs/canary-display/dash/config.h).

**Already done in-repo (no bench action needed):**
- **Deps:** none to add — `LittleFS` ships with the arduino-esp32 framework and
  `ArduinoJson` is already a dash `lib_dep`. The vault env carries no extra deps.
- **Partition:** the stock `default_16MB.csv` the dash builds against already has
  a ~3 MB `spiffs` data partition (what `LittleFS` mounts). If it were ever
  absent, `LittleFS.begin(formatOnFail=true)` fails **safe** (RAM-only).
- **Emulator:** resolved. `journal_store.cpp` is gated `!defined(__EMSCRIPTEN__)`,
  so the wasm build always selects the no-op stubs and the `canary-local` dist
  gate stays green **with no rebuild**, even after the flag is flipped.

**Activate (one code change + one bench pass):**
1. Flip `FEATURE_TIME_MACHINE_PERSIST 1` in the dash config (byte-neutral to the
   emulator, per above).
2. **Pass:** trigger a few events, power-cycle, and confirm the journal reloads
   (the proof-carrying `chain_raw` survives). Soak-watch flash wear. Then move
   the capability-map row to **Driven**.

---

## 5. Trusted time (RTC) & power resilience (battery) — verify the silicon

Potential **honesty corrections**: `pins.h` declares `HAS_RTC 0` and
`HAS_BATTERY 0`, but Waveshare's spec for the 4.3B lists an onboard RTC + battery
holder and a CS8501 Li-ion charger. The pin map is compile-verified only, so this
needs eyes on the actual board.

**Check:**
- Run an I²C census (the playground's census station, or a bus scan on GPIO8/9).
  An RTC (e.g. PCF85063 at 0x51) will show up at its address.
- Inspect the board for a coin-cell holder + the CS8501 charger IC.

**Then, either way, make the map honest:**
- If present: set `HAS_RTC 1` / `HAS_BATTERY 1`, add a runtime-probing RTC layer
  (use it if it ACKs on I²C, else SNTP — fail-safe) so timestamps stay trustworthy
  offline, and update the map's §6.
- If absent: leave the flags at 0 and update the map's §6 to state it was
  bench-confirmed absent (turn the "❓ verify" into a settled fact).

**Do not invent the battery ADC/sense pin** — confirm it on the board before
writing any monitor.

---

## What each pass retires

| Section | VERIFY / flag it clears |
|---|---|
| 1. Field I/O | `pins.h` isolated DI active-LOW + DO sink polarity; backlight-flicker note |
| 2. RS485 | `pins.h` RS485 TX/RX orientation |
| 3. CAN | `pins.h` CAN timing + 120 Ω terminator notes |
| 4. Evidence vault | `FEATURE_TIME_MACHINE_PERSIST` (0 → 1) |
| 5. RTC/battery | `HAS_RTC` / `HAS_BATTERY` (settle the "❓ verify") |

When a section passes, edit the cited `VERIFY`/flag and note the result in the
[capability map](./board_capability_map_43b.md) so its status column reflects
bench reality, not compile-time hope.
