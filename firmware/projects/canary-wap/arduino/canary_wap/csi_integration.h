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
 * The api_token argument is the device's Bearer token (g_device.api_token_str).
 * Every CSI HTTP handler verifies the inbound Authorization header against it
 * via api_auth_check(), matching the handle_*_auth pattern used by /api/status,
 * /api/chain, /api/witness, etc. The dashboard at / bootstraps the token by
 * reading window.__CV_TOKEN, which handle_ui injects into the HTML at request
 * time. The pointer must remain valid for the lifetime of the integration
 * (g_device lives forever, so passing &g_device.api_token_str[0] is fine).
 *
 * Idempotent: a second call returns immediately.
 */
bool init(httpd_handle_t server, const char* api_token);

/**
 * Per-tick pump. Call once per main-loop iteration after init().
 *
 * Drains the WiFi-task SPSC ring into the feature aggregator, finalizes
 * 1-second windows, dispatches each window to the v1 module pipeline, and
 * runs the deferred-start retry if csi_hal::start() was queued before WiFi
 * came up. Without this call the entire CSI pipeline is dead and
 * /api/csi/stream returns the boot-fallback "sensing" state forever.
 *
 * Also runs a one-shot boot self-test ~3 seconds after init() returns,
 * logging exactly one of:
 *   [CSI] OK: <N> frames received, <M> windows emitted in 3s
 *   [CSI] STALLED: 0 frames received in 3s — check antenna / WiFi mode
 *   [CSI] DROPS: <K> frames dropped (ring full), windows=<M> — main loop starved
 *
 * Cheap (single ring drain + millis() compare); safe to call from the
 * Arduino main loop at full rate.
 */
void loop();

/**
 * Has any v1 module committed an event since boot? (i.e. has the snapshot
 * served by /api/csi/stream ever transitioned from the boot fallback to a
 * real event?) Used by /api/status for diagnostic visibility.
 */
bool snapshot_valid();

/** csi_hal::is_running() bridge. False ⇒ start was deferred or chip
 *  lacks CSI; either is diagnostic gold. */
bool csi_running();

/** csi_hal::get_stats() bridge. Fills out frames_received,
 *  windows_emitted, frames_dropped_*, etc. Returns false on null arg. */
bool csi_get_stats(csi_stats_t* out);

/**
 * Length of the hex-encoded session cookie value (32 random bytes →
 * 64 hex chars). Callers issuing a Set-Cookie need this to size their
 * scratch buffers safely.
 */
constexpr size_t SESSION_COOKIE_HEX_LEN = 64;

/**
 * True if the request carries a `cv_session=<hex>` cookie that matches
 * a live, unexpired session in the in-RAM session store. Never sends
 * a response — caller decides what to do on failure (handle_ui shows
 * a pair landing; CSI API handlers fall through to Bearer auth or 401).
 *
 * Replaces the previous design where the device's Bearer api_token was
 * injected into dashboard HTML — that approach made the token
 * harvestable by anyone on the SoftAP who could view-source on /
 * (per pull-request review #392 r3213361582). Cookies are HttpOnly +
 * SameSite=Strict, so JS can't read them and cross-origin requests
 * can't forge them.
 */
bool session_validate_cookie(httpd_req_t* req);

/**
 * Mint a fresh session and write 64 hex chars + NUL to hex_out.
 * out_cap must be >= SESSION_COOKIE_HEX_LEN + 1 (65). Returns true on
 * success. Used by handle_ui's `?cv_pair=<token>` consumption branch:
 * after a valid one-shot pair-token, we issue a session and bake the
 * hex into a Set-Cookie header.
 */
bool session_issue(char* hex_out, size_t out_cap);

/**
 * Render the "tap to enter" landing page served by handle_ui when a
 * visitor has neither a valid session cookie nor a pending pair-token
 * URL. Mints a fresh pair token, embeds a /?cv_pair=<hex> link and a
 * QR-friendly URL, and sends the response. Returns true on success.
 *
 * Idempotent in effect — every call mints a new pair token (rate-
 * limited by PAIR_SLOTS), so a refresh always works even if the prior
 * token expired or was consumed by a different tab.
 */
bool send_pair_landing(httpd_req_t* req);

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

/* ──────────────────────────────────────────────────────────────────────────
 * PAIRING TOKENS — Tier 5 #11 (captive-portal QR + companion handoff)
 *
 * The captive-portal landing page bakes a fresh one-shot pairing token into
 * the QR code (and into the manual /companion?token=<hex> fallback link).
 * The companion PWA validates that token with the device before showing
 * the WiFi credentials form, so a casual visitor on the AP can't drive
 * provisioning by guessing the URL.
 *
 * Properties:
 *   - 32 random bytes per token (256-bit entropy from esp_fill_random).
 *   - RAM-only — never persisted. A reboot invalidates every outstanding
 *     token, which is the right behaviour for "you have 10 minutes to
 *     finish onboarding" UX.
 *   - 10-minute expiry from issuance.
 *   - Single-use: pair_token_consume() invalidates the slot it matches.
 *   - Bounded slot table (4 active tokens at a time). Ninth issuance
 *     evicts the oldest unused slot.
 *
 * Threading: protected by the existing httpd worker model (handlers are
 * the only callers, and ESP-IDF's httpd serializes URI handlers behind
 * a single worker). If a future code path adds task-level callers, drop
 * a portMUX around the slot table.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t PAIR_TOKEN_BYTES   = 32;
constexpr size_t PAIR_TOKEN_HEX_LEN = PAIR_TOKEN_BYTES * 2;  // 64

/* Issue a fresh pairing token; writes 64 hex chars + NUL into hex_out
 * (out_cap must be >= 65). Returns true on success.
 * out_cap is bytes available; on success the buffer is NUL-terminated. */
bool pair_token_issue(char* hex_out, size_t out_cap);

/* Validate a token without consuming it. Used by the companion PWA's
 * "is the token I scanned still alive?" preflight. */
bool pair_token_valid(const char* hex);

/* Consume a token: validate, mark used, return true iff the call
 * itself was the consumer (subsequent calls with the same hex
 * return false). */
bool pair_token_consume(const char* hex);

/* ──────────────────────────────────────────────────────────────────────────
 * SHARED UTILITIES
 *
 * Lowercase hex-encode `len` bytes of `in` into `out`, NUL-terminated.
 * `out` must be sized for 2*len + 1. Exposed publicly so csi_mqtt and
 * any future export path use one canonical encoder rather than spawning
 * duplicates (PR #394 review r3213674564). canary_wap.ino has its own
 * `hex_to_str(out, in, n)` with reversed argument order — that one
 * stays as-is to avoid touching unrelated callers; new code should
 * reach for this signature.
 * ────────────────────────────────────────────────────────────────────────── */
void hex_encode(const uint8_t* in, size_t len, char* out);

}  /* namespace csi_integration */

#endif /* SECURACV_CSI_INTEGRATION_H */
