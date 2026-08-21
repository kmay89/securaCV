/**
 * @file pins.h
 * @brief Pin definitions for the Waveshare ESP32-S3-Touch-AMOLED-2.41 (V2)
 *        ("Canary Glance" — the flagship pocket/desk AMOLED glance glass)
 *
 * Handheld-sized ESP32-S3R8 board in Waveshare's metal case: 2.41" 450x600
 * AMOLED (RM690B0) on a 4-data-lane QSPI bus, FT6336 capacitive touch,
 * QMI8658 6-axis IMU, PCF85063 RTC and a TCA9554 IO expander on one shared
 * I2C bus, ETA6098 charger + Li-po with a soft power latch, SDMMC microSD,
 * three case buttons (RST / BOOT / PWR) and a 34-pin GPIO header.
 *
 * PIN MAP SOURCE: the CircuitPython board definition
 *    (adafruit/circuitpython, ports/espressif/boards/
 *    waveshare_esp32_s3_amoled_241 — bench-tested by its author against this
 *    panel), cross-checked against the V2 case label (UART TX43/RX44, I2C
 *    SDA47/SCL48, FT6336 touch, RM690B0 panel, ETA6098, TCA9554, 16 MB
 *    flash / 8 MB PSRAM). Waveshare distributes the 2.41 demo tree as a wiki
 *    zip, not a vendor GitHub repo, so CircuitPython is the best code-level
 *    authority available.
 *
 * ⚠️ DEV STATUS: transcribed from those sources — NOT yet validated on OUR
 *    bench. Verify against your board revision before trusting a pin.
 *
 * ⚠️ PANEL WINDOW OFFSET: the 450-px axis sits 16 columns into the RM690B0's
 *    RAM — every address window needs a +16 column offset at rotation 0
 *    (LCD_COL_OFFSET below; the CircuitPython init writes CASET 16..465).
 *
 * ⚠️ TOUCH INT IS BEHIND THE EXPANDER. The FT6336's interrupt line routes to
 *    TCA9554 EXIO2, not to an ESP32-S3 GPIO — so there is no touch interrupt
 *    to attach. Poll the controller (it answers over I2C whether or not INT
 *    is watched); TOUCH_PIN_INT is -1 on purpose.
 *
 * ⚠️ GPIO16 IS THE POWER LATCH *AND* THE PANEL POWER. On battery, firmware
 *    must drive GPIO16 HIGH early in boot to keep the board powered and the
 *    AMOLED rail up (the vendor's boot code and the CircuitPython port both
 *    do this before touching the panel). Dropping it on battery is power-off.
 *
 * ⚠️ NO BACKLIGHT PIN. AMOLED brightness is a panel command (0x51), not PWM.
 *
 * ⚠️ OCTAL PSRAM: GPIO33-37 belong to the in-package PSRAM — never route
 *    anything there (see firmware/boards/PIN_BUDGET.md).
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_WAVESHARE_ESP32S3_AMOLED241
#define BOARD_WAVESHARE_ESP32S3_AMOLED241 1
#endif

// Which physical board this is, in the fleet-figure vocabulary. Named here
// rather than in the build env because THIS header is what a build compiles
// against — the load-bearing declaration, so the id cannot drift away from
// the pins it travels with. canary::figures::my_figure() reads it.
#ifndef CANARY_FIGURE_HARDWARE
#define CANARY_FIGURE_HARDWARE "waveshare-esp32s3-amoled241"
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Waveshare ESP32-S3-Touch-AMOLED-2.41"
#define BOARD_ID                "waveshare-esp32s3-amoled241"
#define BOARD_VENDOR            "Waveshare"
#define BOARD_MCU               "ESP32-S3"
#define BOARD_VARIANT           "esp32s3-touch-amoled-2.41"

// ============================================================================
// AMOLED — RM690B0 450x600 portrait, QSPI (1 clock + 4 data + CS + RST)
// True black costs no power; brightness is panel command 0x51, max 0xFF.
// ============================================================================

#define LCD_WIDTH               450
#define LCD_HEIGHT              600
#define LCD_IS_AMOLED           1

// TFT_* aliases: the shared portrait UI + LVGL port size themselves off
// TFT_WIDTH/TFT_HEIGHT (the ST7789 boards' vocabulary). Same glass, both
// names — the AMOLED HAL reads LCD_*, the shared code reads TFT_*.
#define TFT_WIDTH               450
#define TFT_HEIGHT              600

#define LCD_PIN_CS              9
#define LCD_PIN_SCLK            10
#define LCD_PIN_SDIO0           11
#define LCD_PIN_SDIO1           12
#define LCD_PIN_SDIO2           13
#define LCD_PIN_SDIO3           14
#define LCD_PIN_RST             21

// The 450-px axis is a window 16 columns into the controller RAM (CASET
// 16..465 per the bench-tested CircuitPython init). Rows start at 0.
#define LCD_COL_OFFSET          16
#define LCD_ROW_OFFSET          0

// 40 MHz is the bench-tested QSPI clock (CircuitPython). The controller is
// rated faster; treat any bump as a bench experiment, not a default.
#define LCD_QSPI_HZ             40000000

// AMOLED has no backlight — brightness is a panel command, not a PWM pin.
#define LCD_PIN_BL              -1
#define LCD_BRIGHTNESS_IS_CMD   1

// ============================================================================
// I2C — one shared bus: touch, IMU, RTC, IO expander (also on the side
// header, so external add-ons must avoid these four addresses).
// ============================================================================

#define I2C_PIN_SDA             47
#define I2C_PIN_SCL             48
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

#define TOUCH_I2C_ADDR          0x38  // FT6336 (FocalTech FT6x36 protocol)
#define TOUCH_PIN_RST           3     // pulse low at init
#define TOUCH_PIN_INT           -1    // routed to TCA9554 EXIO2 — poll, don't wait

#define IMU_I2C_ADDR            0x6B  // QMI8658 (probe 0x6A too; strap varies)
#define RTC_I2C_ADDR            0x51  // PCF85063
#define EXIO_I2C_ADDR           0x20  // TCA9554 IO expander (EXIO2 = touch INT;
                                      // remaining lines unverified — bench work)

// ============================================================================
// POWER — ETA6098 Li-po charger (autonomous, no I2C) + soft power latch.
// The PWR case button is a readable GPIO here (unlike the 1.69's latch-only
// button): short-press events belong to firmware, long-press behavior rides
// the latch. VBAT reaches an ADC pin through a divider.
// ============================================================================

#define PWR_LATCH_PIN           16    // BAT_CONTROL / panel power — drive HIGH
                                      // early in boot, keep HIGH while running
#define PWR_KEY_PIN             15    // KEY_BAT — the case PWR button (input)
#define BAT_ADC_PIN             17    // battery divider; VERIFY ratio on the
                                      // bench before trusting a voltage (the
                                      // sibling boards use VBAT = VADC * 3)

// ============================================================================
// microSD — SDMMC 1-bit (D3 held high keeps the card in SD mode)
// ============================================================================

#define SD_PIN_CLK              4
#define SD_PIN_CMD              5
#define SD_PIN_D0               6
#define SD_PIN_D3               2

// SPI-era aliases the SD deep archive mounts through (fleet/sd_archive.cpp
// routes SDMMC 1-bit via SD_MMC.setPins(SCK, MOSI, MISO); the electrical
// mapping is CLK=SCK, CMD=MOSI, D0=MISO). Unlike the dash family's slot,
// DAT3 here is a native GPIO — no expander latch stands between the card
// and SD mode.
#define SD_PIN_SCK              SD_PIN_CLK
#define SD_PIN_MOSI             SD_PIN_CMD
#define SD_PIN_MISO             SD_PIN_D0

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

#define BOOT_BUTTON_PIN         0     // BOOT case button (strapping pin — chip fact)
#define BOOT_BUTTON_ACTIVE      LOW
// RST case button is CHIP_PU (hardware reset line) — not a readable GPIO.

// No piezo pad on this SKU — the chime engine no-ops on chime_init(-1)
// (the same arrangement the pre-buzzer Touch-1.69 map used), which keeps
// the emulator build (FEATURE_CHIME forced on there) compiling honestly.
#define BUZZER_PIN              -1

// Native USB (S3 USB-OTG on the Type-C) — chip-fixed, not for general GPIO.
#define USB_PIN_DM              19    // USB_N (D-)
#define USB_PIN_DP              20    // USB_P (D+)

// UART side connector (U0 console pair, also the flash UART).
#define UART_PIN_TX             43
#define UART_PIN_RX             44

// 34-pin header pads free for user I/O, left undeclared so the pin budget
// keeps them in their true buckets: GPIO1, 7, 8, 18, 40, 41, 42, 45, 46.

// ============================================================================
// BOARD CAPABILITIES
//
// These describe what the FIRMWARE MAY TOUCH, which is not always what is
// soldered down. No camera, no microphone — this Canary shows, it doesn't
// watch or listen, by construction.
// ============================================================================

#define HAS_CAMERA              0     // No camera — this Canary shows, it doesn't watch
#define HAS_MICROPHONE          0     // No microphone — quiet-room-safe by construction
#define HAS_SD_CARD             1     // SDMMC 1-bit microSD slot
#define HAS_PSRAM               1     // 8 MB octal (GPIO33-37 reserved)
#define HAS_USB_CDC             1
#define HAS_NATIVE_USB          1     // S3 USB-OTG on the Type-C
#define HAS_WIFI                1     // 2.4 GHz — antenna prefers TX power
                                      // capped ~15 dBm (CircuitPython derates
                                      // for this board's antenna design)
#define HAS_BLE                 1
#define HAS_THREAD_ZIGBEE       0

#define HAS_DISPLAY             1     // 450x600 RM690B0 QSPI AMOLED
#define HAS_AMOLED              1
#define HAS_TOUCH               1     // FT6336 — the input surface
#define HAS_RGBLED              0     // no addressable LED; the glass is the beacon
#define HAS_RTC                 1     // PCF85063
#define HAS_IMU                 1     // QMI8658 6-axis
#define HAS_IO_EXPANDER         1     // TCA9554 @ 0x20
#define HAS_BATTERY             1     // Li-po + ETA6098 charger (no I2C gauge)
#define HAS_PMU                 0     // ETA6098 is autonomous — no PMU bus to talk to
#define HAS_BACKLIGHT_PWM       0     // AMOLED: brightness is a panel command
#define HAS_BUZZER              0     // no piezo on this SKU — chime is visual
