/**
 * @file pins.h
 * @brief Pin definitions for the Waveshare ESP32-S3-Touch-AMOLED-2.06
 *        ("the Tin Can" — wrist-worn kid watch, docs/design/canary_tincan_kids_watch.md)
 *
 * Watch-shaped ESP32-S3R8 board: 2.06" 410x502 AMOLED (CO5300) on a 4-data-lane
 * QSPI bus, FT3168 capacitive touch, QMI8658 6-axis IMU, PCF85063 RTC, AXP2101
 * PMU + Li-po, and an ES8311 codec / ES7210 AEC with a dual digital mic array.
 *
 * Pin map transcribed from the VENDOR SAMPLE TREE, not from a datasheet
 * reading: waveshareteam/ESP32-S3-Touch-AMOLED-2.06 —
 * `examples/arduino/libraries/Mylibrary/pin_config.h` (panel, touch, I2C, SD)
 * and `examples/arduino/08_ES8311/08_ES8311.ino` (the I2S pin order and the
 * PA-enable line).
 *
 * ⚠️ DEV STATUS: transcribed from vendor sources — NOT yet validated on bench
 *    hardware. Verify against your board revision before trusting a pin.
 *
 * ⚠️ TOUCH IS FT3168, not CST9220. Waveshare's own store copy says CST9220;
 *    every sample sketch in the vendor tree drives an **FT3168** at 0x38 via
 *    Arduino_DriveBus. Vendor code beats vendor marketing — but this is the
 *    first thing to confirm at bring-up, because a wrong touch driver is a
 *    dead watch, not a degraded one.
 *
 * ⚠️ NO HAPTIC MOTOR ON THE BOARD. The Tin Can's whole premise is a knock you
 *    *feel*, so a DRV2605L + LRA on the exposed I²C port is required hardware,
 *    not an accessory. HAS_HAPTIC is 0 here and the build defines
 *    TINCAN_HAPTIC_I2C_ADDR when one is fitted; the firmware probes at boot and
 *    degrades honestly (see canary/hal/haptics.h).
 *
 * ⚠️ MICS ARE PRESENT AND DELIBERATELY UNUSED. HAS_MICROPHONE is 0 — it
 *    describes what this firmware may touch, not what is soldered down. The
 *    Tin Can carries no speech by construction; the I2S *input* pin is recorded
 *    below for completeness and must not be wired to a capture path.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_WAVESHARE_ESP32S3_AMOLED206
#define BOARD_WAVESHARE_ESP32S3_AMOLED206 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Waveshare ESP32-S3-Touch-AMOLED-2.06"
#define BOARD_ID                "waveshare-esp32s3-amoled206"
#define BOARD_VENDOR            "Waveshare"
#define BOARD_MCU               "ESP32-S3"
#define BOARD_VARIANT           "esp32s3-touch-amoled-2.06"

// ============================================================================
// AMOLED — CO5300 410x502, QSPI (1 clock + 4 data + CS + RST)
// ============================================================================

#define LCD_WIDTH               410
#define LCD_HEIGHT              502
#define LCD_IS_AMOLED           1

#define LCD_PIN_SDIO0           4
#define LCD_PIN_SDIO1           5
#define LCD_PIN_SDIO2           6
#define LCD_PIN_SDIO3           7
#define LCD_PIN_SCLK            11
#define LCD_PIN_CS              12
#define LCD_PIN_RST             8

// AMOLED has no backlight — brightness is a panel command, not a PWM pin.
#define LCD_PIN_BL              -1
#define LCD_BRIGHTNESS_IS_CMD   1

// ============================================================================
// I²C — one bus: touch, IMU, RTC, PMU, and the haptic add-on
// ============================================================================

#define I2C_PIN_SDA             15
#define I2C_PIN_SCL             14
#define I2C_BUS_HZ              400000

#define TOUCH_PIN_INT           38
#define TOUCH_PIN_RST           9
#define TOUCH_I2C_ADDR          0x38   // FT3168 (see the warning above)

#define IMU_I2C_ADDR            0x6B   // QMI8658, low-address strap
#define RTC_I2C_ADDR            0x51   // PCF85063
#define PMU_I2C_ADDR            0x34   // AXP2101

// The DRV2605L the Tin Can requires. Fixed address; the part has no straps.
#define HAPTIC_I2C_ADDR         0x5A

// ============================================================================
// AUDIO — ES8311 codec + ES7210 AEC, dual digital mics
//
// Recorded for completeness and for the PA-enable line, which is what lets a
// knock be *heard* when no motor is fitted. The mic input pin is documented
// and unused: this firmware never opens a capture path (Invariant I posture,
// docs/hardware/display_mic_variant.md).
// ============================================================================

#define I2S_PIN_BCLK            41
#define I2S_PIN_WS              45
#define I2S_PIN_DOUT            40
#define I2S_PIN_DIN             42     // mics — NEVER opened by this firmware
#define I2S_PIN_MCLK            16
#define AUDIO_PIN_PA_EN         46     // drive HIGH to enable the output amp
#define AUDIO_CODEC_I2C_ADDR    0x18   // ES8311

// ============================================================================
// microSD — SDMMC 1-bit
// ============================================================================

#define SD_PIN_CLK              2
#define SD_PIN_CMD              1
#define SD_PIN_D0               3
#define SD_PIN_CS               17

// ============================================================================
// CAPABILITY FLAGS
//
// These describe what the FIRMWARE MAY TOUCH, which is not always what is
// soldered down. HAS_MICROPHONE is the load-bearing example: the mics exist
// and this firmware must never open them.
// ============================================================================

#define HAS_CAMERA              0
#define HAS_MICROPHONE          0   // present on the board; refused by design
#define HAS_SD_CARD             1
#define HAS_PSRAM               1   // 8 MB octal (ESP32-S3R8)
#define HAS_USB_CDC             1
#define HAS_WIFI                1
#define HAS_BLE                 1

#define HAS_AMOLED              1
#define HAS_TOUCH               1
#define HAS_IMU                 1
#define HAS_RTC                 1
#define HAS_PMU                 1
#define HAS_BATTERY             1
#define HAS_SPEAKER             1   // codec + PA enable; transducer fit varies
#define HAS_HAPTIC              0   // required add-on, not onboard — see above
