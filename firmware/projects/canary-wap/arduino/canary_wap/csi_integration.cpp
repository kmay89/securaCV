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
 * Witness-chain integration is wired below: the strong override of
 * csi_event_commit_witness routes every committed P0/P1 event through
 * canary_wap.ino's create_witness_record path so the event is
 * Ed25519-signed and hash-chained. P2 never reaches that hook (the
 * chokepoint gates it).
 */

#include "csi_integration.h"
#include "csi_dashboard_html.h"

#include <Arduino.h>
#include <Preferences.h>          // NVS-backed settings store
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
 * SETTINGS — NVS-backed module settings
 *
 * The library declares csi_module_settings_int/bool/float with weak
 * default symbols that just return the supplied default. We override
 * them here with a thin Preferences-backed reader so the dashboard's
 * Pet Mode toggle (and future preset / sensitivity controls) actually
 * change what the modules do at run-time.
 *
 * NVS key length limit is 15 chars, so we shorten the dotted module
 * keys to a stable abbreviation:
 *
 *   core.presence.pet_mode -> cp.pet_mode   (cp + dot + 8 = 11)
 *   core.presence.motion_threshold -> cp.mt (still valid, mapped below)
 *   ...
 *
 * The dashboard speaks in dotted keys; this map is the only place that
 * knows about the abbreviation, so future setting-key additions touch
 * one table.
 *
 * Defined here (above HTTP HANDLERS) so the GET / POST handlers below
 * can reference SETTINGS_NS, nvs_key_for(), and reinit_module() without
 * forward declarations.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr const char* SETTINGS_NS = "csi";

struct SettingKey {
  const char* full;   // "core.presence.pet_mode"
  const char* nvs;    // "cp.pet_mode" — must be ≤ 15 chars
};
const SettingKey SETTING_KEYS[] = {
  { "core.presence.pet_mode",            "cp.pet_mode"   },
  { "core.presence.motion_threshold",    "cp.mt"         },
  { "core.presence.active_threshold",    "cp.at"         },
  { "core.presence.breathing_threshold", "cp.bt"         },
  { "core.presence.pet_mode_seconds",    "cp.ps"         },
  { "core.breathing.lock_threshold",     "cb.lt"         },
  { "core.breathing.confirm_seconds",    "cb.cs"         },
};

const char* nvs_key_for(const char* full_key) {
  for (const SettingKey& k : SETTING_KEYS) {
    if (strcmp(k.full, full_key) == 0) return k.nvs;
  }
  return nullptr;
}

/* Reinit modules whose settings changed. Cheap — modules are stateless
 * apart from a few static counters that init() resets, and there are
 * only four registered. Called once after each /api/settings POST. */
void reinit_module(const char* module_id) {
  const csi_module_t* m = csi_module_find(module_id);
  if (!m) return;
  if (m->deinit) m->deinit();
  if (m->init)   m->init(nullptr);
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
    char row[400];
    const char* cat = (r->category == CSI_CATEGORY_AMBIENT) ? "ambient"
                    : (r->category == CSI_CATEGORY_ANOMALY) ? "anomaly" : "event";
    const int len = snprintf(row, sizeof(row),
      "%s{"
        "\"id\":%lu,"
        "\"module\":\"%s\","
        "\"type\":\"%s\","
        "\"category\":\"%s\","
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
      r->module_id, r->type_name, cat,
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

/* ──────────────────────────────────────────────────────────────────────────
 * /api/settings — GET reads NVS, POST writes NVS + reinits affected module
 *
 * Wire format (intentionally tiny, dashboard-friendly):
 *   GET  → {"pet_mode":true|false}
 *   POST {"pet_mode":true|false}  → 200 {"ok":true} after persisting
 *
 * Pet Mode is the only key on the wire today; preset / sensitivity-slider
 * round-trips will land in a follow-up that maps preset → motion/active/
 * breathing thresholds. The NVS schema (cp.pet_mode et al.) is already
 * defined in SETTING_KEYS, so future endpoint expansion is purely
 * additive.
 * ────────────────────────────────────────────────────────────────────────── */

esp_err_t handle_settings_get(httpd_req_t* req) {
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/true)) {
    /* Don't silently report pet_mode=false — the dashboard would
     * reconcile localStorage to that value and quietly disable Pet
     * Mode for any user who had it on. Surface the unavailability
     * so the client skips reconciliation and keeps its current
     * localStorage source of truth. */
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"settings store unavailable\"}", -1);
    return ESP_OK;
  }
  bool pet_mode = prefs.getBool("cp.pet_mode", false);
  prefs.end();
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"pet_mode\":%s}", pet_mode ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, -1);
  return ESP_OK;
}

esp_err_t handle_settings_post(httpd_req_t* req) {
  /* Body is small JSON: {"pet_mode": true|false}. Hand-parse to keep
   * ArduinoJson out of this TU. */
  char body[96];
  const int got = httpd_req_recv(req, body, sizeof(body) - 1);
  if (got <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"empty body\"}", -1);
    return ESP_OK;
  }
  body[got] = '\0';

  bool wrote_anything = false;
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"nvs unavailable\"}", -1);
    return ESP_OK;
  }

  /* Look for "pet_mode": true | false. Tolerant scanner — accepts
   * surrounding whitespace and either lowercase boolean. We search for
   * the QUOTED key so a body like {"not_pet_mode": true} doesn't
   * accidentally match here. */
  const char* k = strstr(body, "\"pet_mode\"");
  if (k) {
    const char* v = strchr(k, ':');
    if (v) {
      v++;
      while (*v == ' ' || *v == '\t' || *v == '"') v++;
      if (strncmp(v, "true", 4) == 0) {
        prefs.putBool("cp.pet_mode", true);
        wrote_anything = true;
      } else if (strncmp(v, "false", 5) == 0) {
        prefs.putBool("cp.pet_mode", false);
        wrote_anything = true;
      }
    }
  }
  prefs.end();

  if (!wrote_anything) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"no recognised keys\"}", -1);
    return ESP_OK;
  }

  /* Re-init affected module so it picks up the new value on the next tick. */
  reinit_module("core.presence");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"ok\":true}", -1);
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

