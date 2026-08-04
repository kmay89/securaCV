/**
 * @file pins.h
 * @brief Pin definitions for the Waveshare ESP32-S3-Touch-LCD-7
 *        ("Canary Dash 7" — the big-glass desk dashboard)
 *
 * Integrated board: ESP32-S3R8 (16 MB flash / 8 MB octal PSRAM) driving a 7"
 * 800x480 IPS panel over the S3's parallel RGB565 LCD peripheral, a GT911
 * 5-point capacitive touch controller (I2C), and a CH422G I2C IO expander
 * that owns the touch-reset and backlight-enable lines. This is the SAME
 * electrical architecture as the 4.3" Canary Dash (boards/waveshare-esp32s3-
 * lcd43) at the SAME 800x480 resolution — only the physical glass is larger.
 * So the Dash RGB HAL, GT911 driver, and CH422G handling all carry over; the
 * layout is "same canvas, roomier glass + bigger touch targets."
 *
 * ⚠️ DEV STATUS: pin map compiled from vendor sources — NOT yet validated on
 *    bench hardware. Waveshare revised the 7" RGB porch timings and the
 *    touch-INT GPIO across V1.x revisions — verify against your board's demo
 *    esp_lcd_rgb_timing_t and the CH422G bit map before relying on them.
 *
 * ⚠️ PSRAM IS REQUIRED: the 800x480x2 = ~768 KB framebuffer cannot live in
 *    internal SRAM — it MUST sit in the 8 MB octal (OPI) PSRAM, with the S3
 *    "bounce buffer" enabled and PSRAM at 80 MHz. The 7" is bandwidth-bound
 *    at high PCLK; keep PCLK modest (16-21 MHz) and verify tearing.
 *
 * ⚠️ BACKLIGHT + TOUCH RESET RIDE THE CH422G, NOT NATIVE GPIOs. The GT911
 *    will not enumerate until it is reset through the expander first; the
 *    backlight is ON/OFF only (no PWM path) — night dimming is dark rendering
 *    + scheduled backlight-off, same as the 4.3" Dash.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_WAVESHARE_ESP32S3_LCD7
#define BOARD_WAVESHARE_ESP32S3_LCD7 1
#endif

// Which physical board this is, in the fleet-figure vocabulary. Named here
// rather than in the build env because THIS header is what a build compiles
// against — the load-bearing declaration, so the id cannot drift away from
// the pins it travels with. canary::figures::my_figure() reads it.
#ifndef CANARY_FIGURE_HARDWARE
#define CANARY_FIGURE_HARDWARE "waveshare-esp32s3-lcd7"
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Waveshare ESP32-S3-Touch-LCD-7"
#define BOARD_ID                "waveshare-esp32s3-lcd7"
#define BOARD_VENDOR            "Waveshare"
#define BOARD_MCU               "ESP32-S3"
#define BOARD_VARIANT           "esp32s3-touch-lcd-7"

// ============================================================================
// LCD — 7" 800x480 IPS, parallel RGB565 (EK9716-class RGB TCON)
// ============================================================================

#define LCD_WIDTH               800
#define LCD_HEIGHT              480

#define LCD_PIN_DE              5
#define LCD_PIN_VSYNC           3
#define LCD_PIN_HSYNC           46
#define LCD_PIN_PCLK            7

// RGB565 data lines (5-6-5): R3..R7, G2..G7, B3..B7 (same map as the 4.3").
#define LCD_PIN_R3              1
#define LCD_PIN_R4              2
#define LCD_PIN_R5              42
#define LCD_PIN_R6              41
#define LCD_PIN_R7              40

#define LCD_PIN_G2              39
#define LCD_PIN_G3              0
#define LCD_PIN_G4              45
#define LCD_PIN_G5              48
#define LCD_PIN_G6              47
#define LCD_PIN_G7              21

#define LCD_PIN_B3              14
#define LCD_PIN_B4              38
#define LCD_PIN_B5              18
#define LCD_PIN_B6              17
#define LCD_PIN_B7              10

// Panel timings (Waveshare 7" demo starting values — VERIFY per board rev;
// the 7" often runs PCLK ~21 MHz, but 16 MHz gives PSRAM-bandwidth headroom).
#define LCD_PCLK_HZ             16000000
#define LCD_HSYNC_PULSE         4
#define LCD_HSYNC_BACK_PORCH    8
#define LCD_HSYNC_FRONT_PORCH   8
#define LCD_VSYNC_PULSE         4
#define LCD_VSYNC_BACK_PORCH    16
#define LCD_VSYNC_FRONT_PORCH   16

// ============================================================================
// TOUCH — GT911 5-point capacitive, I2C (same controller as the 4.3" Dash)
// ============================================================================

#define I2C_PIN_SDA             8
#define I2C_PIN_SCL             9
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

#define TOUCH_I2C_ADDR          0x5D  // GT911 primary (0x14 alternate)
#define TOUCH_I2C_ADDR_ALT      0x14
#define TOUCH_PIN_INT           4     // VERIFY per board rev
#define TOUCH_PIN_RST           -1    // via CH422G, not a native GPIO
#define TOUCH_MAX_POINTS        5     // full 5-point report — wire all of them

// ============================================================================
// CH422G — I2C IO expander (write-only device, fixed addresses)
// ============================================================================
//
// Owns the panel-adjacent control lines (bits per Waveshare's demo — VERIFY):
//   EXIO1 = touch reset, EXIO2 = backlight/display enable.
// The GT911 must be reset via EXIO1 before it will enumerate on I2C.

#define CH422G_ADDR_SYS         0x24  // system/mode register
#define CH422G_ADDR_OUT         0x38  // output latch (EXIO0..7)
#define CH422G_BIT_TOUCH_RST    (1 << 1)
#define CH422G_BIT_BACKLIGHT    (1 << 2)

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

#define BOOT_BUTTON_PIN         -1    // GPIO0 is claimed by LCD_PIN_G3; strap circuit only
#define BOOT_BUTTON_ACTIVE      LOW

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

#define HAS_CAMERA              0     // No camera — this Canary shows, it doesn't watch
#define HAS_MICROPHONE          0     // No microphone — quiet-room-safe by construction
#define HAS_SD_CARD             1     // TF slot. No SD data pins declared yet: this
                                      // board's CH422G map is still VERIFY, so the deep
                                      // archive (fleet/sd_archive.cpp) #errors here if
                                      // forced on. Pin the slot's routing against the
                                      // vendor schematic, add SD_PIN_SCK/MOSI/MISO (and
                                      // the DAT3 EXIO bit), then it lights up like 4.3
#define HAS_PSRAM               1     // 8 MB octal — REQUIRED for the 800x480 framebuffer
#define HAS_USB_CDC             1
#define HAS_NATIVE_USB          1
#define HAS_WIFI                1
#define HAS_BLE                 1
#define HAS_DISPLAY             1     // 800x480 RGB565 parallel
#define HAS_TOUCH               1     // GT911 5-point
#define HAS_RGBLED              0
#define HAS_RTC                 0
#define HAS_BATTERY             0     // mains/USB-C powered
#define HAS_BACKLIGHT_PWM       0     // CH422G on/off only — dark rendering for night
#define HAS_CAN_RS485           1     // broken out (unused)
