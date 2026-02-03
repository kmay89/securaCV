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
// CAMERA (OV2640) - Built-in on Sense variant
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

// Camera feature flags
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

// Built-in LED (active low on some variants)
#define LED_BUILTIN             21
#define LED_ACTIVE_LOW          0

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

// Pins that should NOT be used (reserved for internal functions)
#define PIN_RESERVED_FLASH_0    26    // Flash/PSRAM
#define PIN_RESERVED_FLASH_1    27
#define PIN_RESERVED_FLASH_2    28
#define PIN_RESERVED_FLASH_3    29
#define PIN_RESERVED_FLASH_4    30
#define PIN_RESERVED_FLASH_5    31
#define PIN_RESERVED_FLASH_6    32
#define PIN_RESERVED_FLASH_7    33

// ============================================================================
// POWER MANAGEMENT
// ============================================================================

// Battery monitoring (if connected via voltage divider)
#define VBAT_PIN                (-1)  // Not available on base board
#define VBAT_DIVIDER_RATIO      2.0f  // If using external divider

// USB power detect
#define USB_POWER_DETECT_PIN    (-1)  // Not available directly

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

#endif // BOARD_XIAO_ESP32S3_SENSE
