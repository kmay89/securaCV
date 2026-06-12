# SecuraCV Canary Sense (XIAO ESP32-C6 + MR60BHA2)

Privacy-preserving **radar-native** witness firmware: 60GHz FMCW mmWave presence
(and optional P1-gated wellbeing vitals) on the Seeed MR60BHA2 kit. No camera,
no microphone, no MAC surface — the raw radar IQ never leaves the radar module's
own DSP; the host MCU only ever sees pre-digested scalar claims over UART.

Design + roadmap: [`docs/canary_sense_mr60bha2_design.md`](../../../docs/canary_sense_mr60bha2_design.md).

> **Status: Phase 0 scaffold.** This is the *hello-witness* skeleton that proves
> the ESP32-C6 toolchain and the board/config/common layering compile in CI
> before hardware arrives. The radar UART frame **decoder is a documented stub**
> (`common/sensors/mmwave_mr60/mr60_uart.cpp`), and the network / witness-chain /
> OTA layers are **not yet wired** (Phase 2). It builds, links, and runs the
> presence/vitals FSMs against their stall deadlines; it does not yet publish.

## Quickstart (PlatformIO)

```
# from this directory:
pio run                            # canary-sense-default (presence-only)
pio run -e canary-sense-wellbeing  # adds vitals (-DCANARY_SENSE_VITALS)
pio run -e canary-sense-debug      # verbose ESP-IDF logging
pio run -t upload                  # build + flash
pio device monitor -b 115200       # USB-CDC console
```

The C6 builds on the pinned **pioarduino** platform fork (arduino-esp32 3.x);
see `../../envs/platformio/canary-sense.ini` for the pin and the rationale. The
first `pio run` downloads that platform (~hundreds of MB) — needs network.

## Layout

```
projects/canary-sense/
  platformio.ini          # extra_configs -> common.ini + canary-sense.ini
  src/main.cpp            # hello-witness skeleton
boards/xiao-esp32c6-mr60/ # pin map (radar UART, BH1750 I2C, WS2812, BOOT)
configs/canary-sense/
  default/   config.h     # presence-only
  wellbeing/ config.h     # presence + vitals
common/sensors/mmwave_mr60/  # board-agnostic parser + FSMs
envs/platformio/canary-sense.ini  # the three build environments
```

## Build flavors

| Env | Config | Vitals | Notes |
|-----|--------|:------:|-------|
| `canary-sense-default`   | `configs/canary-sense/default`   | off | CI build + check env |
| `canary-sense-wellbeing` | `configs/canary-sense/wellbeing` | on  | `-DCANARY_SENSE_VITALS=1` |
| `canary-sense-debug`     | `configs/canary-sense/default`   | off | `CORE_DEBUG_LEVEL=4` |

The vitals switch reaches `common/sensors/mmwave_mr60` only as the
`-DCANARY_SENSE_VITALS` build flag (never a `config.h` include in `common/`),
per the firmware layering rules (`firmware/ARCHITECTURE.md`).

## Phase 0 spike checklist (design doc §7)

The exit criterion for Phase 0 is this env compiling a hello-witness binary in
CI plus bench notes in `docs/hardware/`. Items to validate during the spike:

- [ ] **C6 PlatformIO env builds in CI** (this scaffold; pinned pioarduino fork).
- [ ] **UART frames parse on bench** — implement the real decode in
      `mr60_uart.cpp::try_decode` against `Seeed_Arduino_mmWave`; confirm the
      frame header / length / CRC layout.
- [ ] **Ed25519 + hardware RNG** on RISC-V C6 (our `Crypto` dep) — Phase 2 link.
- [ ] **NVS** read/write on C6.
- [ ] **NimBLE on C6** (if BLE is used downstream).
- [ ] **WS2812** single-pixel drive on GPIO1 (this skeleton uses
      `neopixelWrite`).
- [ ] **BOOT button pin** confirmed (assumed GPIO9 in `pins.h` — verify).
- [ ] **core 2.x/3.x API audit** of every `common/` module to be linked into
      canary-sense (e.g. `mbedtls_sha256` void-vs-int return, LEDC/ADC/WiFi
      event API drift) — add version-gated shims where needed.
- [ ] **OTA / partition layout** for the C6's 4 MB flash; add a `size_guard`
      to `firmware/flavors.json` once the OTA slot size is fixed.
- [ ] **Go/no-go**: C6 native vs the S3 + bare-module fallback wiring.

## License
Apache-2.0 (see repository root).
