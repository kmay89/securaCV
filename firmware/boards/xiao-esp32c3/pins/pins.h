/**
 * @file pins.h
 * @brief Pin definitions for Seeed Studio XIAO ESP32-C3 board
 *
 * This file contains all hardware pin mappings for the Seeed Studio
 * XIAO ESP32C3 board. Pin assignments are based on the official Seeed
 * documentation and hardware schematics.
 *
 * The XIAO ESP32-C3 plugs directly into the XIAO socket on the Grove
 * Vision AI V2 module (I2C on D4/D5, UART on D6/D7), or connects to the
 * module's Grove connector via a 4-pin Grove cable (I2C only).
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_XIAO_ESP32C3
#define BOARD_XIAO_ESP32C3 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "XIAO ESP32-C3"
#define BOARD_ID                "xiao-esp32c3"
#define BOARD_VENDOR            "Seeed Studio"
#define BOARD_MCU               "ESP32-C3"
#define BOARD_VARIANT           "xiao"

// ============================================================================
// GPIO EXPANSION CONNECTOR
// ============================================================================

// Digital pins (directly accessible on XIAO expansion header)
#define PIN_D0                  2
#define PIN_D1                  3
#define PIN_D2                  4
#define PIN_D3                  5
#define PIN_D4                  6     // Also SDA
#define PIN_D5                  7     // Also SCL
#define PIN_D6                  21    // Also TX
#define PIN_D7                  20    // Also RX
#define PIN_D8                  8     // Also SCK
#define PIN_D9                  9     // Also MISO
#define PIN_D10                 10    // Also MOSI

// Analog pins (directly accessible)
#define PIN_A0                  2
#define PIN_A1                  3
#define PIN_A2                  4
#define PIN_A3                  5

// Strapping pins (be careful during boot)
#define PIN_STRAP_GPIO2         2     // Boot mode (D0)
#define PIN_STRAP_GPIO8         8     // Boot mode (D8)
#define PIN_STRAP_GPIO9         9     // Boot mode / boot button (D9)

// ============================================================================
// I2C (VISION AI MODULE - GROVE CONNECTOR OR XIAO SOCKET)
// ============================================================================

#define I2C_PIN_SDA             6     // D4 - Grove white wire
#define I2C_PIN_SCL             7     // D5 - Grove yellow wire
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

// ============================================================================
// UART
// ============================================================================

// UART0 console is routed over the native USB Serial/JTAG peripheral.
// The header UART is available for external sensors (and is wired to the
// Grove Vision AI V2's UART when the XIAO is stacked on the module socket).
#define UART1_PIN_TX            21    // D6
#define UART1_PIN_RX            20    // D7
#define UART1_BAUD_DEFAULT      115200

// Grove Vision AI V2 UART runs at a fixed high baud rate
#define VISION_AI_UART_BAUD     921600

// ============================================================================
// SPI
// ============================================================================

#define SPI_PIN_SCK             8     // D8
#define SPI_PIN_MISO            9     // D9
#define SPI_PIN_MOSI            10    // D10

// ============================================================================
// ADC (12-bit)
// ============================================================================

// ADC1 (always available)
#define ADC1_CH2_PIN            2     // A0
#define ADC1_CH3_PIN            3     // A1
#define ADC1_CH4_PIN            4     // A2

// ADC2 (not available when WiFi active)
#define ADC2_CH0_PIN            5     // A3

// ============================================================================
// USB (NATIVE)
// ============================================================================

#define USB_PIN_DP              19    // USB D+
#define USB_PIN_DN              18    // USB D-

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

// No user-controllable LED on the XIAO ESP32-C3 (power + charge LEDs only)
#define LED_BUILTIN_AVAILABLE   0

// Boot button (directly connected)
#define BOOT_BUTTON_PIN         9
#define BOOT_BUTTON_ACTIVE      LOW

// ============================================================================
// OPTIONAL PERIPHERALS
// ============================================================================

// External status LED (if connected)
#define EXT_LED_PIN_DEFAULT     3     // D1
#define EXT_LED_ACTIVE          HIGH

// ============================================================================
// PIN VALIDATION
// ============================================================================

// GPIO11-17 are bonded to the internal flash - never exposed, never use.
// GPIO18/19 are the USB data lines - using them breaks USB CDC/JTAG.

// ============================================================================
// BOARD CAPABILITIES (for conditional compilation)
// ============================================================================

#define HAS_CAMERA              0     // No built-in camera
#define HAS_MICROPHONE          0     // No built-in microphone
#define HAS_SD_CARD             0     // No built-in SD slot
#define HAS_PSRAM               0     // No PSRAM
#define HAS_USB_CDC             1
#define HAS_WIFI                1     // External u.FL antenna required
#define HAS_BLE                 1
#define HAS_GNSS_UART           0     // Not typically used
#define HAS_TAMPER_INPUT        0     // Not typically used
#define HAS_VISION_AI           1     // Grove Vision AI V2 support

#endif // BOARD_XIAO_ESP32C3
