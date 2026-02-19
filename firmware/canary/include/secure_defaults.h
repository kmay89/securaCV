/*
 * SecuraCV Canary — Compile-Time Security Defaults
 *
 * All security-sensitive configuration defaults are defined here.
 * Every default is hardened (most-secure value).
 *
 * To weaken any default, a developer MUST:
 *   1. Document the justification in the commit message
 *   2. Add an entry to firmware/LESSONS_LEARNED.md
 *   3. Get explicit approval referencing THREAT_MODEL.md
 *   4. Verify the change does not affect the most vulnerable user class
 *
 * See: THREAT_MODEL.md — "The Ten Security Principles"
 * See: SECURITY_MODEL.md — User-facing security guarantees
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURE_DEFAULTS_H
#define SECURE_DEFAULTS_H

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 1: KEYS NEVER LEAVE THE DEVICE
// ════════════════════════════════════════════════════════════════════
// No configuration needed — enforced by the absence of any export API.
// The Ed25519 private key is stored in NVS and has no read interface.
// This comment exists to document that the omission is intentional.

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 2: ZERO PHONE-HOME
// ════════════════════════════════════════════════════════════════════

// MQTT is an outbound connection — off by default.
// Enabling MQTT means the device will transmit data to a broker.
// Only enable for Home Assistant integration on trusted local networks.
#ifndef DEFAULT_MQTT_ENABLED
  #define DEFAULT_MQTT_ENABLED        0
#endif

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 3: NO IDENTIFIER LEAKS
// ════════════════════════════════════════════════════════════════════

// BLE broadcasts enable tracking. Off by default.
// Enabling BLE also pulls in Espressif closed-source BT stack
// (includes CVE-2025-27840 attack surface).
#ifndef DEFAULT_BLE_ENABLED
  #define DEFAULT_BLE_ENABLED         0
#endif

// Never store raw MAC addresses. Hash before any storage.
#ifndef DEFAULT_PRESENCE_STORE_RAW_MAC
  #define DEFAULT_PRESENCE_STORE_RAW_MAC  0
#endif

// GPS time coarsening bucket (milliseconds).
// 5000ms = 5-second buckets. Prevents correlation attacks
// via precise timestamp matching.
#ifndef DEFAULT_GPS_COARSENING_MS
  #define DEFAULT_GPS_COARSENING_MS   5000
#endif

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 6: PRIVACY BY ARCHITECTURE
// ════════════════════════════════════════════════════════════════════

// Serial output disabled in production.
// Prevents UART sniffing of debug data, tokens, or key material.
#ifndef DEFAULT_SERIAL_OUTPUT
  #define DEFAULT_SERIAL_OUTPUT       0
#endif

// JTAG disabled via eFuse in production.
// Prevents debug probe attachment and memory inspection.
#ifndef DEFAULT_JTAG_ENABLED
  #define DEFAULT_JTAG_ENABLED        0
#endif

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 7: MINIMAL ATTACK SURFACE
// ════════════════════════════════════════════════════════════════════

// Maximum simultaneous WiFi AP clients.
// 1 = only the device owner can connect.
// Higher values increase multi-client attack surface.
#ifndef DEFAULT_MAX_WIFI_CLIENTS
  #define DEFAULT_MAX_WIFI_CLIENTS    1
#endif

// TLS required for all API access. No HTTP fallback.
// Setting this to 0 would expose all API traffic in plaintext.
#ifndef DEFAULT_TLS_REQUIRED
  #define DEFAULT_TLS_REQUIRED        1
#endif

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 8: CRYPTOGRAPHIC MINIMALISM
// (No configuration — enforced by code. Only vetted primitives used.)
// ════════════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 9: FAIL SECURE
// ════════════════════════════════════════════════════════════════════

// Authentication lockout: seconds of backoff after max failed attempts.
// Exponential backoff starting from this base value.
#ifndef DEFAULT_AUTH_LOCKOUT_BASE_SEC
  #define DEFAULT_AUTH_LOCKOUT_BASE_SEC  2
#endif

// Maximum authentication attempts before lockout engages.
#ifndef DEFAULT_AUTH_MAX_ATTEMPTS
  #define DEFAULT_AUTH_MAX_ATTEMPTS    5
#endif

// Authentication lockout cap (seconds). Backoff will not exceed this.
#ifndef DEFAULT_AUTH_LOCKOUT_CAP_SEC
  #define DEFAULT_AUTH_LOCKOUT_CAP_SEC 300
#endif

// Watchdog timeout (seconds). Auto-recover from firmware hangs.
#ifndef DEFAULT_WATCHDOG_TIMEOUT_SEC
  #define DEFAULT_WATCHDOG_TIMEOUT_SEC 30
#endif

// Automatic chain integrity verification interval (seconds).
// 0 = disabled. 3600 = hourly check.
#ifndef DEFAULT_AUTO_CHAIN_VERIFY_SEC
  #define DEFAULT_AUTO_CHAIN_VERIFY_SEC  3600
#endif

// ════════════════════════════════════════════════════════════════════
// HARDWARE SECURITY (production builds)
// ════════════════════════════════════════════════════════════════════

// Secure Boot: verify firmware signature before execution.
// Without this, an adversary with physical access can flash
// arbitrary firmware.
#ifndef DEFAULT_SECURE_BOOT
  #define DEFAULT_SECURE_BOOT         1
#endif

// Flash Encryption: encrypt flash contents at rest.
// Without this, an adversary can read firmware and NVS data
// by reading the flash chip directly.
#ifndef DEFAULT_FLASH_ENCRYPTION
  #define DEFAULT_FLASH_ENCRYPTION    1
#endif

// ════════════════════════════════════════════════════════════════════
// STATIC ASSERTIONS — compile-time enforcement
// ════════════════════════════════════════════════════════════════════
// These ensure secure defaults are not accidentally overridden to
// insecure values during development without triggering a build error.

#if defined(SECURACV_ENFORCE_SECURE_DEFAULTS) && SECURACV_ENFORCE_SECURE_DEFAULTS

  #if DEFAULT_BLE_ENABLED != 0
    #error "SECURITY VIOLATION: BLE must be disabled by default (attack surface + tracking vector)"
  #endif

  #if DEFAULT_TLS_REQUIRED != 1
    #error "SECURITY VIOLATION: TLS must be required (no plaintext HTTP)"
  #endif

  #if DEFAULT_MQTT_ENABLED != 0
    #error "SECURITY VIOLATION: MQTT must be disabled by default (outbound connection)"
  #endif

  #if DEFAULT_PRESENCE_STORE_RAW_MAC != 0
    #error "SECURITY VIOLATION: Raw MAC storage must be disabled (privacy)"
  #endif

  #if DEFAULT_SERIAL_OUTPUT != 0
    #error "SECURITY VIOLATION: Serial output must be disabled in production"
  #endif

  #if DEFAULT_JTAG_ENABLED != 0
    #error "SECURITY VIOLATION: JTAG must be disabled in production"
  #endif

  #if DEFAULT_GPS_COARSENING_MS < 5000
    #error "SECURITY VIOLATION: GPS coarsening must be >= 5000ms (privacy)"
  #endif

  #if DEFAULT_SECURE_BOOT != 1
    #error "SECURITY VIOLATION: Secure boot must be enabled in production"
  #endif

  #if DEFAULT_FLASH_ENCRYPTION != 1
    #error "SECURITY VIOLATION: Flash encryption must be enabled in production"
  #endif

#endif // SECURACV_ENFORCE_SECURE_DEFAULTS

#endif // SECURE_DEFAULTS_H
