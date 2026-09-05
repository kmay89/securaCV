/**
 * @file secrets.h
 * @brief canary-wap takes NO compile-time credentials.
 *
 * This file exists because setup.sh has always created it and the Arduino
 * IDE workflow expects it to be present; nothing in the firmware includes it.
 * It used to advertise Wi-Fi, MQTT, AP-password and API-token macros that no
 * source ever read — including an AP_PASSWORD_CUSTOM "change this before
 * deployment" instruction that changed nothing.
 *
 * Where each credential actually lives:
 *   - Wi-Fi station credentials: provisioned over the captive portal at first
 *     boot and stored in NVS (canary_wap.ino, NVS_KEY_WIFI_SSID / _PASS).
 *   - The setup-AP password: derived per device from the private key
 *     (derive_ap_password) and shown on the serial banner and /enroll page.
 *   - The API token: derived per device from the private key
 *     (derive_api_token), stored in NVS, shown on the serial banner.
 *   - MQTT: configured at runtime through the device web UI.
 *
 * Leave this file empty. Adding macros here does nothing.
 */

#pragma once
