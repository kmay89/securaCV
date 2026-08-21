/**
 * @file pins.h
 * @brief Pin definitions for the Seeed XIAO ESP32-S3 seated in the Seeed
 *        Round Display for XIAO ("Canary Watch" glance station)
 *
 * The Round Display for XIAO is a 39 mm disc carrying a 1.28" 240x240 GC9A01
 * TFT (SPI), a CST816S capacitive touch controller (I2C), a PCF8563 RTC
 * (I2C), a microSD slot (shared SPI), a JST-1.25 LiPo connector with charger,
 * and a back socket the XIAO seats into — zero wiring, like the Sense/Vision
 * stacks. Pin assignments follow Seeed's published wiki for the Round
 * Display (XIAO Dx labels), translated to the XIAO ESP32-S3's GPIO numbers:
 *
 *   D0=GPIO1  D1=GPIO2  D2=GPIO3  D3=GPIO4  D4=GPIO5(SDA)  D5=GPIO6(SCL)
 *   D6=GPIO43 D7=GPIO44 D8=GPIO7(SCK) D9=GPIO8(MISO) D10=GPIO9(MOSI)
 *
 * ⚠️ DEV STATUS: compile-verified only — NOT yet validated on bench
 *    hardware. Verify against your display revision before relying on the
 *    backlight/touch-int lines (Seeed has revised this board before).
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_XIAO_ESP32S3_ROUND
#define BOARD_XIAO_ESP32S3_ROUND 1
#endif

// Which physical board this is, in the fleet-figure vocabulary. Named here
// rather than in the build env because THIS header is what a build compiles
// against — the load-bearing declaration, so the id cannot drift away from
// the pins it travels with. canary::figures::my_figure() reads it.
#ifndef CANARY_FIGURE_HARDWARE
#define CANARY_FIGURE_HARDWARE "xiao-esp32s3-round"
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Seeed XIAO ESP32-S3 (Round Display)"
#define BOARD_ID                "xiao-esp32s3-round"
#define BOARD_VENDOR            "Seeed Studio"
#define BOARD_MCU               "ESP32-S3"
#define BOARD_VARIANT           "xiao-round-display"

// ============================================================================
// TFT — GC9A01 1.28" 240x240 round, SPI
// ============================================================================
//
// The GC9A01's reset line is not broken out on the Round Display (tied to
// the board reset); drivers must pass "no reset pin".

#define TFT_PIN_SCK             7     // D8
#define TFT_PIN_MOSI            9     // D10
#define TFT_PIN_MISO            8     // D9  (shared bus; GC9A01 is write-only)
#define TFT_PIN_CS              2     // D1
#define TFT_PIN_DC              4     // D3
#define TFT_PIN_RST             -1    // not broken out — board reset only
#define TFT_PIN_BL              43    // D6, backlight enable (PWM-dimmable).
                                      // VERIFY at bench (Track W4): Seeed has
                                      // revised this board's backlight line
                                      // before — confirm against your revision.
#define TFT_BL_ACTIVE_HIGH      1
#define TFT_WIDTH               240
#define TFT_HEIGHT              240
#define TFT_SPI_HZ              40000000  // GC9A01 handles 40 MHz writes

// ============================================================================
// TOUCH — CST816S capacitive, I2C
// ============================================================================

#define I2C_PIN_SDA             5     // D4
#define I2C_PIN_SCL             6     // D5
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

#define TOUCH_I2C_ADDR          0x15  // CST816S
#define TOUCH_PIN_INT           44    // D7 (active low). VERIFY at bench
                                      // (Track W3): confirm the INT line
                                      // against your display revision.
#define TOUCH_PIN_RST           -1    // not broken out

// ============================================================================
// RTC — PCF8563 (shared I2C). Unused in v0.1 (NTP is the time source);
// listed so a future flavor can keep time across router outages.
// ============================================================================

#define RTC_I2C_ADDR            0x51  // PCF8563

// ============================================================================
// microSD — shared SPI bus (deep archive NOT available on this flavor yet)
// ============================================================================
//
// The slot shares SCK/MOSI/MISO with the GC9A01 panel, which the HAL drives
// through Arduino_GFX's private bus handle (Arduino_ESP32SPI) — two masters,
// one set of pins, no shared transaction arbiter. Until panel + card ride one
// bus handle, FEATURE_SD_STORAGE must stay 0 here (sd_archive.cpp refuses it
// at compile time with this explanation). The dash TF slot, which has its own
// data pins, carries the deep archive first — fleet/sd_archive.h.

#define SD_PIN_CS               3     // D2

// ============================================================================
// ONBOARD PERIPHERALS (XIAO ESP32-S3)
// ============================================================================

// XIAO ESP32-S3 user LED (yellow, active LOW on GPIO21).
#define LED_STATUS_PIN          21
#define LED_ACTIVE_HIGH         0

// Optional passive piezo (severity-tiered chime — display_trailblazer_spec
// §5). D0/GPIO1 is unused by the Round Display stack; pad is unpopulated
// on stock builds (FEATURE_CHIME=0 until fitted). VERIFY free at bench.
#define BUZZER_PIN              1

// BOOT button (GPIO0, active LOW) — reserved for a future factory-reset
// long-press; no gesture is wired to it yet. The on-glass path exists
// today: settings › reset › wifi › forget reopens the join wizard.
#define BOOT_BUTTON_PIN         0
#define BOOT_BUTTON_ACTIVE      LOW

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

#define HAS_CAMERA              0     // No camera — this Canary shows, it doesn't watch
#define HAS_MICROPHONE          0     // No microphone — bedroom-safe by construction
#define HAS_SD_CARD             1     // on the display board (archive blocked on the shared panel bus - see microSD note above)
#define HAS_PSRAM               1     // 8 MB on the XIAO ESP32-S3
#define HAS_USB_CDC             1
#define HAS_WIFI                1
#define HAS_BLE                 1     // future: passive Chirp scan fallback
#define HAS_DISPLAY             1     // GC9A01 240x240 round
#define HAS_TOUCH               1     // CST816S single-point + gestures
#define HAS_RTC                 1     // PCF8563 on the display board
#define HAS_BATTERY             1     // JST-1.25 + charger on the display board
#define HAS_BACKLIGHT_PWM       1     // TFT_PIN_BL is LEDC-dimmable
