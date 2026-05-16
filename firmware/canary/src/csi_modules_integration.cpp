/*
 * SecuraCV Canary — CSI module pipeline integration (PIO build)
 *
 * Includes ONLY the common library headers so the local
 * `csi_features_t` (declared in `securacv_csi.h`) and the common
 * `csi_features_t` (declared in `csi_types.h`) never collide in the
 * same translation unit. The bridge takes a `void*` from main.cpp's
 * features callback; the runtime size check below verifies the two
 * structs stay byte-compatible.
 */

#include "csi_modules_integration.h"

/* Common CSI library — module pipeline + chokepoint + bundler. */
#include "csi_types.h"
#include "csi_module.h"
#include "csi_event.h"

/* v1 modules — same set the canary-wap Arduino build registers. */
#include "core_presence.h"
#include "core_breathing.h"
#include "core_activity_ribbon.h"
#include "meta_daily_summary.h"
#include "anomaly_baseline.h"
#include "core_multilink_fusion.h"
#include "meta_empty_room_baseline.h"

#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
#include "ble_scout.h"
#endif

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

namespace {

bool s_initialized = false;

/* ──────────────────────────────────────────────────────────────────────────
 * SETTINGS — NVS-backed, mapped through the same short-key convention the
 * canary-wap Arduino build uses. ESP32 Preferences imposes a 15-char key
 * limit, so module-style "core.presence.preset" maps to "cp.preset" etc.
 * Keeping the same NVS layout means a device flashed with canary-wap and
 * later re-flashed with canary PIO retains its tunables.
 * ────────────────────────────────────────────────────────────────────────── */

constexpr const char* SETTINGS_NS = "csi";

struct SettingKey {
  const char* full;
  const char* nvs;
};

const SettingKey SETTING_KEYS[] = {
  /* core.presence */
  { "core.presence.pet_mode",            "cp.pet_mode"   },
  { "core.presence.preset",              "cp.preset"     },
  { "core.presence.sensitivity",         "cp.sens"       },
  { "core.presence.motion_threshold",    "cp.mt"         },
  { "core.presence.active_threshold",    "cp.at"         },
  { "core.presence.breathing_threshold", "cp.bt"         },
  { "core.presence.pet_mode_seconds",    "cp.ps"         },
  /* core.breathing */
  { "core.breathing.lock_threshold",     "cb.lt"         },
  { "core.breathing.confirm_seconds",    "cb.cs"         },
  /* core.quiet_hours */
  { "core.quiet_hours.enabled",          "qh.en"         },
  { "core.quiet_hours.start_min",        "qh.start"      },
  { "core.quiet_hours.end_min",          "qh.end"        },
  /* anomaly.baseline */
  { "anomaly.baseline.spike_ratio",      "ab.sr"         },
  { "anomaly.baseline.min_motion",       "ab.mm"         },
  { "anomaly.baseline.min_breathing",    "ab.mb"         },
  { "anomaly.baseline.cooldown_sec",     "ab.cd"         },
};

const char* nvs_key_for(const char* full_key) {
  if (!full_key) return nullptr;
  for (const SettingKey& k : SETTING_KEYS) {
    if (strcmp(k.full, full_key) == 0) return k.nvs;
  }
  return nullptr;
}

}  /* namespace */

/* ──────────────────────────────────────────────────────────────────────────
 * STRONG OVERRIDES — csi_module_settings_*
 *
 * The library declares each accessor `__attribute__((weak))` returning the
 * caller's default. Here we look up the canonical full key, map to the
 * short NVS key, and read the persisted value. Read-only Preferences
 * handles are opened per call — settings reads are infrequent (boot +
 * post-POST reinit) so the small open/close cost is fine.
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
 * PUBLIC BRIDGE
 * ────────────────────────────────────────────────────────────────────────── */

extern "C" bool securacv_csi_modules_init(void) {
  if (s_initialized) {
    /* Re-running init re-reads NVS — it's a deliberate path used after a
     * /api/settings POST. The module dispatcher tolerates a second
     * register_module() of the same id by replacing the prior entry. */
  }

  csi_event_set_privacy_ceiling(CSI_PRIVACY_P0);

  csi_module_register(core_presence_module());
  csi_module_register(core_breathing_module());
  csi_module_register(core_activity_ribbon_module());
  csi_module_register(meta_daily_summary_module());
  csi_module_register(anomaly_baseline_module());
  /* Multi-link motion confirmation (PR 3) — promotes single-link
   * "observed" to multi-link "confirmed" when ≥2 links agree within
   * a 3-second window. PR 3 added the module but never wired it; this
   * PR closes that gap alongside meta.empty_room_baseline. */
  csi_module_register(core_multilink_fusion_module());
  /* Scheduled 10-min empty-room baseline calibration (PR 4a). */
  csi_module_register(meta_empty_room_baseline_module());

#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN
  /* BLE Scout — paired-beacon room-attribution (PR 5b). Lives in
   * the securacv_ble_scan library; only registered when the build
   * opts in via -DFEATURE_BLE_SCAN=1 (and pulls in NimBLE-Arduino
   * via the [env:full] lib_deps). The module's csi_event_decl_t
   * manifest constrains every emit to state_name/note/time_bucket
   * — no MAC or hashed_id ever lands in an event payload. */
  csi_module_register(ble_scout::ble_scout_module());
  /* Load the per-device key + start the NimBLE passive scan loop.
   * Safe to call even if NimBLE isn't actually available — the
   * scan-loop TU is an empty file in that case and ble_scout_init
   * still wires up the registry+tracker. */
  ble_scout::ble_scout_init();
#endif

  s_initialized = true;
  return true;
}

extern "C" void securacv_csi_modules_feed(const void* features_blob) {
  if (!s_initialized || !features_blob) return;
  /* Both csi_features_t structs are layout-identical (same field order,
   * same int8/uint16/uint8 widths). The library's csi_types.h
   * static-asserts the 32-byte vector width and we trust the canary HAL
   * to honor the documented contract — this cast is the bridge.
   *
   * The size assertion guarding this lives in main.cpp where both types
   * are visible (this TU only sees the common one). */
  const csi_features_t* f = static_cast<const csi_features_t*>(features_blob);
  csi_module_tick_all(f);
  /* Drain bundled events whose 10-minute window has elapsed. The
   * bundler buffers same-state observations and commits one row per
   * window; this call is what makes the long tail land. */
  csi_event_flush_bundles();
}

extern "C" void securacv_csi_modules_deinit(void) {
  s_initialized = false;
}
