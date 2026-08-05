/**
 * @file pins.h
 * @brief Pin definitions for the generic ESP32-WROOM-32 DevKit
 *
 * Covers the huge family of classic ESP32 development boards built on the
 * ESP32-WROOM-32 module: Espressif's DevKitC, DOIT DevKit V1, NodeMCU-32S,
 * and the countless white-label 30/38-pin clones. Pin assignments follow
 * the Espressif DevKitC reference; boards in this family differ in which
 * module pins reach the headers, not in what the pins do.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 * @note No camera, mic, or SD slot on board — this is the "an ESP32" that
 *       people own. The witness role it serves is Wi-Fi CSI presence
 *       sensing plus the signed witness chain; see README.md.
 */

#pragma once

#ifndef BOARD_ESP32_WROOM_DEVKIT
#define BOARD_ESP32_WROOM_DEVKIT 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "ESP32-WROOM-32 DevKit"
#define BOARD_ID                "esp32-wroom-devkit"
#define BOARD_VENDOR            "Espressif (and clones)"
#define BOARD_MCU               "ESP32"
#define BOARD_VARIANT           "wroom-devkit"

// ============================================================================
// UART
// ============================================================================

// UART0 — routed through the onboard USB-UART bridge (CP2102/CH340,
// board-dependent). Console + flashing; auto-reset via DTR/RTS on most
// boards, so no manual GPIO0 dance is needed.
#define UART0_PIN_TX            1
#define UART0_PIN_RX            3

// UART2 — free on this board family; the port's default GNSS map
// (an L76K or NEO-6M wired externally is the classic pairing).
#define UART2_PIN_TX            17
#define UART2_PIN_RX            16

// ============================================================================
// GNSS (EXTERNAL MODULE, OPTIONAL)
// ============================================================================

#define GNSS_PIN_TX             17    // ESP32 TX -> GPS RX
#define GNSS_PIN_RX             16    // ESP32 RX <- GPS TX
#define GNSS_BAUD_DEFAULT       9600

// ============================================================================
// I2C (DEFAULT)
// ============================================================================

#define I2C_PIN_SDA             21
#define I2C_PIN_SCL             22
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

// ============================================================================
// SPI (VSPI DEFAULT) — an external SD adapter would go here (VERIFY:
// external wiring, not an onboard peripheral)
// ============================================================================

#define SPI_PIN_SCK             18
#define SPI_PIN_MISO            19
#define SPI_PIN_MOSI            23
#define SPI_PIN_CS              5

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

// Most DevKit-family boards wire a blue LED to GPIO2 (DOIT V1, NodeMCU-32S).
// Bare DevKitC boards have only a power LED — then this drives nothing,
// which is harmless.
#ifndef LED_BUILTIN
#define LED_BUILTIN             2
#endif
#define LED_ACTIVE_HIGH         1

// External status LED (if connected)
#define EXT_LED_PIN_DEFAULT     27
#define EXT_LED_ACTIVE          HIGH

// Boot button (IO0), present on the whole family
#define BOOT_BUTTON_PIN         0
#define BOOT_BUTTON_ACTIVE      LOW

// ============================================================================
// OPTIONAL PERIPHERALS
// ============================================================================

// Tamper detection (if enclosure sensor connected)
// Connect normally-closed switch between this pin and GND
#define TAMPER_PIN_DEFAULT      26    // configure as INPUT_PULLUP
#define TAMPER_ACTIVE           LOW

// ============================================================================
// BOOT-STRAP PINS (safe to use after boot; mind them at reset)
// ============================================================================

#define PIN_STRAP_GPIO0         0     // Boot mode (boot button)
#define PIN_STRAP_GPIO2         2     // Must not be high at reset (onboard LED is fine)
#define PIN_STRAP_GPIO5         5     // SDIO timing strap
#define PIN_STRAP_GPIO12        12    // Flash voltage — do NOT pull high
#define PIN_STRAP_GPIO15        15    // Silences boot log if low

// ============================================================================
// PIN VALIDATION
// ============================================================================

// GPIO6-11 carry the SPI flash — never use (mostly not broken out, but
// NodeMCU-32S-style 38-pin boards do expose them).
#define PIN_RESERVED_FLASH_0    6
#define PIN_RESERVED_FLASH_1    7
#define PIN_RESERVED_FLASH_2    8
#define PIN_RESERVED_FLASH_3    9
#define PIN_RESERVED_FLASH_4    10
#define PIN_RESERVED_FLASH_5    11

// Input-only pins (no output, no internal pull-ups)
#define PIN_INPUT_ONLY_0        34
#define PIN_INPUT_ONLY_1        35
#define PIN_INPUT_ONLY_2        36
#define PIN_INPUT_ONLY_3        39

// ============================================================================
// BOARD CAPABILITIES (for conditional compilation)
// ============================================================================

#define HAS_CAMERA              0     // No camera connector
#define HAS_MICROPHONE          0     // No onboard mic (external I2S possible — VERIFY)
#define HAS_SD_CARD             0     // No onboard slot (external SPI adapter possible — VERIFY)
#define HAS_PSRAM               0     // WROOM-32 has no PSRAM (WROVER does — different board)
#define HAS_USB_CDC             0     // USB-UART bridge, not native CDC
#define HAS_WIFI                1     // Wi-Fi CSI supported on classic ESP32
#define HAS_BLE                 1
#define HAS_GNSS_UART           1     // Free UART2 (external module)
#define HAS_TAMPER_INPUT        1     // Plenty of free GPIOs
