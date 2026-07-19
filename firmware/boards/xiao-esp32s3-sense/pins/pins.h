/**
 * @file pins.h
 * @brief Pin definitions for XIAO ESP32-S3 Sense board
 *
 * This file contains all hardware pin mappings for the Seeed Studio
 * XIAO ESP32S3 Sense board. Pin assignments are based on the official
 * Seeed documentation and hardware schematics.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 * @note Pin assignments are validated against the ESP32-S3 datasheet.
 */

#pragma once

#ifndef BOARD_XIAO_ESP32S3_SENSE
#define BOARD_XIAO_ESP32S3_SENSE 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "XIAO ESP32-S3 Sense"
#define BOARD_ID                "xiao-esp32s3-sense"
#define BOARD_VENDOR            "Seeed Studio"
#define BOARD_MCU               "ESP32-S3"
#define BOARD_VARIANT           "sense"

// ============================================================================
// GPIO EXPANSION CONNECTOR
// ============================================================================

// Digital pins (directly accessible on XIAO expansion header)
#define PIN_D0                  1
#define PIN_D1                  2
#define PIN_D2                  3
#define PIN_D3                  4
#define PIN_D4                  5
#define PIN_D5                  6
#define PIN_D6                  43    // Also TX
#define PIN_D7                  44    // Also RX
#define PIN_D8                  7
#define PIN_D9                  8
#define PIN_D10                 9

// Analog pins (directly accessible)
#define PIN_A0                  1
#define PIN_A1                  2
#define PIN_A2                  3
#define PIN_A3                  4
#define PIN_A4                  5
#define PIN_A5                  6

// ============================================================================
// CAMERA (OV2640 / OV3660, kit-dependent) - Built-in on Sense variant
//
// Seeed discontinued the OV2640 module; current Sense kits ship the OV3660
// (2048x1536), and the OV5640 is a supported drop-in. The pin map below is
// identical for all three (the firmware auto-detects the sensor PID over
// SCCB at init). See
// https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/ (camera tip box).
// ============================================================================

#define CAM_PIN_PWDN            (-1)  // Not connected (always on)
#define CAM_PIN_RESET           (-1)  // Not connected (use software reset)
#define CAM_PIN_XCLK            10    // Camera clock input
#define CAM_PIN_SIOD            40    // I2C data (SCCB)
#define CAM_PIN_SIOC            39    // I2C clock (SCCB)

// Camera parallel data bus (D7-D0)
#define CAM_PIN_D7              48
#define CAM_PIN_D6              11
#define CAM_PIN_D5              12
#define CAM_PIN_D4              14
#define CAM_PIN_D3              16
#define CAM_PIN_D2              18
#define CAM_PIN_D1              17
#define CAM_PIN_D0              15

// Camera sync signals
#define CAM_PIN_VSYNC           38    // Vertical sync
#define CAM_PIN_HREF            47    // Horizontal reference
#define CAM_PIN_PCLK            13    // Pixel clock

// Camera feature flags. CAM_MAX_* describes the OV2640 floor common to
// every shipped kit; OV3660 units reach 2048x1536 (QXGA) and OV5640 units
// 2592x1944 — resolve the real ceiling at runtime from the detected PID.
#define CAM_SUPPORTS_JPEG       1
#define CAM_MAX_WIDTH           1600
#define CAM_MAX_HEIGHT          1200
#define CAM_DEFAULT_WIDTH       640
#define CAM_DEFAULT_HEIGHT      480

// ============================================================================
// SD CARD (SPI MODE)
// ============================================================================

#define SD_PIN_CS               21    // Chip select
#define SD_PIN_SCK              7     // SPI clock (shared with D8)
#define SD_PIN_MISO             8     // SPI data in (shared with D9)
#define SD_PIN_MOSI             9     // SPI data out (shared with D10)

// SD card SPI configuration
#define SD_SPI_FREQ_FAST        4000000   // 4 MHz normal operation
#define SD_SPI_FREQ_SLOW        1000000   // 1 MHz for init/recovery

// ============================================================================
// UART (GNSS/GPS MODULE)
// ============================================================================

// Default GNSS UART pins (L76K module recommended)
#define GNSS_PIN_TX             43    // ESP32 TX -> GPS RX (D6)
#define GNSS_PIN_RX             44    // ESP32 RX <- GPS TX (D7)
#define GNSS_BAUD_DEFAULT       9600

// Alternative UART pins (if GNSS not used on primary)
#define UART1_PIN_TX            43
#define UART1_PIN_RX            44

// ============================================================================
// I2C (DEFAULT)
// ============================================================================

#define I2C_PIN_SDA             5     // D4
#define I2C_PIN_SCL             6     // D5
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

// ============================================================================
// SPI (USER)
// ============================================================================

