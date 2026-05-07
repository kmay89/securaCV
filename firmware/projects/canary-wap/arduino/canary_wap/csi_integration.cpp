/**
 * @file csi_integration.cpp
 * @brief Host-side wiring of the SecuraCV CSI library into canary-wap.
 *
 * Responsibilities:
 *   1. Register the four v1 modules (core.presence, core.breathing,
 *      core.activity_ribbon, meta.daily_summary) at boot.
 *   2. Initialize csi_hal and route every CSI features window through
 *      csi_module_tick_all() AND (optionally) the legacy
 *      rf_presence::feed_csi_window() path.
 *   3. Maintain an in-memory snapshot of the most recently committed event
 *      via the strong override of csi_event_on_committed().
 *   4. Serve four HTTP endpoints:
 *        GET  /api/csi/stream     polling-friendly snapshot @ 1 Hz cadence
 *        GET  /api/csi/window     latest 32-dim feature window (P2-gated)
 *        GET  /api/events/today   list of today's committed events
 *        POST /api/events/dismiss local-only "that was nothing" feedback
 *
 * Why polling and not Server-Sent Events?
 *   ESP-IDF httpd holds a worker per request until the handler returns.
 *   True long-lived SSE requires the async-handler API and validation on
 *   actual hardware before we can confidently land it; polling at the
 *   library's natural 1-Hz cadence covers the v1 needs (live orb, Today
 *   sheet, Python listener) without that risk. SSE upgrade is tracked as
 *   a Phase 4 follow-up.
 *
 * Witness-chain integration (a strong override of
 * csi_event_commit_witness) is intentionally not wired here — the host
 * can plug it in via a small follow-up so this surface stays reviewable.
 * In the meantime, P0/P1 events still emit through the chokepoint and
 * appear on the snapshot stream and the Today sheet; they just don't
 * write to the witness chain yet.
 */

#include "csi_integration.h"
#include "csi_dashboard_html.h"

#include <Arduino.h>
#include <esp_http_server.h>
#include <string.h>
#include <stdlib.h>

#include <csi_hal.h>
#include <csi_features.h>
#include <csi_types.h>
#include <csi_module.h>
#include <csi_event.h>

/* The four v1 modules ship with the library. After the Phase-4 flattening
 * (see commit notes) these live at the library root rather than in a
 * modules/ subdir, so that arduino-cli's library-1.5 root-only compile
 * picks up their .cpp files without needing a src/ tree. */
#include <core_presence.h>
#include <core_breathing.h>
#include <core_activity_ribbon.h>
#include <meta_daily_summary.h>

