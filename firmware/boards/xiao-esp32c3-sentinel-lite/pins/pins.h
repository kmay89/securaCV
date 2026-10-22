/**
 * @file pins.h
 * @brief Pin map — Canary Sentinel LITE tier (Seeed XIAO ESP32-C3 + PIR +
 *        BH1750 lux). NO radar module: Lite is PIR + WiFi-RF + BLE + light,
 *        with WiFi/BLE on the onboard radio. Pin numbers ONLY here; no logic.
 *
 * ASSUMPTIONS marked [BENCH] are confirmed on hardware (project README
 * checklist).
 */

#pragma once

#ifndef BOARD_XIAO_ESP32C3_SENTINEL_LITE
#define BOARD_XIAO_ESP32C3_SENTINEL_LITE 1
#endif

// Which physical board this is, in the fleet-figure vocabulary. Named here
// rather than in the build env because THIS header is what a build compiles
// against, so the id cannot drift away from the pins it travels with. It is
// what GET /api/status (`hardware`), the GET /api/fleet row (`hw`) and the
// mDNS TXT (`hw`) name, and the board a version-2 OTA manifest binds to
// (release manifests are version 1 today, which bind no board). The board
// has no figure yet (canary-local/devices/figures.json lists it under
// hardware.unmapped), so canary::figures::my_figure() returns nullptr.
#ifndef CANARY_FIGURE_HARDWARE
#define CANARY_FIGURE_HARDWARE "xiao-esp32c3-sentinel-lite"
#endif

#define BOARD_NAME    "Seeed XIAO ESP32-C3 (Sentinel Lite)"
#define BOARD_ID      "xiao-esp32c3-sentinel-lite"
#define BOARD_VENDOR  "Seeed Studio"
#define BOARD_MCU     "ESP32-C3"

// ── I2C (BH1750 ambient light) — XIAO C3 default I2C pins (D4/D5) ─────────────
#define I2C_SDA          6      // XIAO C3 D4 = GPIO6
#define I2C_SCL          7      // XIAO C3 D5 = GPIO7
#define BH1750_ADDR      0x23   // ADDR low (default); 0x5C when high

// ── PIR motion input ─────────────────────────────────────────────────────────
// [BENCH] GPIO3 (XIAO C3 D1) is a free digital pin clear of the I2C bus (6/7)
// and the USB/UART0 console. Active-HIGH push-pull PIR, INPUT (no pull).
#define PIR_PIN          3
#define PIR_ACTIVE_LEVEL HIGH
#define PIR_INPUT_MODE   INPUT

// ── Status LED — XIAO C3 onboard user LED (active LOW) on GPIO10 ─────────────
#define LED_USER_PIN     10
#define LED_ACTIVE_LOW   1

// ── Reserved ─────────────────────────────────────────────────────────────────
// GPIO11-17 are the in-package SPI flash on the ESP32-C3 — NEVER use.
#define PIN_RESERVED_FLASH_LO 11
#define PIN_RESERVED_FLASH_HI 17

// ── Capabilities (FEATURE_* vs HAS_* cross-check) ────────────────────────────
#define HAS_MMWAVE_RADAR  0     // Lite has no radar — honest tier limit
#define HAS_AMBIENT_LIGHT 1
#define HAS_PIR           1
#define HAS_WIFI          1     // 2.4 GHz — RF channel (CSI pipeline not wired on Lite)
#define HAS_BLE           1     // BLE — BLE channel
#define HAS_USB_CDC       1     // USB Serial/JTAG console
#define HAS_CAMERA        0
#define HAS_MICROPHONE    0
#define HAS_SD_CARD       0
#define HAS_PSRAM         0     // ESP32-C3 XIAO has no PSRAM
