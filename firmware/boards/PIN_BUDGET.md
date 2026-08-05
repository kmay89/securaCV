# Pin budget — the board fullness gauge

<!-- GENERATED FILE — do not edit by hand. -->
<!-- Regenerate: python3 firmware/scripts/pin_budget.py --write -->
<!-- CI guard:   python3 firmware/scripts/pin_budget.py --check -->

One standardized question, answered per board: **how full is this
firmware's pin map, and what room is left — for what?** Derived
mechanically from each board's data-only `pins/pins.h` (the
registry guard enforces "pins are data") scored against the MCU's
GPIO/peripheral tables. Buckets, in claim order:

- **committed** — copper spent on onboard hardware (or an explicit
  pin-map commitment). Counts toward the gauge percentage.
- **assigned** — header-convention assignments (I2C/SPI/UART
  suggestions) that are reclaimable when nothing onboard rides them.
- **conditional** — usable only at a cost: USB-Serial/JTAG data
  lines, the UART0 console pair, strapping pins (⚠ = check boot
  level before repurposing).
- **free** — genuinely available, annotated with ADC and
  deep-sleep-wake capability.

Each board also carries a **Thermals** line (from `thermal_notes`
in `boards.json`): where the heat comes from and what to derate
before adding load to the free pins. The *runtime* thermal gauge
is the die-temperature watchdog every build ships
(`FEATURE_DIAGNOSTICS` — see `firmware/build_matrix.json`).

The gauge reads the *declared pin map*, not runtime code: a TF slot
with no SD driver still counts committed (the copper is gone), and
off-map wiring documented only in a board README (e.g. a radar on
the UART0 header pins) shows up as conditional. The per-board
README stays the narrative source for those stories.

## Fleet summary

| board | MCU | usable GPIOs | committed | assigned | conditional | free |
|---|---|---|---|---|---|---|
| `esp32-c3` | ESP32-C3 | 15 | 2 (13%) | 6 | 5 | 2 |
| `esp32-wroom-devkit` | ESP32 | 26 | 8 (31%) | 7 | 5 | 6 |
| `esp32c3-super-mini` | ESP32-C3 | 15 | 2 (13%) | 6 | 5 | 2 |
| `esp32cam-ai-thinker` | ESP32 | 24 | 21 (88%) | 0 | 3 | 0 |
| `freenove-esp32s3-cam` | ESP32-S3 | 33 | 21 (64%) | 4 | 5 | 3 |
| `waveshare-esp32c3-lcd147` | ESP32-C3 | 15 | 6 (40%) | 2 | 4 | 3 |
| `waveshare-esp32c6-lcd147` | ESP32-C6 | 24 | 10 (42%) | 2 | 4 | 8 |
| `waveshare-esp32c6-lcd169` | ESP32-C6 | 24 | 7 (29%) | 0 | 7 | 10 |
| `waveshare-esp32s3-amoled206` | ESP32-S3 | 33 | 21 (64%) | 0 | 5 | 7 |
| `waveshare-esp32s3-lcd147` | ESP32-S3 | 38 | 11 (29%) | 1 | 6 | 20 |
| `waveshare-esp32s3-lcd43` | ESP32-S3 | 33 | 27 (82%) | 0 | 4 | 2 |
| `waveshare-esp32s3-lcd43b` | ESP32-S3 | 33 | 30 (91%) | 0 | 2 | 1 |
| `waveshare-esp32s3-lcd43c` | ESP32-S3 | 33 | 28 (85%) | 0 | 2 | 3 |
| `waveshare-esp32s3-lcd7` | ESP32-S3 | 33 | 23 (70%) | 0 | 4 | 6 |
| `waveshare-esp32s3-touch-lcd169` | ESP32-S3 | 33 | 17 (52%) | 0 | 7 | 9 |
| `xiao-esp32c3` | ESP32-C3 | 15 | 1 (7%) | 4 | 6 | 4 |
| `xiao-esp32c3-sentinel-lite` | ESP32-C3 | 15 | 2 (13%) | 0 | 7 | 6 |
| `xiao-esp32c6-mr60` | ESP32-C6 | 24 | 2 (8%) | 2 | 8 | 12 |
| `xiao-esp32c6-sentinel` | ESP32-C6 | 24 | 2 (8%) | 0 | 9 | 13 |
| `xiao-esp32s3` | ESP32-S3 | 33 | 3 (9%) | 5 | 7 | 18 |
| `xiao-esp32s3-round` | ESP32-S3 | 33 | 13 (39%) | 0 | 4 | 16 |
| `xiao-esp32s3-sense` | ESP32-S3 | 33 | 24 (73%) | 3 | 5 | 1 |

## Per-board budgets

### `esp32-c3` — ESP32-C3 DevKitM-1 (generic)

ESP32-C3 · flash 4 MB · PSRAM 0 MB · pin map [`pins/pins.h`](esp32-c3/pins/pins.h)

**2/15 committed** · 6 assigned · 5 conditional · **2 free** (2 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | **free** | — | ADC, sleep-wake |
| 1 | **free** | — | ADC, sleep-wake |
| 2 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing; (declared as BUZZER_PIN_DEFAULT) | ADC, sleep-wake, strap⚠ |
| 3 | assigned | EXT_LED_PIN_DEFAULT | ADC, sleep-wake |
| 4 | assigned | I2C_PIN_SDA | ADC, sleep-wake |
| 5 | assigned | I2C_PIN_SCL, SPI_PIN_MOSI | ADC, sleep-wake |
| 6 | assigned | SPI_PIN_SCK, UART1_PIN_TX |  |
| 7 | assigned | SPI_PIN_MISO, UART1_PIN_RX |  |
| 8 | committed | LED_BUILTIN | strap⚠ |
| 9 | committed | BOOT_BUTTON_PIN | strap⚠ |
| 10 | assigned | SPI_PIN_CS |  |
| 18 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DN) |  |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DP) |  |
| 20 | conditional | UART0 console — free only if you give up the serial log; (declared as UART0_PIN_RX) |  |
| 21 | conditional | UART0 console — free only if you give up the serial log; (declared as UART0_PIN_TX) |  |

Physically broken out (from `PIN_D*/PIN_A*/PIN_GPIO*` aliases): [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 18, 19, 20, 21] — of the free pins, **[0, 1]** reach a header.

Peripheral demand (declared pin map vs MCU): SPI 1/1 · I2C 1/1 · UART 1/2 · RMT TX 0/2 · LEDC 1/6.

Capabilities on: `HAS_BLE`, `HAS_USB_CDC`, `HAS_VISION_AI`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_CAMERA`, `HAS_GNSS_UART`, `HAS_MICROPHONE`, `HAS_PSRAM`, `HAS_SD_CARD`, `HAS_TAMPER_INPUT`.

**Thermals:** Open-air devkit at Grove-cable duty; negligible.

### `esp32-wroom-devkit` — ESP32-WROOM-32 DevKit (generic)

ESP32 · flash 4 MB · PSRAM 0 MB · pin map [`pins/pins.h`](esp32-wroom-devkit/pins/pins.h)

**8/26 committed** · 7 assigned · 5 conditional · **6 free** (6 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | committed | BOOT_BUTTON_PIN | ADC, sleep-wake, strap⚠ |
| 1 | conditional | UART0 console — free only if you give up the serial log; (declared as UART0_PIN_TX) |  |
| 2 | committed | LED_BUILTIN | ADC, sleep-wake, strap⚠ |
| 3 | conditional | UART0 console — free only if you give up the serial log; (declared as UART0_PIN_RX) |  |
| 4 | **free** | — | ADC, sleep-wake |
| 5 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing; (declared as SPI_PIN_CS) | strap⚠ |
| 12 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 13 | **free** | — | ADC, sleep-wake |
| 14 | **free** | — | ADC, sleep-wake |
| 15 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 16 | committed | GNSS_PIN_RX, UART2_PIN_RX |  |
| 17 | committed | GNSS_PIN_TX, UART2_PIN_TX |  |
| 18 | assigned | SPI_PIN_SCK |  |
| 19 | assigned | SPI_PIN_MISO |  |
| 21 | assigned | I2C_PIN_SDA |  |
| 22 | assigned | I2C_PIN_SCL |  |
| 23 | assigned | SPI_PIN_MOSI |  |
| 25 | **free** | — | ADC, sleep-wake |
| 26 | assigned | TAMPER_PIN_DEFAULT | ADC, sleep-wake |
| 27 | assigned | EXT_LED_PIN_DEFAULT | ADC, sleep-wake |
| 32 | **free** | — | ADC, sleep-wake |
| 33 | **free** | — | ADC, sleep-wake |
| 34 | committed | PIN_INPUT_ONLY_0 | ADC, sleep-wake |
| 35 | committed | PIN_INPUT_ONLY_1 | ADC, sleep-wake |
| 36 | committed | PIN_INPUT_ONLY_2 | ADC, sleep-wake |
| 39 | committed | PIN_INPUT_ONLY_3 | ADC, sleep-wake |

Peripheral demand (declared pin map vs MCU): SPI 1/2 · I2C 1/2 · UART 1/3 · RMT TX 0/8 · LEDC 0/16.

Capabilities on: `HAS_BLE`, `HAS_GNSS_UART`, `HAS_TAMPER_INPUT`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_CAMERA`, `HAS_MICROPHONE`, `HAS_PSRAM`, `HAS_SD_CARD`, `HAS_USB_CDC`.

