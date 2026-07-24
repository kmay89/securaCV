/**
 * @file pins.h
 * @brief Pin definitions for the Waveshare ESP32-C6-LCD-1.47
 *        ("Canary Nightstand" — pin-header 1.47" glance/ambient node)
 *
 * Integrated board: ESP32-C6 (single-core RISC-V 160 MHz, Wi-Fi 6 + BLE 5 +
 * 802.15.4; 4 MB flash, NO PSRAM) driving a 1.47" 172x320 ST7789 IPS panel
 * over 4-wire SPI, plus ONE onboard WS2812-class addressable RGB LED — the
 * primary ambient state channel for this device (see the display line doc).
 * Pin assignments per Waveshare's wiki + the CircuitPython/espp board defs.
 *
 * ⚠️ DEV STATUS: pin map compiled from vendor sources — NOT yet validated on
 *    bench hardware. Verify backlight/RGB-LED lines and the ST7789 offset
 *    against your board revision before relying on them.
 *
 * ⚠️ ST7789 OFFSET: the 172-wide glass is a window into the controller's
 *    240-wide RAM, so every draw needs a 34-px column offset at rotation 0
 *    (TFT_COL_OFFSET). The offset migrates to the opposite edge when rotated
 *    — recompute per rotation in the HAL.
 *
 * ⚠️ NO PSRAM: the 172x320x2 = ~110 KB framebuffer fits internal SRAM, but
 *    there is no headroom for double-buffering; the C6 flavor renders lean
 *    (single buffer, dirty-region LVGL). The S3 sibling (-lcd147) has 8 MB
 *    PSRAM and can breathe.
 *
 * ⚠️ RGB LED under a single core: drive the WS2812 via the RMT peripheral,
 *    never bit-banged — a bit-banged strobe glitches colors under Wi-Fi load
 *    on this single-core part.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_WAVESHARE_ESP32C6_LCD147
#define BOARD_WAVESHARE_ESP32C6_LCD147 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Waveshare ESP32-C6-LCD-1.47"
#define BOARD_ID                "waveshare-esp32c6-lcd147"
#define BOARD_VENDOR            "Waveshare"
#define BOARD_MCU               "ESP32-C6"
#define BOARD_VARIANT           "esp32c6-lcd-1.47"

// ============================================================================
// TFT — ST7789 1.47" 172x320 portrait, 4-wire SPI (RGB565)
// ============================================================================

#define LCD_WIDTH               172
#define LCD_HEIGHT              320
#define TFT_WIDTH               172
#define TFT_HEIGHT              320

#define TFT_PIN_SCK             7
#define TFT_PIN_MOSI            6
#define TFT_PIN_MISO            5     // shared SPI (TF card); ST7789 is write-only
#define TFT_PIN_CS              14
#define TFT_PIN_DC              15
#define TFT_PIN_RST             21
#define TFT_PIN_BL              22    // backlight enable (drive with LEDC PWM)
#define TFT_BL_ACTIVE_HIGH      1
#define TFT_SPI_HZ              40000000  // ST7789 handles 40 MHz writes

// ST7789 172x320 window offset (240x320 controller RAM cutout) at rotation 0.
#define TFT_COL_OFFSET          34
#define TFT_ROW_OFFSET          0

// ============================================================================
// RGB LED — 1x WS2812-class addressable (the primary ambient state channel)
// ============================================================================

#define RGBLED_PIN              8
#define RGBLED_COUNT            1
#define RGBLED_ORDER_GRB        1     // GRB byte order
#define RGBLED_USE_RMT          1     // RMT-driven; never bit-bang under Wi-Fi

// ============================================================================
// I2C (broken-out header) — no onboard touch on this board
// ============================================================================

#define I2C_PIN_SCL             0
#define I2C_PIN_SDA             1
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

// ============================================================================
// microSD — shared SPI bus (event cache; unused in v0.1)
// ============================================================================

#define SD_PIN_CLK              7
#define SD_PIN_MOSI             6
#define SD_PIN_MISO             5
#define SD_PIN_CS               4

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

#define BOOT_BUTTON_PIN         9     // BOOT/user (also a strapping pin)
#define BOOT_BUTTON_ACTIVE      LOW

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

#define HAS_CAMERA              0     // No camera — this Canary shows, it doesn't watch
#define HAS_MICROPHONE          0     // No microphone — quiet-room-safe by construction
#define HAS_SD_CARD             1     // TF slot (unused in v0.1)
#define HAS_PSRAM               0     // none — lean single-buffer rendering
#define HAS_USB_CDC             1     // USB-Serial/JTAG only (no native USB-OTG on C6)
#define HAS_NATIVE_USB          0
#define HAS_WIFI                1     // Wi-Fi 6 (2.4 GHz)
#define HAS_BLE                 1
#define HAS_THREAD_ZIGBEE       1     // 802.15.4 radio (unused in v0.1)
#define HAS_DISPLAY             1     // 172x320 ST7789 SPI
#define HAS_TOUCH               0     // no touch — RGB LED + tap-via-button only
#define HAS_RGBLED              1     // the ambient state channel
#define HAS_RTC                 0
#define HAS_BATTERY             0     // USB-C / pin-header powered
#define HAS_BACKLIGHT_PWM       1     // LEDC PWM on TFT_PIN_BL — night dimming works
