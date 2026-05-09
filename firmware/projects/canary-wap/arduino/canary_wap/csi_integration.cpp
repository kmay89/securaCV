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
#include "tune_ui.h"

#include <Arduino.h>
#include <Preferences.h>          // NVS-backed settings store
#include <esp_http_server.h>
#include <esp_random.h>           // esp_fill_random() — pairing token entropy
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
#include <meta_quiet_hours.h>
#include <anomaly_baseline.h>
#include <ble_events_module.h>

namespace {

bool                                    g_initialized        = false;
csi_integration::legacy_features_hook_t g_legacy_hook        = nullptr;
csi_features_t                          g_latest_window      = {};
bool                                    g_have_latest_window = false;
uint32_t                                g_stream_started_ms  = 0;

/* Privacy Budget byte counter. Increments only when host code calls
 * csi_integration::add_outbound_bytes() — i.e. when bytes go to a
 * destination outside the user's immediate network. The dashboard
 * polls this via GET /api/privacy-budget. Resets at boot for now;
 * a future wall-clock-aware reset hooks the same place. */
uint32_t                                g_outbound_bytes      = 0;

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
  /* Tier-3 dashboard surface: preset (0=sensitive, 1=balanced,
   * 2=quiet) + sensitivity slider (0..100). Module reads these and
   * computes the three thresholds below; users who change the
   * dashboard's preset / slider land here. */
  { "core.presence.preset",              "cp.preset"     },
  { "core.presence.sensitivity",         "cp.sens"       },
  /* Per-coefficient overrides (Tuning Lab path, Tier 4): if any of
   * these are explicitly set in NVS they win over the preset
   * baseline. Default value supplied at init() is the
   * preset+sensitivity-derived baseline so common-case users
   * never trip these. */
  { "core.presence.motion_threshold",    "cp.mt"         },
  { "core.presence.active_threshold",    "cp.at"         },
  { "core.presence.breathing_threshold", "cp.bt"         },
  { "core.presence.pet_mode_seconds",    "cp.ps"         },
  { "core.breathing.lock_threshold",     "cb.lt"         },
  { "core.breathing.confirm_seconds",    "cb.cs"         },
  /* Quiet Hours — a single time range (minutes-of-day, 0..1439) that
   * the dashboard renders as dimmed ribbon cells and that future
   * notification / anomaly modules can consult to suppress alerts.
   * The setting is forward-compat scaffolding for PR 7 and beyond;
   * today its only visible effect is the dimmed ribbon. */
  { "core.quiet_hours.enabled",          "qh.en"         },
  { "core.quiet_hours.start_min",        "qh.start"      },
  { "core.quiet_hours.end_min",          "qh.end"        },
  /* Anomaly baseline — out-of-pattern detector tunables. Defaults
   * cover a quiet home; Tuning Lab (PR 10) exposes them as sliders. */
  { "anomaly.baseline.spike_ratio",      "ab.sr"         },
  { "anomaly.baseline.min_motion",       "ab.mm"         },
  { "anomaly.baseline.min_breathing",    "ab.mb"         },
  { "anomaly.baseline.cooldown_sec",     "ab.cd"         },
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

/* Read the persisted Quiet Hours range from NVS and push it into the
 * chokepoint. Called both at boot (register_v1_modules) and on
 * /api/settings POST. Defaults match the dashboard's UI defaults
 * (23:00 → 07:00) so a never-set device is congruent with what a
 * fresh installer sees. The chokepoint setter is a pure state update
 * — held-summary flushing happens on the next emit, not here. */
void apply_quiet_hours_from_nvs() {
  Preferences qprefs;
  if (!qprefs.begin(SETTINGS_NS, /*readOnly=*/true)) return;
  const bool    qh_en    = qprefs.getBool("qh.en",    false);
  const int32_t qh_start = qprefs.getInt ("qh.start", 23 * 60);
  const int32_t qh_end   = qprefs.getInt ("qh.end",    7 * 60);
  qprefs.end();
  csi_event_set_quiet_window((uint16_t)qh_start, (uint16_t)qh_end, qh_en);
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
    /* Don't silently report defaults — the dashboard would reconcile
     * localStorage to those values and quietly clobber any choice the
     * user had previously made. Surface the unavailability so the
     * client skips reconciliation and keeps its current localStorage
     * source of truth. */
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"settings store unavailable\"}", -1);
    return ESP_OK;
  }
  const bool    pet_mode    = prefs.getBool("cp.pet_mode", false);
  const int32_t preset_idx  = prefs.getInt ("cp.preset",   1);   // default balanced
  const int32_t sensitivity = prefs.getInt ("cp.sens",     50);  // default neutral
  const bool    qh_enabled  = prefs.getBool("qh.en",       false);
  const int32_t qh_start    = prefs.getInt ("qh.start",    23 * 60);  // 11 PM default
  const int32_t qh_end      = prefs.getInt ("qh.end",       7 * 60);  //  7 AM default
  prefs.end();

  /* Map preset index back to a stable string for the dashboard. The
   * mapping is the only place this conversion lives — keep it in sync
   * with the parser in handle_settings_post and the switch in
   * core_presence.cpp's on_init. */
  const char* preset_str = (preset_idx == 0) ? "sensitive"
                         : (preset_idx == 2) ? "quiet" : "balanced";

  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"pet_mode\":%s,\"preset\":\"%s\",\"sensitivity\":%ld,"
     "\"quiet_hours\":{\"enabled\":%s,\"start_min\":%ld,\"end_min\":%ld}}",
    pet_mode ? "true" : "false", preset_str, (long)sensitivity,
    qh_enabled ? "true" : "false", (long)qh_start, (long)qh_end);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, -1);
  return ESP_OK;
}

