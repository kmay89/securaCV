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
- **Touch**: **FT6336** @ 0x38 (up to 5 points). Its INT line rides the
  TCA9554's EXIO2, not an S3 GPIO — poll it.
- **IMU**: QMI8658 6-axis @ 0x6B — orientation, wake-on-motion
- **RTC**: PCF85063 @ 0x51 — the clock survives with the radio down
- **IO expander**: TCA9554 @ 0x20 (EXIO2 = touch INT; other lines
  unverified)
- **Power**: ETA6098 Li-po charger (autonomous — **no I2C PMU**), battery
  divider on GPIO17, soft power latch + panel power on GPIO16, PWR case
  button readable on GPIO15
- **microSD**: SDMMC 1-bit (CLK 4 / CMD 5 / D0 6 / D3 2)
- **Buttons**: RST (CHIP_PU), BOOT (GPIO0), PWR (GPIO15)
- **Breakout**: 34-pin header (free: GPIO1, 7, 8, 18, 40–42, 45–46), UART
  connector (TX 43 / RX 44), I2C connector (the shared SDA 47 / SCL 48 bus)

## Pin map provenance

Transcribed from the **CircuitPython board definition**
(`adafruit/circuitpython` → `ports/espressif/boards/waveshare_esp32_s3_amoled_241`,
bench-tested by its author against this panel: the RM690B0 init sequence
there is the source for the 16-px CASET offset, the GPIO16 power-latch note
and the 40 MHz QSPI clock), cross-checked against the **V2 case label**
(UART TX43/RX44 · I2C SDA47/SCL48 · Touch FT6336 · Display RM690B0 ·
PMIC ETA6098 · IO expander TCA9554 · 16 MB flash · 8 MB PSRAM). Waveshare
ships the 2.41 demo tree as a wiki zip rather than a vendor GitHub repo, so
the CircuitPython port is the best code-level authority available.

This is *source* verification, not our bench validation. The tier is
`compile-tested` until a hardware test report promotes it.

## Four things that will bite you

### 1. GPIO16 is the power latch AND the panel power

On battery, drive GPIO16 **HIGH early in boot** and keep it high — it holds
the board's power rail *and* the AMOLED's supply. Both the vendor boot code
and the CircuitPython port raise it (then wait ~200 ms for the rail to
settle) before touching the panel. On USB power a forgotten latch merely
darkens the panel; on battery it is power-off.

### 2. The touch interrupt is behind the IO expander

The FT6336's INT routes to TCA9554 **EXIO2**. There is no S3 GPIO to attach
an interrupt to, so the touch driver polls the controller over I2C — which
the FT6336 answers happily. `TOUCH_PIN_INT` is `-1` on purpose; do not
"fix" it by picking a GPIO.

### 3. The panel window starts 16 columns in

The 450-px axis maps to controller columns **16..465**. Miss the offset and
the face is shifted with a 16-px garbage stripe. `LCD_COL_OFFSET` carries
it; the bench-tested CircuitPython init (`CASET 0x10..0x1D1`) is the
receipt.

### 4. The charger has no bus

The ETA6098 charges autonomously — there is no PMU to query (`HAS_PMU 0`).
Battery telemetry is the GPIO17 divider only, and the divider ratio is
**unverified** (siblings use VBAT = VADC × 3). Calibrate before showing a
battery percentage anyone might trust. The PWR button (GPIO15) *is*
firmware-readable, unlike the Touch-LCD-1.69's latch-only button.

## Antenna note

The CircuitPython port derates default Wi-Fi TX power to 15 dBm for this
board's antenna design. Worth honoring: full-power TX on a compromised
antenna wastes battery for no link budget.
