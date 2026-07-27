/**
 * @file pins.h
 * @brief Pin definitions for the Waveshare ESP32-C6-LCD-1.69
 *        (Nightstand Line candidate — battery-capable 1.69" glance node)
 *
 * Integrated board: ESP32-C6 (single-core RISC-V 160 MHz, Wi-Fi 6 + BLE 5 +
 * 802.15.4; 4 MB flash, NO PSRAM) driving a 1.69" 240x280 ST7789V2 IPS panel
 * over 4-wire SPI, plus onboard QMI8658 6-axis IMU + PCF85063 RTC (shared
 * I2C) and a 3.7 V MX1.25 lithium battery charge/discharge interface.
 * Waveshare markets the board with an "AI speech" audio path (ES8311 codec
 * + microphone) — treat it as MIC-BEARING until the schematic proves
 * otherwise (same privacy posture as the 4.3C dash).
 *
 * ⚠️ DEV STATUS: PARTIAL pin map. The vendor wiki
 *    (https://docs.waveshare.com/ESP32-C6-LCD-1.69) was unreachable from
 *    the CI sandbox (network policy), so only two classes of pins are
 *    filled in:
 *      - chip-level facts (BOOT strap, USB-Serial/JTAG, UART0), and
 *      - the ST7789 SPI block, which Waveshare shares across its C6-LCD
 *        family (identical on the 1.47: SCK7/MOSI6/CS14/DC15/RST21/BL22).
 *    Every pin we could NOT verify is -1 with a VERIFY note. Fill them
 *    from the wiki pin-allocation table / schematic PDF and bench-check
 *    before first flash — do not guess.
 *
 * ⚠️ ST7789V2 OFFSET: the 240x280 glass is a window into the controller's
 *    240x320 RAM — every draw needs a 20-px ROW offset at rotation 0
 *    (TFT_ROW_OFFSET), migrating on rotation. (The 1.47 sibling has the
 *    same disease on the other axis: 34-px column offset.)
 *
 * ⚠️ NO PSRAM: 240x280x2 = ~134 KB framebuffer fits internal SRAM with no
 *    double-buffer headroom — lean single-buffer, dirty-region rendering,
 *    same budget as the 1.47 C6.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_WAVESHARE_ESP32C6_LCD169
#define BOARD_WAVESHARE_ESP32C6_LCD169 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Waveshare ESP32-C6-LCD-1.69"
#define BOARD_ID                "waveshare-esp32c6-lcd169"
#define BOARD_VENDOR            "Waveshare"
#define BOARD_MCU               "ESP32-C6"
#define BOARD_VARIANT           "esp32c6-lcd-1.69"

// ============================================================================
// TFT — ST7789V2 1.69" 240x280 portrait, 4-wire SPI (RGB565)
// Family-shared C6-LCD map (matches the 1.47) — VERIFY against the wiki
// pin-allocation table before first flash.
// ============================================================================

#define LCD_WIDTH               240
#define LCD_HEIGHT              280
#define TFT_WIDTH               240
#define TFT_HEIGHT              280

#define TFT_PIN_SCK             7     // VERIFY (family-shared map)
#define TFT_PIN_MOSI            6     // VERIFY (family-shared map)
#define TFT_PIN_MISO            -1    // ST7789 is write-only; no TF slot
#define TFT_PIN_CS              14    // VERIFY (family-shared map)
#define TFT_PIN_DC              15    // VERIFY (family-shared map)
#define TFT_PIN_RST             21    // VERIFY (family-shared map)
#define TFT_PIN_BL              22    // VERIFY (family-shared map); LEDC PWM
#define TFT_BL_ACTIVE_HIGH      1
#define TFT_SPI_HZ              40000000

// ST7789V2 240x280 window offset (240x320 controller RAM) at rotation 0.
#define TFT_COL_OFFSET          0
#define TFT_ROW_OFFSET          20

// ============================================================================
// I2C — QMI8658 6-axis IMU + PCF85063 RTC share the bus
// ============================================================================

#define I2C_PIN_SDA             -1    // VERIFY from wiki/schematic
#define I2C_PIN_SCL             -1    // VERIFY from wiki/schematic
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

#define IMU_PIN_INT1            -1    // VERIFY (QMI8658 INT1, if wired)
#define RTC_PIN_INT             -1    // VERIFY (PCF85063 INT, if wired)

// ============================================================================
// AUDIO — "AI speech" path (ES8311 codec + mic + speaker per marketing)
// ============================================================================

#define I2S_PIN_BCLK            -1    // VERIFY (present only if codec fitted)
#define I2S_PIN_LRCLK           -1    // VERIFY
#define I2S_PIN_DOUT            -1    // VERIFY (codec DAC -> speaker)
#define I2S_PIN_DIN             -1    // VERIFY (mic -> MCU)

// ============================================================================
// POWER — 3.7 V MX1.25 battery, charge/discharge managed onboard
// ============================================================================

#define BAT_ADC_PIN             -1    // VERIFY (expect an ADC1 ch 0-6 GPIO)
#define PWR_KEY_PIN             -1    // VERIFY (PWR button / power latch)

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

#define BOOT_BUTTON_PIN         9     // BOOT/user (strapping pin — chip fact)
#define BOOT_BUTTON_ACTIVE      LOW

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

#define HAS_CAMERA              0
#define HAS_MICROPHONE          1     // "AI speech" mic path — a PRIVACY
                                      // SURFACE: ship mic-off by default
                                      // (same posture as the 4.3C dash);
                                      // VERIFY on schematic
#define HAS_SD_CARD             0     // no TF slot on this SKU
#define HAS_PSRAM               0     // none — lean single-buffer rendering
#define HAS_USB_CDC             1     // USB-Serial/JTAG only (no OTG on C6)
#define HAS_NATIVE_USB          0
#define HAS_WIFI                1     // Wi-Fi 6 (2.4 GHz)
#define HAS_BLE                 1
#define HAS_THREAD_ZIGBEE       1     // 802.15.4 radio (unused in v0.1)
#define HAS_DISPLAY             1     // 240x280 ST7789V2 SPI
#define HAS_TOUCH               0     // this SKU; Touch sibling adds CST816T
#define HAS_RGBLED              0     // no addressable LED on this SKU
#define HAS_RTC                 1     // PCF85063
#define HAS_IMU                 1     // QMI8658 6-axis
#define HAS_BATTERY             1     // 3.7 V MX1.25 + onboard charging
#define HAS_BACKLIGHT_PWM       1     // LEDC PWM on TFT_PIN_BL