esp_err_t handle_settings_post(httpd_req_t* req) {
  /* Body is small JSON. Recognized keys:
   *   "pet_mode":    true|false  → cp.pet_mode (bool)
   *   "preset":      "sensitive"|"balanced"|"quiet" → cp.preset (int 0..2)
   *   "sensitivity": 0..100      → cp.sens (int)
   * Hand-parse to keep ArduinoJson out of this TU. We search for the
   * QUOTED key in every case so a body like {"not_pet_mode": true}
   * doesn't accidentally match. Buffer sized for the full payload:
   *   pet_mode + preset + sensitivity + quiet_hours{enabled, start, end}
   * is ~130 chars; 256 leaves comfortable headroom for future keys. */
  char body[256];
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

  /* "pet_mode": true|false */
  if (const char* k = strstr(body, "\"pet_mode\"")) {
    if (const char* v = strchr(k, ':')) {
      v++;
      while (*v == ' ' || *v == '\t' || *v == '"') v++;
      if (strncmp(v, "true", 4) == 0) {
        prefs.putBool("cp.pet_mode", true);  wrote_anything = true;
      } else if (strncmp(v, "false", 5) == 0) {
        prefs.putBool("cp.pet_mode", false); wrote_anything = true;
      }
    }
  }

  /* "preset": "sensitive" | "balanced" | "quiet". Stored as int 0/1/2
   * so core_presence.cpp's switch is fast and the NVS row is small. */
  if (const char* k = strstr(body, "\"preset\"")) {
    if (const char* v = strchr(k, ':')) {
      v++;
      while (*v == ' ' || *v == '\t' || *v == '"') v++;
      int32_t idx = -1;
      if      (strncmp(v, "sensitive", 9) == 0) idx = 0;
      else if (strncmp(v, "balanced",  8) == 0) idx = 1;
      else if (strncmp(v, "quiet",     5) == 0) idx = 2;
      if (idx >= 0) {
        prefs.putInt("cp.preset", idx);
        wrote_anything = true;
      }
    }
  }

  /* "sensitivity": 0..100 (clamped). Skip `"` too so a value sent as
   * a string ({"sensitivity":"75"}) parses the same as a bare number,
   * matching the pet_mode and preset parsers above. */
  if (const char* k = strstr(body, "\"sensitivity\"")) {
    if (const char* v = strchr(k, ':')) {
      v++;
      while (*v == ' ' || *v == '\t' || *v == '"') v++;
      char* end = nullptr;
      long n = strtol(v, &end, 10);
      if (end != v) {
        if (n < 0)   n = 0;
        if (n > 100) n = 100;
        prefs.putInt("cp.sens", (int32_t)n);
        wrote_anything = true;
      }
    }
  }

  /* "quiet_hours": {"enabled": true|false, "start_min": M, "end_min": M}
   *
   * The original implementation gated on "\"quiet_hours\"" at the top
   * level but then searched for "\"enabled\"" / "\"start_min\"" /
   * "\"end_min\"" from the start of the body — meaning a future
   * top-level `enabled` field (or any other object that happens to
   * contain `enabled`) could overwrite qh.en with the wrong value.
   *
   * Walk the brace pair of the quiet_hours object and search ONLY
   * within that span. We temporarily nul-terminate at the closing
   * brace so strstr can't see past it, then restore the byte. Body
   * is a local buffer; mutating it is fine. */
  if (char* qh_key = (char*)strstr(body, "\"quiet_hours\"")) {
    char* qh_open = strchr(qh_key, '{');
    if (qh_open) {
      int depth = 1;
      char* p = qh_open + 1;
      for (; *p; ++p) {
        if (*p == '{') depth++;
        else if (*p == '}') {
          if (--depth == 0) break;
        }
      }
      /* p now points at the matching close brace, or '\0' if malformed.
       * Either way, nul-terminate one past it so strstr sees only the
       * object's contents. Save the byte to restore after parsing. */
      char saved = *p;
      *p = '\0';

      if (const char* e = strstr(qh_open, "\"enabled\"")) {
        if (const char* v = strchr(e, ':')) {
          v++;
          while (*v == ' ' || *v == '\t' || *v == '"') v++;
          if (strncmp(v, "true", 4) == 0) {
            prefs.putBool("qh.en", true);  wrote_anything = true;
          } else if (strncmp(v, "false", 5) == 0) {
            prefs.putBool("qh.en", false); wrote_anything = true;
          }
        }
      }
      auto put_minute = [&](const char* tag, const char* nvs) {
        const char* k = strstr(qh_open, tag);
        if (!k) return;
        const char* v = strchr(k, ':');
        if (!v) return;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '"') v++;
        char* vend = nullptr;
        long n = strtol(v, &vend, 10);
        if (vend == v) return;
        if (n < 0)    n = 0;
        if (n > 1439) n = 1439;
        prefs.putInt(nvs, (int32_t)n);
        wrote_anything = true;
      };
      put_minute("\"start_min\"", "qh.start");
      put_minute("\"end_min\"",   "qh.end");

      *p = saved;  /* restore for any later parsers and for cleanliness */
    }
  }

  prefs.end();

  if (!wrote_anything) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "{\"ok\":false,\"reason\":\"no recognised keys\"}", -1);
    return ESP_OK;
  }

  /* Re-init affected module so it picks up the new values on the next tick. */
  reinit_module("core.presence");

  /* Re-apply Quiet Hours to the chokepoint. The dashboard's settings
   * panel may have changed any of the three keys; rather than parse
   * the request again, just re-read NVS via the shared helper. The
   * next module emit on the main loop sees the new config and
   * flushes a held_summary if we just transitioned out of an active
   * window. */
  apply_quiet_hours_from_nvs();

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"ok\":true}", -1);
  return ESP_OK;
}

