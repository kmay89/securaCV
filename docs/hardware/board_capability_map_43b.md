# Board capability map — Waveshare ESP32-S3-Touch-LCD-4.3B

**What this is:** a single, honest ledger of everything the `canary-display`
dash board (Waveshare **ESP32-S3-Touch-LCD-4.3B**) can physically do, what the
firmware drives today, and — for every capability we are *not* yet using — the
exact gate to flip and bench step to validate it. It exists so we never leave a
hardware capability on the table by forgetting it was there, and never *claim*
one we haven't validated. It complements `dev_playground_43b.md` (the bench
tool) and `esp32s3_power_battery_guide.md` (power).

The honesty rule that governs this board still governs this document: a
capability is **Driven** only if code actually exercises it; **Built · bench-gated**
if the code exists but a `FEATURE_*` flag holds it off pending hardware
validation; **Staged** if pins/flags are declared but no driver exists; and
**Absent** if the silicon isn't populated. Nothing here is aspirational — each
row cites the file that backs the claim.

---

## Status at a glance

| Capability | Silicon | Status | Product value for a witness/Canary dash |
|---|---|---|---|
| 4.3" RGB565 800×480 panel | ✅ | **Driven** | The glass itself |
| GT911 5-pt cap touch | ✅ | **Driven** | Tap = wake, long-press = acknowledge |
| CH422G I²C IO expander | ✅ | **Driven** | Owns panel control + isolated field IO |
| 2× isolated digital **inputs** (DI0/DI1) | ✅ | **Driven (runtime, 4.3B)** | Wire in a real PIR / reed / tamper switch → **unsigned local event** (the dash can't sign) |
| 2× isolated open-drain **outputs** (DO0/DO1, ≤450 mA) | ✅ | **Driven (runtime, 4.3B)** | DO0 sirens on an unacked alert — the dash can *act*, not just watch |
| I²C sensor header (VEML7700 / BH1750 / VL53L0X / MPR121) | ✅ | **Driven (bench only)** | Ambient light, ToF beam-gap, cap-touch coupons |
| WiFi / MQTT / mDNS / OTA / web mirror | ✅ | **Driven** | Fleet ingest + self-heal |
| BLE (passive NimBLE scan) | ✅ | **Driven** | Off-grid "Chirp" fallback |
| **Evidence vault** (proof-carrying journal → flash) | ✅ | **Built · bench-gated** | Signed event log survives a network cut |
| Chime engine (LEDC tone) | ⚠️ pad unpopulated on B | **Built · bench-gated** | Audible alert **via DO**, not onboard piezo |
| **RS485 / Modbus RTU** (A/B terminal) | ✅ | **Built · bench-gated** | Integrate alarm panels, access control, HVAC/energy meters |
| **CAN / TWAI** (H/L terminal) | ✅ | **Built · bench-gated** | Vehicle & industrial witness (gate/barrier, CANopen) |
| **microSD** (TF slot) | ✅ | **Staged (CS blocker)** | Bulk local archive — see the CH422G-CS note |
| **Battery-backed RTC** (trusted time) | ❓ verify | **Built · bench-gated** | Trustworthy timestamps when NTP is blocked — runtime-probing layer, compile-verified (`-rtc` env) |
| **Battery operation** (CS8501 charge/boost) | ❓ verify | **Staged / verify** | "Cut the power, the Canary keeps witnessing" |
| ESP-NOW peer presence | ✅ (radio) | **Built · bench-gated** | Router-independent peer-liveness ingest (rx-only) — compile-verified (`-espnow` env) |
| Camera / microphone | ❌ by design | **Absent (intentional)** | *Not a gap* — "it shows, it doesn't watch" |
| Backlight PWM dimming | ❌ (CH422G on/off) | **Absent (hardware)** | Night = dark theme + backlight off |

Board contract, PSRAM, and every pin fact:
`firmware/boards/waveshare-esp32s3-lcd43b/pins/pins.h`. That header is
**compile-verified, not yet bench-validated** (`pins.h:20-23`) — RGB timings,
CH422G bits, and RS485/DO polarity carry VERIFY tags. Treat every ❓/Staged row
below as *unproven on hardware until a bench pass says otherwise*.

---

## 1. Evidence vault — the highest-value latent capability

**Status:** Built, complete, **now compile-verified in CI**, default
`FEATURE_TIME_MACHINE_PERSIST 0` (`configs/canary-display/dash/config.h:44`,
"bench-gated like CHIME"). The dedicated `canary-display-dash-vault` PlatformIO
env sets the flag so CI builds the full LittleFS + ArduinoJson persistence body
against the real toolchain — same pattern as `-rs485` / `-can` — while the
default dash / playground / emulator builds stay byte-identical. One bench soak
(below) is the last gate before the default flag flips on.

The dash already carries the whole durable-history layer. The event sink
(`src/fleet/journal_instance.cpp`) builds a `JournalRecord` — which holds the
**verbatim signed `chain_raw` payload** (`include/canary/fleet/journal.h`), so an
event stays re-provable years later — coarsens the timestamp to a 10-minute
bucket (metadata minimization), and calls `journal_store_append()`. The LittleFS
backend (`src/fleet/journal_store.cpp`) is a finished JSONL store with byte-budget
compaction (`CD_JOURNAL_MAX_BYTES = 96 KB`). It is off only because (a) the flag
is bench-gated and (b) `LittleFS` isn't in the env's `lib_deps` yet.

**Why it matters:** securaCV's entire thesis is a tamper-evident, hash-chained
log. Today that log lives only in RAM on the dash (`FleetModel` ring) and
evaporates on reboot / power loss. Enabling the vault gives the dash a durable,
power-loss-surviving local archive of the fleet's *already-signed* chain heads —
a black-box recorder for the household. Note the dash **verifies but never signs**
(`src/trust.cpp` does Ed25519 verify + TOFU pinning only), so the vault stores
proof, it never mints it — which keeps the never-overclaim rule intact.

**Activation — most of it is now done in-repo:**
1. ~~Add `LittleFS` to `lib_deps`~~ — **not needed:** `LittleFS` ships with the
   arduino-esp32 framework (built-in include), and `ArduinoJson` is already a
   dash `lib_dep` (`mqtt_mgr.cpp` uses it). The `canary-display-dash-vault` env
   carries no extra deps.
2. ~~Confirm the partition~~ — **confirmed:** the stock `default_16MB.csv` the
   dash already builds against carries a ~3 MB `spiffs` data partition, which is
   what `LittleFS` mounts. If it were ever absent, `LittleFS.begin(formatOnFail=
   true)` fails **safe** → RAM-only (non-fatal, just non-functional).
3. **Flip `FEATURE_TIME_MACHINE_PERSIST 1` for the dash flavor** — this is the
   only remaining code change, and it is now **byte-neutral to the emulator**:
   `journal_store.cpp` is additionally gated `!defined(__EMSCRIPTEN__)`, so the
   wasm build (which compiles `src/fleet/*.cpp`) always sees the no-op stubs and
   the `dist/*.js` byte-drift gate stays green with **no rebuild**.
4. **Bench-validate** (the real gate, needs hardware): trigger events,
   power-cycle, confirm the journal reloads; watch flash wear over a soak. Then
   flip the default flag on and move this row to **Driven**.

**Do not** enable microSD for this — flash is the right medium (a removable card
is pull-and-walk-away tamperable; internal flash isn't).

---

## 2. Isolated DI/DO — promoted into the witness runtime (4.3B)

**Status:** Driven in the production runtime (gated on `HAS_ISOLATED_IO`, so 4.3B
only), not just the dev playground.

**Crucial provenance correction:** an earlier draft of this doc said a DI contact
becomes a "signed event." **It cannot** — the dash has *no signing identity*. It
verifies others' Ed25519 chains and TOFU-pins their keys (`src/trust.cpp`) but
holds no private key and never calls the signer; its own MQTT health payload
says so in plain text ("a display has no witness key"). So a contact read is an
honestly **UNSIGNED local event** (`signed_flag=false`), on the same footing as
the fleet model's `on_chirp`/`on_beacon` observations — never a forged witness.

**Shipped in this build (`src/io/field_io.cpp`, `field_io_logic.h`):**
- **DI0 / DI1** (optocoupled, active-LOW, fail-closed read): a debounced edge →
  `the_fleet().on_event(self_id, "door_contact"/"tamper_contact", signed_flag=false)`.
  `tamper_contact` classifies as `Sev::Tamper` (can sound the siren); `door_contact`
  as `Sev::Notice`. Shows on-glass and journals, with no forged provenance.
- **DO0** (≤450 mA open-drain): a bounded **siren** — driven while the fleet's
  worst severity is an unacked alert, released on ack/all-clear, and hard-capped
  at 5 min so a standing alert can't blare forever (re-arms on clear/ack). The
  debounce + bounded-siren decisions are the host-tested pure core.
- Wired into `main.cpp` setup/loop behind `HAS_ISOLATED_IO`; byte-neutral to the
  emulator (its dash build uses the non-B pins, which don't declare the flag).

**Opt-in siren arming (shipped):** the on-glass **Settings → siren** toggle
(`src/ui/settings_ui.cpp`, `HAS_ISOLATED_IO`-gated) arms/disarms the DO0 siren,
**disarmed by default** and NVS-persisted (`field_io_set_armed` / `field_io_armed`,
own `scv-field` key — no glass-blob migration). Disarmed, an unacked alert still
shows on the glass and lands in the journal; only the physical output stays
silent — which is also the safe posture while the DO sink polarity is unproven.
The pure `SirenController::update` takes `armed` and treats `!armed` as resolved,
host-tested in `test_field_io.cpp`. Byte-neutral to the emulator (no
`HAS_ISOLATED_IO` there).

**Remaining to go live:** bench-validate the optocoupler DI polarity and the DO
sink polarity (both VERIFY-tagged in `pins.h`) on real hardware, and confirm the
input-poll's brief expander direction-flip causes no backlight flicker.

---

## 3. RS485 / Modbus RTU — genuinely net-new, highest integration payoff

**Status:** Built · bench-gated (`FEATURE_RS485 0`). Pins declared in `pins.h`
(TX=GPIO44 / RX=GPIO43, 9600 default, **auto-direction** transceiver — no DE pin).

**Shipped in this build (compile-verified, bench-pending):**
- `include/canary/io/modbus_rtu.h` — a pure, allocation-free Modbus RTU master
  core: CRC-16/MODBUS, request builders (read holding 0x03 / input 0x04, write
  single 0x06), and a response parser with exception decode. No Arduino deps.
- `tests_host/test_modbus_rtu.cpp` — host test anchored on the cataloged
  CRC-16/MODBUS check value `0x4B37`, plus build/parse roundtrips and CRC / addr
  / func / capacity / exception rejection cases. Run by the "Modbus RTU host
  test" step in `firmware.yml`.
- `src/io/rs485.{h,cpp}` — thin `Serial1` transport (auto-direction, bounded
  request→response wait) behind `FEATURE_RS485`; the whole TU is empty without
  the flag, so the default/emulator builds stay byte-identical.
- `canary-display-dash-rs485` PlatformIO env (in `flavors.json` build_envs) so
  CI compiles the gated driver against the real toolchain.

**Why it matters:** RS485/Modbus is the lingua franca of building security and
industrial gear. This turns the dash into a **gateway**: read alarm panels,
access-control controllers, energy/HVAC meters, and re-witness their state into
the signed log. The single biggest expansion of the addressable use-case.

**Constraint:** GPIO43/44 are **shared with the CH343 USB-UART console**, so
RS485 and console logging are mutually exclusive — the `FEATURE_RS485` build
keeps logging on the native USB CDC.

**Remaining to go live:** bench-validate TX/RX orientation + bus timing against a
real Modbus slave (VERIFY-tagged in `pins.h`), then wire a Modbus register-map
into the fleet event pipeline and add a matching playground station (which also
needs an emulator `dist/*.js` rebuild). Until then it ships **off**.

---

## 4. CAN / TWAI — vehicle & industrial witness

**Status:** Built · bench-gated (`FEATURE_CAN 0`). Pins declared in `pins.h`
(TX=GPIO15 / RX=GPIO16, **dedicated transceiver** — unlike the plain 4.3 there's
no USB mux — plus a jumper-selectable on-board 120 Ω terminator, OFF by default;
default bit rate `CAN_BITRATE_DEFAULT` 500 kbit/s).

**Shipped in this build (compile-verified, bench-pending):**
- `include/canary/io/can_frame.h` — a pure CAN 2.0 (ISO 11898-1) frame core: the
  `Frame` struct, id-width validity (11-bit base / 29-bit extended), DLC ≤ 8,
  standard-bitrate validity, SocketCAN-style acceptance filtering
  (`(rx & mask) == (want & mask)`), and a bounded ASCII log formatter. No Arduino.
- `tests_host/test_can_frame.cpp` — host test of every rule above (id ranges,
  DLC bounds, filter mask cases, exact formatter output, buffer guards). Run by
  the "CAN frame host test" step in `firmware.yml`.
- `src/io/can_bus.{h,cpp}` — thin ESP-IDF **TWAI** transport on the H/L terminal
  (`twai_driver_install`/`twai_start`, bounded `twai_transmit`/`twai_receive`,
  frame ⇄ `twai_message_t` conversion, accept-all filter, normal mode) behind
  `FEATURE_CAN`; the whole TU is empty without the flag, so the default/emulator
  builds stay byte-identical.
- `canary-display-dash-can` PlatformIO env (in `flavors.json` build_envs) so CI
  compiles the gated driver — including `<driver/twai.h>` — against the toolchain.

**Why it matters:** a Canary that witnesses a **vehicle gate/barrier controller,
fleet telematics, or CANopen building automation** — a tamper-evident CAN event
log. The transceiver is free and dedicated, so it's pure upside.

**Remaining to go live:** bench-validate TX/RX orientation, bit timing, and the
terminator jumper against a real bus, then wire received frames into the fleet
event pipeline and add a matching playground station (needs an emulator dist
rebuild). Until then it ships **off**.

---

## 5. microSD — real, but blocked on a chip-select problem

**Status:** Staged. `HAS_SD_CARD 1`; pins declared (MOSI=11, SCK=12, MISO=13) but
**`SD_PIN_CS = -1`** — the card's chip-select is on the **CH422G expander (EXIO4)**,
not a native GPIO. The header explicitly notes this is why the evidence vault
chose internal flash instead (`include/canary/fleet/journal_store.h:4-7`).

**The blocker:** the stock Arduino `SD.h` / `SdSpiCard` driver toggles CS as a
GPIO on every SPI transaction; it cannot drive a CS that lives behind an I²C
expander. Options, in order of preference:
1. **SdFat with a software-CS callback** that writes `CH422G_ADDR_OUT (0x38)` /
   `CH422G_BIT_SD_CS (1<<4)` per transaction (reuse the expander-write path in
   `src/hal/display_dash.cpp`). Watch throughput — an I²C write per CS toggle is
   slow; SD is best for bulk/occasional writes, not the hot event path.
2. Keep CS asserted across a burst and frame manually (fragile).
3. **Don't** — the evidence vault (§1) already meets the "durable local log"
   need on flash, which is also more tamper-resistant than a removable card.

**Recommendation:** treat SD as a *bulk-archive* stretch goal, not the evidence
store. If pursued, it needs the WAP-style **background mount worker**
(`canary-wap/.../hardware_state.h`) so a slow/wedged card can't trip the 30 s dash
watchdog (`CD_WATCHDOG_TIMEOUT_SEC`).

---

## 6. Trusted time (RTC) & power resilience (battery) — verify on bench first

Both are **potential honesty corrections**, not just features: our header
declares them absent (`HAS_RTC 0`, `HAS_BATTERY 0`) while Waveshare's published
spec for the 4.3B lists an onboard RTC + battery holder and a **CS8501**
Li-ion charge/boost chip. The `pins.h` header is compile-verified only, so this
needs eyes on the physical board.

- **RTC → trusted time.** Time comes from SNTP today (`FEATURE_SNTP`). For a
  cryptographic witness, NTP-only time is a weakness: block or spoof NTP and every
  signed timestamp is suspect. The **runtime-probing** RTC layer now exists
  (`FEATURE_RTC 0`, compile-verified by `canary-display-dash-rtc`): the pure core
  `include/canary/io/rtc_pcf.h` (BCD + civil↔days epoch math + the voltage-low
  validity gate, host-tested against libc `timegm` in `test_rtc_pcf.cpp`) and the
  gated runtime `src/io/rtc.cpp`, which **probes 0x51 on the shared I²C bus** and
  uses the RTC only if it ACKs with a reliable time — else SNTP, unchanged. On
  boot it seeds the clock from the RTC before the network is up; the loop mirrors
  NTP back once a real wall time arrives. By design it does **not** require
  `HAS_RTC` (both 4.3B headers declare it 0), so it is honest whether or not the
  silicon is populated. Byte-neutral to the emulator (`src/io/rtc.cpp` is empty
  without the flag). The watch's `pins.h:72-114` documents the same PCF8563 at
  `0x51`. **Bench-pending:** confirm the RTC silicon + address on a real 4.3B,
  then flip `FEATURE_RTC` on and update `HAS_RTC`.
- **Battery.** If the CS8501 is populated, the dash can run and log through a
  power cut and record the outage itself — a strong security property. **Do not
  invent the ADC/sense pin**; confirm it on the board before writing any monitor.

**Bench step:** scope the I²C census on a real 4.3B (the playground's census
station already lists every address); an RTC will show up at its address. Inspect
the board for the battery holder + CS8501. Update `HAS_RTC` / `HAS_BATTERY` and
this table to match reality either way.

---

## 7. ESP-NOW peer presence — router-independent resilience

**Status:** Built · bench-gated (`FEATURE_ESPNOW 0`, compile-verified by
`canary-display-dash-espnow`). The **receive side** now exists:
`src/net/espnow_peer.cpp` brings up an ESP-NOW listener on the WiFi channel and
drains peer **fleet-link presence beacons** into the fleet model
(`on_beacon`), reusing the exact host-tested wire contract the BLE chirp path
uses (`canary/net/beacon_parse.h`) via the pure `espnow_peer_logic.h`
(`test_espnow_peer.cpp`). One wire format, one parser — an ESP-NOW frame and a
BLE advert from one canary resolve to one witness, and foreign traffic is
rejected before it reaches the fleet.

**Why it matters:** an attacker who kills the WiFi AP silences an MQTT-only
fleet. ESP-NOW is connectionless and router-independent, so the dash keeps
hearing peer liveness off the raw 2.4 GHz channel when the broker is dark.

**Honesty correction (like §2):** the earlier note said Canaries "cross-sign
each other's chain heads." **The dash cannot** — it has no signing identity
(`src/trust.cpp` verifies + TOFU-pins only, never mints). So this is a
**receive-only observer**: it never transmits and never signs, and a peer's
*signed* chain head still travels over the signed MQTT pipeline. ESP-NOW carries
only the coarse presence summary the beacon already coarsens (an UNSIGNED
observation, same footing as the BLE beacon). Byte-neutral to the emulator
(`src/net/espnow_peer.cpp` is empty without the flag).

**Remaining to go live:** the paired WAP-side ESP-NOW *broadcast* of the beacon
(the sender for this receiver) and WiFi-STA channel coexistence tuning are the
follow-ups; then flip `FEATURE_ESPNOW` on. Cross-signing proper is a
Canary-to-Canary (signing device) property, not a display one.

---

## Not gaps — deliberately closed

- **Camera / microphone:** `HAS_CAMERA 0` / `HAS_MICROPHONE 0` by design — the
  dash *shows, it doesn't watch* ("quiet-room-safe by construction"). Adding
  either would break a core privacy promise. Leave closed.
- **Backlight PWM:** the CH422G backlight line is on/off only
  (`HAS_BACKLIGHT_PWM 0`); night mode is dark-theme + backlight off. Hardware
  limit, not an omission.
- **Onboard piezo:** not routed on the B (`BUZZER_PIN -1`); audible feedback is
  intended through DO0/DO1 driving an external buzzer. The `chime.cpp` engine is
  compiled but inert on this SKU by design.

---

## How anything here actually ships (the verification reality)

This board has **no local toolchain** in CI-authoring environments — firmware is
verified through CI + PR review, not a local `pio run`. Two consequences shape
every activation above:

1. **Feature-gate everything, default 0.** New peripheral code must be byte-neutral
   to the emulator's wasm build, or it trips the `canary-local` `dist/*.js`
   byte-drift gate (which needs emcc to regenerate). The proven pattern: gate the
   code off in the default/emulator build, add a **dedicated build env** that
   enables it so PlatformIO/arduino-cli **compile-verify** it (as
   `canary-display-dash-b` did).
2. **The playground is a drift-locked mirror.** A live demo station in
   `playground-sim.js` / `playground.json` requires the matching firmware station
   *in the default build* (`gen_playground.py` regenerates `playground.json` from
   `pins.h`; `playground.test.js` greps `playground_ui.cpp`). So a capability
   becomes *visible in the demo* only after it's bench-validated and enabled — up
   to then it lands as compile-verified, bench-pending firmware.

**Bottom line:** the board is a full industrial witness gateway. We're driving
the display, touch, isolated IO (now in the runtime on the 4.3B), and the radios.
The value left on the table is: **turn on the evidence vault** (now compile-
verified in CI via `canary-display-dash-vault` — one power-cycle bench soak from
Driven), **bench-validate the field-I/O polarity and the RS485/Modbus + CAN/TWAI
drivers (all built, the buses off, field-I/O 4.3B-only)**, and **verify the
RTC/battery silicon** (the RTC trusted-time layer is now built + compile-verified
via `-rtc`; a bench check of the silicon/address is the last step before it goes
live) — in that order. The recurring theme: the code is largely written; a bench session on
real 4.3B hardware is now the gating step for most of it —
[`board_43b_activation_bench.md`](./board_43b_activation_bench.md) is the
per-capability checklist for that session (wiring, flag, pass signal, and the
`VERIFY` note each pass retires).
