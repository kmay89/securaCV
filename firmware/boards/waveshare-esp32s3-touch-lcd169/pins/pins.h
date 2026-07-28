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
 * ⚠️ DEV STATUS: pin map compiled from the vendor demo-code layout — the
 *    wiki was unreachable from the CI sandbox (network policy), so every
 *    assignment below is marked by confidence: the LCD/touch/I2C block is
 *    the widely-mirrored demo map (verify once against the schematic); the
 *    battery/PWR lines are lower confidence and partly -1. Bench-check
 *    before first flash; correct against the schematic PDF, not by trial.
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
// Vendor demo map — VERIFY against the schematic before first flash.
// ============================================================================

#define LCD_WIDTH               240
#define LCD_HEIGHT              280
#define TFT_WIDTH               240
#define TFT_HEIGHT              280

#define TFT_PIN_SCK             6     // VERIFY (demo map)
#define TFT_PIN_MOSI            7     // VERIFY (demo map)
#define TFT_PIN_MISO            -1    // ST7789 is write-only; no TF slot
#define TFT_PIN_CS              5     // VERIFY (demo map)
#define TFT_PIN_DC              4     // VERIFY (demo map)
#define TFT_PIN_RST             8     // VERIFY (demo map)
#define TFT_PIN_BL              15    // VERIFY (demo map); LEDC PWM
#define TFT_BL_ACTIVE_HIGH      1     // VERIFY polarity on the bench
#define TFT_SPI_HZ              40000000

// ST7789V2 240x280 window offset (240x320 controller RAM) at rotation 0.
#define TFT_COL_OFFSET          0
#define TFT_ROW_OFFSET          20

// ============================================================================
// TOUCH — CST816T capacitive, shared I2C bus
// ============================================================================

#define TOUCH_I2C_ADDR          0x15  // CST816-family default
#define TOUCH_PIN_INT           14    // VERIFY (demo map); polled, INT optional
#define TOUCH_PIN_RST           13    // VERIFY (demo map); pulse low at init

// ============================================================================
// I2C — CST816T touch + QMI8658 IMU + PCF85063 RTC share the bus
// ============================================================================

#define I2C_PIN_SDA             11    // VERIFY (demo map)
#define I2C_PIN_SCL             10    // VERIFY (demo map)
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

#define IMU_PIN_INT1            -1    // VERIFY (QMI8658 INT, if wired)
#define RTC_PIN_INT             9     // VERIFY (PCF85063 INT per demo map)

// ============================================================================
// POWER — 3.7 V MX1.25 battery, charge/discharge managed onboard
// ============================================================================

#define BAT_ADC_PIN             1     // VERIFY (battery divider on ADC1_CH0)
#define PWR_KEY_PIN             -1    // VERIFY (PWR button / power latch line)

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

#define BOOT_BUTTON_PIN         0     // BOOT/user (strapping pin — chip fact)
#define BOOT_BUTTON_ACTIVE      LOW

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