esp_err_t handle_privacy_budget(httpd_req_t* req) {
  /* Returns the literal outbound-byte count plus the current privacy
   * ceiling so the dashboard can warm-tint the pill when the user has
   * raised the ceiling above P0. ceiling=p0 + bytes=0 → cool pill;
   * any change → warm pill. Cheap: a single 32-bit read and a
   * three-letter switch.
   *
   * Cache-Control: no-store. The whole point of the pill is "what is
   * the device sending right now" — a cached zero would lie. */
  const csi_privacy_class_t ceiling = csi_event_get_privacy_ceiling();
  const char* ceiling_str = (ceiling == CSI_PRIVACY_P0) ? "p0"
                          : (ceiling == CSI_PRIVACY_P1) ? "p1" : "p2";

  /* Atomic load — the counter is updated lock-free from any export
   * path; see add_outbound_bytes() above for the threading rationale. */
  const uint32_t bytes = __atomic_load_n(&g_outbound_bytes, __ATOMIC_RELAXED);
  char buf[96];
  snprintf(buf, sizeof(buf),
    "{\"bytes_today\":%lu,\"ceiling\":\"%s\",\"since_ms\":%lu}",
    (unsigned long)bytes,
    ceiling_str,
    (unsigned long)(millis() - g_stream_started_ms));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, buf, -1);
  return ESP_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * PWA assets — /manifest.webmanifest + /sw.js
 *
 * The companion PWA at /companion ships its own SW scoped to /companion.
 * The headline Sensing dashboard at / didn't have a PWA layer, so
 * "Add to Home Screen" landed on a generic browser bookmark with no
 * offline shell. This pair gives the dashboard a proper PWA identity:
 * an install promptable manifest and a tiny SW that caches the shell.
 *
 * The SW uses a network-first strategy for the cached URLs so live
 * dashboard updates land whenever WiFi is reachable; cache fallback
 * only when offline. Live API routes (/api/csi/stream, etc.) are
 * deliberately NOT in the precache list — they always hit the device.
 *
 * The icon is rendered inline as an SVG data URI so we don't need a
 * separate /icon.png route. Apple/Android home-screen icons accept
 * SVG; the orb-style gradient circle matches the dashboard's hero
 * widget.
 * ────────────────────────────────────────────────────────────────────────── */

const char SENSE_MANIFEST_JSON[] PROGMEM =
  "{"
    "\"name\":\"SecuraCV Canary\","
    "\"short_name\":\"Canary\","
    "\"start_url\":\"/\","
    "\"scope\":\"/\","
    "\"display\":\"standalone\","
    "\"background_color\":\"#0c0a18\","
    "\"theme_color\":\"#8e9eff\","
    "\"description\":\"Camera-free sensing dashboard.\","
    "\"icons\":["
      "{"
        "\"src\":\"data:image/svg+xml;utf8,"
          "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 256 256'>"
          "<defs><radialGradient id='g' cx='40%25' cy='35%25' r='65%25'>"
          "<stop offset='0%25' stop-color='%23cfd6ff'/>"
          "<stop offset='55%25' stop-color='%238e9eff'/>"
          "<stop offset='100%25' stop-color='%231f2546'/>"
          "</radialGradient></defs>"
          "<circle cx='128' cy='128' r='118' fill='url(%23g)'/>"
          "</svg>\","
        "\"sizes\":\"any\","
        "\"type\":\"image/svg+xml\","
        "\"purpose\":\"any maskable\""
      "}"
    "]"
  "}";

const char SENSE_SW_JS[] PROGMEM =
  "const CACHE='securacv-sense-v1';\n"
  "const URLS=['/','/manifest.webmanifest'];\n"
  "self.addEventListener('install',e=>{e.waitUntil(caches.open(CACHE).then(c=>c.addAll(URLS)));self.skipWaiting();});\n"
  "self.addEventListener('activate',e=>{e.waitUntil(caches.keys().then(keys=>Promise.all(keys.filter(k=>k!==CACHE).map(k=>caches.delete(k)))).then(()=>self.clients.claim()));});\n"
  "self.addEventListener('fetch',e=>{\n"
  "  if(e.request.method!=='GET')return;\n"
  "  const u=new URL(e.request.url);\n"
  "  /* Live data endpoints always hit the network — never cache. */\n"
  "  if(u.pathname.startsWith('/api/'))return;\n"
  "  const wantsCache=URLS.some(p=>u.pathname===p);\n"
  "  if(!wantsCache)return; /* pass-through for everything outside our shell */\n"
  "  e.respondWith(fetch(e.request).then(r=>{\n"
  "    if(r&&r.ok){const copy=r.clone();caches.open(CACHE).then(c=>c.put(e.request,copy));}\n"
  "    return r;\n"
  "  }).catch(()=>caches.match(e.request)));\n"
  "});\n";

