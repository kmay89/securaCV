/**
 * @file pins.h
 * @brief Pin definitions for Seeed Studio XIAO ESP32-S3 board (plain, non-Sense)
 *
 * This file contains all hardware pin mappings for the Seeed Studio
 * XIAO ESP32S3 board WITHOUT the Sense expansion (no camera, microphone,
 * or SD slot). For the Sense variant use boards/xiao-esp32s3-sense.
 *
 * The XIAO ESP32-S3 plugs directly into the XIAO socket on the Grove
 * Vision AI V2 module (I2C on D4/D5, UART on D6/D7), or connects to the
 * module's Grove connector via a 4-pin Grove cable (I2C only). This is
 * the pairing sold as the "Grove Vision AI V2 Kit".
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_XIAO_ESP32S3
#define BOARD_XIAO_ESP32S3 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "XIAO ESP32-S3"
#define BOARD_ID                "xiao-esp32s3"
#define BOARD_VENDOR            "Seeed Studio"
#define BOARD_MCU               "ESP32-S3"
#define BOARD_VARIANT           "plain"

// ============================================================================
// GPIO EXPANSION CONNECTOR
// ============================================================================

// Digital pins (directly accessible on XIAO expansion header)
#define PIN_D0                  1
#define PIN_D1                  2
#define PIN_D2                  3
#define PIN_D3                  4
#define PIN_D4                  5     // Also SDA
#define PIN_D5                  6     // Also SCL
#define PIN_D6                  43    // Also TX
#define PIN_D7                  44    // Also RX
#define PIN_D8                  7     // Also SCK
#define PIN_D9                  8     // Also MISO
#define PIN_D10                 9     // Also MOSI

// Analog pins (directly accessible)
#define PIN_A0                  1
#define PIN_A1                  2
#define PIN_A2                  3
#define PIN_A3                  4
#define PIN_A4                  5
#define PIN_A5                  6

// ============================================================================
// I2C (VISION AI MODULE - GROVE CONNECTOR OR XIAO SOCKET)
// ============================================================================

#define I2C_PIN_SDA             5     // D4 - Grove white wire
#define I2C_PIN_SCL             6     // D5 - Grove yellow wire
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

// ============================================================================
// UART
// ============================================================================

// UART0 console is routed over native USB CDC.
// The header UART is available for external sensors (and is wired to the
// Grove Vision AI V2's UART when the XIAO is stacked on the module socket).
#define UART1_PIN_TX            43    // D6
#define UART1_PIN_RX            44    // D7
#define UART1_BAUD_DEFAULT      115200

// Grove Vision AI V2 UART runs at a fixed high baud rate
#define VISION_AI_UART_BAUD     921600

// ============================================================================
// SPI (USER)
// ============================================================================

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

// Built-in user LED (active low). The Arduino variant header also defines
// LED_BUILTIN, so only define it when compiling without that variant.
#define LED_BUILTIN_AVAILABLE   1
#ifndef LED_BUILTIN
#define LED_BUILTIN             21
#endif
#define LED_ACTIVE_LOW          1

// Boot button (directly connected)
#define BOOT_BUTTON_PIN         0
#define BOOT_BUTTON_ACTIVE      LOW

// ============================================================================
// OPTIONAL PERIPHERALS
// ============================================================================

// External status LED (if connected)
#define EXT_LED_PIN_DEFAULT     3     // D2
#define EXT_LED_ACTIVE          HIGH

// ============================================================================
// PIN VALIDATION
// ============================================================================

// Pins that should NOT be used (reserved for internal flash/PSRAM).
// GPIO26-32 carry the quad SPI flash/PSRAM bus; the ESP32-S3R8's OCTAL
// PSRAM additionally claims GPIO33-37 (SPIIO4-SPIIO7 + SPIDQS). None are
// broken out on the XIAO header, but routing a peripheral to them in
// software can hang or corrupt PSRAM.
#define PIN_RESERVED_FLASH_0    26
#define PIN_RESERVED_FLASH_1    27
#define PIN_RESERVED_FLASH_2    28
#define PIN_RESERVED_FLASH_3    29
#define PIN_RESERVED_FLASH_4    30
#define PIN_RESERVED_FLASH_5    31
#define PIN_RESERVED_FLASH_6    32
#define PIN_RESERVED_PSRAM_0    33
#define PIN_RESERVED_PSRAM_1    34
#define PIN_RESERVED_PSRAM_2    35
#define PIN_RESERVED_PSRAM_3    36
#define PIN_RESERVED_PSRAM_4    37

// ============================================================================
// POWER MANAGEMENT
// ============================================================================

// Battery monitoring via ADC1 on GPIO 1 (D0/A0).
// Requires a 2:1 voltage divider (two 100K resistors) from VBAT to this
// pin for accurate readings; see boards/xiao-esp32s3-sense/pins/pins.h
// for the full warning. NEVER wire VBAT directly to this pin.
#define VBAT_PIN                1     // GPIO 1 (D0/A0) — ADC1_CH0
#define VBAT_DIVIDER_RATIO      2.0f

#define USB_POWER_DETECT_PIN    (-1)

// ============================================================================
// BOARD CAPABILITIES (for conditional compilation)
// ============================================================================

#define HAS_CAMERA              0     // No camera (Sense add-on only)
#define HAS_MICROPHONE          0     // No microphone (Sense add-on only)
#define HAS_SD_CARD             0     // No SD slot (Sense add-on only)
#define HAS_PSRAM               1     // 8MB PSRAM
#define HAS_USB_CDC             1
#define HAS_WIFI                1     // External u.FL antenna required
#define HAS_BLE                 1
#define HAS_GNSS_UART           0     // Not typically used
#define HAS_TAMPER_INPUT        0     // Not typically used
#define HAS_VISION_AI           1     // Grove Vision AI V2 support
