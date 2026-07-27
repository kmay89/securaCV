/**
 * @file pins.h
 * @brief Pin definitions for the Waveshare ESP32-S3-Touch-LCD-4.3B
 *        ("Canary Dash B" / dev playground host — the industrial-IO SKU)
 *
 * Same ESP32-S3 + 4.3" 800x480 RGB565 panel + GT911 touch + CH422G
 * expander as the plain 4.3 (waveshare-esp32s3-lcd43), plus the Type B's
 * field-wiring terminal block: 2x isolated digital inputs (DI0/DI1,
 * 5-36 V, dry/wet contact), 2x isolated open-drain digital outputs
 * (DO0/DO1, up to 450 mA sink), RS485, CAN (dedicated TJA-class
 * transceiver — NOT USB-muxed like the plain 4.3), an I2C sensor header
 * (VOUT/GND/SDA/SCL), and a 6-36 V wide-input supply. No raw ESP32 GPIO
 * is broken out on this SKU — every external wire lands on an isolated,
 * buffered, or bused interface, which is exactly what makes it the safe
 * host for the dev playground (docs/hardware/dev_playground_43b.md).
 *
 * Pin assignments per Waveshare's wiki/demo sources for the 4.3B
 * (https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B).
 *
 * ⚠️ DEV STATUS: compile-verified only — NOT yet validated on bench
 *    hardware. Waveshare does not publish a full electrical drawing;
 *    verify RGB timings, expander bits, and RS485/DO polarity against
 *    your board revision before relying on them.
 *
 * DIFFERENCES vs the plain 4.3 map (waveshare-esp32s3-lcd43):
 *   - CAN moved to dedicated GPIO15/16 + transceiver (the 4.3 muxes CAN
 *     onto the USB D+/D- pair via FSUSB42UMX; the B does not, so there is
 *     no CH422G USB_SEL bit here).
 *   - RS485 rides GPIO43/44 — the SAME wires as the CH343 USB-UART
 *     console. Console logging and RS485 are mutually exclusive in use.
 *   - CH422G bit map differs: EXIO0/EXIO5 are the isolated inputs,
 *     EXIO3 is LCD_RST, SD_CS moved to EXIO4; DO0/DO1 are the OD0/OD1
 *     open-drain latch bits (separate write address).
 *   - GPIO6 (the plain 4.3's optional piezo pad) is not exposed on the B.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_WAVESHARE_ESP32S3_LCD43B
#define BOARD_WAVESHARE_ESP32S3_LCD43B 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Waveshare ESP32-S3-Touch-LCD-4.3B"
#define BOARD_ID                "waveshare-esp32s3-lcd43b"
#define BOARD_VENDOR            "Waveshare"
#define BOARD_MCU               "ESP32-S3"
#define BOARD_VARIANT           "esp32s3-touch-lcd-4.3b"

// ============================================================================
// LCD — 4.3" 800x480 IPS, parallel RGB565 (ST7262-class panel)
// ============================================================================
//
// Identical GPIO set to the plain 4.3 (Waveshare kept the panel nets);
// the repo's R3..R7/G2..G7/B3..B7 naming maps the wiki's R0..R4/G0..G5/
// B0..B4 labels onto the RGB565 bit positions the S3 peripheral drives.

#define LCD_WIDTH               800
#define LCD_HEIGHT              480

#define LCD_PIN_DE              5
#define LCD_PIN_VSYNC           3
#define LCD_PIN_HSYNC           46
#define LCD_PIN_PCLK            7

// RGB565 data lines (5-6-5): R3..R7, G2..G7, B3..B7.
#define LCD_PIN_R3              1
#define LCD_PIN_R4              2
#define LCD_PIN_R5              42
#define LCD_PIN_R6              41
#define LCD_PIN_R7              40

#define LCD_PIN_G2              39
#define LCD_PIN_G3              0
#define LCD_PIN_G4              45
#define LCD_PIN_G5              48
#define LCD_PIN_G6              47
#define LCD_PIN_G7              21

#define LCD_PIN_B3              14
#define LCD_PIN_B4              38
#define LCD_PIN_B5              18
#define LCD_PIN_B6              17
#define LCD_PIN_B7              10

// Panel timings (starting values from Waveshare's demo config — VERIFY).
#define LCD_PCLK_HZ             16000000
#define LCD_HSYNC_PULSE         4
#define LCD_HSYNC_BACK_PORCH    8
#define LCD_HSYNC_FRONT_PORCH   8
#define LCD_VSYNC_PULSE         4
#define LCD_VSYNC_BACK_PORCH    16
#define LCD_VSYNC_FRONT_PORCH   16

// ============================================================================
// TOUCH — GT911 5-point capacitive, I2C
// ============================================================================

#define I2C_PIN_SDA             8
#define I2C_PIN_SCL             9
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

#define TOUCH_I2C_ADDR          0x5D  // GT911 primary (0x14 alternate)
#define TOUCH_I2C_ADDR_ALT      0x14
#define TOUCH_PIN_INT           4
#define TOUCH_PIN_RST           -1    // via CH422G EXIO1, not a native GPIO

// ============================================================================
// CH422G — I2C IO expander (fixed command addresses, shared 8/9 bus)
// ============================================================================
//
// The expander owns the panel-adjacent control lines AND the Type B's
// isolated digital IO. Bits per Waveshare's 4.3B demo sources (VERIFY on
// your revision):
//   EXIO0 = DI0 (isolated input),  EXIO1 = touch reset,
//   EXIO2 = DISP/backlight enable, EXIO3 = LCD reset,
//   EXIO4 = SD_CS,                 EXIO5 = DI1 (isolated input),
//   OD0   = DO0 (isolated output), OD1   = DO1 (isolated output).
//
// The CH422G is command-addressed: each function is a fixed I2C address,
// so 0x23/0x24/0x26/0x38 are ALL burned by the expander — external I2C
// peripherals on the sensor header must avoid them (notably BH1750's
// default 0x23 and AHT20's 0x38; see the board README's address table).
//
// EXIO0..7 share ONE direction (WR_SET bit 0): reading DI0/DI1 means a
// brief all-inputs window (mode 0x00 -> read 0x26 -> mode 0x01 -> rewrite
// latch). The control lines ride external pulls through the blip —
// VERIFY no backlight flicker at the bench poll rate.
//
// NOTE: backlight through the expander is ON/OFF only — there is no PWM
// dimming path on this board. Night-mode UX must come from dark rendering
// plus scheduled backlight-off (see the display UX doc).

#define CH422G_ADDR_SYS         0x24  // WR_SET: mode register
#define CH422G_ADDR_OUT         0x38  // WR_IO: output latch (EXIO0..7)
#define CH422G_ADDR_OC          0x23  // WR_OC: open-drain latch (OD0..3)
#define CH422G_ADDR_IN          0x26  // RD_IO: input read (EXIO0..7)

#define CH422G_BIT_TOUCH_RST    (1 << 1)
#define CH422G_BIT_BACKLIGHT    (1 << 2)
#define CH422G_BIT_LCD_RST      (1 << 3)
#define CH422G_BIT_SD_CS        (1 << 4)

// Isolated inputs (read via CH422G_ADDR_IN; 5-36 V dry/wet, NPN or PNP,
// referenced to the DI COM terminal — never to board GND).
#define ISO_IN_BIT_DI0          (1 << 0)   // EXIO0
#define ISO_IN_BIT_DI1          (1 << 5)   // EXIO5

// Isolated open-drain outputs (write via CH422G_ADDR_OC; optocoupled,
// up to 450 mA sink per channel, external supply required).
// Latch polarity per Waveshare demo: bit HIGH releases the output,
// bit LOW conducts (sinks). VERIFY at bench.
#define ISO_OUT_BIT_DO0         (1 << 0)   // OD0
#define ISO_OUT_BIT_DO1         (1 << 1)   // OD1

// ============================================================================
// RS485 — terminal block A/B, auto direction control
// ============================================================================
//
// ⚠️ Shares GPIO43/44 with the CH343 USB-UART console (flash/log). Using
// RS485 means the console UART carries bus traffic: keep logging on the
// native USB CDC, and expect programming over UART to conflict with a
// live bus. TX/RX orientation per the wiki demo — VERIFY at bench.

#define RS485_PIN_TX            44
#define RS485_PIN_RX            43
#define RS485_BAUD_DEFAULT      9600

// ============================================================================
// CAN (TWAI) — terminal block H/L, dedicated transceiver
// ============================================================================
//
// Unlike the plain 4.3 there is no USB mux: CAN and native USB coexist.
// 120R terminator is jumper-selectable on-board (OFF by default).

#define CAN_PIN_TX              15
#define CAN_PIN_RX              16
// Recommended default bit rate (bit/s). 500k is the common industrial/vehicle
// rate; the TWAI driver also presets 125k/250k/1M. VERIFY against your bus.
#define CAN_BITRATE_DEFAULT     500000

// ============================================================================
// I2C SENSOR HEADER — VOUT / GND / SDA / SCL on the terminal block
// ============================================================================
//
// Same physical bus as the GT911 + CH422G (GPIO8/9). VOUT is 5 V or 3.3 V
// selected by an onboard resistor option (factory default 5 V — VERIFY
// your unit before wiring 3.3 V-only sensors to VOUT).
// Reserved addresses on this bus: 0x23/0x24/0x26/0x38 (CH422G),
// 0x5D/0x14 (GT911). See the board README for the safe-sensor table.

#define I2C_HEADER_PIN_SDA      I2C_PIN_SDA
#define I2C_HEADER_PIN_SCL      I2C_PIN_SCL

// ============================================================================
// USB / UART / SD
// ============================================================================

// Native USB-C (CDC console, flashing). Dedicated on the B — no CAN mux.
#define USB_PIN_DN              19
#define USB_PIN_DP              20

// CH343 USB-UART (second Type-C): UART0 on GPIO43/44 — same wires as
// RS485 above.

// microSD (TF) slot — SPI wiring per Waveshare demo sources, CS on the
// expander. Unused by firmware v0.1 (VERIFY pins before enabling).
#define SD_PIN_MOSI             11
#define SD_PIN_SCK              12
#define SD_PIN_MISO             13
#define SD_PIN_CS               -1    // via CH422G EXIO4, not a native GPIO

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

// No runtime button: GPIO0 is claimed by LCD_PIN_G3; BOOT rides the strap
// circuit only.
#define BOOT_BUTTON_PIN         -1
#define BOOT_BUTTON_ACTIVE      LOW

// No spare GPIO is exposed on the B (GPIO6, the plain 4.3's optional
// piezo pad, is not routed to anything reachable) — audible feedback for
// playground tests goes through DO0/DO1 driving an external buzzer.
#define BUZZER_PIN              -1

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

#define HAS_CAMERA              0     // No camera — this Canary shows, it doesn't watch
#define HAS_MICROPHONE          0     // No microphone — quiet-room-safe by construction
#define HAS_SD_CARD             1     // TF slot (unused in v0.1)
#define HAS_PSRAM               1     // 8 MB octal — hosts the 800x480 framebuffer
#define HAS_USB_CDC             1
#define HAS_WIFI                1
#define HAS_BLE                 1     // future: passive Chirp scan fallback
#define HAS_DISPLAY             1     // 800x480 RGB565 parallel
#define HAS_TOUCH               1     // GT911 5-point
#define HAS_RTC                 0
#define HAS_BATTERY             0     // 6-36 V DC / USB-C powered
#define HAS_BACKLIGHT_PWM       0     // CH422G on/off only — see note above
#define HAS_CAN_RS485           1     // terminal block, both populated
#define HAS_ISOLATED_IO         1     // DI0/DI1 in + DO0/DO1 out (CH422G)
#define HAS_I2C_HEADER          1     // VOUT/GND/SDA/SCL terminal (shared bus)