**Thermals:** Open-air devkit at CSI-witness duty runs cool; no special measures. No die-temp watchdog on classic ESP32.

### `esp32c3-super-mini` — ESP32-C3 Super Mini

ESP32-C3 · flash 4 MB · PSRAM 0 MB · pin map [`pins/pins.h`](esp32c3-super-mini/pins/pins.h)

**2/15 committed** · 6 assigned · 5 conditional · **2 free** (2 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | **free** | — | ADC, sleep-wake |
| 1 | **free** | — | ADC, sleep-wake |
| 2 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing; (declared as BUZZER_PIN_DEFAULT) | ADC, sleep-wake, strap⚠ |
| 3 | assigned | EXT_LED_PIN_DEFAULT | ADC, sleep-wake |
| 4 | assigned | I2C_PIN_SDA | ADC, sleep-wake |
| 5 | assigned | I2C_PIN_SCL, SPI_PIN_MOSI | ADC, sleep-wake |
| 6 | assigned | SPI_PIN_SCK, UART1_PIN_TX |  |
| 7 | assigned | SPI_PIN_MISO, UART1_PIN_RX |  |
| 8 | committed | LED_BUILTIN | strap⚠ |
| 9 | committed | BOOT_BUTTON_PIN | strap⚠ |
| 10 | assigned | SPI_PIN_CS |  |
| 18 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DN) |  |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DP) |  |
| 20 | conditional | UART0 console — free only if you give up the serial log; (declared as UART0_PIN_RX) |  |
| 21 | conditional | UART0 console — free only if you give up the serial log; (declared as UART0_PIN_TX) |  |

Physically broken out (from `PIN_D*/PIN_A*/PIN_GPIO*` aliases): [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21] — of the free pins, **[0, 1]** reach a header.

Peripheral demand (declared pin map vs MCU): SPI 1/1 · I2C 1/1 · UART 1/2 · RMT TX 0/2 · LEDC 1/6.

Capabilities on: `HAS_BLE`, `HAS_USB_CDC`, `HAS_VISION_AI`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_CAMERA`, `HAS_GNSS_UART`, `HAS_MICROPHONE`, `HAS_PSRAM`, `HAS_SD_CARD`, `HAS_TAMPER_INPUT`.

**Thermals:** C3 at Vision-host duty runs cool; no special measures.

### `esp32cam-ai-thinker` — AI-Thinker ESP32-CAM

ESP32 · flash 4 MB · PSRAM 4 MB · pin map [`pins/pins.h`](esp32cam-ai-thinker/pins/pins.h)

**21/24 committed** · 0 assigned · 3 conditional · **0 free** (0 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | committed | CAM_PIN_XCLK | ADC, sleep-wake, strap⚠ |
| 1 | conditional | UART0 console — free only if you give up the serial log; (declared as UART0_PIN_TX) |  |
| 2 | committed | SD_PIN_MISO | ADC, sleep-wake, strap⚠ |
| 3 | conditional | UART0 console — free only if you give up the serial log; (declared as UART0_PIN_RX) |  |
| 4 | committed | FLASH_LED_PIN | ADC, sleep-wake |
| 5 | committed | CAM_PIN_D0 | strap⚠ |
| 12 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 13 | committed | SD_PIN_CS | ADC, sleep-wake |
| 14 | committed | SD_PIN_SCK | ADC, sleep-wake |
| 15 | committed | SD_PIN_MOSI | ADC, sleep-wake, strap⚠ |
| 18 | committed | CAM_PIN_D1 |  |
| 19 | committed | CAM_PIN_D2 |  |
| 21 | committed | CAM_PIN_D3 |  |
| 22 | committed | CAM_PIN_PCLK |  |
| 23 | committed | CAM_PIN_HREF |  |
| 25 | committed | CAM_PIN_VSYNC | ADC, sleep-wake |
| 26 | committed | CAM_PIN_SIOD | ADC, sleep-wake |
| 27 | committed | CAM_PIN_SIOC | ADC, sleep-wake |
| 32 | committed | CAM_PIN_PWDN | ADC, sleep-wake |
| 33 | committed | EXT_LED_PIN_DEFAULT, LED_BUILTIN | ADC, sleep-wake |
| 34 | committed | CAM_PIN_D6 | ADC, sleep-wake |
| 35 | committed | CAM_PIN_D7 | ADC, sleep-wake |
| 36 | committed | CAM_PIN_D4 | ADC, sleep-wake |
| 39 | committed | CAM_PIN_D5 | ADC, sleep-wake |

Peripheral demand (declared pin map vs MCU): SPI 1/2 · I2C 0/2 · UART 0/3 · RMT TX 0/8 · LEDC 0/16.

Capabilities on: `HAS_BLE`, `HAS_CAMERA`, `HAS_PSRAM`, `HAS_SD_CARD`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_GNSS_UART`, `HAS_MICROPHONE`, `HAS_TAMPER_INPUT`, `HAS_USB_CDC`.

**Thermals:** Camera + Wi-Fi bursts on a thumb-sized PCB with no thermal relief; the notorious brown-out reboots are usually the 5 V supply, not heat. No die-temp watchdog on classic ESP32 — derate PEEK cadence in enclosures by design margin, not by telemetry.

### `freenove-esp32s3-cam` — Freenove ESP32-S3-WROOM CAM (FNK0085)

ESP32-S3 · flash 16 MB · PSRAM 8 MB · pin map [`pins/pins.h`](freenove-esp32s3-cam/pins/pins.h)

**21/33 committed** · 4 assigned · 5 conditional · **3 free** (1 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | committed | BOOT_BUTTON_PIN | sleep-wake, strap⚠ |
| 1 | assigned | I2C_PIN_SDA | ADC, sleep-wake |
| 2 | assigned | I2C_PIN_SCL | ADC, sleep-wake |
| 3 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 4 | committed | CAM_PIN_SIOD | ADC, sleep-wake |
| 5 | committed | CAM_PIN_SIOC | ADC, sleep-wake |
| 6 | committed | CAM_PIN_VSYNC | ADC, sleep-wake |
| 7 | committed | CAM_PIN_HREF | ADC, sleep-wake |
| 8 | committed | CAM_PIN_D2 | ADC, sleep-wake |
| 9 | committed | CAM_PIN_D1 | ADC, sleep-wake |
| 10 | committed | CAM_PIN_D3 | ADC, sleep-wake |
| 11 | committed | CAM_PIN_D0 | ADC, sleep-wake |
| 12 | committed | CAM_PIN_D4 | ADC, sleep-wake |
| 13 | committed | CAM_PIN_PCLK | ADC, sleep-wake |
| 14 | **free** | — | ADC, sleep-wake |
| 15 | committed | CAM_PIN_XCLK | ADC, sleep-wake |
| 16 | committed | CAM_PIN_D7 | ADC, sleep-wake |
| 17 | committed | CAM_PIN_D6 | ADC, sleep-wake |
| 18 | committed | CAM_PIN_D5 | ADC, sleep-wake |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DN) | ADC, sleep-wake |
| 20 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DP) | ADC, sleep-wake |
| 21 | assigned | TAMPER_PIN_DEFAULT | sleep-wake |
| 38 | committed | SDMMC_PIN_CMD |  |
| 39 | committed | SDMMC_PIN_CLK |  |
| 40 | committed | SDMMC_PIN_D0 |  |
| 41 | **free** | — |  |
| 42 | **free** | — |  |
| 43 | committed | GNSS_PIN_TX, UART0_PIN_TX |  |
| 44 | committed | GNSS_PIN_RX, UART0_PIN_RX |  |
| 45 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 46 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 47 | assigned | EXT_LED_PIN_DEFAULT |  |
| 48 | committed | LED_BUILTIN |  |

Peripheral demand (declared pin map vs MCU): SPI 0/2 · I2C 1/2 · UART 0/3 · RMT TX 0/4 · LEDC 0/8.