esp_err_t handle_sense_manifest(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/manifest+json");
  /* The shell rarely changes within a session — let the browser cache
   * the manifest itself for an hour. The SW separately revalidates the
   * shell's HTML on each visit. */
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
  return httpd_resp_send(req, SENSE_MANIFEST_JSON, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handle_sense_sw(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/javascript");
  /* The Service-Worker-Allowed header lets the SW take a wider scope
   * than its own URL when the page registers it with `scope: '/'`.
   * The SW lives at the root so this is decorative for now, but
   * keeping it makes future relocations harmless. */
  httpd_resp_set_hdr(req, "Service-Worker-Allowed", "/");
  /* SW updates need to bypass HTTP cache so a new version of this
   * string activates on next install. The browser still caches the
   * SW for ~24h max regardless of headers; this is the lower bound. */
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, SENSE_SW_JS, HTTPD_RESP_USE_STRLEN);
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
 * TUNING LAB (Pillar D / Tier 4 #10)
 *
 * Hidden P2 surface at /tune. Lists every NVS-backed coefficient in
 * SETTING_KEYS as a labeled slider with min, max, default. Save/Load
 * preset writes/reads a local JSON bundle (no network egress) so a
 * tinkerer can ship a baseline between devices or back up before
 * experiments.
 *
 * Why a separate metadata table next to SETTING_KEYS?
 *   SETTING_KEYS only knows the (full_key, nvs_key) pair — it can't
 *   render a slider on its own. The metadata below adds the bits the
 *   UI needs (label, kind, range, default) and the bit the POST
 *   handler needs (which module to reinit). Co-locating these two
 *   tables keeps the abbreviation map small while still making "add
 *   a new coefficient" a one-place change.
 * ────────────────────────────────────────────────────────────────────────── */

enum TuneKind { TK_INT, TK_BOOL, TK_MINUTES };

struct TuneCoeff {
  const char* full_key;       /* e.g. "core.presence.preset" */
  const char* group;          /* "core.presence" */
  const char* label;          /* short human label for the slider */
  TuneKind    kind;           /* INT | BOOL | MINUTES (HH:MM render) */
  int32_t     min_v;
  int32_t     max_v;
  int32_t     default_v;
  const char* reinit_module;  /* module id to reinit on change ("" = none) */
};

const TuneCoeff TUNE_COEFFS[] = {
  /* Presence — preset (0=sensitive,1=balanced,2=quiet) + sensitivity slider
   * map onto the three direct thresholds; exposing all five lets a
   * tuner pin individual values without the preset overriding them. */
  { "core.presence.preset",              "core.presence",  "Preset (0=sensitive 1=balanced 2=quiet)", TK_INT,     0,    2,    1,  "core.presence" },
  { "core.presence.sensitivity",         "core.presence",  "Sensitivity (0..100)",                     TK_INT,     0,    100,  50, "core.presence" },
  { "core.presence.motion_threshold",    "core.presence",  "Motion threshold",                          TK_INT,     5,    120,  35, "core.presence" },
  { "core.presence.active_threshold",    "core.presence",  "Active threshold",                          TK_INT,     5,    120,  75, "core.presence" },
  { "core.presence.breathing_threshold", "core.presence",  "Breathing threshold",                       TK_INT,     5,    120,  30, "core.presence" },
  { "core.presence.pet_mode",            "core.presence",  "Pet mode",                                  TK_BOOL,    0,    1,    0,  "core.presence" },
  { "core.presence.pet_mode_seconds",    "core.presence",  "Pet-mode confirm window (sec)",             TK_INT,     5,    120,  30, "core.presence" },

  /* Breathing — Goertzel band lock parameters. */
  { "core.breathing.lock_threshold",     "core.breathing", "Lock threshold",                            TK_INT,     5,    120,  30, "core.breathing" },
  { "core.breathing.confirm_seconds",    "core.breathing", "Confirm window (sec)",                      TK_INT,     5,    60,   20, "core.breathing" },

  /* Quiet hours — minutes-of-day window the dashboard dims and future
   * notification paths can suppress against. */
  { "core.quiet_hours.enabled",          "core.quiet_hours","Enabled",                                  TK_BOOL,    0,    1,    0,  "" },
  { "core.quiet_hours.start_min",        "core.quiet_hours","Start",                                    TK_MINUTES, 0,    1439, 0,  "" },
  { "core.quiet_hours.end_min",          "core.quiet_hours","End",                                      TK_MINUTES, 0,    1439, 480,"" },

  /* Anomaly baseline — out-of-pattern detector envelope. The runtime
   * clamps these inside the module on read; the UI mirrors the same
   * envelope so a tuner can't accidentally pick a value the runtime
   * will silently round off. */
  { "anomaly.baseline.spike_ratio",      "anomaly.baseline","Spike ratio (× baseline, 100 = 1.0×)",    TK_INT,     110,  1000, 250,"anomaly.baseline" },
  { "anomaly.baseline.min_motion",       "anomaly.baseline","Motion floor",                            TK_INT,     1,    100,  60, "anomaly.baseline" },
  { "anomaly.baseline.min_breathing",    "anomaly.baseline","Breathing floor",                         TK_INT,     1,    100,  50, "anomaly.baseline" },
  { "anomaly.baseline.cooldown_sec",     "anomaly.baseline","Per-channel cooldown (sec)",              TK_INT,     30,   3600, 600,"anomaly.baseline" },
};

const TuneCoeff* tune_coeff_for(const char* full_key) {
  if (!full_key) return nullptr;
  for (const TuneCoeff& c : TUNE_COEFFS) {
    if (strcmp(c.full_key, full_key) == 0) return &c;
  }
  return nullptr;
}

int32_t tune_clamp(const TuneCoeff& c, int32_t v) {
  if (v < c.min_v) return c.min_v;
  if (v > c.max_v) return c.max_v;
  return v;
}

/* Read the persisted value for one coefficient, or fall back to its
 * declared default. The declared default mirrors what each module
 * passes as its `csi_module_settings_int default` argument; if a value
 * has never been written, GET should still return that exact default
 * so the slider position matches what the module would actually use.
 *
 * Defensive guard: if a TuneCoeff is ever added without a matching
 * SETTING_KEYS row, nvs_key_for() returns nullptr and we fall back to
 * the declared default rather than passing NULL into Preferences. */
int32_t tune_read_value(Preferences& prefs, const TuneCoeff& c) {
  const char* nvs = nvs_key_for(c.full_key);
  if (!nvs) return c.default_v;
  if (c.kind == TK_BOOL) {
    return prefs.getBool(nvs, c.default_v != 0) ? 1 : 0;
  }
  return prefs.getInt(nvs, c.default_v);
}

void tune_write_value(Preferences& prefs, const TuneCoeff& c, int32_t v) {
  const char* nvs = nvs_key_for(c.full_key);
  if (!nvs) return;
  v = tune_clamp(c, v);
  if (c.kind == TK_BOOL) {
    prefs.putBool(nvs, v != 0);
  } else {
    prefs.putInt(nvs, v);
  }
}

esp_err_t handle_tune_page(httpd_req_t* req) {
  /* P2 surface — no auth gate; the URL is unguessable by design and
   * the dashboard's long-press affordance is the canonical entry. */
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, TUNE_UI_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handle_tune_get_coefficients(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  Preferences prefs;
  bool prefs_ok = prefs.begin(SETTINGS_NS, /*readOnly=*/true);

  /* Stream out one big JSON object; chunked send keeps RAM bounded
   * even as the table grows past the 16-coefficient v1 set. */
  httpd_resp_send_chunk(req, "{\"coefficients\":[", -1);
  bool first = true;
  for (const TuneCoeff& c : TUNE_COEFFS) {
    int32_t v = prefs_ok ? tune_read_value(prefs, c) : c.default_v;
    char buf[320];
    const char* kind_str = (c.kind == TK_BOOL) ? "bool"
                         : (c.kind == TK_MINUTES) ? "minutes" : "int";
    int n = snprintf(buf, sizeof(buf),
      "%s{\"full_key\":\"%s\",\"group\":\"%s\",\"label\":\"%s\",\"kind\":\"%s\","
      "\"min\":%ld,\"max\":%ld,\"default\":%ld,\"value\":%ld,\"step\":1}",
      first ? "" : ",",
      c.full_key, c.group, c.label, kind_str,
      (long)c.min_v, (long)c.max_v, (long)c.default_v, (long)v);
    if (n > 0) httpd_resp_send_chunk(req, buf, n);
    first = false;
  }
  httpd_resp_send_chunk(req, "]}", -1);
  httpd_resp_send_chunk(req, nullptr, 0);  /* end-of-chunks */
  if (prefs_ok) prefs.end();
  return ESP_OK;
}

/* Find one or more "key":value pairs in the body and write each. The
 * parser is intentionally minimal — it walks the body looking for
 * keys we recognise from TUNE_COEFFS and a numeric or true/false RHS.
 * Any unrecognised key is silently ignored (P2; tinkerers are not
 * expected to need detailed feedback on typos). */
esp_err_t handle_tune_post_coefficients(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");

  size_t total = req->content_len;
  if (total == 0 || total > 4096) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"empty or too large\"}", -1);
  }
  char* body = (char*)malloc(total + 1);
  if (!body) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"oom\"}", -1);
  }
  size_t read = 0;
  while (read < total) {
    int n = httpd_req_recv(req, body + read, total - read);
    if (n <= 0) { free(body); return ESP_FAIL; }
    read += (size_t)n;
  }
  body[total] = '\0';

  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) {
    free(body);
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"nvs unavailable\"}", -1);
  }

  /* Track which modules we need to reinit. A small fixed set keeps
   * us from reinit-spamming when one POST changes several coefficients
   * that all live under the same module. */
  bool reinit_presence  = false;
  bool reinit_breathing = false;
  bool reinit_anomaly   = false;
  int  changed = 0;

  for (const TuneCoeff& c : TUNE_COEFFS) {
    /* Locate "<full_key>" in the body, then walk to the colon and
     * the value. We require the surrounding quotes so that
     * "core.presence.pet_mode" doesn't accidentally match
     * "not_pet_mode" or similar substrings. */
    char needle[80];
    int nl = snprintf(needle, sizeof(needle), "\"%s\"", c.full_key);
    if (nl <= 0 || nl >= (int)sizeof(needle)) continue;
    const char* p = strstr(body, needle);
    if (!p) continue;
    p += nl;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') continue;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    int32_t v;
    if (c.kind == TK_BOOL) {
      if      (strncmp(p, "true",  4) == 0) v = 1;
      else if (strncmp(p, "false", 5) == 0) v = 0;
      else if (strncmp(p, "1",     1) == 0) v = 1;
      else if (strncmp(p, "0",     1) == 0) v = 0;
      else continue;
    } else {
      char* end = nullptr;
      long n = strtol(p, &end, 10);
      if (end == p) continue;
      v = (int32_t)n;
    }

    tune_write_value(prefs, c, v);
    changed++;
    if      (strcmp(c.reinit_module, "core.presence")    == 0) reinit_presence  = true;
    else if (strcmp(c.reinit_module, "core.breathing")   == 0) reinit_breathing = true;
    else if (strcmp(c.reinit_module, "anomaly.baseline") == 0) reinit_anomaly   = true;
  }
  prefs.end();
  free(body);

  if (changed == 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "{\"ok\":false,\"reason\":\"no recognised keys\"}", -1);
  }

  if (reinit_presence)  reinit_module("core.presence");
  if (reinit_breathing) reinit_module("core.breathing");
  if (reinit_anomaly)   reinit_module("anomaly.baseline");

  char ok[48];
  snprintf(ok, sizeof(ok), "{\"ok\":true,\"changed\":%d}", changed);
  return httpd_resp_send(req, ok, -1);
}

