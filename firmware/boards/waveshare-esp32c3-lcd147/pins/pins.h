/**
 * @file pins.h
 * @brief Pin definitions for the Waveshare ESP32-C3-LCD-1.47
 *        ("Canary Nightlight" — pocket 1.47" clock/lamp/companion node)
 *
 * Integrated board: ESP32-C3FH4 (single-core RISC-V 160 MHz, Wi-Fi 4 +
 * BLE 5; 4 MB in-package flash, NO PSRAM) driving a 1.47" portrait ST7789T
 * IPS panel over 4-wire SPI, plus a QMI8658 6-axis IMU and a TF slot on the
 * shared bus. Pin assignments per Waveshare's own engineering-sample demo
 * (github.com/waveshareteam/ESP32-C3-LCD-1.47) and the schematic PDF it
 * ships — both agree, and the factory firmware runs this exact map.
 *
 * ⚠️ DEV STATUS: pin map compiled from vendor sources — NOT yet validated on
 *    bench hardware. Verify panel geometry and EXIO behavior against your
 *    board revision before relying on them.
 *
 * ⚠️ THE EXIO EXPANDER: unlike its C6/S3 siblings, this board routes LCD CS,
 *    LCD RST, SD CS and the BACKLIGHT PWM through Waveshare's I2C IO
 *    expander (addr 0x24 on the shared I2C bus, SCL=3/SDA=4 — the QMI8658
 *    lives there too). There is no LEDC backlight pin at all: brightness is
 *    a one-byte I2C register write (0..255) into the expander's PWM output.
 *    TFT_PIN_CS/RST/BL are therefore -1 here, and TFT_USE_EXIO tells the
 *    HAL to take the expander path (hal/display_1in47.cpp).
 *
 * ⚠️ PANEL GEOMETRY: Waveshare's marketing page says 172x320, but the
 *    vendor demo AND the factory firmware drive this glass as 180x320 with
 *    a 30-px column window into the ST7789's 240-wide RAM (30+180+30=240).
 *    We follow the code that ships on the board. If your glass proves to be
 *    the 172 variant the symptom is a 4-px junk band on each long edge —
 *    the fix is 172/34 below, one line each.
 *
 * ⚠️ PANEL INIT: the vendor init ends with MADCTL 0x48 (MX | BGR) and INVON
 *    — mirrored-X scan and BGR color order, unlike the stock ST7789 init.
 *    TFT_MADCTL_ROT0 carries that byte to the HAL; a swapped panel shows
 *    blue where it should show yellow, so bench-verify colors on first boot.
 *
 * ⚠️ NO PSRAM: the 180x320x2 = ~113 KB framebuffer fits internal SRAM, but
 *    there is no headroom for double-buffering; render lean (single buffer,
 *    dirty-region LVGL) exactly like the C6 nightstand.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_WAVESHARE_ESP32C3_LCD147
#define BOARD_WAVESHARE_ESP32C3_LCD147 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Waveshare ESP32-C3-LCD-1.47"
#define BOARD_ID                "waveshare-esp32c3-lcd147"
#define BOARD_VENDOR            "Waveshare"
#define BOARD_MCU               "ESP32-C3"
#define BOARD_VARIANT           "esp32c3-lcd-1.47"

// ============================================================================
// TFT — ST7789T 1.47" portrait, 4-wire SPI (RGB565), CS/RST/BL via EXIO
// ============================================================================

#define LCD_WIDTH               180   // vendor demo + factory firmware (see
#define LCD_HEIGHT              320   // the PANEL GEOMETRY note above)
#define TFT_WIDTH               180
#define TFT_HEIGHT              320

#define TFT_PIN_SCK             7
#define TFT_PIN_MOSI            5
#define TFT_PIN_MISO            6     // shared SPI (TF card); ST7789 is write-only
#define TFT_PIN_CS              -1    // EXIO IO0 — not a GPIO (see TFT_USE_EXIO)
#define TFT_PIN_DC              8
#define TFT_PIN_RST             -1    // EXIO IO1 — not a GPIO
#define TFT_PIN_BL              -1    // EXIO PWM register — not a GPIO
#define TFT_BL_ACTIVE_HIGH      1
#define TFT_SPI_HZ              80000000  // the vendor demo clocks 80 MHz writes

// ST7789 window offset (240-wide controller RAM cutout) at rotation 0.
#define TFT_COL_OFFSET          30
#define TFT_ROW_OFFSET          0

// Vendor-specific panel personality (consumed by hal/display_1in47.cpp):
// replay Waveshare's ST7789T init table after begin() — gamma/voltage set,
// INVON, and the 0x48 MADCTL (MX | BGR) the stock rotation-0 init lacks.
#define TFT_PANEL_VENDOR_INIT   1
#define TFT_MADCTL_ROT0         0x48  // MX | BGR, per the vendor init table

// ============================================================================
// EXIO — Waveshare I2C IO expander (LCD CS/RST, SD CS, backlight PWM)
// ============================================================================

#define TFT_USE_EXIO            1
#define EXIO_I2C_ADDR           0x24
#define EXIO_IO_LCD_CS          0     // expander IO0
#define EXIO_IO_LCD_RST         1     // expander IO1
#define EXIO_IO_SD_CS           2     // expander IO2

// ============================================================================
// I2C (shared bus: EXIO expander + QMI8658 IMU + broken-out header)
// ============================================================================

#define I2C_PIN_SCL             3
#define I2C_PIN_SDA             4
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

// ============================================================================
// IMU — QMI8658 6-axis (accel + gyro), unused in v0.1 (tap-wake follow-up)
// ============================================================================

#define IMU_QMI8658             1
#define IMU_I2C_ADDR            0x6A  // SA0-high address; 0x6B if strapped low
#define IMU_INT_PIN             2

// ============================================================================
// microSD — shared SPI bus, CS via EXIO (event cache; unused in v0.1)
// ============================================================================

#define SD_PIN_CLK              7
#define SD_PIN_MOSI             5
#define SD_PIN_MISO             6
#define SD_PIN_CS               -1    // EXIO IO2 — not a GPIO

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

#define BOOT_BUTTON_PIN         9     // BOOT/user (also a strapping pin)

// No piezo pad on this board — the chime engine compiles (CI coverage /
// the emulator's forced FEATURE_CHIME) and chime_init(-1) no-ops, the
// same unpopulated-pad convention the C6 nightstand carries.
#define BUZZER_PIN              -1
#define BOOT_BUTTON_ACTIVE      LOW

// Broken-out header: GPIO0, GPIO1, GPIO10 + UART0 (GPIO20 RX / GPIO21 TX).

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

#define HAS_CAMERA              0     // No camera — this Canary shows, it doesn't watch
#define HAS_MICROPHONE          0     // No microphone — quiet-room-safe by construction
#define HAS_SD_CARD             1     // TF slot (unused in v0.1)
#define HAS_PSRAM               0     // none — lean single-buffer rendering
#define HAS_USB_CDC             1     // USB-Serial/JTAG only (no USB-OTG on C3)
#define HAS_NATIVE_USB          0
#define HAS_WIFI                1     // Wi-Fi 4 (2.4 GHz)
#define HAS_BLE                 1
#define HAS_THREAD_ZIGBEE       0
#define HAS_DISPLAY             1     // 180x320 ST7789T SPI (EXIO CS/RST/BL)
#define HAS_TOUCH               0     // no touch — the BOOT button is the input
#define HAS_RGBLED              0     // no WS2812 on this board: the GLASS is the lamp
#define HAS_IMU                 1     // QMI8658 (unused in v0.1)
#define HAS_RTC                 0
#define HAS_BATTERY             0     // USB-C powered (VBUS→VSYS diode-OR, no charger IC)
#define HAS_BACKLIGHT_PWM       1     // via the EXIO expander's PWM register (I2C)
