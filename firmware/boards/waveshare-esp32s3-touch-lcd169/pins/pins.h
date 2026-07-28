/**
 * @file pins.h
 * @brief Pin definitions for the Waveshare ESP32-S3-Touch-LCD-1.69
 *        ("Canary Nightstand Touch" — battery-capable 1.69" touch glance node)
 *
 * Integrated board: ESP32-S3R8 (dual-core LX7 240 MHz, 16 MB flash, 8 MB
 * OCTAL PSRAM) driving a 1.69" 240x280 ST7789V2 IPS panel over 4-wire SPI,
 * with a CST816T capacitive touch panel, QMI8658 6-axis IMU and PCF85063
 * RTC on a shared I2C bus, a 3.7 V MX1.25 lithium battery interface with
 * onboard charging, and a PWR button. Vendor doc:
 * https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.69 (schematic:
 * files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.69/ESP32-S3-Touch-LCD-1.69-Sch.pdf)
 *
 * PIN MAP SOURCE: reconciled against the vendor GPIO / peripheral pinout
 *    table (docs.waveshare.com/ESP32-S3-Touch-LCD-1.69, 2026-07). The demo
 *    map's LCD/touch/I2C block was correct; the table additionally resolves
 *    what had been stubbed: RTC_INT was WRONG in the demo map (was GPIO9 —
 *    the table says GPIO39), the "-1" power lines are SYS_OUT=GPIO40 /
 *    SYS_EN=GPIO41 (soft power latch), and it adds the buzzer (GPIO42),
 *    the QMI8658 interrupt (GPIO38) and the I2C device addresses. Only the
 *    electrical details a pin table can't give — backlight polarity, INT
 *    edge — still want a bench check; the schematic PDF remains the ultimate
 *    authority.
 *
 * ⚠️ ST7789V2 OFFSET: the 240x280 glass is a window into the controller's
 *    240x320 RAM — every draw needs a 20-px ROW offset at rotation 0
 *    (TFT_ROW_OFFSET), migrating on rotation. (The 1.47" siblings have the
 *    same disease on the other axis: a 34-px column offset.)
 *
 * ⚠️ OCTAL PSRAM: GPIO33-37 belong to the in-package PSRAM — never route
 *    anything there (see firmware/boards/PIN_BUDGET.md).
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_WAVESHARE_ESP32S3_TOUCH_LCD169
#define BOARD_WAVESHARE_ESP32S3_TOUCH_LCD169 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Waveshare ESP32-S3-Touch-LCD-1.69"
#define BOARD_ID                "waveshare-esp32s3-touch-lcd169"
#define BOARD_VENDOR            "Waveshare"
#define BOARD_MCU               "ESP32-S3"
#define BOARD_VARIANT           "esp32s3-touch-lcd-1.69"

// ============================================================================
// TFT — ST7789V2 1.69" 240x280 portrait, 4-wire SPI (RGB565)
// Per vendor pinout table (LCD_CLK/DIN/CS/DC/RST/BL).
// ============================================================================

#define LCD_WIDTH               240
#define LCD_HEIGHT              280
#define TFT_WIDTH               240
#define TFT_HEIGHT              280

#define TFT_PIN_SCK             6     // LCD_CLK
#define TFT_PIN_MOSI            7     // LCD_DIN (write only; LCD_DOUT unused)
#define TFT_PIN_MISO            -1    // ST7789 is write-only; no TF slot
#define TFT_PIN_CS              5     // LCD_CS
#define TFT_PIN_DC              4     // LCD_DC
#define TFT_PIN_RST             8     // LCD_RST
#define TFT_PIN_BL              15    // LCD_BL; LEDC PWM
#define TFT_BL_ACTIVE_HIGH      1     // VERIFY polarity on the bench
#define TFT_SPI_HZ              40000000

// ST7789V2 240x280 window offset (240x320 controller RAM) at rotation 0.
#define TFT_COL_OFFSET          0
#define TFT_ROW_OFFSET          20

// ============================================================================
// TOUCH — CST816T capacitive, shared I2C bus
// ============================================================================

#define TOUCH_I2C_ADDR          0x15  // CST816T (TP_INT/TP_RST wired)
#define TOUCH_PIN_INT           14    // TP_INT
#define TOUCH_PIN_RST           13    // TP_RST; pulse low at init

// ============================================================================
// I2C — CST816T touch (0x15) + QMI8658C IMU (0x6B) + PCF85063 RTC (0x51)
// share the bus. Avoid external devices at these three addresses.
// ============================================================================

#define I2C_PIN_SDA             11    // ESP32_SDA (shared, also on ext port)
#define I2C_PIN_SCL             10    // ESP32_SCL (shared, also on ext port)
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

#define IMU_I2C_ADDR            0x6B  // QMI8658C
#define IMU_PIN_INT1            38    // QMI interrupt (vendor tables label it
                                      // INT1 in the device summary / INT2 in
                                      // the GPIO list — same pin; VERIFY which)
#define RTC_I2C_ADDR            0x51  // PCF85063ATL
#define RTC_PIN_INT             39    // RTC_INT (corrected: demo map had 9)

// ============================================================================
// POWER — 3.7 V MX1.25 battery, charge/discharge managed onboard.
// The PWR button (Key2) drives a soft power-latch network, not a plain
// readable GPIO: SYS_EN holds the rail on, SYS_OUT is the control/sense side.
// ============================================================================

#define BAT_ADC_PIN             1     // BAT_ADC divider; VBAT = VADC * 3
#define SYS_PWR_EN_PIN          41    // SYS_EN — hold system power on
#define SYS_PWR_OUT_PIN         40    // SYS_OUT — power control/sense
#define PWR_KEY_PIN             -1    // no direct GPIO — PWR/Key2 acts through
                                      // the SYS_EN/SYS_OUT latch above

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

#define BOOT_BUTTON_PIN         0     // BOOT/Key1 (strapping pin — chip fact)
#define BOOT_BUTTON_ACTIVE      LOW
// RST/Key3 is CHIP_PU (hardware reset line) — not a readable GPIO.

#define BUZZER_PIN              42    // Buzz — passive buzzer, LEDC PWM / tone

// Native USB (S3 USB-OTG on the Type-C) — chip-fixed, not for general GPIO.
#define USB_PIN_DM              19    // USB_N (D-)
#define USB_PIN_DP              20    // USB_P (D+)

// Extension port / reserved header pads (free for user I/O), left undeclared
// so the pin budget keeps them in their true buckets:
//   GPIO2, GPIO3, GPIO17, GPIO18 — free header pads,
//   U0TXD=GPIO43 / U0RXD=GPIO44 — the UART0 console pair (conditional).

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

#define HAS_CAMERA              0     // No camera — this Canary shows, it doesn't watch
#define HAS_MICROPHONE          0     // No microphone — quiet-room-safe by construction
#define HAS_SD_CARD             0     // no TF slot on this SKU
#define HAS_PSRAM               1     // 8 MB octal (GPIO33-37 reserved)
#define HAS_USB_CDC             1
#define HAS_NATIVE_USB          1     // S3 USB-OTG on the Type-C
#define HAS_WIFI                1     // 2.4 GHz
#define HAS_BLE                 1
#define HAS_THREAD_ZIGBEE       0
#define HAS_DISPLAY             1     // 240x280 ST7789V2 SPI
#define HAS_TOUCH               1     // CST816T — the input surface
#define HAS_RGBLED              0     // no addressable LED; the glass is the beacon
#define HAS_RTC                 1     // PCF85063
#define HAS_IMU                 1     // QMI8658 6-axis (driver: room to grow)
#define HAS_BATTERY             1     // 3.7 V MX1.25 + onboard charging
#define HAS_BACKLIGHT_PWM       1     // LEDC PWM on TFT_PIN_BL
#define HAS_BUZZER              1     // passive buzzer on GPIO42 (LEDC tone)