esp_err_t handle_tune_get_preset(httpd_req_t* req) {
  /* Preset bundle: a flat JSON object mapping each coefficient's full
   * key to its current value. Identical shape to what POST consumes,
   * so a Save→Load round-trip is the identity. The bundle is local to
   * the device's filesystem of the user's browser; no network egress.
   *
   * Privacy: nothing in here ties to identity, but it does reveal a
   * tuner's calibration. Treated as P2 (developer only) — the dashboard
   * gates this surface behind the long-press affordance. */
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"tuning-preset.json\"");

  Preferences prefs;
  bool prefs_ok = prefs.begin(SETTINGS_NS, /*readOnly=*/true);

  httpd_resp_send_chunk(req, "{", -1);
  bool first = true;
  for (const TuneCoeff& c : TUNE_COEFFS) {
    int32_t v = prefs_ok ? tune_read_value(prefs, c) : c.default_v;
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "%s\"%s\":%ld",
                     first ? "" : ",", c.full_key, (long)v);
    if (n > 0) httpd_resp_send_chunk(req, buf, n);
    first = false;
  }
  httpd_resp_send_chunk(req, "}", -1);
  httpd_resp_send_chunk(req, nullptr, 0);
  if (prefs_ok) prefs.end();
  return ESP_OK;
}