// User SPI bus (shared with SD card by default)
#define SPI_PIN_SCK             7     // D8
#define SPI_PIN_MISO            8     // D9
#define SPI_PIN_MOSI            9     // D10

// ============================================================================
// USB (NATIVE)
// ============================================================================

#define USB_PIN_DP              20    // USB D+
#define USB_PIN_DN              19    // USB D-

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

// Built-in user LED — ACTIVE LOW per the Seeed wiki ("it will only turn on
// when the pin is set to a low level",
// https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/). The Arduino
// variant header also defines LED_BUILTIN (same pin), so only define it
// when missing.
//
// ⚠ GPIO21 is SHARED with the Sense expansion board's SD chip-select
// (SD_PIN_CS below). While the SD card is mounted, GPIO21 belongs to the
// SD driver: writing the LED asserts/deasserts the card's CS, and doing so
// mid-transaction corrupts SD I/O. Firmware must route status indication
// to EXT_LED_PIN_DEFAULT (or gate LED writes on SD idleness) whenever
// HAS_SD_CARD is in use. Note also that lighting the LED (driving LOW)
// asserts the card's CS as a side effect.
#ifndef LED_BUILTIN
#define LED_BUILTIN             21
#endif
#define LED_ACTIVE_LOW          1

// Boot button (directly connected)
#define BOOT_BUTTON_PIN         0
#define BOOT_BUTTON_ACTIVE      LOW

// ============================================================================
// MICROPHONE (PDM) - Built-in on Sense variant
// ============================================================================

#define MIC_PIN_CLK             42    // PDM clock
#define MIC_PIN_DATA            41    // PDM data

// ============================================================================
// OPTIONAL PERIPHERALS
// ============================================================================

// Tamper detection (if enclosure sensor connected)
// Connect normally-closed switch between this pin and GND
#define TAMPER_PIN_DEFAULT      4     // D3 - configure as INPUT_PULLUP
#define TAMPER_ACTIVE           LOW

// External status LED (if connected)
#define EXT_LED_PIN_DEFAULT     3     // D2
#define EXT_LED_ACTIVE          HIGH

// ============================================================================
// PIN VALIDATION
// ============================================================================

// Pins that should NOT be used (reserved for internal functions).
// GPIO26-32 carry the quad SPI flash/PSRAM bus. This module's ESP32-S3R8
// has OCTAL PSRAM, which additionally claims GPIO33-37 (SPIIO4-SPIIO7 +
// SPIDQS) — see the ESP-IDF ESP32-S3 GPIO notes. None of 26-37 are broken
// out on the XIAO header, but routing a peripheral to them in software
// (GPIO matrix / LEDC / RMT) can hang or corrupt PSRAM.
#define PIN_RESERVED_FLASH_0    26    // Flash/PSRAM quad bus
#define PIN_RESERVED_FLASH_1    27
#define PIN_RESERVED_FLASH_2    28
#define PIN_RESERVED_FLASH_3    29
#define PIN_RESERVED_FLASH_4    30
#define PIN_RESERVED_FLASH_5    31
#define PIN_RESERVED_FLASH_6    32
#define PIN_RESERVED_PSRAM_0    33    // Octal PSRAM SPIIO4-SPIIO7 + SPIDQS
#define PIN_RESERVED_PSRAM_1    34
#define PIN_RESERVED_PSRAM_2    35
#define PIN_RESERVED_PSRAM_3    36
#define PIN_RESERVED_PSRAM_4    37

// ============================================================================
// POWER MANAGEMENT
// ============================================================================

// Battery monitoring via ADC1 on GPIO 1 (D0/A0).
// Requires a 2:1 voltage divider (two 100K resistors) from VBAT to
// this pin for accurate readings. The securacv_power library auto-
// detects whether the divider is present and reports USB-only mode
// if not (a floating pin carries no battery information).
// NEVER wire VBAT directly to this pin: a full LiPo (4.2 V) exceeds
// the pin's absolute maximum and can damage the SoC.
#define VBAT_PIN                1     // GPIO 1 (D0/A0) — ADC1_CH0
#define VBAT_DIVIDER_RATIO      2.0f  // Two 100K resistors in series

// USB power detect — not directly available on XIAO; the power
// library infers USB presence from voltage trends.
#define USB_POWER_DETECT_PIN    (-1)

// ============================================================================
// BOARD CAPABILITIES (for conditional compilation)
// ============================================================================

#define HAS_CAMERA              1
#define HAS_MICROPHONE          1
#define HAS_SD_CARD             1
#define HAS_PSRAM               1
#define HAS_USB_CDC             1
#define HAS_WIFI                1
#define HAS_BLE                 1
#define HAS_GNSS_UART           1
#define HAS_TAMPER_INPUT        1