namespace {

bool                                    g_initialized        = false;
csi_integration::legacy_features_hook_t g_legacy_hook        = nullptr;
csi_features_t                          g_latest_window      = {};
bool                                    g_have_latest_window = false;
uint32_t                                g_stream_started_ms  = 0;

/* Indices into csi_features_t::v for the two bands the snapshot fallback
 * surfaces. Layout is documented in csi_features.h:11-18:
 *   v[0..7]   amplitude variance
 *   v[8..11]  phase-Doppler  (4 bands → motion)
 *   v[12..19] breathing FFT  (8 bins  → micro-motion / breath rhythm)
 *   v[20..23] RSSI stats
 *   v[24..31] frame health + reserved
 * Mirrored, deliberately by-value, in core_presence.cpp / core_breathing.cpp. */
constexpr int IDX_DOPPLER_BASE   = 8;
constexpr int IDX_DOPPLER_COUNT  = 4;
constexpr int IDX_BREATHING_BASE = 12;
constexpr int IDX_BREATHING_COUNT = 8;

/* ──────────────────────────────────────────────────────────────────────────
 * MOST-RECENT-EVENT SNAPSHOT
 *
 * Updated by the strong override of csi_event_on_committed() so the
 * /api/csi/stream snapshot endpoint always has the freshest published
 * state without needing to walk the ring.
 * ────────────────────────────────────────────────────────────────────────── */

struct Snapshot {
  bool                  valid;
  uint32_t              event_id;
  uint32_t              committed_ms;     /* monotonic; converted to relative t */
  csi_event_category_t  category;
  csi_privacy_class_t   privacy;
  csi_event_values_t    values;
  char                  module_id[CSI_EVENT_NAME_MAX];
  char                  type_name[CSI_EVENT_NAME_MAX];
};

Snapshot g_snapshot = {};

/* ──────────────────────────────────────────────────────────────────────────
 * CSI features callback — drives the module pipeline + legacy fusion.
 * ────────────────────────────────────────────────────────────────────────── */

void on_csi_window(const csi_features_t* features, void* /*user*/) {
  if (!features) return;
  g_latest_window      = *features;
  g_have_latest_window = true;
  csi_module_tick_all(features);
  if (g_legacy_hook) g_legacy_hook(features);
}

/* ──────────────────────────────────────────────────────────────────────────
 * HTTP HANDLERS
 * ────────────────────────────────────────────────────────────────────────── */

esp_err_t handle_stream(httpd_req_t* req) {
  /* Polling-friendly snapshot. Returns the most recently committed event,
   * or an "ambient" record derived from the latest feature window if no
   * event has fired yet. The client reconnects once per second.
   *
   * The wire format mirrors what a future SSE upgrade will emit, so the
   * Python listener and the dashboard work unchanged when SSE lands. */
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  char buf[640];
  if (g_snapshot.valid) {
    const Snapshot* s = &g_snapshot;
    const char* cat = (s->category == CSI_CATEGORY_AMBIENT) ? "ambient"
                    : (s->category == CSI_CATEGORY_ANOMALY) ? "anomaly" : "event";
    const char* priv = (s->privacy == CSI_PRIVACY_P0) ? "p0"
                     : (s->privacy == CSI_PRIVACY_P1) ? "p1" : "p2";
    snprintf(buf, sizeof(buf),
      "{"
        "\"t\":%lu,"
        "\"id\":%lu,"
        "\"module\":\"%s\","
        "\"type\":\"%s\","
        "\"category\":\"%s\","
        "\"privacy\":\"%s\","
        "\"state\":\"%s\","
        "\"confidence\":\"%s\","
        "\"motion\":%u,"
        "\"breathing\":%u,"
        "\"bpm\":%u,"
        "\"duration_sec\":%u,"
        "\"bundled\":%u,"
        "\"time_bucket\":%u"
      "}",
      /* `t` is the relative seconds at which THIS event was committed, not
       * the time of the HTTP request — otherwise consecutive polls of the
       * same event_id would tick `t` upward and break client-side duration
       * math. */
      (unsigned long)((s->committed_ms - g_stream_started_ms) / 1000u),
      (unsigned long)s->event_id, s->module_id, s->type_name, cat, priv,
      s->values.state_name, s->values.confidence,
      (unsigned)s->values.motion_score,
      (unsigned)s->values.breathing_score,
      (unsigned)s->values.breathing_rate_bpm,
      (unsigned)s->values.duration_sec,
      (unsigned)s->values.bundled_count,
      (unsigned)s->values.time_bucket
    );
  } else {
    /* No committed event yet — surface the latest raw window's two scalars
     * so the dashboard's orb has something honest to render at boot. */
    uint8_t motion = 0, breathing = 0;
    if (g_have_latest_window) {
      int32_t m = 0, b = 0;
      for (int i = IDX_DOPPLER_BASE;
           i < IDX_DOPPLER_BASE + IDX_DOPPLER_COUNT; ++i) {
        m += abs((int)g_latest_window.v[i]);
      }
      for (int i = IDX_BREATHING_BASE;
           i < IDX_BREATHING_BASE + IDX_BREATHING_COUNT; ++i) {
        b += abs((int)g_latest_window.v[i]);
      }
      const int32_t m_avg = m / IDX_DOPPLER_COUNT;
      const int32_t b_avg = b / IDX_BREATHING_COUNT;
      motion    = (uint8_t)(m_avg > 100 ? 100 : m_avg);
      breathing = (uint8_t)(b_avg > 100 ? 100 : b_avg);
    }
    snprintf(buf, sizeof(buf),
      "{\"t\":%lu,\"category\":\"ambient\",\"state\":\"sensing\","
       "\"confidence\":\"tentative\","
       "\"motion\":%u,\"breathing\":%u}",
      (unsigned long)((millis() - g_stream_started_ms) / 1000u),
      (unsigned)motion, (unsigned)breathing);
  }
  httpd_resp_send(req, buf, -1);
  return ESP_OK;
}

esp_err_t handle_window(httpd_req_t* req) {
  /* Raw 32-dim feature vector. P2 only — the chokepoint enforces the
   * privacy ceiling, so unauthorized callers get a 403 with no data leak. */
  if (csi_event_get_privacy_ceiling() < CSI_PRIVACY_P2) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_send(req,
      "{\"error\":\"raw window requires P2 privacy ceiling\"}", -1);
    return ESP_OK;
  }
  if (!g_have_latest_window) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
  }
  char buf[768];
  int  off = snprintf(buf, sizeof(buf),
    "{\"frames\":%u,\"time_bucket\":%u,\"v\":[",
    (unsigned)g_latest_window.frames_in_window,
    (unsigned)g_latest_window.time_bucket);
  for (int i = 0; i < CSI_FEATURE_DIM; ++i) {
    off += snprintf(buf + off, sizeof(buf) - off, "%s%d",
                    i ? "," : "", (int)g_latest_window.v[i]);
    if (off >= (int)sizeof(buf) - 8) break;
  }
  snprintf(buf + off, sizeof(buf) - off, "]}");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, -1);
  return ESP_OK;
}