esp_err_t handle_tune_post_preset(httpd_req_t* req) {
  /* The preset bundle uses the same key/value shape as
   * handle_tune_post_coefficients, so we can just re-use that
   * handler — it walks the body looking for known keys and writes
   * each. The only difference is a preset typically carries every
   * coefficient at once. */
  return handle_tune_post_coefficients(req);
}

/* ──────────────────────────────────────────────────────────────────────────
 * PAIRING TOKEN STORE — Tier 5 #11
 *
 * RAM-only ring of one-shot tokens for the captive-portal QR onboarding
 * flow. The captive-portal handler bakes the token into the QR; the
 * companion PWA validates / consumes it before showing the WiFi
 * credentials form.
 *
 * Slots: small fixed pool. Each slot tracks 32 random bytes, the
 * issuance time, and a "used" flag. Eviction prefers (a) used slots,
 * (b) expired slots, (c) the oldest unused slot. That last clause means
 * a determined attacker can churn out token issuances and force the
 * eviction of a token a real user is mid-onboarding with — but the
 * legitimate user is already on the device's AP at that point, and
 * the PWA simply re-fetches /api/pair/token if validation fails. No
 * security regression vs. the older 302-redirect design.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr size_t   PAIR_SLOTS        = 4;
constexpr uint32_t PAIR_TTL_MS       = 10UL * 60UL * 1000UL;  /* 10 min */
constexpr size_t   PAIR_TOK_BYTES    = 32;                    /* 256-bit entropy */
constexpr size_t   PAIR_TOK_HEX_LEN  = PAIR_TOK_BYTES * 2;    /* 64 hex chars */

struct PairSlot {
  bool     used;
  bool     active;
  uint32_t issued_ms;
  uint8_t  token[PAIR_TOK_BYTES];
};
PairSlot g_pair_slots[PAIR_SLOTS] = {};

