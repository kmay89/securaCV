/**
 * @file csi_integration.h
 * @brief Wire the SecuraCV CSI library (firmware/common/csi/) into the
 *        canary-wap firmware's HTTP server, witness chain, and module pipeline.
 *
 * Single-call host integration. Call `csi_integration::init(http_server)`
 * once after the HTTP server is up (after register_api_routes()). The module
 * pipeline starts ticking, the four v1 modules register themselves, and the
 * /api/csi/stream + /api/events/today + /api/events/dismiss + /api/csi/window
 * endpoints come live.
 */

#ifndef SECURACV_CSI_INTEGRATION_H
#define SECURACV_CSI_INTEGRATION_H

#include "esp_http_server.h"
// Pull the real csi_features_t — it's a typedef of an anonymous struct so
// a `struct csi_features` forward declaration would silently create a
// different type, exactly the bug that broke an earlier rev of this file.
#include <csi_types.h>

namespace csi_integration {

/**
 * Boot-time initialisation. Must be called AFTER:
 *   - WiFi AP/STA is up (so esp_wifi_set_csi_rx_cb has a context)
 *   - the HTTP server is started (we register URI handlers on it)
 *
 * Idempotent: a second call returns immediately.
 */
bool init(httpd_handle_t server);

/**
 * Optional: called by the existing CSI features callback when this firmware
 * still wants to drive rf_presence::feed_csi_window() in addition to the
 * module pipeline. Pass nullptr to disable.
 */
typedef void (*legacy_features_hook_t)(const csi_features_t* features);
void set_legacy_features_hook(legacy_features_hook_t hook);

/**
 * Number of currently-connected SSE clients (for /api/status diagnostics).
 */
unsigned int sse_client_count();

/**
 * Privacy Budget — literal byte counter for outbound traffic.
 *
 * The device is local-first by default: nothing leaves it. The dashboard
 * surfaces this as a "Today: 0 bytes left the device" pill so the user
 * can see the local-first invariant on screen, not just in docs.
 *
 * Whenever the host sends data to an OFF-device destination (MQTT, an
 * SD-card export the user took home, BLE-paired phone export, future
 * cloud integrations), it MUST call add_outbound_bytes() with the
 * payload size. Calls are cheap — single increment, no allocation. The
 * counter resets at boot; future work will hook a wall-clock-aware
 * day-rollover reset to it.
 *
 * What does NOT count:
 *   - Responses to the dashboard's own polling at /api/csi/stream and
 *     friends (the user's own browser is local).
 *   - Captive-portal redirects.
 *   - mDNS broadcasts.
 * The intent is "bytes leaving the device toward a destination outside
 * the user's immediate network", not "bytes the network card emitted".
 */
void add_outbound_bytes(uint32_t bytes);

/** Bytes counted since boot (or last reset). Read-only accessor. */
uint32_t outbound_bytes_today();

}  /* namespace csi_integration */

#endif /* SECURACV_CSI_INTEGRATION_H */