esp_err_t handle_events_today(httpd_req_t* req) {
  /* Walk the in-memory ring; emit at most 64 newest rows. Static buffer
   * so we don't blow the ESP32 task stack (csi_event_record_t is ~120 B). */
  static csi_event_record_t buffer[64];
  size_t n = csi_event_recent(buffer, sizeof(buffer) / sizeof(buffer[0]));

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send_chunk(req, "{\"events\":[", 11);

  bool first = true;
  for (size_t i = 0; i < n; ++i) {
    const csi_event_record_t* r = &buffer[i];
    if (r->event_id == 0) continue;
    if (r->privacy > csi_event_get_privacy_ceiling()) continue;
    char row[384];
    const int len = snprintf(row, sizeof(row),
      "%s{"
        "\"id\":%lu,"
        "\"module\":\"%s\","
        "\"type\":\"%s\","
        "\"state\":\"%s\","
        "\"confidence\":\"%s\","
        "\"motion\":%u,"
        "\"breathing\":%u,"
        "\"bpm\":%u,"
        "\"duration_sec\":%u,"
        "\"bundled\":%u,"
        "\"time_bucket\":%u,"
        "\"dismissed\":%u"
      "}",
      first ? "" : ",",
      (unsigned long)r->event_id,
      r->module_id, r->type_name,
      r->values.state_name, r->values.confidence,
      (unsigned)r->values.motion_score,
      (unsigned)r->values.breathing_score,
      (unsigned)r->values.breathing_rate_bpm,
      (unsigned)r->values.duration_sec,
      (unsigned)r->bundled_count,
      (unsigned)r->values.time_bucket,
      (unsigned)r->values.dismissed);
    if (len > 0 && len < (int)sizeof(row)) {
      httpd_resp_send_chunk(req, row, len);
      first = false;
    }
  }
  httpd_resp_send_chunk(req, "]}", 2);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

esp_err_t handle_events_dismiss(httpd_req_t* req) {
  /* Body is small JSON: {"event_id": <number>}. We parse with a tiny
   * scanner to avoid pulling ArduinoJson into this TU. */
  char body[96];
  const int got = httpd_req_recv(req, body, sizeof(body) - 1);
  if (got <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"empty body\"}", -1);
    return ESP_OK;
  }
  body[got] = '\0';
  const char* k = strstr(body, "event_id");
  if (!k) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"missing event_id\"}", -1);
    return ESP_OK;
  }
  const char* digit = k;
  while (*digit && (*digit < '0' || *digit > '9')) digit++;
  const uint32_t event_id = (uint32_t)strtoul(digit, nullptr, 10);
  const bool ok = csi_event_dismiss(event_id);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", -1);
  return ESP_OK;
}

esp_err_t handle_sense_page(httpd_req_t* req) {
  /* The headline Sensing dashboard. Static asset served straight from
   * PROGMEM. The page itself fetches /api/csi/stream + /api/events/today
   * + /api/events/dismiss + /api/csi/window for live data. No auth on the
   * page render — everything privacy-sensitive is gated by the chokepoint's
   * own privacy ceiling at the data endpoints. */
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, CSI_DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ──────────────────────────────────────────────────────────────────────────
 * MODULE REGISTRATION
 * ────────────────────────────────────────────────────────────────────────── */

void register_v1_modules() {
  csi_module_register(core_presence_module());
  csi_module_register(core_breathing_module());
  csi_module_register(core_activity_ribbon_module());
  csi_module_register(meta_daily_summary_module());
}

}  /* namespace */

