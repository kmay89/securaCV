/**
 * @file pins.h
 * @brief Pin definitions for the Freenove ESP32-S3-WROOM CAM board (FNK0085)
 *
 * The default Amazon "ESP32-S3 camera kit" a US newcomer actually receives.
 * ESP32-S3-WROOM-1 (N16R8: 16 MB flash, 8 MB octal PSRAM) with an OV2640
 * on the ESP32-S3-EYE camera map — Freenove's board follows Espressif's
 * S3-EYE reference wiring, so the camera pins match CAMERA_MODEL_ESP32S3_EYE
 * in the esp32-camera examples.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 * @note The microSD slot is wired for 1-bit SDMMC ONLY (CLK/CMD/DATA0; the
 *       card's DAT3/CS line is not routed to a GPIO), so the canary SPI-mode
 *       SD driver cannot use it. See README.md.
 */

#pragma once

#ifndef BOARD_FREENOVE_ESP32S3_CAM
#define BOARD_FREENOVE_ESP32S3_CAM 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Freenove ESP32-S3-WROOM CAM"
#define BOARD_ID                "freenove-esp32s3-cam"
#define BOARD_VENDOR            "Freenove"
#define BOARD_MCU               "ESP32-S3"
#define BOARD_VARIANT           "fnk0085"

// ============================================================================
// CAMERA (OV2640) — the ESP32-S3-EYE reference map
// ============================================================================

#define CAM_PIN_PWDN            (-1)  // Not connected (always on)
#define CAM_PIN_RESET           (-1)  // Not connected (use software reset)
#define CAM_PIN_XCLK            15
#define CAM_PIN_SIOD            4     // I2C data (SCCB)
#define CAM_PIN_SIOC            5     // I2C clock (SCCB)

// Camera parallel data bus (D7-D0)
#define CAM_PIN_D7              16
#define CAM_PIN_D6              17
#define CAM_PIN_D5              18
#define CAM_PIN_D4              12
#define CAM_PIN_D3              10
#define CAM_PIN_D2              8
#define CAM_PIN_D1              9
#define CAM_PIN_D0              11

// Camera sync signals
#define CAM_PIN_VSYNC           6
#define CAM_PIN_HREF            7
#define CAM_PIN_PCLK            13

// Camera feature flags (OV2640)
#define CAM_SUPPORTS_JPEG       1
#define CAM_MAX_WIDTH           1600
#define CAM_MAX_HEIGHT          1200
#define CAM_DEFAULT_WIDTH       640
#define CAM_DEFAULT_HEIGHT      480

// ============================================================================
// SD CARD (1-BIT SDMMC ONLY — NOT USABLE BY THE SPI DRIVER)
//
// The slot wires CLK=39, CMD=38, DATA0=40; DAT3 (which becomes CS in SPI
// mode) is not routed to a GPIO, so SPI mode is electrically impossible.
// The canary storage driver is SPI-mode, so FEATURE_SD_STORAGE stays off
// in this port until an SDMMC path exists. Pins documented for that day.
// ============================================================================

#define SDMMC_PIN_CLK           39
#define SDMMC_PIN_CMD           38
#define SDMMC_PIN_D0            40

// ============================================================================
// UART
// ============================================================================

// UART0 header pins (console is native USB CDC; these stay free)
#define UART0_PIN_TX            43
#define UART0_PIN_RX            44

// Default GNSS UART map (external module, optional) — same pins as the
// XIAO port so the canary defaults carry over unchanged.
#define GNSS_PIN_TX             43    // ESP32 TX -> GPS RX
#define GNSS_PIN_RX             44    // ESP32 RX <- GPS TX
#define GNSS_BAUD_DEFAULT       9600

// ============================================================================
// I2C (DEFAULT, EXTERNAL HEADER)
//
// GPIO1/2 are free header pins; the camera SCCB owns GPIO4/5.
// ============================================================================

#define I2C_PIN_SDA             1
#define I2C_PIN_SCL             2
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

// ============================================================================
// USB (NATIVE)
// ============================================================================

#define USB_PIN_DP              20    // USB D+
#define USB_PIN_DN              19    // USB D-

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

// WS2812 RGB LED on GPIO48 (addressable — not a plain on/off LED). The
// Arduino variant header may define LED_BUILTIN; only define when missing.
#ifndef LED_BUILTIN
#define LED_BUILTIN             48
#endif
#define LED_IS_WS2812           1

// Boot button (IO0) + reset button both present
#define BOOT_BUTTON_PIN         0
#define BOOT_BUTTON_ACTIVE      LOW

// ============================================================================
// OPTIONAL PERIPHERALS
// ============================================================================

// Tamper detection (if enclosure sensor connected)
// Connect normally-closed switch between this pin and GND
#define TAMPER_PIN_DEFAULT      21    // free header pin - configure as INPUT_PULLUP
#define TAMPER_ACTIVE           LOW

// External status LED (if connected)
#define EXT_LED_PIN_DEFAULT     47    // free header pin
#define EXT_LED_ACTIVE          HIGH

// ============================================================================
// PIN VALIDATION
// ============================================================================

// GPIO26-32 carry the quad SPI flash bus; the WROOM-1 N16R8's OCTAL PSRAM
// additionally claims GPIO33-37. None are broken out, but routing a
// peripheral to them in software can hang or corrupt PSRAM.
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
// BOARD CAPABILITIES (for conditional compilation)
// ============================================================================

#define HAS_CAMERA              1
#define HAS_MICROPHONE          0     // No onboard mic
#define HAS_SD_CARD             1     // Slot exists, but 1-bit SDMMC only —
                                      // unusable by the SPI-mode storage
                                      // driver (see the SD section above)
#define HAS_PSRAM               1     // 8 MB octal PSRAM
#define HAS_USB_CDC             1     // Native USB (S3)
#define HAS_WIFI                1
#define HAS_BLE                 1
#define HAS_GNSS_UART           1     // Free UART pins (external module)
#define HAS_TAMPER_INPUT        1     // Free header pins