void hex_encode(const uint8_t* in, size_t len, char* out) {
  static const char* H = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out[2*i  ] = H[(in[i] >> 4) & 0xF];
    out[2*i+1] = H[ in[i]       & 0xF];
  }
  out[2*len] = '\0';
}

bool hex_decode_to(const char* hex, uint8_t* out, size_t out_len) {
  if (!hex || strlen(hex) != out_len * 2) return false;
  for (size_t i = 0; i < out_len; ++i) {
    char hi = hex[2*i], lo = hex[2*i+1];
    auto val = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return 10 + c - 'a';
      if (c >= 'A' && c <= 'F') return 10 + c - 'A';
      return -1;
    };
    int hi_v = val(hi), lo_v = val(lo);
    if (hi_v < 0 || lo_v < 0) return false;
    out[i] = (uint8_t)((hi_v << 4) | lo_v);
  }
  return true;
}

/* Constant-time compare so a timing oracle can't tell us what's wrong. */
bool ct_eq(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; ++i) d |= a[i] ^ b[i];
  return d == 0;
}

PairSlot* pick_slot_for_issuance() {
  const uint32_t now = millis();
  /* Pass 1: prefer a used or expired slot. */
  for (size_t i = 0; i < PAIR_SLOTS; ++i) {
    PairSlot& s = g_pair_slots[i];
    if (!s.active || s.used || (now - s.issued_ms) >= PAIR_TTL_MS) return &s;
  }
  /* Pass 2: evict the oldest active-and-unused slot. */
  size_t oldest = 0;
  for (size_t i = 1; i < PAIR_SLOTS; ++i) {
    if ((now - g_pair_slots[i].issued_ms) > (now - g_pair_slots[oldest].issued_ms)) {
      oldest = i;
    }
  }
  return &g_pair_slots[oldest];
}

PairSlot* find_slot(const uint8_t* token) {
  const uint32_t now = millis();
  for (size_t i = 0; i < PAIR_SLOTS; ++i) {
    PairSlot& s = g_pair_slots[i];
    if (!s.active || s.used) continue;
    if ((now - s.issued_ms) >= PAIR_TTL_MS) continue;
    if (ct_eq(s.token, token, PAIR_TOK_BYTES)) return &s;
  }
  return nullptr;
}

esp_err_t handle_pair_token(httpd_req_t* req) {
  /* Issues a fresh one-shot token. The captive-portal handler also calls
   * the C++ helper directly (it embeds the same token in the QR), but
   * having the route lets a manually-typed companion path or a future
   * mobile flow refresh on demand. */
  char hex[PAIR_TOK_HEX_LEN + 1];
  if (!csi_integration::pair_token_issue(hex, sizeof(hex))) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "{\"ok\":false}", -1);
  }
  /* 256 is comfortable for the current envelope (URL-encoded token is
   * 64 chars and the surrounding JSON is ~110 chars). We still pass
   * -1 (HTTPD_RESP_USE_STRLEN) so that future schema changes that
   * stretch this payload past the buffer get safely truncated by
   * snprintf and reflected by strlen — the alternative of using
   * snprintf's return value directly would trip a stack read overflow
   * on truncation. */
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"ok\":true,\"token\":\"%s\",\"expires_in_sec\":%lu,\"pair_url\":\"http://192.168.4.1/companion?token=%s\"}",
    hex, (unsigned long)(PAIR_TTL_MS / 1000UL), hex);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, buf, -1);
}

/* ──────────────────────────────────────────────────────────────────────────
 * MODULE REGISTRATION
 * ────────────────────────────────────────────────────────────────────────── */

