# Waveshare ESP32-S3-Touch-AMOLED-2.41 (the flagship glance glass)

The prettiest panel in the fleet: a 2.41" **450 × 600 AMOLED** in Waveshare's
metal case, with touch, an IMU, an RTC, a microSD slot and a battery — the
board the `canary-display` **glance** flavor treats as its flagship. True
black costs no power on AMOLED, which is exactly what a witness display that
is mostly dark wants to be.

- [`canary-display`](../../projects/canary-display/README.md) — the display
  app; this board carries the `canary-display-amoled241` env (the nightstand
  portrait app — portrait face + witness column + honest night — with the
  full tap/long-press ladder by finger and the deep-black AMOLED look).

## Hardware Specifications

- **MCU**: ESP32-S3R8 (Xtensa dual-core LX7, 240 MHz), Wi-Fi + BLE 5
- **Flash**: 16 MB QIO · **PSRAM**: 8 MB octal (GPIO33–37 reserved)
- **Display**: 2.41" **450 × 600 AMOLED**, RM690B0 driver, 4-lane QSPI,
  16-px column window offset. **No backlight pin** — brightness is panel
  command `0x51`.
- **Touch**: **FT6336** @ 0x38 (up to 5 points). INT on GPIO3, reset on
  TCA9554 EXIO1. The HAL polls it.
- **IMU**: QMI8658 6-axis @ 0x6B — orientation, wake-on-motion
- **RTC**: PCF85063 @ 0x51 — the clock survives with the radio down
- **IO expander**: TCA9554 @ 0x20, INT to GPIO18. EXIO0 = panel reset
  (pulled down), EXIO1 = touch reset, EXIO2 = RTC INT, EXIO3/4 = IMU INT2/1,
  EXIO5–7 to the header
- **Power**: ETA6098 Li-po charger (autonomous — **no I2C PMU**), battery
  divider on GPIO17, soft power latch + panel power on GPIO16, PWR case
  button readable on GPIO15
- **microSD**: SDMMC 1-bit (CLK 4 / CMD 5 / D0 6 / D3 2)
- **Buttons**: RST (CHIP_PU), BOOT (GPIO0), PWR (GPIO15)
- **Breakout**: 34-pin header (free: GPIO1, 7, 8, 18, 40–42, 45–46), UART
  connector (TX 43 / RX 44), I2C connector (the shared SDA 47 / SCL 48 bus)

## Pin map provenance

Taken from **Waveshare's own V2 tree**,
[`waveshareteam/ESP32-S3-Touch-AMOLED-2.41-V2`](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.41-V2):
the V2.0 schematic (`04_schematic/`) and the three display examples built
on it (Arduino `09_LVGL_Test`, ESP-IDF `09_LVGL_V9_Test` and
`10_FactoryProgram`, which share one init table, a 40 MHz QSPI clock and
the 16-px CASET offset). Cross-checked against the **V2 case label**
(UART TX43/RX44 · I2C SDA47/SCL48 · Touch FT6336 · Display RM690B0 ·
PMIC ETA6098 · IO expander TCA9554 · 16 MB flash · 8 MB PSRAM).

The map was first transcribed from the CircuitPython board definition
(`adafruit/circuitpython` → `waveshare_esp32_s3_amoled_241`). That
describes an **earlier revision**: panel reset on GPIO21, touch reset on
GPIO3, touch INT on EXIO2. Firmware up to 2.4.18 used it, and a V2 board
stayed black (thing 1 below).

This is *source* verification, not our bench validation. The tier is
`compile-tested` until a hardware test report promotes it.

## Seven things that will bite you

### 1. The panel reset is an expander line, and it is pulled down

On V2, OLED_RESET runs from TCA9554 **EXIO0**, with a 10 kΩ pull-down to
ground (R10). The expander powers up with every line an input, so the
panel stays in reset until firmware makes EXIO0 an output and drives it
high. QSPI writes carry no acknowledgement, so a panel held in reset gives
a perfect serial log and a black glass. `display_init` releases EXIO0 and
EXIO1 (touch reset) the way Waveshare's examples do, then reads EXIO0 back
and prints `first light 2/5: panel reset released`. There is no reset
GPIO: **GPIO21 is the panel's TE output and GPIO3 the touch INT**, both
inputs. Never drive them.

### 2. GPIO16 is the power latch AND the panel power

On battery, drive GPIO16 **HIGH early in boot** and keep it high — it holds
the board's power rail *and* the AMOLED's supply. The vendor's examples
raise it (`BAT_Control`) before touching the panel, and the HAL waits
~200 ms for the rail to settle. On USB power a forgotten latch merely
darkens the panel; on battery it is power-off.

### 3. Touch is polled, and its reset is on the expander

The FT6336's INT is on **GPIO3** and its reset on **EXIO1** (pulled up).
The touch driver polls the controller over I2C, which needs neither line.
`TOUCH_PIN_RST` is `-1` because there is no reset GPIO.

### 4. The panel window starts 16 columns in

The 450-px axis maps to controller columns **16..465**. Miss the offset and
the face is shifted with a 16-px garbage stripe. `LCD_COL_OFFSET` carries
it; Waveshare's V2 init (`CASET 0x10..0x1D1`) is the receipt.

### 5. The charger has no bus

The ETA6098 charges autonomously — there is no PMU to query (`HAS_PMU 0`).
Battery telemetry is the GPIO17 divider only, and the divider ratio is
**unverified** (siblings use VBAT = VADC × 3). Calibrate before showing a
battery percentage anyone might trust. The PWR button (GPIO15) *is*
firmware-readable, unlike the Touch-LCD-1.69's latch-only button.

### 6. The stock Arduino_GFX RM690B0 init is the WRONG panel personality

`Arduino_RM690B0`'s built-in table is the LilyGO T4-S3's: its `0x5A`/`0x5B`
writes ("SWIRE FOR BV6804") program that module's AMOLED **power chip**,
values Waveshare's init for this board never sends. The HAL
(`display_amoled241.cpp`) overrides `tftInit()` to play Waveshare's V2
sequence byte for byte instead. Don't "simplify" it back to the stock
`begin()`. `scripts/tests/test_amoled241_v2_wiring.py` holds the table, the
reset order and the pin map to the vendor's tree.

### 7. Nobody has told us which level the PWR key rests at

GPIO15 ("KEY_BAT") reaches the S3 through the soft power latch, and no
source we can read says whether it rests HIGH (a key isolated from the
latch, under the pull-up) or LOW (a sense line taken off the latched node).
The old glue assumed active-low; on a board that rests LOW it read "PWR
held" from the first loop pass and powered a working glass off three
seconds later, then hung on USB waiting for a release. The firmware now
learns the resting level after boot (`canary/io/pwr_key.h`, host-tested)
and prints it: look for `PWR key rests HIGH` or `rests LOW` in the serial
log, and put that line in the hardware test report that promotes this
board.

## Antenna note

The CircuitPython port derates default Wi-Fi TX power to 15 dBm for this
board's antenna design. Worth honoring: full-power TX on a compromised
antenna wastes battery for no link budget.
