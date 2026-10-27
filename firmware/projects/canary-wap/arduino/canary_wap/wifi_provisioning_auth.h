/*
 * SecuraCV Canary WAP — Wi-Fi provisioning credential-gate result
 *
 * This enum is the return type of wifi_change_authorize() (defined in
 * canary_wap.ino), the credential gate shared by /api/wifi/connect and
 * /api/wifi/ap-only. It lives in a header, NOT inline in the .ino, because
 * arduino-cli auto-generates a prototype for wifi_change_authorize() and
 * hoists it ABOVE every definition in the sketch — so the return type must
 * already be declared via an include, or the build fails with
 * "'WifiChangeAuth' does not name a type".
 *
 *   PROCEED       — after first-boot setup, a valid bearer (sweep F334:
 *                   the network moves on the bearer alone); during setup,
 *                   a valid pair token or admin credential.
 *   INVALID_TOKEN — during setup only: no credential at all (wizard path);
 *                   caller replies with the friendly invalid_token JSON so
 *                   the token self-heals.
 *   RESPONDED     — the gate refused and already sent its own 401/403/429
 *                   (after setup api_auth_check_bearer's, during setup
 *                   api_auth_check's on a presented admin credential); the
 *                   caller just returns ESP_OK.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_WIFI_PROVISIONING_AUTH_H
#define SECURACV_WIFI_PROVISIONING_AUTH_H

enum class WifiChangeAuth { PROCEED, INVALID_TOKEN, RESPONDED };

#endif  // SECURACV_WIFI_PROVISIONING_AUTH_H
