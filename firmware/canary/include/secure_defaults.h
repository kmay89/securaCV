/*
 * SecuraCV Canary — Security Defaults: where each one actually lives
 *
 * DOCUMENTATION ONLY. No source includes this file and it defines nothing.
 *
 * Until 2026-10-09 it defined DEFAULT_* macros (DEFAULT_BLE_ENABLED,
 * DEFAULT_TLS_REQUIRED, DEFAULT_SECURE_BOOT ...) and an #error block "to
 * enforce" them. Nothing included the header and no build defined
 * SECURACV_ENFORCE_SECURE_DEFAULTS, so none of its fourteen macros reached
 * compiled code, and the #error block compared the header's constants to
 * themselves, so it could not fire on a real setting even when enabled.
 * Several values also contradicted the shipped firmware (TLS "required" on
 * an image that serves HTTP; secure boot and flash encryption "1" on boards
 * that ship without either). A file that reads as enforcement and enforces
 * nothing is worse than no file, so the macros went and the map below
 * stayed: each principle, and the knob that really decides it.
 *
 * To weaken any of these, a developer MUST:
 *   1. Document the justification in the commit message
 *   2. Add an entry to firmware/LESSONS_LEARNED.md
 *   3. Get explicit approval referencing docs/security/THREAT_MODEL.md
 *   4. Verify the change does not affect the most vulnerable user class
 *
 * firmware/scripts/regression_check.sh ("Security: hardened defaults") holds
 * the BLE, MQTT and SoftAP-client knobs below to these values in the files
 * that set them; the rest are named here, not checked by it.
 *
 * See: docs/security/THREAT_MODEL.md — "The Ten Security Principles"
 * See: docs/security/SECURITY_MODEL.md — User-facing security guarantees
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
//
// That is the SOFTWARE boundary. At-rest confidentiality against a bench
// read of the flash is a separate, opt-in tier (Tier 3+ of
// docs/design/hardware_root_of_trust.md), and it needs NVS encryption on top
// of flash encryption: flash encryption alone does not cover NVS, where the
// key lives, and NVS encryption is not available in this framework = arduino
// build. So on every board this image runs on — fused or not — the key's NVS
// is plaintext. The default is stated in docs/security/SECURITY_MODEL.md
// ("Physical extraction and the flash-encryption default") and reported live
// by the device as `key_at_rest` (common/identity/key_at_rest.h) — never
// assumed.

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 2: ZERO PHONE-HOME
// ════════════════════════════════════════════════════════════════════
// MQTT is an outbound connection, so it is compiled out by default:
// canary_config.h's FEATURE_HA_MQTT defaults to 0, and only the *_ha images
// (release_ha, dev_ha, provisioning's secure_ha) set it. Even there the
// client connects only to a broker the owner provisions (the setup wizard's
// hub step, POST /api/mqtt/config, stored in NVS); no broker is compiled in.

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 3: NO IDENTIFIER LEAKS
// ════════════════════════════════════════════════════════════════════
// BLE broadcasts enable tracking, and the BLE stack is closed-source
// attack surface (CVE-2025-27840). It is off in dev, release (and the board
// envs that extend it) and the secure envs. The code that brings the radio
// up is gated by FEATURE_BLE_SCAN=0 (set by dev, release and the secure envs;
// it has no default, and every reader tests `defined && 1`) and
// FEATURE_BLE_STATUS=0 (the platformio.ini [env] base flags), plus the [env]
// lib_ignore of securacv_ble_scan / securacv_ble_status; FEATURE_BLE
// (canary_config.h, default 0) only adds `ble` to the reported feature list.
// Only [env:full] compiles BLE in.
//
// MAC addresses: the BLE Scout ([env:full] only) stores a keyed SHA-256 of a
// beacon's MAC, never the MAC (lib/securacv_ble_scan/src/ble_scan.h), and the
// CSI HAL copies only subcarrier samples and aggregate RSSI/timing out of the
// driver's callback, so no MAC or BSSID enters its ring buffer (the privacy
// barrier note in common/csi/src/csi_hal.h).
//
// Time coarsening is canary_config.h's TIME_BUCKET_MS: the ten-minute grid,
// floor and default (Invariant III, IR-TIMEBUCKET), held there by
// regression_check.sh.

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 6: PRIVACY BY ARCHITECTURE
// ════════════════════════════════════════════════════════════════════
// Not build defaults today, stated so nobody assumes otherwise: release
// images keep the USB serial console (CORE_DEBUG_LEVEL=1, errors only), and
// JTAG is disabled only by the eFuses firmware/provisioning/ burns, on a
// device that goes through that kit.

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 7: MINIMAL ATTACK SURFACE
// ════════════════════════════════════════════════════════════════════
// One SoftAP client at a time: canary_config.h's AP_MAX_CONNECTIONS (1).
// TLS is NOT required everywhere: [env:dev] and [env:full] serve self-signed
// HTTPS on 443 with a port-80 redirect (FEATURE_HTTPS=1); release stays
// HTTP until the size guard shows the TLS stack fits its OTA slot
// (platformio.ini [env:release]); firmware/FEATURES.md has the per-image cell.

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 8: CRYPTOGRAPHIC MINIMALISM
// (No configuration — enforced by code. Only vetted primitives used.)
// ════════════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════════════
// PRINCIPLE 9: FAIL SECURE
// ════════════════════════════════════════════════════════════════════
// API authentication lockout: lib/securacv_auth/src/securacv_auth.h
// (SECURACV_AUTH_MAX_FAILURES 5, SECURACV_AUTH_LOCKOUT_BASE_MS 2 s,
// SECURACV_AUTH_LOCKOUT_MAX_MS 5 min). Hardware watchdog:
// canary_config.h's WATCHDOG_TIMEOUT_SEC. There is no periodic chain
// re-verify: the boot self-test and the sign-then-verify on every record
// are the checks (firmware/FEATURES.md, "Self-verification").

// ════════════════════════════════════════════════════════════════════
// HARDWARE SECURITY
// ════════════════════════════════════════════════════════════════════
// Secure Boot v2 and flash encryption are opt-in, through the provisioning
// kit (firmware/provisioning/: platformio_secure.ini, provision_canary.sh).
// Shipped dev/release images enable neither, and a device reports what it
// has (`key_at_rest`) rather than what this file once claimed.

#endif // SECURE_DEFAULTS_H
