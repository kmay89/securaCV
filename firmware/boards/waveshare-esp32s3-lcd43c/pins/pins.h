/**
 * @file pins.h
 * @brief Pin definitions for the Waveshare ESP32-S3-Touch-LCD-4.3C
 *        ("AI voice" SKU — the MIC-BEARING Canary Dash variant)
 *
 * Integrated board: ESP32-S3 (16 MB flash / 8 MB octal PSRAM) driving a 4.3"
 * 800x480 IPS panel (ST7701-class controller) over the S3's parallel RGB565
 * LCD peripheral, a GT911 5-point capacitive touch controller (I2C), a
 * CH422G I2C IO expander — and, unlike every other display in the family,
 * an ES8311 audio codec + ES7210 ADC fronting a DUAL-MIC ARRAY.
 *
 * ⚠️ PRIVACY SURFACE — READ THIS FIRST. This board physically carries
 *    microphones, so the family's "no microphone by construction" posture
 *    does NOT hold here; it is a DISTINCT privacy surface (same rule that
 *    separates Sense from Sense-Wellbeing) with its own build env and its
 *    own OTA product, never cross-installed with the mic-free dashes.
 *    What the firmware does — and provably cannot do — with these mics is
 *    specified in docs/hardware/display_mic_variant.md: acoustic ALARM
 *    PATTERNS only (smoke T3 / CO T4 cadences), scalars-not-samples, OFF
 *    by default, hard mute = I2S driver uninstalled, and an on-glass
 *    indicator that is lit exactly while the driver runs.
 *
 * ⚠️ DEV STATUS: compile-verified only — NOT yet validated on bench
 *    hardware. Shares the 4.3's RGB control pins (DE=5 VS=3 HS=46 PCLK=7),
 *    16-bit data GPIO set, and I2C 8/9 with GT911 + CH422G per the sibling
 *    note in waveshare-esp32s3-lcd43/pins/pins.h. DIFFERENCES vs that map:
 *      - ST7701 panel controller (VERIFY timings; may need an init
 *        sequence — see the board README's bring-up section).
 *      - LCD_RST rides an extra CH422G bit — drive it HIGH at init
 *        (bit position VERIFY: EXIO3 assumed, mirroring the 4.3B).
 *      - ES8311 + ES7210 audio stack. The I2S pins are deliberately -1
 *        until read off the vendor wiki/schematic AT THE BENCH: the mic
 *        layer refuses to start while any pin is -1 and says so on the
 *        glass and the console — a guessed microphone pin map is the one
 *        thing this file must never carry.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_WAVESHARE_ESP32S3_LCD43C
#define BOARD_WAVESHARE_ESP32S3_LCD43C 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Waveshare ESP32-S3-Touch-LCD-4.3C"
#define BOARD_ID                "waveshare-esp32s3-lcd43c"
#define BOARD_VENDOR            "Waveshare"
#define BOARD_MCU               "ESP32-S3"
#define BOARD_VARIANT           "esp32s3-touch-lcd-4.3c"

// ============================================================================
// LCD — 4.3" 800x480 IPS, parallel RGB565 (ST7701-class panel — VERIFY)
// ============================================================================

#define LCD_WIDTH               800
#define LCD_HEIGHT              480

#define LCD_PIN_DE              5
#define LCD_PIN_VSYNC           3
#define LCD_PIN_HSYNC           46
#define LCD_PIN_PCLK            7

// RGB565 data lines (5-6-5): R3..R7, G2..G7, B3..B7 — same set as the 4.3.
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

// Panel timings (starting values carried from the 4.3's map — the C's
// ST7701 controller may want different porches. VERIFY at bench; if the
// glass stays dark, the ST7701 init-sequence work in the board README's
// bring-up section is the follow-up).
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
// Same expander scheme as the plain 4.3 (VERIFY bits on the C revision),
// PLUS the C's panel reset: the ST7701's LCD_RST rides an extra expander
// bit that must be driven HIGH at init (per the family sibling notes).
// Bit position VERIFY — EXIO3 assumed, mirroring the 4.3B's LCD-reset bit.

#define CH422G_ADDR_SYS         0x24  // system/mode register
#define CH422G_ADDR_OUT         0x38  // output latch (EXIO0..7)
#define CH422G_BIT_TOUCH_RST    (1 << 1)
#define CH422G_BIT_BACKLIGHT    (1 << 2)
#define CH422G_BIT_LCD_RST      (1 << 3)  // ST7701 reset — drive HIGH (VERIFY bit)
#define CH422G_BIT_USB_SEL      (1 << 4)
#define CH422G_BIT_SD_CS        (1 << 5)

// ============================================================================
// AUDIO — ES8311 codec + ES7210 ADC fronting the dual-MIC array
// ============================================================================
//
// The C's microphones sit behind an ES7210 4-channel ADC (the mic front
// end) with an ES8311 codec alongside (speaker path), both on the shared
// I2C bus. Standard silicon addresses below (VERIFY with the playground /
// debug-mode I2C census — the census names them when they ACK).
//
// The I2S capture pins are DELIBERATELY -1: Waveshare's public material
// for the C does not land the audio GPIOs unambiguously, and a guessed
// microphone pin is the one guess this repo must never ship. The mic layer
// (canary/io/mic_alarm) refuses to start while any pin is -1, states so on
// the glass ("mic: pins unset") and the console, and the board README's
// bench section walks through reading the real GPIOs off the vendor
// wiki/schematic and filling these in. Until then the mics are provably
// un-driven: no driver install ever happens.

#define AUDIO_ES8311_ADDR       0x18  // codec (VERIFY via I2C census)
#define AUDIO_ES7210_ADDR       0x40  // mic ADC (VERIFY via I2C census)

// ============================================================================
// RTC — PCF85063 (vendor-listed for the C; NOT the family's PCF8563)
// ============================================================================
//
// The C carries a PCF85063 per Waveshare's product material. Same I2C
// address as the family's PCF8563 (0x51) but a DIFFERENT register map
// (time registers start at 0x04, control regs differ) — the existing
// FEATURE_RTC layer (canary/io/rtc_pcf.h) targets the PCF8563 and would
// misread this part. Do NOT enable FEATURE_RTC on this board until the
// PCF85063 register variant lands; the I2C census confirming an ACK at
// 0x51 is the bench signal that the silicon is really there.

#define RTC_I2C_ADDR            0x51  // PCF85063 (VERIFY via I2C census)

#define AUDIO_PIN_I2S_MCLK      -1    // VERIFY: fill from vendor schematic at bench
#define AUDIO_PIN_I2S_SCLK      -1    // VERIFY: fill from vendor schematic at bench
#define AUDIO_PIN_I2S_LRCK      -1    // VERIFY: fill from vendor schematic at bench
#define AUDIO_PIN_I2S_SDIN      -1    // VERIFY: mic data in — fill at bench
#define AUDIO_PIN_PA_ENABLE     -1    // speaker amp enable, if routed (VERIFY)

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

// BOOT button (GPIO0 is claimed by LCD_PIN_G3 on this board; Waveshare
// routes BOOT through the usual strap circuit — no runtime button in v0.1).
#define BOOT_BUTTON_PIN         -1
#define BOOT_BUTTON_ACTIVE      LOW

// No piezo pad on the C: audible feedback belongs to the ES8311 speaker
// path (unused by this firmware), and GPIO6 may be claimed by the audio
// stack (VERIFY). Chime stays disabled.
#define BUZZER_PIN              -1

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

#define HAS_CAMERA              0     // No camera — this Canary shows, it doesn't watch
#define HAS_MICROPHONE          1     // DUAL-MIC ARRAY — a distinct privacy surface.
                                      // Off by default; alarm-pattern scalars only;
                                      // hard mute = driver uninstalled. See
                                      // docs/hardware/display_mic_variant.md.
#define HAS_AUDIO_CODEC         1     // ES8311 + ES7210 on the shared I2C bus
#define HAS_SD_CARD             1     // TF slot (unused in v0.1)
#define HAS_PSRAM               1     // 8 MB octal — hosts the 800x480 framebuffer
#define HAS_USB_CDC             1
#define HAS_WIFI                1
#define HAS_BLE                 1     // passive Chirp scan fallback, same as siblings
#define HAS_DISPLAY             1     // 800x480 RGB565 parallel (ST7701 — VERIFY init)
#define HAS_TOUCH               1     // GT911 5-point
#define HAS_RTC                 1     // PCF85063 (vendor-listed) — but see the
                                      // RTC section: the family's FEATURE_RTC
                                      // layer targets the PCF8563 register map
                                      // and must NOT be enabled here until the
                                      // PCF85063 variant lands. Silicon != driver.
#define HAS_BATTERY             0     // demo UI shows a battery glyph; no charge
                                      // silicon confirmed — VERIFY before claiming
#define HAS_BACKLIGHT_PWM       0     // CH422G on/off only
#define HAS_CAN_RS485           0     // the BOX edition DOES expose a screw-
                                      // terminal strip (visible on the case edge);
                                      // its functions are unverified — read them
                                      // off the vendor wiki/schematic at bench
                                      // before declaring RS485/CAN/GPIO here
