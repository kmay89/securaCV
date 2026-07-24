/**
 * @file pins.h
 * @brief Pin map — Canary Sentinel STANDARD tier (Seeed XIAO ESP32-C6 +
 *        MR60BHA2 60GHz radar + PIR + BH1750 lux).
 *
 * Reuses the canary-sense MR60BHA2 wiring (radar UART on GPIO16/17, I2C on
 * GPIO22/23, WS2812 on GPIO1 — Seeed's kit reference) and adds a PIR motion
 * input. Pin numbers ONLY here (firmware/ARCHITECTURE.md); no logic.
 *
 * ASSUMPTIONS marked [BENCH] are confirmed on hardware (project README
 * checklist). If a spike shows otherwise, fix the one line — no other layer
 * references the raw pin number.
 */

#pragma once

#ifndef BOARD_XIAO_ESP32C6_SENTINEL
#define BOARD_XIAO_ESP32C6_SENTINEL 1
#endif

#define BOARD_NAME    "Seeed XIAO ESP32-C6 (Sentinel Standard)"
#define BOARD_ID      "xiao-esp32c6-sentinel"
#define BOARD_VENDOR  "Seeed Studio"
#define BOARD_MCU     "ESP32-C6"

// ── Radar UART (MR60BHA2) — host perspective, per Seeed kit reference ────────
#define RADAR_UART_TX    16      // host TX -> radar RX
#define RADAR_UART_RX    17      // host RX <- radar TX
#define RADAR_UART_BAUD  115200
#define RADAR_UART_NUM   1       // UART1; UART0 stays on USB-CDC console

// ── I2C (BH1750 ambient light) ───────────────────────────────────────────────
#define I2C_SDA          22
#define I2C_SCL          23
#define BH1750_ADDR      0x23    // ADDR low (default); 0x5C when high

// ── PIR motion input ─────────────────────────────────────────────────────────
// [BENCH] GPIO2 is a free XIAO C6 digital pin clear of the radar UART (16/17),
// the I2C bus (22/23), the WS2812 (1) and the BOOT strap (9). Most PIR modules
// (HC-SR501 class) drive an active-HIGH push-pull output, so no pull is needed;
// INPUT is used rather than INPUT_PULLUP for that reason.
#define PIR_PIN          2
#define PIR_ACTIVE_LEVEL HIGH
#define PIR_INPUT_MODE   INPUT

// ── Status LED ───────────────────────────────────────────────────────────────
#define LED_WS2812_PIN   1
#define LED_WS2812_COUNT 1

// ── Reserved ─────────────────────────────────────────────────────────────────
// GPIO24-30 are in-package SPI flash on the ESP32-C6 — NEVER use.
#define PIN_RESERVED_FLASH_LO 24
#define PIN_RESERVED_FLASH_HI 30

// ── Capabilities (FEATURE_* vs HAS_* cross-check, core/feature_sanity.h) ──────
#define HAS_MMWAVE_RADAR  1
#define HAS_AMBIENT_LIGHT 1
#define HAS_PIR           1
#define HAS_WIFI          1     // WiFi 6 (2.4 GHz) — RF/CSI channels
#define HAS_BLE           1     // BLE 5 — BLE channel
#define HAS_CAMERA        0
#define HAS_MICROPHONE    0
