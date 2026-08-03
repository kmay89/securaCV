/**
 * @file pins.h
 * @brief Pin definitions for the Seeed XIAO ESP32-C6 + MR60BHA2 mmWave kit
 *
 * This file contains all hardware pin mappings for the Seeed Studio MR60BHA2
 * 60GHz mmWave kit, whose host MCU is a XIAO ESP32-C6. Pin assignments mirror
 * Seeed's published ESPHome reference YAML for the kit
 * (`xiao-esphome-projects`: UART 115200 on GPIO16/17, I2C on GPIO22/23,
 * WS2812 on GPIO1) so a native firmware can drive the exact same wiring the
 * stock ESPHome image uses.
 *
 * @note This file must NOT contain any logic - only pin definitions.
 */

#pragma once

#ifndef BOARD_XIAO_ESP32C6_MR60
#define BOARD_XIAO_ESP32C6_MR60 1
#endif

// ============================================================================
// BOARD IDENTIFICATION
// ============================================================================

#define BOARD_NAME              "Seeed XIAO ESP32-C6 (MR60BHA2 kit)"
#define BOARD_ID                "xiao-esp32c6-mr60"
#define BOARD_VENDOR            "Seeed Studio"
#define BOARD_MCU               "ESP32-C6"
#define BOARD_VARIANT           "xiao-mr60bha2"

// ============================================================================
// RADAR UART (MR60BHA2 mmWave module)
// ============================================================================
//
// The radar module exposes its pre-digested scalar claims (presence, target
// count, distance, breath/heart rate) over a plain UART at 115200 8N1 — the
// raw radar IQ never crosses this link. Pins per Seeed's kit reference YAML.
//
// Naming is from the HOST's perspective: RADAR_UART_TX is the host pin that
// transmits toward the radar; RADAR_UART_RX receives frames from the radar.

#define RADAR_UART_TX           16    // host TX -> radar RX
#define RADAR_UART_RX           17    // host RX <- radar TX
#define RADAR_UART_BAUD         115200
// 8N1: 8 data bits, no parity, 1 stop bit (Arduino SERIAL_8N1).
#define RADAR_UART_NUM          1     // use UART1; UART0 stays on USB-CDC console

// ============================================================================
// I2C (BH1750 ambient light sensor)
// ============================================================================
//
// The kit's BH1750 lux sensor shares the I2C bus. Address and pins per the
// Seeed reference YAML. Lux feeds tamper corroboration ("lights-out + presence").

#define I2C_PIN_SDA             22
#define I2C_PIN_SCL             23
#define I2C_FREQ_DEFAULT        100000
#define I2C_FREQ_FAST           400000

#define BH1750_I2C_ADDR         0x23  // ADDR pin low (default); 0x5C when high

// ============================================================================
// ONBOARD PERIPHERALS
// ============================================================================

// WS2812 addressable RGB LED — single pixel, used for status/identify.
#define LED_WS2812_PIN          1
#define LED_WS2812_COUNT        1
#define LED_ACTIVE_HIGH         1

// Boot button.
//
// ASSUMPTION: The XIAO ESP32-C6 board does not break out a dedicated, labeled
// user button the way a DevKit does; the common XIAO/ESP32-C convention (and
// the strapping pin used for download mode on this part) is GPIO9. We map BOOT
// to GPIO9 (active LOW, internal pull-up) to match that convention until bench
// hardware confirms it. If the spike shows otherwise, fix this one line — no
// other layer references the raw pin number.
#define BOOT_BUTTON_PIN         9
#define BOOT_BUTTON_ACTIVE      LOW

// ============================================================================
// UART0 — USB-CDC console
// ============================================================================
//
// The ESP32-C6 USB Serial/JTAG peripheral provides the boot/log console.
// Listed for documentation; the Arduino core wires `Serial` to it.

// ============================================================================
// PIN VALIDATION / RESERVED
// ============================================================================
//
// GPIO24-30 are used for in-package SPI flash on the ESP32-C6 — NEVER use.
#define PIN_RESERVED_FLASH_LO   24
#define PIN_RESERVED_FLASH_HI   30

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

#define HAS_CAMERA              0     // No camera — radar-native, by design
#define HAS_MICROPHONE          0     // No microphone
#define HAS_SD_CARD             0     // No built-in SD slot
#define HAS_PSRAM               0     // ESP32-C6 XIAO has no PSRAM
#define HAS_USB_CDC             1
#define HAS_WIFI                1     // WiFi 6 (2.4 GHz)
#define HAS_BLE                 1     // BLE 5
#define HAS_IEEE802154          1     // Thread / Zigbee capable (unused here)
#define HAS_MMWAVE_RADAR        1     // MR60BHA2 60GHz FMCW over UART
#define HAS_AMBIENT_LIGHT       1     // BH1750 over I2C
#define HAS_RGB_LED             1     // WS2812

