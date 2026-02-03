/**
 * @file pins.h
 * @brief Pin definitions for ESP32-C3 DevKitM-1 board
 *
 * This file contains all hardware pin mappings for the ESP32-C3 DevKitM-1
 * development board. Pin assignments follow the official Espressif documentation.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_ESP32_C3
#define BOARD_ESP32_C3 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "ESP32-C3 DevKitM-1"
#define BOARD_ID                "esp32-c3"
#define BOARD_VENDOR            "Espressif"
#define BOARD_MCU               "ESP32-C3"
#define BOARD_VARIANT           "devkitm-1"

// ============================================================================
// GPIO (DIRECTLY ACCESSIBLE)
// ============================================================================

// Safe GPIOs (always available)
#define PIN_GPIO0               0
#define PIN_GPIO1               1
#define PIN_GPIO2               2
#define PIN_GPIO3               3
#define PIN_GPIO4               4
#define PIN_GPIO5               5
#define PIN_GPIO6               6
#define PIN_GPIO7               7
#define PIN_GPIO8               8
#define PIN_GPIO9               9
#define PIN_GPIO10              10
#define PIN_GPIO18              18
#define PIN_GPIO19              19
#define PIN_GPIO20              20
#define PIN_GPIO21              21

// Strapping pins (be careful during boot)
#define PIN_STRAP_GPIO2         2     // Boot mode
#define PIN_STRAP_GPIO8         8     // Boot mode
#define PIN_STRAP_GPIO9         9     // Boot mode / boot button

// ============================================================================
// I2C (VISION AI MODULE - GROVE CONNECTOR)
// ============================================================================

#define I2C_PIN_SDA             4     // Grove white wire
#define I2C_PIN_SCL             5     // Grove yellow wire
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

// Alternative I2C pins
#define I2C_ALT_SDA             8
#define I2C_ALT_SCL             9

// ============================================================================
// UART
// ============================================================================

// UART0 - USB CDC (console)
#define UART0_PIN_TX            21    // USB CDC TX
#define UART0_PIN_RX            20    // USB CDC RX

// UART1 - External sensors
#define UART1_PIN_TX            6
#define UART1_PIN_RX            7
#define UART1_BAUD_DEFAULT      115200

// ============================================================================
// SPI
// ============================================================================

#define SPI_PIN_SCK             6
#define SPI_PIN_MISO            7
#define SPI_PIN_MOSI            5
#define SPI_PIN_CS              10

// ============================================================================
// ADC (12-bit)
// ============================================================================

// ADC1 (always available)
#define ADC1_CH0_PIN            0
#define ADC1_CH1_PIN            1
#define ADC1_CH2_PIN            2
#define ADC1_CH3_PIN            3
#define ADC1_CH4_PIN            4

// ADC2 (not available when WiFi active)
#define ADC2_CH0_PIN            5

// ============================================================================
// USB (NATIVE)
// ============================================================================

#define USB_PIN_DP              19    // USB D+
#define USB_PIN_DN              18    // USB D-

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

// Built-in RGB LED (active high)
#define LED_BUILTIN             8     // WS2812 or standard LED
#define LED_ACTIVE_HIGH         1

// Boot button
#define BOOT_BUTTON_PIN         9
#define BOOT_BUTTON_ACTIVE      LOW

// ============================================================================
// OPTIONAL PERIPHERALS
// ============================================================================

// Status LED (if using external)
#define EXT_LED_PIN_DEFAULT     3
#define EXT_LED_ACTIVE          HIGH

// Buzzer (if connected)
#define BUZZER_PIN_DEFAULT      2

// ============================================================================
// PIN VALIDATION
// ============================================================================

// Pins that should NOT be used (reserved)
#define PIN_RESERVED_FLASH_VDD  11    // Flash internal VDD - NEVER USE
// GPIO12-17 may be used by flash in some configurations

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

#define HAS_CAMERA              0     // No built-in camera
#define HAS_MICROPHONE          0     // No built-in microphone
#define HAS_SD_CARD             0     // No built-in SD slot
#define HAS_PSRAM               0     // No PSRAM
#define HAS_USB_CDC             1
#define HAS_WIFI                1
#define HAS_BLE                 1
#define HAS_GNSS_UART           0     // Not typically used
#define HAS_TAMPER_INPUT        0     // Not typically used
#define HAS_VISION_AI           1     // Grove Vision AI V2 support

#endif // BOARD_ESP32_C3
