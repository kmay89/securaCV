/**
 * @file pins.h
 * @brief Pin definitions for the Waveshare ESP32-S3-LCD-1.47
 *        ("Canary Nightstand" — USB-A stick 1.47" glance/ambient node)
 *
 * Integrated board: ESP32-S3 (dual-core LX7 240 MHz, Wi-Fi + BLE 5; 16 MB
 * flash, 8 MB quad PSRAM) driving the SAME 1.47" 172x320 ST7789 IPS panel as
 * the C6 sibling, over 4-wire SPI, plus ONE onboard WS2812-class addressable
 * RGB LED (the primary ambient state channel). USB-A plug is wired to the
 * S3's NATIVE USB. Pin assignments per Waveshare's wiki + the TFT_eSPI /
 * espp / CircuitPython board defs.
 *
 * ⚠️ DEV STATUS: pin map compiled from vendor sources — NOT yet validated on
 *    bench hardware. Verify against your board revision (there is also a
 *    non-stick "-LCD-1.47B" with a different form factor and map).
 *
 * ⚠️ EVERY DISPLAY PIN DIFFERS FROM THE C6 BOARD — do not share a pin map.
 *    Same ST7789 panel + same 34-px column offset, different GPIOs.
 *
 * ⚠️ PANEL QUIRKS (TFT_eSPI community): this board wants the HSPI port,
 *    BGR color order, and active-HIGH backlight; init at ~8 MHz then run fast.
 *
 * ⚠️ NATIVE USB vs JTAG: the USB-A plug is the S3's native USB (D-=GPIO19,
 *    D+=GPIO20). Enabling native-USB CDC can take over the console; default
 *    flashing is over USB-Serial/JTAG. Mind "USB CDC On Boot".
 *
 * ⚠️ RGB LED: drive the WS2812 via RMT.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_WAVESHARE_ESP32S3_LCD147
#define BOARD_WAVESHARE_ESP32S3_LCD147 1
#endif

// Which physical board this is, in the fleet-figure vocabulary. Named here
// rather than in the build env because THIS header is what a build compiles
// against — the load-bearing declaration, so the id cannot drift away from
// the pins it travels with. canary::figures::my_figure() reads it, and the
// display publishes it on /api/fleet and in its mDNS advert so the apps can
// draw this board rather than a generic marker.
#ifndef CANARY_FIGURE_HARDWARE
#define CANARY_FIGURE_HARDWARE "waveshare-esp32s3-lcd147"
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Waveshare ESP32-S3-LCD-1.47"
#define BOARD_ID                "waveshare-esp32s3-lcd147"
#define BOARD_VENDOR            "Waveshare"
#define BOARD_MCU               "ESP32-S3"
#define BOARD_VARIANT           "esp32s3-lcd-1.47"

// ============================================================================
// TFT — ST7789 1.47" 172x320 portrait, 4-wire SPI (RGB565)
// ============================================================================

#define LCD_WIDTH               172
#define LCD_HEIGHT              320
#define TFT_WIDTH               172
#define TFT_HEIGHT              320

#define TFT_PIN_SCK             40
#define TFT_PIN_MOSI            45
#define TFT_PIN_MISO            -1    // not broken out for the panel; ST7789 write-only
#define TFT_PIN_CS              42
#define TFT_PIN_DC              41
#define TFT_PIN_RST             39
#define TFT_PIN_BL              48    // backlight enable (drive with LEDC PWM)
#define TFT_BL_ACTIVE_HIGH      1
#define TFT_SPI_HZ              40000000
#define TFT_USE_HSPI            1     // community fix for this board
#define TFT_COLOR_ORDER_BGR     1     // BGR, per TFT_eSPI setup

// ST7789 172x320 window offset (240x320 controller RAM cutout) at rotation 0.
#define TFT_COL_OFFSET          34
#define TFT_ROW_OFFSET          0

// ============================================================================
// RGB LED — 1x WS2812-class addressable (the primary ambient state channel)
// ============================================================================

#define RGBLED_PIN              38
#define RGBLED_COUNT            1
#define RGBLED_ORDER_GRB        1
#define RGBLED_USE_RMT          1

// ============================================================================
// I2C (broken-out header) — no onboard touch on this board
// ============================================================================

#define I2C_PIN_SCL             17
#define I2C_PIN_SDA             16
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

// ============================================================================
// microSD — onboard slot (separate SPI; event cache; unused in v0.1)
// ============================================================================

#define SD_PIN_CLK              14
#define SD_PIN_MOSI             15
#define SD_PIN_MISO             16
#define SD_PIN_CS               -1    // VERIFY CS pin on your revision

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

#define BOOT_BUTTON_PIN         0

// No piezo pad on this board — the chime engine compiles (CI coverage /
// the emulator's forced FEATURE_CHIME) and chime_init(-1) no-ops, the
// same unpopulated-pad convention the 4.3B carries.
#define BUZZER_PIN              -1
#define BOOT_BUTTON_ACTIVE      LOW

// Native USB data lines (informational; managed by the USB stack).
#define USB_DM_PIN              19
#define USB_DP_PIN              20

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

#define HAS_CAMERA              0
#define HAS_MICROPHONE          0
#define HAS_SD_CARD             1
#define HAS_PSRAM               1     // 8 MB quad — room to double-buffer + animate
#define HAS_USB_CDC             1
#define HAS_NATIVE_USB          1     // USB-A plug on the S3's native USB
#define HAS_WIFI                1
#define HAS_BLE                 1
#define HAS_DISPLAY             1     // 172x320 ST7789 SPI
#define HAS_TOUCH               0
#define HAS_RGBLED              1
#define HAS_RTC                 0
#define HAS_BATTERY             0
#define HAS_BACKLIGHT_PWM       1
