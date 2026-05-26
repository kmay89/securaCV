/*
 * SecuraCV Canary — First-Time Setup & Device Identity
 *
 * Detects whether the device has been set up before (NVS flag) and
 * drives the onboarding flow:
 *
 *   1. First boot: AP SSID becomes "SecuraCV-Setup-XXXX", web UI
 *      redirects to a mobile-optimized setup wizard, captive portal
 *      DNS intercept triggers the phone's "sign in to network" dialog.
 *
 *   2. Setup wizard: WiFi scan → network select → password → test →
 *      optional device name → "Setup Complete" confirmation.
 *
 *   3. After successful WiFi connection: NVS flag set, device reboots
 *      into normal operation with the configured credentials.
 *
 * The captive portal works by running a DNS server that resolves ALL
 * queries to the AP's IP (192.168.4.1). iOS, Android, macOS, and
 * Windows all detect this and show their "sign in to network" sheet
 * automatically — zero app install required.
 *
 * Device naming: a user-friendly name ("Kitchen Canary") stored in
 * NVS and surfaced in the web UI header, mDNS TXT record, MQTT
 * device name, and serial banner.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_SETUP_H
#define SECURACV_SETUP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SETUP_DEVICE_NAME_MAX  32
#define SETUP_TIMEOUT_MS       (15UL * 60UL * 1000UL)

#ifdef __cplusplus
extern "C" {
#endif

bool setup_init(void);

bool setup_is_first_boot(void);

bool setup_is_active(void);

void setup_mark_complete(void);

/* Captive portal DNS server — call from main loop when setup is
 * active. Responds to all DNS queries with the AP IP. */
void setup_dns_process(void);

bool setup_start_captive_portal(void);

void setup_stop_captive_portal(void);

/* Auto-exit setup mode after SETUP_TIMEOUT_MS if no client connects. */
void setup_check_timeout(void);

/* Device naming. Name is persisted in NVS. */
bool setup_get_device_name(char* out, size_t len);
bool setup_set_device_name(const char* name);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_SETUP_H */
