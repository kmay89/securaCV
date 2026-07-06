/**
 * @file pins.h
 * @brief Pin definitions for the Waveshare ESP32-S3-Touch-LCD-4.3
 *        ("Canary Dash" wall/desk dashboard)
 *
 * Integrated board: ESP32-S3 (16 MB flash / 8 MB octal PSRAM) driving a 4.3"
 * 800x480 IPS panel over the S3's parallel RGB565 LCD peripheral, a GT911
 * 5-point capacitive touch controller (I2C), and a CH422G I2C IO expander
 * that owns the touch-reset and backlight-enable lines. Pin assignments per
 * Waveshare's wiki/demo sources for the board.
 *
 * ⚠️ DEV STATUS: compile-verified only — NOT yet validated on bench
 *    hardware. Waveshare does not publish a full mechanical/electrical
 *    drawing; verify RGB timings and expander bits against your board
 *    revision before relying on them.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_WAVESHARE_ESP32S3_LCD43
#define BOARD_WAVESHARE_ESP32S3_LCD43 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Waveshare ESP32-S3-Touch-LCD-4.3"
#define BOARD_ID                "waveshare-esp32s3-lcd43"
#define BOARD_VENDOR            "Waveshare"
#define BOARD_MCU               "ESP32-S3"
#define BOARD_VARIANT           "esp32s3-touch-lcd-4.3"

// ============================================================================
// LCD — 4.3" 800x480 IPS, parallel RGB565 (ST7262-class panel)
// ============================================================================

#define LCD_WIDTH               800
#define LCD_HEIGHT              480

#define LCD_PIN_DE              5
#define LCD_PIN_VSYNC           3
#define LCD_PIN_HSYNC           46
#define LCD_PIN_PCLK            7

// RGB565 data lines (5-6-5): R3..R7, G2..G7, B3..B7.
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

// Panel timings (starting values from Waveshare's demo config — VERIFY).
#define LCD_PCLK_HZ             16000000
#define LCD_HSYNC_PULSE         4
#define LCD_HSYNC_BACK_PORCH    8
#define LCD_HSYNC_FRONT_PORCH   8
#define LCD_VSYNC_PULSE         4
#define LCD_VSYNC_BACK_PORCH    16
#define LCD_VSYNC_FRONT_PORCH   16

// ============================================================================
// TOUCH — GT911 5-point capacitive, I2C
// ============================================================================

#define I2C_PIN_SDA             8
#define I2C_PIN_SCL             9
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

#define TOUCH_I2C_ADDR          0x5D  // GT911 primary (0x14 alternate)
#define TOUCH_I2C_ADDR_ALT      0x14
#define TOUCH_PIN_INT           4
#define TOUCH_PIN_RST           -1    // via CH422G EXIO1, not a native GPIO

// ============================================================================
// CH422G — I2C IO expander (write-only device, fixed addresses)
// ============================================================================
//
// The expander owns the panel-adjacent control lines. Bits per Waveshare's
// demo sources (VERIFY on your revision):
//   EXIO1 = touch reset, EXIO2 = backlight/display enable,
//   EXIO4 = USB/CAN mux select, EXIO5 = SD_CS.
// NOTE: backlight through the expander is ON/OFF only — there is no PWM
// dimming path on this board. Night-mode UX must come from dark rendering
// plus scheduled backlight-off (see the display UX doc).

#define CH422G_ADDR_SYS         0x24  // system/mode register
#define CH422G_ADDR_OUT         0x38  // output latch (EXIO0..7)
#define CH422G_BIT_TOUCH_RST    (1 << 1)
#define CH422G_BIT_BACKLIGHT    (1 << 2)
#define CH422G_BIT_USB_SEL      (1 << 4)
#define CH422G_BIT_SD_CS        (1 << 5)

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

// BOOT button (GPIO0 is claimed by LCD_PIN_G3 on this board; Waveshare
// routes BOOT through the usual strap circuit — no runtime button in v0.1).
#define BOOT_BUTTON_PIN         -1
#define BOOT_BUTTON_ACTIVE      LOW

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

#define HAS_CAMERA              0     // No camera — this Canary shows, it doesn't watch
#define HAS_MICROPHONE          0     // No microphone — quiet-room-safe by construction
#define HAS_SD_CARD             1     // TF slot (unused in v0.1)
#define HAS_PSRAM               1     // 8 MB octal — hosts the 800x480 framebuffer
#define HAS_USB_CDC             1
#define HAS_WIFI                1
#define HAS_BLE                 1     // future: passive Chirp scan fallback
#define HAS_DISPLAY             1     // 800x480 RGB565 parallel
#define HAS_TOUCH               1     // GT911 5-point
#define HAS_RTC                 0
#define HAS_BATTERY             0     // mains/USB-C powered
#define HAS_BACKLIGHT_PWM       0     // CH422G on/off only — see note above
#define HAS_CAN_RS485           1     // terminal block (unused)