void register_v1_modules() {
  csi_module_register(core_presence_module());
  csi_module_register(core_breathing_module());
  csi_module_register(core_activity_ribbon_module());
  csi_module_register(meta_daily_summary_module());
  /* meta.quiet_hours holds the manifest entry for the held_summary row
   * the chokepoint synthesises at the moment a configured Quiet Hours
   * window closes. Registering the module is what lets that synthetic
   * emit pass the chokepoint's allow-list check. */
  csi_module_register(meta_quiet_hours_module());
  /* Tier 3 #7: baseline-aware anomaly detector. P0, no identity, just
   * "this room rarely looks like that." Watches the same features
   * stream the four core modules see. */
  csi_module_register(anomaly_baseline_module());

  /* spec/event_contract.md §10: BLE Discovery semantic events. The
   * module exists so any BLE → witness-chain emit MUST go through the
   * chokepoint, where the per-event allow-list strips fields that
   * carry MAC addresses, RSSI at tracking precision, or stable
   * hardware identifiers. Helpers in ble_events_module.h are the only
   * legitimate BLE→witness path going forward. */
  csi_module_register(ble_events_module());

  /* Wire the persisted Quiet Hours range into the chokepoint. The
   * dashboard's settings panel writes qh.en / qh.start / qh.end via
   * /api/settings POST; rebooting the device picks them back up. */
  apply_quiet_hours_from_nvs();
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

void add_outbound_bytes(uint32_t bytes) {
  /* Lock-free CAS loop. The function is documented as callable from
   * any host export path, including paths that run on different
   * FreeRTOS tasks (a future MQTT publisher on the WiFi task, an SD
   * exporter on the storage task, the HTTP task reading the counter
   * for /api/privacy-budget). A naive read-modify-write would lose
   * concurrent increments — the BLE export task's bytes could be
   * stomped by the MQTT task and the user would see an under-count,
   * which silently undermines the privacy promise the pill makes.
   *
   * GCC built-in atomics are available on ESP32's xtensa toolchain
   * with no extra header. Saturating add at UINT32_MAX rather than
   * wrap, since silently rolling back to 0 would lie. */
  uint32_t expected = __atomic_load_n(&g_outbound_bytes, __ATOMIC_RELAXED);
  uint32_t desired;
  do {
    desired = expected + bytes;
    if (desired < expected) desired = UINT32_MAX;  // overflow → saturate
  } while (!__atomic_compare_exchange_n(
      &g_outbound_bytes, &expected, desired,
      /*weak=*/false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

uint32_t outbound_bytes_today() {
  return __atomic_load_n(&g_outbound_bytes, __ATOMIC_RELAXED);
}

/* ──────────────────────────────────────────────────────────────────────────
 * PUBLIC TOKEN API (Tier 5 #11)
 * ────────────────────────────────────────────────────────────────────────── */

bool pair_token_issue(char* hex_out, size_t out_cap) {
  if (!hex_out || out_cap < PAIR_TOK_HEX_LEN + 1) return false;
  PairSlot* s = pick_slot_for_issuance();
  if (!s) return false;
  esp_fill_random(s->token, PAIR_TOK_BYTES);
  s->issued_ms = millis();
  s->used      = false;
  s->active    = true;
  hex_encode(s->token, PAIR_TOK_BYTES, hex_out);
  return true;
}

bool pair_token_valid(const char* hex) {
  if (!hex) return false;
  uint8_t raw[PAIR_TOK_BYTES];
  if (!hex_decode_to(hex, raw, PAIR_TOK_BYTES)) return false;
  return find_slot(raw) != nullptr;
}

bool pair_token_consume(const char* hex) {
  if (!hex) return false;
  uint8_t raw[PAIR_TOK_BYTES];
  if (!hex_decode_to(hex, raw, PAIR_TOK_BYTES)) return false;
  PairSlot* s = find_slot(raw);
  if (!s) return false;
  s->used = true;
  return true;
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

  /* /api/privacy-budget — literal byte counter for outbound traffic.
   * 0 by default (the device is local-first); other code calls
   * csi_integration::add_outbound_bytes() when it sends data to a
   * destination outside the user's immediate network. */
  static httpd_uri_t r_privacy_budget = {
    .uri = "/api/privacy-budget", .method = HTTP_GET, .handler = handle_privacy_budget
  };
  httpd_register_uri_handler(server, &r_privacy_budget);

  /* Dashboard PWA shell — manifest + service worker. The SW is
   * scope-/ so it can intercept dashboard fetches; live API routes
   * are explicitly passed through inside the SW. */
  static httpd_uri_t r_manifest = {
    .uri = "/manifest.webmanifest", .method = HTTP_GET, .handler = handle_sense_manifest
  };
  httpd_register_uri_handler(server, &r_manifest);
  static httpd_uri_t r_sw = {
    .uri = "/sw.js", .method = HTTP_GET, .handler = handle_sense_sw
  };
  httpd_register_uri_handler(server, &r_sw);

  /* Tier 4 #10 — Tuning Lab. P2 surface; the route reservation in
   * canary_wap.ino's start_http_server() needs five extra slots for
   * the page + the two coefficient endpoints + the two preset
   * endpoints. */
  static httpd_uri_t r_tune_page = {
    .uri = "/tune", .method = HTTP_GET, .handler = handle_tune_page
  };
  httpd_register_uri_handler(server, &r_tune_page);
  static httpd_uri_t r_tune_get = {
    .uri = "/api/tune/coefficients", .method = HTTP_GET, .handler = handle_tune_get_coefficients
  };
  httpd_register_uri_handler(server, &r_tune_get);
  static httpd_uri_t r_tune_post = {
    .uri = "/api/tune/coefficients", .method = HTTP_POST, .handler = handle_tune_post_coefficients
  };
  httpd_register_uri_handler(server, &r_tune_post);
  static httpd_uri_t r_tune_preset_get = {
    .uri = "/api/tune/preset", .method = HTTP_GET, .handler = handle_tune_get_preset
  };
  httpd_register_uri_handler(server, &r_tune_preset_get);
  static httpd_uri_t r_tune_preset_post = {
    .uri = "/api/tune/preset", .method = HTTP_POST, .handler = handle_tune_post_preset
  };
  httpd_register_uri_handler(server, &r_tune_preset_post);

  /* Tier 5 #11 — pairing token issuance. The captive portal handler in
   * canary_wap.ino calls pair_token_issue() directly to bake the token
   * into the QR; this route is for the companion PWA to refresh a
   * token if the captive-portal copy expired before the user finished
   * entering credentials. */
  static httpd_uri_t r_pair_token = {
    .uri = "/api/pair/token", .method = HTTP_GET, .handler = handle_pair_token
  };
  httpd_register_uri_handler(server, &r_pair_token);

  g_initialized = true;
  Serial.printf("[CSI] integration ready: %u modules, 16 routes registered\n",
                (unsigned)csi_module_count());
  return true;
}

}  /* namespace csi_integration */