Capabilities on: `HAS_BLE`, `HAS_CAMERA`, `HAS_GNSS_UART`, `HAS_PSRAM`, `HAS_SD_CARD`, `HAS_TAMPER_INPUT`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_MICROPHONE`.

**Thermals:** Same S3 heat sources as the flagship (camera PEEK + Wi-Fi bursts) on a larger PCB with more copper — runs cooler than the XIAO. Die-temp watchdog covers it.

### `waveshare-esp32c3-lcd147` — Waveshare ESP32-C3-LCD-1.47

ESP32-C3 · flash 4 MB · PSRAM 0 MB · pin map [`pins/pins.h`](waveshare-esp32c3-lcd147/pins/pins.h)

**6/15 committed** · 2 assigned · 4 conditional · **3 free** (2 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | **free** | — | ADC, sleep-wake |
| 1 | **free** | — | ADC, sleep-wake |
| 2 | committed | IMU_INT_PIN | ADC, sleep-wake, strap⚠ |
| 3 | assigned | I2C_PIN_SCL | ADC, sleep-wake |
| 4 | assigned | I2C_PIN_SDA | ADC, sleep-wake |
| 5 | committed | SD_PIN_MOSI, TFT_PIN_MOSI | ADC, sleep-wake |
| 6 | committed | SD_PIN_MISO, TFT_PIN_MISO |  |
| 7 | committed | SD_PIN_CLK, TFT_PIN_SCK |  |
| 8 | committed | TFT_PIN_DC | strap⚠ |
| 9 | committed | BOOT_BUTTON_PIN | strap⚠ |
| 10 | **free** | — |  |
| 18 | conditional | USB-Serial/JTAG — free only if you give up USB |  |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB |  |
| 20 | conditional | UART0 console — free only if you give up the serial log |  |
| 21 | conditional | UART0 console — free only if you give up the serial log |  |

⚠ 5 pin define(s) are `-1` — not wired OR not yet verified (see the comments in pins.h): `BUZZER_PIN`, `SD_PIN_CS`, `TFT_PIN_BL`, `TFT_PIN_CS`, `TFT_PIN_RST`. Free counts above may shrink as these resolve.

Peripheral demand (declared pin map vs MCU): SPI 1/1 · I2C 1/1 · UART 0/2 · RMT TX 0/2 · LEDC 0/6.

Capabilities on: `HAS_BACKLIGHT_PWM`, `HAS_BLE`, `HAS_DISPLAY`, `HAS_IMU`, `HAS_SD_CARD`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_BATTERY`, `HAS_CAMERA`, `HAS_MICROPHONE`, `HAS_NATIVE_USB`, `HAS_PSRAM`, `HAS_RGBLED`, `HAS_RTC`, `HAS_THREAD_ZIGBEE`, `HAS_TOUCH`.

**Thermals:** Enclosed pocket case (canary_c3_lcd147.scad) with a lamp duty cycle: the nightlight flavor hard-caps backlight duty at 50% in the HAL — a heat budget, not a preference. Thermally trivial below that cap.

### `waveshare-esp32c6-lcd147` — Waveshare ESP32-C6-LCD-1.47

ESP32-C6 · flash 4 MB · PSRAM 0 MB · pin map [`pins/pins.h`](waveshare-esp32c6-lcd147/pins/pins.h)