/* ──────────────────────────────────────────────────────────────────────────
 * STRONG OVERRIDES — csi_module_settings_*
 *
 * The library's weak defaults return whatever default the caller passes;
 * here we look up the canonical full key, map to the short NVS key, and
 * read the persisted value. Falls back to the caller's default when the
 * key is absent or this is the first boot.
 *
 * Read-only Preferences handle is opened per call. Settings reads are
 * infrequent (boot + post-POST reinit), so the small open/close cost
 * is fine and avoids holding an NVS handle across the firmware lifetime.
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" int32_t csi_module_settings_int(const csi_module_settings_t*,
                                           const char* key,
                                           int32_t default_value) {
  if (!key) return default_value;
  const char* nvs_key = nvs_key_for(key);
  if (!nvs_key) return default_value;
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/true)) return default_value;
  int32_t v = prefs.getInt(nvs_key, default_value);
  prefs.end();
  return v;
}

extern "C" bool csi_module_settings_bool(const csi_module_settings_t*,
                                         const char* key,
                                         bool default_value) {
  if (!key) return default_value;
  const char* nvs_key = nvs_key_for(key);
  if (!nvs_key) return default_value;
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/true)) return default_value;
  bool v = prefs.getBool(nvs_key, default_value);
  prefs.end();
  return v;
}

extern "C" float csi_module_settings_float(const csi_module_settings_t*,
                                           const char* key,
                                           float default_value) {
  if (!key) return default_value;
  const char* nvs_key = nvs_key_for(key);
  if (!nvs_key) return default_value;
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/true)) return default_value;
  float v = prefs.getFloat(nvs_key, default_value);
  prefs.end();
  return v;
}

/* ──────────────────────────────────────────────────────────────────────────
 * STRONG OVERRIDE — csi_event_commit_witness
 *
 * The library declares this hook with a weak no-op default in
 * firmware/common/csi/src/csi_event.cpp so the standalone build links
 * cleanly. Here in the canary-wap host we route every committed P0/P1
 * event into the existing witness chain via the public bridge defined in
 * canary_wap.ino's create_witness_record path. P2 never reaches us — the
 * chokepoint already gates that.
 *
 * The bridge function is defined in canary_wap.ino as extern "C"; we
 * forward-declare it here (the .ino doesn't ship a header). Ed25519
 * signing, hash-chaining, and SD persistence all happen inside the
 * existing create_witness_record + persist_chain_state pipeline; this
 * override is just the glue.
 *
 * Best-effort: a witness-chain failure (e.g. signing self-test broken,
 * SD full) doesn't bubble back to the caller — the in-memory ring + SSE
 * stream still update. This matches how create_witness_record's other
 * call sites in canary_wap.ino treat failures.
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" bool csi_witness_emit_event(const char* module_id,
                                       const char* type_name,
                                       uint8_t     category,
                                       const char* state_name,
                                       const char* confidence,
                                       uint8_t     motion_score,
                                       uint8_t     breathing_score,
                                       uint8_t     bpm,
                                       uint16_t    duration_sec,
                                       uint8_t     time_bucket);

extern "C" bool csi_event_commit_witness(uint32_t                  /*event_id*/,
                                         const char*               module_id,
                                         const char*               type_name,
                                         csi_event_category_t      category,
                                         const csi_event_values_t* values) {
  if (!values || !module_id || !type_name) return false;
  /* Only persist Event / Anomaly. Ambient never reaches us thanks to the
   * chokepoint, but defensive check keeps the contract local to this TU. */
  if (category == CSI_CATEGORY_AMBIENT) return false;

  return csi_witness_emit_event(
    module_id,
    type_name,
    (uint8_t)category,
    values->state_name,
    values->confidence,
    values->motion_score,
    values->breathing_score,
    values->breathing_rate_bpm,
    values->duration_sec,
    values->time_bucket);
}

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

  /* /api/settings — GET returns persisted module settings, POST writes
   * them and triggers a module reinit so the device responds immediately
   * to dashboard changes. Pet Mode is the only key on the wire today;
   * the NVS schema is set up for preset / sensitivity follow-up. */
  static httpd_uri_t r_settings_get = {
    .uri = "/api/settings", .method = HTTP_GET, .handler = handle_settings_get
  };
  httpd_register_uri_handler(server, &r_settings_get);
  static httpd_uri_t r_settings_post = {
    .uri = "/api/settings", .method = HTTP_POST, .handler = handle_settings_post
  };
  httpd_register_uri_handler(server, &r_settings_post);

  g_initialized = true;
  Serial.printf("[CSI] integration ready: %u modules, 7 routes registered\n",
                (unsigned)csi_module_count());
  return true;
}

}  /* namespace csi_integration */