/* ──────────────────────────────────────────────────────────────────────────
 * STRONG OVERRIDE — csi_event_on_committed
 *
 * The library declares this hook __attribute__((weak)) so the standalone
 * build links cleanly. Here we provide the strong implementation that
 * records the snapshot for /api/csi/stream.
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" void csi_event_on_committed(uint32_t                  event_id,
                                       const char*               module_id,
                                       const char*               type_name,
                                       csi_event_category_t      category,
                                       csi_privacy_class_t       privacy,
                                       const csi_event_values_t* values) {
  if (!values) return;
  /* Don't expose P2 events on the public stream unless the user has
   * explicitly raised the privacy ceiling. The chokepoint already respects
   * this for emit; a defensive check here keeps the wire surface obvious. */
  if (privacy > csi_event_get_privacy_ceiling()) return;

  Snapshot* s = &g_snapshot;
  s->valid        = true;
  s->event_id     = event_id;
  s->committed_ms = millis();
  s->category     = category;
  s->privacy      = privacy;
  s->values       = *values;
  strncpy(s->module_id, module_id ? module_id : "?", CSI_EVENT_NAME_MAX - 1);
  strncpy(s->type_name, type_name ? type_name : "?", CSI_EVENT_NAME_MAX - 1);
  s->module_id[CSI_EVENT_NAME_MAX - 1] = '\0';
  s->type_name[CSI_EVENT_NAME_MAX - 1] = '\0';
}

/* ──────────────────────────────────────────────────────────────────────────
 * PUBLIC API
 * ────────────────────────────────────────────────────────────────────────── */

namespace csi_integration {

void set_legacy_features_hook(legacy_features_hook_t hook) {
  g_legacy_hook = hook;
}

unsigned int sse_client_count() {
  /* Polling currently — no persistent clients. Reserved for SSE upgrade. */
  return 0;
}

bool init(httpd_handle_t server) {
  if (g_initialized) return true;
  if (!server) return false;

  register_v1_modules();

  /* Bring up the CSI HAL. start() defers until WiFi is up; the deferred
   * retry is silent and handled by csi_hal::process(). */
  csi_hal::Config cfg = csi_hal::Config::defaults();
  cfg.bandwidth_mhz     = 20;
  cfg.max_frame_rate_hz = 20;
  if (!csi_hal::init(cfg)) {
    Serial.println("[CSI] csi_hal::init failed; sensing disabled");
    return false;
  }
  csi_set_features_callback(on_csi_window, nullptr);
  csi_hal::start();   /* may defer; that's fine */

  g_stream_started_ms = millis();

  /* Register HTTP routes. Four endpoints; the route reservation in
   * canary_wap.ino's start_http_server() needs to budget for them. */
  static httpd_uri_t r_stream = {
    .uri = "/api/csi/stream", .method = HTTP_GET, .handler = handle_stream
  };
  httpd_register_uri_handler(server, &r_stream);

  static httpd_uri_t r_window = {
    .uri = "/api/csi/window", .method = HTTP_GET, .handler = handle_window
  };
  httpd_register_uri_handler(server, &r_window);

  static httpd_uri_t r_today = {
    .uri = "/api/events/today", .method = HTTP_GET, .handler = handle_events_today
  };
  httpd_register_uri_handler(server, &r_today);

  static httpd_uri_t r_dismiss = {
    .uri = "/api/events/dismiss", .method = HTTP_POST, .handler = handle_events_dismiss
  };
  httpd_register_uri_handler(server, &r_dismiss);

  /* /sense is kept as an alias for the headline dashboard for backward
   * compatibility — the canonical landing route is now "/" (handled by
   * canary_wap.ino's handle_ui), and the legacy tabbed dashboard moved
   * to /admin. Any links the companion PWA or third-party tools may
   * have made during the Phase-3 staging period keep working. */
  static httpd_uri_t r_sense = {
    .uri = "/sense", .method = HTTP_GET, .handler = handle_sense_page
  };
  httpd_register_uri_handler(server, &r_sense);

  g_initialized = true;
  Serial.printf("[CSI] integration ready: %u modules, 5 routes registered\n",
                (unsigned)csi_module_count());
  return true;
}

}  /* namespace csi_integration */