**10/24 committed** · 2 assigned · 4 conditional · **8 free** (2 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | assigned | I2C_PIN_SCL | ADC, sleep-wake |
| 1 | assigned | I2C_PIN_SDA | ADC, sleep-wake |
| 2 | **free** | — | ADC, sleep-wake |
| 3 | **free** | — | ADC, sleep-wake |
| 4 | committed | SD_PIN_CS | ADC, sleep-wake, strap⚠ |
| 5 | committed | SD_PIN_MISO, TFT_PIN_MISO | ADC, sleep-wake, strap⚠ |
| 6 | committed | SD_PIN_MOSI, TFT_PIN_MOSI | ADC, sleep-wake |
| 7 | committed | SD_PIN_CLK, TFT_PIN_SCK | sleep-wake |
| 8 | committed | RGBLED_PIN | strap⚠ |
| 9 | committed | BOOT_BUTTON_PIN | strap⚠ |
| 10 | **free** | — |  |
| 11 | **free** | — |  |
| 12 | conditional | USB-Serial/JTAG — free only if you give up USB |  |
| 13 | conditional | USB-Serial/JTAG — free only if you give up USB |  |
| 14 | committed | TFT_PIN_CS |  |
| 15 | committed | TFT_PIN_DC | strap⚠ |
| 16 | conditional | UART0 console — free only if you give up the serial log |  |
| 17 | conditional | UART0 console — free only if you give up the serial log |  |
| 18 | **free** | — |  |
| 19 | **free** | — |  |
| 20 | **free** | — |  |
| 21 | committed | TFT_PIN_RST |  |
| 22 | committed | TFT_PIN_BL |  |
| 23 | **free** | — |  |

⚠ 1 pin define(s) are `-1` — not wired OR not yet verified (see the comments in pins.h): `BUZZER_PIN`. Free counts above may shrink as these resolve.

Peripheral demand (declared pin map vs MCU): SPI 1/1 · I2C 1/1 · UART 0/2 · RMT TX 1/2 · LEDC 1/6.

Capabilities on: `HAS_BACKLIGHT_PWM`, `HAS_BLE`, `HAS_DISPLAY`, `HAS_RGBLED`, `HAS_SD_CARD`, `HAS_THREAD_ZIGBEE`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_BATTERY`, `HAS_CAMERA`, `HAS_MICROPHONE`, `HAS_NATIVE_USB`, `HAS_PSRAM`, `HAS_RTC`, `HAS_TOUCH`.

**Thermals:** Coolest display in the fleet: single-core C6 + a 1.47" backlight. Nightstand duty is thermally trivial, and night dimming already floors what little there is.

### `waveshare-esp32c6-lcd169` — Waveshare ESP32-C6-LCD-1.69

ESP32-C6 · flash 4 MB · PSRAM 0 MB · pin map [`pins/pins.h`](waveshare-esp32c6-lcd169/pins/pins.h)

**7/24 committed** · 0 assigned · 7 conditional · **10 free** (4 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | **free** | — | ADC, sleep-wake |
| 1 | **free** | — | ADC, sleep-wake |
| 2 | **free** | — | ADC, sleep-wake |
| 3 | **free** | — | ADC, sleep-wake |
| 4 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 5 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 6 | committed | TFT_PIN_MOSI | ADC, sleep-wake |
| 7 | committed | TFT_PIN_SCK | sleep-wake |
| 8 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 9 | committed | BOOT_BUTTON_PIN | strap⚠ |
| 10 | **free** | — |  |
| 11 | **free** | — |  |
| 12 | conditional | USB-Serial/JTAG — free only if you give up USB |  |
| 13 | conditional | USB-Serial/JTAG — free only if you give up USB |  |
| 14 | committed | TFT_PIN_CS |  |
| 15 | committed | TFT_PIN_DC | strap⚠ |
| 16 | conditional | UART0 console — free only if you give up the serial log |  |
| 17 | conditional | UART0 console — free only if you give up the serial log |  |
| 18 | **free** | — |  |
| 19 | **free** | — |  |
| 20 | **free** | — |  |
| 21 | committed | TFT_PIN_RST |  |
| 22 | committed | TFT_PIN_BL |  |
| 23 | **free** | — |  |

⚠ 11 pin define(s) are `-1` — not wired OR not yet verified (see the comments in pins.h): `BAT_ADC_PIN`, `I2C_PIN_SCL`, `I2C_PIN_SDA`, `I2S_PIN_BCLK`, `I2S_PIN_DIN`, `I2S_PIN_DOUT`, `I2S_PIN_LRCLK`, `IMU_PIN_INT1`, `PWR_KEY_PIN`, `RTC_PIN_INT`, `TFT_PIN_MISO`. Free counts above may shrink as these resolve.

Peripheral demand (declared pin map vs MCU): SPI 1/1 · I2C 0/1 · UART 0/2 · RMT TX 0/2 · LEDC 1/6.

Capabilities on: `HAS_BACKLIGHT_PWM`, `HAS_BATTERY`, `HAS_BLE`, `HAS_DISPLAY`, `HAS_IMU`, `HAS_MICROPHONE`, `HAS_RTC`, `HAS_THREAD_ZIGBEE`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_CAMERA`, `HAS_NATIVE_USB`, `HAS_PSRAM`, `HAS_RGBLED`, `HAS_SD_CARD`, `HAS_TOUCH`.

**Thermals:** Battery charging warms the PCB next to the PCF85063 crystal (clock drift) and the QMI8658 (bias) — charge with the backlight dimmed and don't calibrate the IMU mid-charge. Die-temp watchdog is the runtime gauge.

### `waveshare-esp32s3-amoled206` — Waveshare ESP32-S3-Touch-AMOLED-2.06

ESP32-S3 · flash 32 MB · PSRAM 8 MB · pin map [`pins/pins.h`](waveshare-esp32s3-amoled206/pins/pins.h)

**21/33 committed** · 0 assigned · 5 conditional · **7 free** (3 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | sleep-wake, strap⚠ |
| 1 | committed | SD_PIN_CMD | ADC, sleep-wake |
| 2 | committed | SD_PIN_CLK | ADC, sleep-wake |
| 3 | committed | SD_PIN_D0 | ADC, sleep-wake, strap⚠ |
| 4 | committed | LCD_PIN_SDIO0 | ADC, sleep-wake |
| 5 | committed | LCD_PIN_SDIO1 | ADC, sleep-wake |
| 6 | committed | LCD_PIN_SDIO2 | ADC, sleep-wake |
| 7 | committed | LCD_PIN_SDIO3 | ADC, sleep-wake |
| 8 | committed | LCD_PIN_RST | ADC, sleep-wake |
| 9 | committed | TOUCH_PIN_RST | ADC, sleep-wake |
| 10 | **free** | — | ADC, sleep-wake |
| 11 | committed | LCD_PIN_SCLK | ADC, sleep-wake |
| 12 | committed | LCD_PIN_CS | ADC, sleep-wake |
| 13 | **free** | — | ADC, sleep-wake |
| 14 | committed | I2C_PIN_SCL | ADC, sleep-wake |
| 15 | committed | I2C_PIN_SDA | ADC, sleep-wake |
| 16 | committed | I2S_PIN_MCLK | ADC, sleep-wake |
| 17 | committed | SD_PIN_CS | ADC, sleep-wake |
| 18 | **free** | — | ADC, sleep-wake |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB | ADC, sleep-wake |
| 20 | conditional | USB-Serial/JTAG — free only if you give up USB | ADC, sleep-wake |
| 21 | **free** | — | sleep-wake |
| 38 | committed | TOUCH_PIN_INT |  |
| 39 | **free** | — |  |
| 40 | committed | I2S_PIN_DOUT |  |
| 41 | committed | I2S_PIN_BCLK |  |
| 42 | committed | I2S_PIN_DIN |  |
| 43 | conditional | UART0 console — free only if you give up the serial log |  |
| 44 | conditional | UART0 console — free only if you give up the serial log |  |
| 45 | committed | I2S_PIN_WS | strap⚠ |
| 46 | committed | AUDIO_PIN_PA_EN | strap⚠ |
| 47 | **free** | — |  |
| 48 | **free** | — |  |

⚠ 1 pin define(s) are `-1` — not wired OR not yet verified (see the comments in pins.h): `LCD_PIN_BL`. Free counts above may shrink as these resolve.

Peripheral demand (declared pin map vs MCU): SPI 1/2 · I2C 1/2 · UART 0/3 · RMT TX 0/4 · LEDC 0/8.

Capabilities on: `HAS_AMOLED`, `HAS_BATTERY`, `HAS_BLE`, `HAS_IMU`, `HAS_PMU`, `HAS_PSRAM`, `HAS_RTC`, `HAS_SD_CARD`, `HAS_SPEAKER`, `HAS_TOUCH`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_CAMERA`, `HAS_HAPTIC`, `HAS_MICROPHONE`.

**Thermals:** Skin contact, so thermals are a comfort question before they are a silicon one. AMOLED on a mostly-black face is cheap and the panel is off most of the time (wake-on-raise); the real budget is radio duty. Charging a Li-po against a wrist is the case to watch — dock it, don't wear it charging.

### `waveshare-esp32s3-lcd147` — Waveshare ESP32-S3-LCD-1.47

ESP32-S3 · flash 16 MB · PSRAM 8 MB · pin map [`pins/pins.h`](waveshare-esp32s3-lcd147/pins/pins.h)

**11/38 committed** · 1 assigned · 6 conditional · **20 free** (13 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | committed | BOOT_BUTTON_PIN | sleep-wake, strap⚠ |
| 1 | **free** | — | ADC, sleep-wake |
| 2 | **free** | — | ADC, sleep-wake |
| 3 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 4 | **free** | — | ADC, sleep-wake |
| 5 | **free** | — | ADC, sleep-wake |
| 6 | **free** | — | ADC, sleep-wake |
| 7 | **free** | — | ADC, sleep-wake |
| 8 | **free** | — | ADC, sleep-wake |
| 9 | **free** | — | ADC, sleep-wake |
| 10 | **free** | — | ADC, sleep-wake |
| 11 | **free** | — | ADC, sleep-wake |
| 12 | **free** | — | ADC, sleep-wake |
| 13 | **free** | — | ADC, sleep-wake |
| 14 | committed | SD_PIN_CLK | ADC, sleep-wake |
| 15 | committed | SD_PIN_MOSI | ADC, sleep-wake |
| 16 | committed | I2C_PIN_SDA, SD_PIN_MISO | ADC, sleep-wake |
| 17 | assigned | I2C_PIN_SCL | ADC, sleep-wake |
| 18 | **free** | — | ADC, sleep-wake |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_DM_PIN) | ADC, sleep-wake |
| 20 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_DP_PIN) | ADC, sleep-wake |
| 21 | **free** | — | sleep-wake |
| 33 | **free** | — |  |
| 34 | **free** | — |  |
| 35 | **free** | — |  |
| 36 | **free** | — |  |
| 37 | **free** | — |  |
| 38 | committed | RGBLED_PIN |  |
| 39 | committed | TFT_PIN_RST |  |
| 40 | committed | TFT_PIN_SCK |  |
| 41 | committed | TFT_PIN_DC |  |
| 42 | committed | TFT_PIN_CS |  |
| 43 | conditional | UART0 console — free only if you give up the serial log |  |
| 44 | conditional | UART0 console — free only if you give up the serial log |  |
| 45 | committed | TFT_PIN_MOSI | strap⚠ |
| 46 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 47 | **free** | — |  |
| 48 | committed | TFT_PIN_BL |  |

⚠ 3 pin define(s) are `-1` — not wired OR not yet verified (see the comments in pins.h): `BUZZER_PIN`, `SD_PIN_CS`, `TFT_PIN_MISO`. Free counts above may shrink as these resolve.

Peripheral demand (declared pin map vs MCU): SPI 2/2 · I2C 1/2 · UART 0/3 · RMT TX 1/4 · LEDC 1/8.

Capabilities on: `HAS_BACKLIGHT_PWM`, `HAS_BLE`, `HAS_DISPLAY`, `HAS_NATIVE_USB`, `HAS_PSRAM`, `HAS_RGBLED`, `HAS_SD_CARD`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_BATTERY`, `HAS_CAMERA`, `HAS_MICROPHONE`, `HAS_RTC`, `HAS_TOUCH`.

**Thermals:** USB-A stick body: S3 + octal PSRAM heat-soak into a small stick that plugs into an already-warm port. The double-buffered animation budget is the throttle knob; die-temp watchdog is the gauge.

### `waveshare-esp32s3-lcd43` — Waveshare ESP32-S3-Touch-LCD-4.3

ESP32-S3 · flash 16 MB · PSRAM 8 MB · pin map [`pins/pins.h`](waveshare-esp32s3-lcd43/pins/pins.h)

**27/33 committed** · 0 assigned · 4 conditional · **2 free** (2 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | committed | LCD_PIN_G3 | sleep-wake, strap⚠ |
| 1 | committed | LCD_PIN_R3 | ADC, sleep-wake |
| 2 | committed | LCD_PIN_R4 | ADC, sleep-wake |
| 3 | committed | LCD_PIN_VSYNC | ADC, sleep-wake, strap⚠ |
| 4 | committed | TOUCH_PIN_INT | ADC, sleep-wake |
| 5 | committed | LCD_PIN_DE | ADC, sleep-wake |
| 6 | committed | BUZZER_PIN | ADC, sleep-wake |
| 7 | committed | LCD_PIN_PCLK | ADC, sleep-wake |
| 8 | committed | I2C_PIN_SDA | ADC, sleep-wake |
| 9 | committed | I2C_PIN_SCL | ADC, sleep-wake |
| 10 | committed | LCD_PIN_B7 | ADC, sleep-wake |
| 11 | committed | SD_PIN_MOSI | ADC, sleep-wake |
| 12 | committed | SD_PIN_SCK | ADC, sleep-wake |
| 13 | committed | SD_PIN_MISO | ADC, sleep-wake |
| 14 | committed | LCD_PIN_B3 | ADC, sleep-wake |
| 15 | **free** | — | ADC, sleep-wake |
| 16 | **free** | — | ADC, sleep-wake |
| 17 | committed | LCD_PIN_B6 | ADC, sleep-wake |
| 18 | committed | LCD_PIN_B5 | ADC, sleep-wake |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB | ADC, sleep-wake |
| 20 | conditional | USB-Serial/JTAG — free only if you give up USB | ADC, sleep-wake |
| 21 | committed | LCD_PIN_G7 | sleep-wake |
| 38 | committed | LCD_PIN_B4 |  |
| 39 | committed | LCD_PIN_G2 |  |
| 40 | committed | LCD_PIN_R7 |  |
| 41 | committed | LCD_PIN_R6 |  |
| 42 | committed | LCD_PIN_R5 |  |
| 43 | conditional | UART0 console — free only if you give up the serial log |  |
| 44 | conditional | UART0 console — free only if you give up the serial log |  |
| 45 | committed | LCD_PIN_G4 | strap⚠ |
| 46 | committed | LCD_PIN_HSYNC | strap⚠ |
| 47 | committed | LCD_PIN_G6 |  |
| 48 | committed | LCD_PIN_G5 |  |

⚠ 3 pin define(s) are `-1` — not wired OR not yet verified (see the comments in pins.h): `BOOT_BUTTON_PIN`, `SD_PIN_CS`, `TOUCH_PIN_RST`. Free counts above may shrink as these resolve.

Peripheral demand (declared pin map vs MCU): SPI 1/2 · I2C 1/2 · UART 0/3 · RMT TX 0/4 · LEDC 1/8.

Capabilities on: `HAS_BLE`, `HAS_CAN_RS485`, `HAS_DISPLAY`, `HAS_PSRAM`, `HAS_SD_CARD`, `HAS_TOUCH`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_BACKLIGHT_PWM`, `HAS_BATTERY`, `HAS_CAMERA`, `HAS_MICROPHONE`, `HAS_RTC`.

**Thermals:** The 800x480 backlight dominates the budget; full brightness in a case raises die temp — night dimming doubles as the thermal relief valve.

### `waveshare-esp32s3-lcd43b` — Waveshare ESP32-S3-Touch-LCD-4.3B

ESP32-S3 · flash 16 MB · PSRAM 8 MB · pin map [`pins/pins.h`](waveshare-esp32s3-lcd43b/pins/pins.h)

**30/33 committed** · 0 assigned · 2 conditional · **1 free** (1 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | committed | LCD_PIN_G3 | sleep-wake, strap⚠ |
| 1 | committed | LCD_PIN_R3 | ADC, sleep-wake |
| 2 | committed | LCD_PIN_R4 | ADC, sleep-wake |
| 3 | committed | LCD_PIN_VSYNC | ADC, sleep-wake, strap⚠ |
| 4 | committed | TOUCH_PIN_INT | ADC, sleep-wake |
| 5 | committed | LCD_PIN_DE | ADC, sleep-wake |
| 6 | **free** | — | ADC, sleep-wake |
| 7 | committed | LCD_PIN_PCLK | ADC, sleep-wake |
| 8 | committed | I2C_PIN_SDA | ADC, sleep-wake |
| 9 | committed | I2C_PIN_SCL | ADC, sleep-wake |
| 10 | committed | LCD_PIN_B7 | ADC, sleep-wake |
| 11 | committed | SD_PIN_MOSI | ADC, sleep-wake |
| 12 | committed | SD_PIN_SCK | ADC, sleep-wake |
| 13 | committed | SD_PIN_MISO | ADC, sleep-wake |
| 14 | committed | LCD_PIN_B3 | ADC, sleep-wake |
| 15 | committed | CAN_PIN_TX | ADC, sleep-wake |
| 16 | committed | CAN_PIN_RX | ADC, sleep-wake |
| 17 | committed | LCD_PIN_B6 | ADC, sleep-wake |
| 18 | committed | LCD_PIN_B5 | ADC, sleep-wake |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DN) | ADC, sleep-wake |
| 20 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DP) | ADC, sleep-wake |
| 21 | committed | LCD_PIN_G7 | sleep-wake |
| 38 | committed | LCD_PIN_B4 |  |
| 39 | committed | LCD_PIN_G2 |  |
| 40 | committed | LCD_PIN_R7 |  |
| 41 | committed | LCD_PIN_R6 |  |
| 42 | committed | LCD_PIN_R5 |  |
| 43 | committed | RS485_PIN_RX |  |
| 44 | committed | RS485_PIN_TX |  |
| 45 | committed | LCD_PIN_G4 | strap⚠ |
| 46 | committed | LCD_PIN_HSYNC | strap⚠ |
| 47 | committed | LCD_PIN_G6 |  |
| 48 | committed | LCD_PIN_G5 |  |

⚠ 4 pin define(s) are `-1` — not wired OR not yet verified (see the comments in pins.h): `BOOT_BUTTON_PIN`, `BUZZER_PIN`, `SD_PIN_CS`, `TOUCH_PIN_RST`. Free counts above may shrink as these resolve.

Peripheral demand (declared pin map vs MCU): SPI 1/2 · I2C 1/2 · UART 0/3 · RMT TX 0/4 · LEDC 0/8.

Capabilities on: `HAS_BLE`, `HAS_CAN_RS485`, `HAS_DISPLAY`, `HAS_I2C_HEADER`, `HAS_ISOLATED_IO`, `HAS_PSRAM`, `HAS_SD_CARD`, `HAS_TOUCH`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_BACKLIGHT_PWM`, `HAS_BATTERY`, `HAS_CAMERA`, `HAS_MICROPHONE`, `HAS_RTC`.

**Thermals:** Same panel thermals as the 4.3, plus the isolated DI/DO, RS485, and CAN transceivers add standing dissipation from the 6-36 V supply — ventilate the enclosure.

### `waveshare-esp32s3-lcd43c` — Waveshare ESP32-S3-Touch-LCD-4.3C

ESP32-S3 · flash 16 MB · PSRAM 8 MB · pin map [`pins/pins.h`](waveshare-esp32s3-lcd43c/pins/pins.h)

**28/33 committed** · 0 assigned · 2 conditional · **3 free** (3 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | committed | LCD_PIN_G3 | sleep-wake, strap⚠ |
| 1 | committed | LCD_PIN_R3 | ADC, sleep-wake |
| 2 | committed | LCD_PIN_R4 | ADC, sleep-wake |
| 3 | committed | LCD_PIN_VSYNC | ADC, sleep-wake, strap⚠ |
| 4 | committed | TOUCH_PIN_INT | ADC, sleep-wake |
| 5 | committed | LCD_PIN_DE | ADC, sleep-wake |
| 6 | committed | AUDIO_PIN_I2S_MCLK | ADC, sleep-wake |
| 7 | committed | LCD_PIN_PCLK | ADC, sleep-wake |
| 8 | committed | I2C_PIN_SDA | ADC, sleep-wake |
| 9 | committed | I2C_PIN_SCL | ADC, sleep-wake |
| 10 | committed | LCD_PIN_B7 | ADC, sleep-wake |
| 11 | **free** | — | ADC, sleep-wake |
| 12 | **free** | — | ADC, sleep-wake |
| 13 | **free** | — | ADC, sleep-wake |
| 14 | committed | LCD_PIN_B3 | ADC, sleep-wake |
| 15 | committed | AUDIO_PIN_I2S_ASDOUT | ADC, sleep-wake |
| 16 | committed | AUDIO_PIN_I2S_LRCK | ADC, sleep-wake |
| 17 | committed | LCD_PIN_B6 | ADC, sleep-wake |
| 18 | committed | LCD_PIN_B5 | ADC, sleep-wake |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB | ADC, sleep-wake |
| 20 | conditional | USB-Serial/JTAG — free only if you give up USB | ADC, sleep-wake |
| 21 | committed | LCD_PIN_G7 | sleep-wake |
| 38 | committed | LCD_PIN_B4 |  |
| 39 | committed | LCD_PIN_G2 |  |
| 40 | committed | LCD_PIN_R7 |  |
| 41 | committed | LCD_PIN_R6 |  |
| 42 | committed | LCD_PIN_R5 |  |
| 43 | committed | AUDIO_PIN_I2S_SDIN |  |
| 44 | committed | AUDIO_PIN_I2S_SCLK |  |
| 45 | committed | LCD_PIN_G4 | strap⚠ |
| 46 | committed | LCD_PIN_HSYNC | strap⚠ |
| 47 | committed | LCD_PIN_G6 |  |
| 48 | committed | LCD_PIN_G5 |  |

⚠ 5 pin define(s) are `-1` — not wired OR not yet verified (see the comments in pins.h): `AUDIO_PIN_PA_ENABLE`, `BATTERY_PIN_ADC`, `BOOT_BUTTON_PIN`, `BUZZER_PIN`, `TOUCH_PIN_RST`. Free counts above may shrink as these resolve.

Peripheral demand (declared pin map vs MCU): SPI 0/2 · I2C 1/2 · UART 0/3 · RMT TX 0/4 · LEDC 0/8.

Capabilities on: `HAS_AUDIO_CODEC`, `HAS_BLE`, `HAS_DISPLAY`, `HAS_MICROPHONE`, `HAS_PSRAM`, `HAS_RTC`, `HAS_SD_CARD`, `HAS_TOUCH`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_BACKLIGHT_PWM`, `HAS_BATTERY`, `HAS_CAMERA`, `HAS_CAN_RS485`.

**Thermals:** Cased BOX edition traps heat from the backlight + ES8311/ES7210 audio stack. Hold brightness headroom so the on-glass mic indicator stays readable, and ventilate the case.

### `waveshare-esp32s3-lcd7` — Waveshare ESP32-S3-Touch-LCD-7

ESP32-S3 · flash 16 MB · PSRAM 8 MB · pin map [`pins/pins.h`](waveshare-esp32s3-lcd7/pins/pins.h)

**23/33 committed** · 0 assigned · 4 conditional · **6 free** (6 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | committed | LCD_PIN_G3 | sleep-wake, strap⚠ |
| 1 | committed | LCD_PIN_R3 | ADC, sleep-wake |
| 2 | committed | LCD_PIN_R4 | ADC, sleep-wake |
| 3 | committed | LCD_PIN_VSYNC | ADC, sleep-wake, strap⚠ |
| 4 | committed | TOUCH_PIN_INT | ADC, sleep-wake |
| 5 | committed | LCD_PIN_DE | ADC, sleep-wake |
| 6 | **free** | — | ADC, sleep-wake |
| 7 | committed | LCD_PIN_PCLK | ADC, sleep-wake |
| 8 | committed | I2C_PIN_SDA | ADC, sleep-wake |
| 9 | committed | I2C_PIN_SCL | ADC, sleep-wake |
| 10 | committed | LCD_PIN_B7 | ADC, sleep-wake |
| 11 | **free** | — | ADC, sleep-wake |
| 12 | **free** | — | ADC, sleep-wake |
| 13 | **free** | — | ADC, sleep-wake |
| 14 | committed | LCD_PIN_B3 | ADC, sleep-wake |
| 15 | **free** | — | ADC, sleep-wake |
| 16 | **free** | — | ADC, sleep-wake |
| 17 | committed | LCD_PIN_B6 | ADC, sleep-wake |
| 18 | committed | LCD_PIN_B5 | ADC, sleep-wake |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB | ADC, sleep-wake |
| 20 | conditional | USB-Serial/JTAG — free only if you give up USB | ADC, sleep-wake |
| 21 | committed | LCD_PIN_G7 | sleep-wake |
| 38 | committed | LCD_PIN_B4 |  |
| 39 | committed | LCD_PIN_G2 |  |
| 40 | committed | LCD_PIN_R7 |  |
| 41 | committed | LCD_PIN_R6 |  |
| 42 | committed | LCD_PIN_R5 |  |
| 43 | conditional | UART0 console — free only if you give up the serial log |  |
| 44 | conditional | UART0 console — free only if you give up the serial log |  |
| 45 | committed | LCD_PIN_G4 | strap⚠ |
| 46 | committed | LCD_PIN_HSYNC | strap⚠ |
| 47 | committed | LCD_PIN_G6 |  |
| 48 | committed | LCD_PIN_G5 |  |

⚠ 2 pin define(s) are `-1` — not wired OR not yet verified (see the comments in pins.h): `BOOT_BUTTON_PIN`, `TOUCH_PIN_RST`. Free counts above may shrink as these resolve.

Peripheral demand (declared pin map vs MCU): SPI 0/2 · I2C 1/2 · UART 0/3 · RMT TX 0/4 · LEDC 0/8.

Capabilities on: `HAS_BLE`, `HAS_CAN_RS485`, `HAS_DISPLAY`, `HAS_NATIVE_USB`, `HAS_PSRAM`, `HAS_SD_CARD`, `HAS_TOUCH`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_BACKLIGHT_PWM`, `HAS_BATTERY`, `HAS_CAMERA`, `HAS_MICROPHONE`, `HAS_RGBLED`, `HAS_RTC`.

**Thermals:** The hottest board in the fleet: a 7" backlight plus continuous 800x480 RGB refresh out of octal PSRAM. Backlight duty (via the CH422G) is the main relief valve and the die-temp watchdog (FEATURE_DIAGNOSTICS) the gauge; keep the GT911 glass out of direct sun — capacitive touch drifts when hot.

### `waveshare-esp32s3-touch-lcd169` — Waveshare ESP32-S3-Touch-LCD-1.69

ESP32-S3 · flash 16 MB · PSRAM 8 MB · pin map [`pins/pins.h`](waveshare-esp32s3-touch-lcd169/pins/pins.h)

**17/33 committed** · 0 assigned · 7 conditional · **9 free** (6 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | committed | BOOT_BUTTON_PIN | sleep-wake, strap⚠ |
| 1 | committed | BAT_ADC_PIN | ADC, sleep-wake |
| 2 | **free** | — | ADC, sleep-wake |
| 3 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 4 | committed | TFT_PIN_DC | ADC, sleep-wake |
| 5 | committed | TFT_PIN_CS | ADC, sleep-wake |
| 6 | committed | TFT_PIN_SCK | ADC, sleep-wake |
| 7 | committed | TFT_PIN_MOSI | ADC, sleep-wake |
| 8 | committed | TFT_PIN_RST | ADC, sleep-wake |
| 9 | **free** | — | ADC, sleep-wake |
| 10 | committed | I2C_PIN_SCL | ADC, sleep-wake |
| 11 | committed | I2C_PIN_SDA | ADC, sleep-wake |
| 12 | **free** | — | ADC, sleep-wake |
| 13 | committed | TOUCH_PIN_RST | ADC, sleep-wake |
| 14 | committed | TOUCH_PIN_INT | ADC, sleep-wake |
| 15 | committed | TFT_PIN_BL | ADC, sleep-wake |
| 16 | **free** | — | ADC, sleep-wake |
| 17 | **free** | — | ADC, sleep-wake |
| 18 | **free** | — | ADC, sleep-wake |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DM) | ADC, sleep-wake |
| 20 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DP) | ADC, sleep-wake |
| 21 | **free** | — | sleep-wake |
| 38 | committed | IMU_PIN_INT1 |  |
| 39 | committed | RTC_PIN_INT |  |
| 40 | committed | SYS_PWR_OUT_PIN |  |
| 41 | committed | SYS_PWR_EN_PIN |  |
| 42 | committed | BUZZER_PIN |  |
| 43 | conditional | UART0 console — free only if you give up the serial log |  |
| 44 | conditional | UART0 console — free only if you give up the serial log |  |
| 45 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 46 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 47 | **free** | — |  |
| 48 | **free** | — |  |

⚠ 2 pin define(s) are `-1` — not wired OR not yet verified (see the comments in pins.h): `PWR_KEY_PIN`, `TFT_PIN_MISO`. Free counts above may shrink as these resolve.

Peripheral demand (declared pin map vs MCU): SPI 1/2 · I2C 1/2 · UART 0/3 · RMT TX 0/4 · LEDC 2/8.

Capabilities on: `HAS_BACKLIGHT_PWM`, `HAS_BATTERY`, `HAS_BLE`, `HAS_BUZZER`, `HAS_DISPLAY`, `HAS_IMU`, `HAS_NATIVE_USB`, `HAS_PSRAM`, `HAS_RTC`, `HAS_TOUCH`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_CAMERA`, `HAS_MICROPHONE`, `HAS_RGBLED`, `HAS_SD_CARD`, `HAS_THREAD_ZIGBEE`.

**Thermals:** Battery charging + the S3R8 render load stack on one small PCB: charging warms the PCF85063 crystal (drift) and QMI8658 (bias) — charge with the backlight dimmed, don't calibrate the IMU mid-charge. CST816 touch drifts when hot; keep the glass out of direct sun. The night backlight floor doubles as the thermal relief valve.

### `xiao-esp32c3` — Seeed Studio XIAO ESP32-C3

ESP32-C3 · flash 4 MB · PSRAM 0 MB · pin map [`pins/pins.h`](xiao-esp32c3/pins/pins.h)

**1/15 committed** · 4 assigned · 6 conditional · **4 free** (4 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | **free** | — | ADC, sleep-wake |
| 1 | **free** | — | ADC, sleep-wake |
| 2 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 3 | assigned | EXT_LED_PIN_DEFAULT | ADC, sleep-wake |
| 4 | **free** | — | ADC, sleep-wake |
| 5 | **free** | — | ADC, sleep-wake |
| 6 | assigned | I2C_PIN_SDA |  |
| 7 | assigned | I2C_PIN_SCL |  |
| 8 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing; (declared as SPI_PIN_SCK) | strap⚠ |
| 9 | committed | BOOT_BUTTON_PIN, SPI_PIN_MISO | strap⚠ |
| 10 | assigned | SPI_PIN_MOSI |  |
| 18 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DN) |  |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DP) |  |
| 20 | conditional | UART0 console — free only if you give up the serial log; (declared as UART1_PIN_RX) |  |
| 21 | conditional | UART0 console — free only if you give up the serial log; (declared as UART1_PIN_TX) |  |

Physically broken out (from `PIN_D*/PIN_A*/PIN_GPIO*` aliases): [2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21] — of the free pins, **[4, 5]** reach a header.

Peripheral demand (declared pin map vs MCU): SPI 1/1 · I2C 1/1 · UART 1/2 · RMT TX 0/2 · LEDC 0/6.

Capabilities on: `HAS_BLE`, `HAS_USB_CDC`, `HAS_VISION_AI`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_CAMERA`, `HAS_GNSS_UART`, `HAS_MICROPHONE`, `HAS_PSRAM`, `HAS_SD_CARD`, `HAS_TAMPER_INPUT`.

**Thermals:** C3 at Vision-host duty runs cool; no special measures.

### `xiao-esp32c3-sentinel-lite` — Seeed Studio XIAO ESP32-C3 (Sentinel Lite)

ESP32-C3 · flash 4 MB · PSRAM 0 MB · pin map [`pins/pins.h`](xiao-esp32c3-sentinel-lite/pins/pins.h)

**2/15 committed** · 0 assigned · 7 conditional · **6 free** (4 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | **free** | — | ADC, sleep-wake |
| 1 | **free** | — | ADC, sleep-wake |
| 2 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 3 | committed | PIR_PIN | ADC, sleep-wake |
| 4 | **free** | — | ADC, sleep-wake |
| 5 | **free** | — | ADC, sleep-wake |
| 6 | **free** | — |  |
| 7 | **free** | — |  |
| 8 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 9 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 10 | committed | LED_USER_PIN |  |
| 18 | conditional | USB-Serial/JTAG — free only if you give up USB |  |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB |  |
| 20 | conditional | UART0 console — free only if you give up the serial log |  |
| 21 | conditional | UART0 console — free only if you give up the serial log |  |

Peripheral demand (declared pin map vs MCU): SPI 0/1 · I2C 0/1 · UART 0/2 · RMT TX 0/2 · LEDC 0/6.

Capabilities on: `HAS_AMBIENT_LIGHT`, `HAS_BLE`, `HAS_PIR`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_CAMERA`, `HAS_MICROPHONE`, `HAS_MMWAVE_RADAR`, `HAS_PSRAM`, `HAS_SD_CARD`.

**Thermals:** PIR needs thermal stability — keep the sensor stalk clear of the MCU and out of drafts.

### `xiao-esp32c6-mr60` — Seeed Studio XIAO ESP32-C6 (MR60BHA2 mmWave kit)

ESP32-C6 · flash 4 MB · PSRAM 0 MB · pin map [`pins/pins.h`](xiao-esp32c6-mr60/pins/pins.h)

**2/24 committed** · 2 assigned · 8 conditional · **12 free** (4 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | **free** | — | ADC, sleep-wake |
| 1 | committed | LED_WS2812_PIN | ADC, sleep-wake |
| 2 | **free** | — | ADC, sleep-wake |
| 3 | **free** | — | ADC, sleep-wake |
| 4 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 5 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 6 | **free** | — | ADC, sleep-wake |
| 7 | **free** | — | sleep-wake |
| 8 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 9 | committed | BOOT_BUTTON_PIN | strap⚠ |
| 10 | **free** | — |  |
| 11 | **free** | — |  |
| 12 | conditional | USB-Serial/JTAG — free only if you give up USB |  |
| 13 | conditional | USB-Serial/JTAG — free only if you give up USB |  |
| 14 | **free** | — |  |
| 15 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 16 | conditional | UART0 console — free only if you give up the serial log |  |
| 17 | conditional | UART0 console — free only if you give up the serial log |  |
| 18 | **free** | — |  |
| 19 | **free** | — |  |
| 20 | **free** | — |  |
| 21 | **free** | — |  |
| 22 | assigned | I2C_PIN_SDA |  |
| 23 | assigned | I2C_PIN_SCL |  |

Peripheral demand (declared pin map vs MCU): SPI 0/1 · I2C 1/1 · UART 0/2 · RMT TX 1/2 · LEDC 0/6.

Capabilities on: `HAS_AMBIENT_LIGHT`, `HAS_BLE`, `HAS_IEEE802154`, `HAS_MMWAVE_RADAR`, `HAS_RGB_LED`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_CAMERA`, `HAS_MICROPHONE`, `HAS_PSRAM`, `HAS_SD_CARD`.

**Thermals:** The MR60 radar dissipates on its own PCB; keep the radome off hot surfaces and out of appliance exhaust — the radar noise floor rises with temperature.

### `xiao-esp32c6-sentinel` — Seeed Studio XIAO ESP32-C6 (Sentinel Standard head)

ESP32-C6 · flash 4 MB · PSRAM 0 MB · pin map [`pins/pins.h`](xiao-esp32c6-sentinel/pins/pins.h)

**2/24 committed** · 0 assigned · 9 conditional · **13 free** (3 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | **free** | — | ADC, sleep-wake |
| 1 | committed | LED_WS2812_PIN | ADC, sleep-wake |
| 2 | committed | PIR_PIN | ADC, sleep-wake |
| 3 | **free** | — | ADC, sleep-wake |
| 4 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 5 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | ADC, sleep-wake, strap⚠ |
| 6 | **free** | — | ADC, sleep-wake |
| 7 | **free** | — | sleep-wake |
| 8 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 9 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 10 | **free** | — |  |
| 11 | **free** | — |  |
| 12 | conditional | USB-Serial/JTAG — free only if you give up USB |  |
| 13 | conditional | USB-Serial/JTAG — free only if you give up USB |  |
| 14 | **free** | — |  |
| 15 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 16 | conditional | UART0 console — free only if you give up the serial log |  |
| 17 | conditional | UART0 console — free only if you give up the serial log |  |
| 18 | **free** | — |  |
| 19 | **free** | — |  |
| 20 | **free** | — |  |
| 21 | **free** | — |  |
| 22 | **free** | — |  |
| 23 | **free** | — |  |

Peripheral demand (declared pin map vs MCU): SPI 0/1 · I2C 0/1 · UART 0/2 · RMT TX 1/2 · LEDC 0/6.

Capabilities on: `HAS_AMBIENT_LIGHT`, `HAS_BLE`, `HAS_MMWAVE_RADAR`, `HAS_PIR`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_CAMERA`, `HAS_MICROPHONE`, `HAS_PSRAM`, `HAS_SD_CARD`.

**Thermals:** PIR needs thermal stability — mount the sensor head clear of the C6's own warmth and of drafts; the radar noise floor rises with temperature.

### `xiao-esp32s3` — Seeed Studio XIAO ESP32-S3 (plain, non-Sense)

ESP32-S3 · flash 8 MB · PSRAM 8 MB · pin map [`pins/pins.h`](xiao-esp32s3/pins/pins.h)

**3/33 committed** · 5 assigned · 7 conditional · **18 free** (11 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | committed | BOOT_BUTTON_PIN | sleep-wake, strap⚠ |
| 1 | committed | VBAT_PIN | ADC, sleep-wake |
| 2 | **free** | — | ADC, sleep-wake |
| 3 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing; (declared as EXT_LED_PIN_DEFAULT) | ADC, sleep-wake, strap⚠ |
| 4 | **free** | — | ADC, sleep-wake |
| 5 | assigned | I2C_PIN_SDA | ADC, sleep-wake |
| 6 | assigned | I2C_PIN_SCL | ADC, sleep-wake |
| 7 | assigned | SPI_PIN_SCK | ADC, sleep-wake |
| 8 | assigned | SPI_PIN_MISO | ADC, sleep-wake |
| 9 | assigned | SPI_PIN_MOSI | ADC, sleep-wake |
| 10 | **free** | — | ADC, sleep-wake |
| 11 | **free** | — | ADC, sleep-wake |
| 12 | **free** | — | ADC, sleep-wake |
| 13 | **free** | — | ADC, sleep-wake |
| 14 | **free** | — | ADC, sleep-wake |
| 15 | **free** | — | ADC, sleep-wake |
| 16 | **free** | — | ADC, sleep-wake |
| 17 | **free** | — | ADC, sleep-wake |
| 18 | **free** | — | ADC, sleep-wake |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DN) | ADC, sleep-wake |
| 20 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DP) | ADC, sleep-wake |
| 21 | committed | LED_BUILTIN | sleep-wake |
| 38 | **free** | — |  |
| 39 | **free** | — |  |
| 40 | **free** | — |  |
| 41 | **free** | — |  |
| 42 | **free** | — |  |
| 43 | conditional | UART0 console — free only if you give up the serial log; (declared as UART1_PIN_TX) |  |
| 44 | conditional | UART0 console — free only if you give up the serial log; (declared as UART1_PIN_RX) |  |
| 45 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 46 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 47 | **free** | — |  |
| 48 | **free** | — |  |

Physically broken out (from `PIN_D*/PIN_A*/PIN_GPIO*` aliases): [1, 2, 3, 4, 5, 6, 7, 8, 9, 43, 44] — of the free pins, **[2, 4]** reach a header.

Peripheral demand (declared pin map vs MCU): SPI 1/2 · I2C 1/2 · UART 1/3 · RMT TX 0/4 · LEDC 0/8.

Capabilities on: `HAS_BLE`, `HAS_PSRAM`, `HAS_USB_CDC`, `HAS_VISION_AI`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_CAMERA`, `HAS_GNSS_UART`, `HAS_MICROPHONE`, `HAS_SD_CARD`, `HAS_TAMPER_INPUT`.

**Thermals:** Light duty — inference runs on the Vision AI module, not this host. Die-temp watchdog covers it; no special measures.

### `xiao-esp32s3-round` — Seeed Studio XIAO ESP32-S3 + Round Display

ESP32-S3 · flash 8 MB · PSRAM 8 MB · pin map [`pins/pins.h`](xiao-esp32s3-round/pins/pins.h)

**13/33 committed** · 0 assigned · 4 conditional · **16 free** (9 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | committed | BOOT_BUTTON_PIN | sleep-wake, strap⚠ |
| 1 | committed | BUZZER_PIN | ADC, sleep-wake |
| 2 | committed | TFT_PIN_CS | ADC, sleep-wake |
| 3 | committed | SD_PIN_CS | ADC, sleep-wake, strap⚠ |
| 4 | committed | TFT_PIN_DC | ADC, sleep-wake |
| 5 | committed | I2C_PIN_SDA | ADC, sleep-wake |
| 6 | committed | I2C_PIN_SCL | ADC, sleep-wake |
| 7 | committed | TFT_PIN_SCK | ADC, sleep-wake |
| 8 | committed | TFT_PIN_MISO | ADC, sleep-wake |
| 9 | committed | TFT_PIN_MOSI | ADC, sleep-wake |
| 10 | **free** | — | ADC, sleep-wake |
| 11 | **free** | — | ADC, sleep-wake |
| 12 | **free** | — | ADC, sleep-wake |
| 13 | **free** | — | ADC, sleep-wake |
| 14 | **free** | — | ADC, sleep-wake |
| 15 | **free** | — | ADC, sleep-wake |
| 16 | **free** | — | ADC, sleep-wake |
| 17 | **free** | — | ADC, sleep-wake |
| 18 | **free** | — | ADC, sleep-wake |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB | ADC, sleep-wake |
| 20 | conditional | USB-Serial/JTAG — free only if you give up USB | ADC, sleep-wake |
| 21 | committed | LED_STATUS_PIN | sleep-wake |
| 38 | **free** | — |  |
| 39 | **free** | — |  |
| 40 | **free** | — |  |
| 41 | **free** | — |  |
| 42 | **free** | — |  |
| 43 | committed | TFT_PIN_BL |  |
| 44 | committed | TOUCH_PIN_INT |  |
| 45 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 46 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 47 | **free** | — |  |
| 48 | **free** | — |  |

⚠ 2 pin define(s) are `-1` — not wired OR not yet verified (see the comments in pins.h): `TFT_PIN_RST`, `TOUCH_PIN_RST`. Free counts above may shrink as these resolve.

Peripheral demand (declared pin map vs MCU): SPI 1/2 · I2C 1/2 · UART 0/3 · RMT TX 0/4 · LEDC 2/8.

Capabilities on: `HAS_BACKLIGHT_PWM`, `HAS_BATTERY`, `HAS_BLE`, `HAS_DISPLAY`, `HAS_PSRAM`, `HAS_RTC`, `HAS_SD_CARD`, `HAS_TOUCH`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): `HAS_CAMERA`, `HAS_MICROPHONE`.

**Thermals:** Backlight + battery charging share one small puck; charging warms the PCF8563 crystal (clock drift). Dim the panel while charging.

### `xiao-esp32s3-sense` — Seeed Studio XIAO ESP32-S3 Sense

ESP32-S3 · flash 8 MB · PSRAM 8 MB · pin map [`pins/pins.h`](xiao-esp32s3-sense/pins/pins.h)

**24/33 committed** · 3 assigned · 5 conditional · **1 free** (1 ADC-capable)

| GPIO | bucket | held by / trade | notes |
|---|---|---|---|
| 0 | committed | BOOT_BUTTON_PIN | sleep-wake, strap⚠ |
| 1 | committed | VBAT_PIN | ADC, sleep-wake |
| 2 | **free** | — | ADC, sleep-wake |
| 3 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing; (declared as EXT_LED_PIN_DEFAULT) | ADC, sleep-wake, strap⚠ |
| 4 | assigned | TAMPER_PIN_DEFAULT | ADC, sleep-wake |
| 5 | assigned | I2C_PIN_SDA | ADC, sleep-wake |
| 6 | assigned | I2C_PIN_SCL | ADC, sleep-wake |
| 7 | committed | SD_PIN_SCK, SPI_PIN_SCK | ADC, sleep-wake |
| 8 | committed | SD_PIN_MISO, SPI_PIN_MISO | ADC, sleep-wake |
| 9 | committed | SD_PIN_MOSI, SPI_PIN_MOSI | ADC, sleep-wake |
| 10 | committed | CAM_PIN_XCLK | ADC, sleep-wake |
| 11 | committed | CAM_PIN_D6 | ADC, sleep-wake |
| 12 | committed | CAM_PIN_D5 | ADC, sleep-wake |
| 13 | committed | CAM_PIN_PCLK | ADC, sleep-wake |
| 14 | committed | CAM_PIN_D4 | ADC, sleep-wake |
| 15 | committed | CAM_PIN_D0 | ADC, sleep-wake |
| 16 | committed | CAM_PIN_D3 | ADC, sleep-wake |
| 17 | committed | CAM_PIN_D1 | ADC, sleep-wake |
| 18 | committed | CAM_PIN_D2 | ADC, sleep-wake |
| 19 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DN) | ADC, sleep-wake |
| 20 | conditional | USB-Serial/JTAG — free only if you give up USB; (declared as USB_PIN_DP) | ADC, sleep-wake |
| 21 | committed | LED_BUILTIN, SD_PIN_CS | sleep-wake |
| 38 | committed | CAM_PIN_VSYNC |  |
| 39 | committed | CAM_PIN_SIOC |  |
| 40 | committed | CAM_PIN_SIOD |  |
| 41 | committed | MIC_PIN_DATA |  |
| 42 | committed | MIC_PIN_CLK |  |
| 43 | committed | GNSS_PIN_TX, UART1_PIN_TX |  |
| 44 | committed | GNSS_PIN_RX, UART1_PIN_RX |  |
| 45 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 46 | conditional | strapping pin — must not be driven at reset; check the boot-mode level before repurposing | strap⚠ |
| 47 | committed | CAM_PIN_HREF |  |
| 48 | committed | CAM_PIN_D7 |  |

Physically broken out (from `PIN_D*/PIN_A*/PIN_GPIO*` aliases): [1, 2, 3, 4, 5, 6, 7, 8, 9, 43, 44] — of the free pins, **[2]** reach a header.

Peripheral demand (declared pin map vs MCU): SPI 1/2 · I2C 1/2 · UART 1/3 · RMT TX 0/4 · LEDC 0/8.

Capabilities on: `HAS_BLE`, `HAS_CAMERA`, `HAS_GNSS_UART`, `HAS_MICROPHONE`, `HAS_PSRAM`, `HAS_SD_CARD`, `HAS_TAMPER_INPUT`, `HAS_USB_CDC`, `HAS_WIFI`.
Capabilities off (room to grow): —.

**Thermals:** Camera PEEK + Wi-Fi bursts are the heat sources; sustained capture warms the sensor (image noise). Runtime gauge is the die-temp watchdog (FEATURE_DIAGNOSTICS). In enclosures, give the camera ribbon and u.FL antenna airspace and derate PEEK cadence before the watchdog throttles.
