/**
 * @file pins.h
 * @brief Pin definitions for the AI-Thinker ESP32-CAM board
 *
 * This file contains all hardware pin mappings for the AI-Thinker ESP32-CAM
 * (classic dual-core ESP32, OV2640, 4 MB flash + 4 MB quad PSRAM). Camera
 * and SD assignments follow the AI-Thinker schematic and match the
 * CAMERA_MODEL_AI_THINKER map in Espressif's esp32-camera examples — the
 * de-facto reference every ESP32-CAM guide uses.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 * @note The ESP32-CAM has NO free GPIOs once the camera and SD card are in
 *       use — every optional peripheral (GNSS, tamper, IR, external mic)
 *       is unavailable without giving up the SD card. See README.md.
 */

#pragma once

#ifndef BOARD_ESP32CAM_AI_THINKER
#define BOARD_ESP32CAM_AI_THINKER 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "AI-Thinker ESP32-CAM"
#define BOARD_ID                "esp32cam-ai-thinker"
#define BOARD_VENDOR            "AI-Thinker"
#define BOARD_MCU               "ESP32"
#define BOARD_VARIANT           "ai-thinker"

// ============================================================================
// CAMERA (OV2640) — the CAMERA_MODEL_AI_THINKER map
// ============================================================================

#define CAM_PIN_PWDN            32    // Camera power-down (driven, unlike XIAO)
#define CAM_PIN_RESET           (-1)  // Not connected (use software reset)
#define CAM_PIN_XCLK            0     // ⚠ Also the boot-strap pin — see below
#define CAM_PIN_SIOD            26    // I2C data (SCCB)
#define CAM_PIN_SIOC            27    // I2C clock (SCCB)

// Camera parallel data bus (D7-D0)
#define CAM_PIN_D7              35
#define CAM_PIN_D6              34
#define CAM_PIN_D5              39
#define CAM_PIN_D4              36
#define CAM_PIN_D3              21
#define CAM_PIN_D2              19
#define CAM_PIN_D1              18
#define CAM_PIN_D0              5

// Camera sync signals
#define CAM_PIN_VSYNC           25
#define CAM_PIN_HREF            23
#define CAM_PIN_PCLK            22

// Camera feature flags (OV2640)
#define CAM_SUPPORTS_JPEG       1
#define CAM_MAX_WIDTH           1600
#define CAM_MAX_HEIGHT          1200
#define CAM_DEFAULT_WIDTH       640
#define CAM_DEFAULT_HEIGHT      480

// ============================================================================
// SD CARD (SPI MODE)
//
// The onboard microSD slot is wired for 4-bit SDMMC; the same pins carry
// SPI mode (which the canary storage driver uses): CLK->SCK, CMD->MOSI,
// DAT0->MISO, DAT3->CS. DAT1 (GPIO4, the flash LED) and DAT2 (GPIO12, a
// boot strap) are unused in SPI mode.
// ============================================================================

#define SD_PIN_CS               13    // DAT3 in SDMMC mode
#define SD_PIN_SCK              14    // CLK
#define SD_PIN_MISO             2     // DAT0 — ⚠ boot strap, see below
#define SD_PIN_MOSI             15    // CMD  — ⚠ boot strap, see below

#define SD_SPI_FREQ_FAST        20000000  // 20 MHz normal operation
#define SD_SPI_FREQ_SLOW        1000000   // 1 MHz for init/recovery

// ============================================================================
// UART (FLASHING + CONSOLE)
//
// No native USB and no onboard USB-UART bridge: flashing and the serial
// console require an external 3.3 V UART adapter on U0TXD/U0RXD, with
// GPIO0 held LOW at reset to enter the bootloader (the classic ESP32-CAM
// flashing dance; the MB programmer shield automates it).
// ============================================================================

#define UART0_PIN_TX            1
#define UART0_PIN_RX            3

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

// White flash LED (high-side driven, VERY bright). Shares GPIO4 with SD
// DAT1 — free in SPI mode, but leave it off while the card is transferring
// if you value your dark-adapted eyes.
#define FLASH_LED_PIN           4
#define FLASH_LED_ACTIVE        HIGH

// Red status LED on the back of the module, active low.
#ifndef LED_BUILTIN
#define LED_BUILTIN             33
#endif
#define LED_ACTIVE_LOW          1

// External status LED default — reuse the onboard red status LED; there is
// no spare GPIO to run one externally with the SD card in use.
#define EXT_LED_PIN_DEFAULT     33
#define EXT_LED_ACTIVE          LOW

// No boot button on the module itself (GPIO0 is grounded manually or by
// the MB shield to flash).
#define BOOT_BUTTON_PIN         (-1)

// ============================================================================
// BOOT-STRAP PINS (document before you repurpose anything)
// ============================================================================

// GPIO0  — boot mode select, AND the camera XCLK. LOW at reset = download
//          mode. The camera driver takes it over after boot.
// GPIO2  — must not be pulled high at reset; it's SD DAT0/MISO here.
// GPIO12 — selects flash voltage at reset (must be low for 3.3 V flash);
//          it's SD DAT2, unused in SPI mode. Do not add pull-ups.
// GPIO15 — strap (MTDO); it's SD CMD/MOSI here.
#define PIN_STRAP_GPIO0         0
#define PIN_STRAP_GPIO2         2
#define PIN_STRAP_GPIO12        12
#define PIN_STRAP_GPIO15        15

// ============================================================================
// PIN VALIDATION
// ============================================================================

// GPIO16 is the quad PSRAM chip-select on this module — NEVER USE.
// (GPIO17 is not bonded out.) GPIO6-11 carry the SPI flash, not broken out.
#define PIN_RESERVED_PSRAM_CS   16

// Input-only pins (34/35/36/39) are all consumed by the camera data bus.

// ============================================================================
// BOARD CAPABILITIES (for conditional compilation)
// ============================================================================

#define HAS_CAMERA              1
#define HAS_MICROPHONE          0     // No onboard mic; no free pins to add one
                                      // without giving up the SD card
#define HAS_SD_CARD             1
#define HAS_PSRAM               1     // 4 MB quad PSRAM (GPIO16 CS)
#define HAS_USB_CDC             0     // UART flashing only (external adapter)
#define HAS_WIFI                1     // Wi-Fi CSI supported on classic ESP32
#define HAS_BLE                 1
#define HAS_GNSS_UART           0     // No free pins with camera + SD in use
#define HAS_TAMPER_INPUT        0     // No free pins
