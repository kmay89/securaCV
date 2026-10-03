/**
 * @file csi_settings_nvs.cpp
 * @brief The canary-wap's CSI module settings readers (sweep F93) and its
 *        stored Quiet Hours (sweeps F123, F128).
 *
 * The library declares csi_module_settings_int / _bool / _float weak, each
 * returning the caller's default (csi_module.cpp). These strong definitions
 * read NVS by csi_module_settings_nvs.h's rule (staged copy), the one the
 * canary's bridge (canary/src/csi_modules_integration.cpp) reads by too:
 * namespace "csi", the shared short keys, typed reads, and the caller's
 * default for an unmapped key, a row that is absent or a namespace that
 * will not open.
 *
 * A module reads them in its init(): once at boot, through
 * csi_settings_nvs_init_modules() below (csi_integration::init() calls it
 * right after register_v1_modules()), with one read-only handle for the
 * whole boot; and again through reinit_module() after /api/settings,
 * /api/csi/calibrate/apply or a Tuning Lab change writes a row, with a
 * handle per read (a NULL settings handle). They lived in
 * csi_integration.cpp, which no host suite compiles; here
 * tests_host/test_wap_module_boot.cpp builds them over a fake Preferences,
 * with the staged library and modules.
 *
 * The Quiet Hours reader and its apply moved here from csi_integration.cpp
 * for the same reason, and so did the dashboard's Quiet Hours store (POST
 * /api/settings), which now writes by the key map the reader reads by:
 * tests_host/test_wap_tune_lab.cpp runs them, with the Tuning Lab's POST
 * (csi_tune_lab.cpp) that now applies them too.
 */

#include "csi_settings_nvs.h"

#include <stdlib.h>
#include <string.h>

#include <Preferences.h>

#include "core_presence.h"   /* core_presence_baseline_thresholds() (F166) */
#include "csi_event.h"
#include "csi_module.h"
#include "csi_module_settings_nvs.h"

/* The settings handle is a read session (csi_module_settings_nvs.h). */
struct csi_module_settings : csi_module_settings_nvs::Session<Preferences> {};

extern "C" int32_t csi_module_settings_int(const csi_module_settings_t* settings,
                                           const char* key,
                                           int32_t default_value) {
  return csi_module_settings_nvs::read_int<Preferences>(settings, key, default_value);
}

extern "C" bool csi_module_settings_bool(const csi_module_settings_t* settings,
                                         const char* key,
                                         bool default_value) {
  return csi_module_settings_nvs::read_bool<Preferences>(settings, key, default_value);
}

extern "C" float csi_module_settings_float(const csi_module_settings_t* settings,
                                           const char* key,
                                           float default_value) {
  return csi_module_settings_nvs::read_float<Preferences>(settings, key, default_value);
}

size_t csi_settings_nvs_init_modules(void) {
  csi_module_settings boot;
  csi_module_settings_nvs::begin(boot);
  const size_t ran = csi_module_init_all(&boot);
  csi_module_settings_nvs::end(boot);
  return ran;
}

QuietHours read_quiet_hours(Preferences& prefs) {
  using csi_module_settings_nvs::nvs_key_for;
  QuietHours qh;
  qh.enabled   = prefs.getBool(nvs_key_for("core.quiet_hours.enabled"), kQuietHoursDefaultEnabled);
  qh.start_min = prefs.getInt(nvs_key_for("core.quiet_hours.start_min"), kQuietHoursDefaultStartMin);
  qh.end_min   = prefs.getInt(nvs_key_for("core.quiet_hours.end_min"), kQuietHoursDefaultEndMin);
  return qh;
}

bool store_quiet_hours_from_settings(Preferences& prefs, char* body) {
  using csi_module_settings_nvs::nvs_key_for;
  /* The original implementation gated on "\"quiet_hours\"" at the top
   * level but then searched for "\"enabled\"" / "\"start_min\"" /
   * "\"end_min\"" from the start of the body — meaning a future
   * top-level `enabled` field (or any other object that happens to
   * contain `enabled`) could overwrite qh.en with the wrong value.
   *
   * Walk the brace pair of the quiet_hours object and search ONLY
   * within that span. We temporarily nul-terminate at the closing
   * brace so strstr can't see past it, then restore the byte. */
  char* qh_key = strstr(body, "\"quiet_hours\"");
  if (qh_key == nullptr) return false;
  char* qh_open = strchr(qh_key, '{');
  if (qh_open == nullptr) return false;
  int depth = 1;
  char* p = qh_open + 1;
  for (; *p; ++p) {
    if (*p == '{') depth++;
    else if (*p == '}') {
      if (--depth == 0) break;
    }
  }
  /* p now points at the matching close brace, or '\0' if malformed.
   * Either way, nul-terminate there so strstr sees only the object's
   * contents. Save the byte to restore after parsing. */
  const char saved = *p;
  *p = '\0';

  bool stored = false;
  if (const char* e = strstr(qh_open, "\"enabled\"")) {
    if (const char* v = strchr(e, ':')) {
      v++;
      while (*v == ' ' || *v == '\t' || *v == '"') v++;
      if (strncmp(v, "true", 4) == 0) {
        prefs.putBool(nvs_key_for("core.quiet_hours.enabled"), true);  stored = true;
      } else if (strncmp(v, "false", 5) == 0) {
        prefs.putBool(nvs_key_for("core.quiet_hours.enabled"), false); stored = true;
      }
    }
  }
  auto put_minute = [&](const char* tag, const char* full_key) {
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
    prefs.putInt(nvs_key_for(full_key), (int32_t)n);
    stored = true;
  };
  put_minute("\"start_min\"", "core.quiet_hours.start_min");
  put_minute("\"end_min\"",   "core.quiet_hours.end_min");

  *p = saved;  /* restore for any later parsers and for cleanliness */
  return stored;
}

void apply_quiet_hours_from_nvs(void) {
  Preferences prefs;
  if (!csi_module_settings_nvs::begin_read_only(prefs)) return;
  const QuietHours qh = read_quiet_hours(prefs);
  prefs.end();
  csi_event_set_quiet_window((uint16_t)qh.start_min, (uint16_t)qh.end_min, qh.enabled);
}

/* ── core.presence: the dashboard and the calibration (sweep F151) ──── */

PresenceSettings presence_settings_defaults(void) {
  PresenceSettings s;
  s.pet_mode    = false;
  s.preset      = 1;    /* balanced */
  s.sensitivity = 50;
  return s;
}

PresenceSettings read_presence_settings(Preferences& prefs) {
  using csi_module_settings_nvs::nvs_key_for;
  const PresenceSettings d = presence_settings_defaults();
  PresenceSettings s;
  s.pet_mode    = prefs.getBool(nvs_key_for("core.presence.pet_mode"), d.pet_mode);
  s.preset      = prefs.getInt(nvs_key_for("core.presence.preset"), d.preset);
  s.sensitivity = prefs.getInt(nvs_key_for("core.presence.sensitivity"), d.sensitivity);
  return s;
}

bool store_presence_from_settings(Preferences& prefs, const char* body) {
  using csi_module_settings_nvs::nvs_key_for;
  bool stored = false;

  /* "pet_mode": true|false */
  if (const char* k = strstr(body, "\"pet_mode\"")) {
    if (const char* v = strchr(k, ':')) {
      v++;
      while (*v == ' ' || *v == '\t' || *v == '"') v++;
      if (strncmp(v, "true", 4) == 0) {
        prefs.putBool(nvs_key_for("core.presence.pet_mode"), true);  stored = true;
      } else if (strncmp(v, "false", 5) == 0) {
        prefs.putBool(nvs_key_for("core.presence.pet_mode"), false); stored = true;
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
        prefs.putInt(nvs_key_for("core.presence.preset"), idx);
        stored = true;
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
        prefs.putInt(nvs_key_for("core.presence.sensitivity"), (int32_t)n);
        stored = true;
      }
    }
  }
  return stored;
}

/* Nothing stored: the baseline core.presence's init() derives from the
 * preset and sensitivity. */
static PresenceThresholdsInUse presence_baseline_in_use(const PresenceSettings& settings) {
  const core_presence_thresholds_t base =
      core_presence_baseline_thresholds(settings.preset, settings.sensitivity);
  PresenceThresholdsInUse t;
  t.thresholds.motion    = base.motion;
  t.thresholds.active    = base.active;
  t.thresholds.breathing = base.breathing;
  t.motion_stored = false;
  t.active_stored = false;
  t.breathing_stored = false;
  return t;
}

/* One threshold row over its baseline: init() reads it with getInt and the
 * baseline as the default, so a row getInt reads wins. Two reads with
 * different defaults tell a read row from a default without a sentinel. */
static void presence_threshold_row(Preferences& prefs, const char* full_key, int32_t* value,
                                   bool* stored) {
  const char* key = csi_module_settings_nvs::nvs_key_for(full_key);
  const int32_t a = prefs.getInt(key, 0);
  const int32_t b = prefs.getInt(key, 1);
  *stored = (a == b);
  if (*stored) *value = a;
}

PresenceThresholdsInUse read_presence_thresholds_in_use(Preferences& prefs) {
  PresenceThresholdsInUse t = presence_baseline_in_use(read_presence_settings(prefs));
  presence_threshold_row(prefs, "core.presence.motion_threshold", &t.thresholds.motion,
                         &t.motion_stored);
  presence_threshold_row(prefs, "core.presence.active_threshold", &t.thresholds.active,
                         &t.active_stored);
  presence_threshold_row(prefs, "core.presence.breathing_threshold", &t.thresholds.breathing,
                         &t.breathing_stored);
  return t;
}

PresenceThresholdsInUse presence_thresholds_in_use_unread(void) {
  return presence_baseline_in_use(presence_settings_defaults());
}

const char* presence_thresholds_source(const PresenceThresholdsInUse& in_use) {
  const int stored = (in_use.motion_stored ? 1 : 0) + (in_use.active_stored ? 1 : 0) +
                     (in_use.breathing_stored ? 1 : 0);
  if (stored == 3) return "stored";
  if (stored == 0) return "preset";
  return "mixed";
}

bool store_presence_thresholds(Preferences& prefs, const PresenceThresholds& thresholds) {
  using csi_module_settings_nvs::nvs_key_for;
  const bool motion = prefs.putInt(nvs_key_for("core.presence.motion_threshold"), thresholds.motion) > 0;
  const bool active = prefs.putInt(nvs_key_for("core.presence.active_threshold"), thresholds.active) > 0;
  const bool breathing =
      prefs.putInt(nvs_key_for("core.presence.breathing_threshold"), thresholds.breathing) > 0;
  return motion && active && breathing;
}

/* ── The privacy ceiling ─────────────────────────────────────────────── */

int32_t read_privacy_ceiling(Preferences& prefs) {
  return prefs.getInt(csi_module_settings_nvs::nvs_key_for("core.privacy_ceiling"),
                      (int32_t)CSI_PRIVACY_P0);
}

bool store_privacy_ceiling_from_settings(Preferences& prefs, const char* body) {
  /* "privacy_ceiling": "p0" | "p1" | "p2". Persisted as int 0/1/2 so
   * apply_privacy_ceiling_from_nvs() can compare against the
   * CSI_PRIVACY_* enum directly. Unrecognized values are ignored — the
   * existing persisted value (or P0 default) survives. */
  const char* k = strstr(body, "\"privacy_ceiling\"");
  if (k == nullptr) return false;
  const char* v = strchr(k, ':');
  if (v == nullptr) return false;
  v++;
  while (*v == ' ' || *v == '\t' || *v == '"') v++;
  int32_t val = -1;
  if      (strncmp(v, "p0", 2) == 0) val = (int32_t)CSI_PRIVACY_P0;
  else if (strncmp(v, "p1", 2) == 0) val = (int32_t)CSI_PRIVACY_P1;
  else if (strncmp(v, "p2", 2) == 0) val = (int32_t)CSI_PRIVACY_P2;
  if (val < 0) return false;
  prefs.putInt(csi_module_settings_nvs::nvs_key_for("core.privacy_ceiling"), val);
  return true;
}

/* Restore the persisted privacy ceiling. Without this every reboot reverts
 * to P0 and the user has to re-consent to P1/P2 every power cycle, which made
 * the Tuning Lab effectively unreachable. Default is P0 (privacy-first): any
 * out-of-range value falls back to P0 rather than silently elevating to a more
 * permissive level. Moved from csi_integration.cpp (sweep F151). */
void apply_privacy_ceiling_from_nvs(void) {
  Preferences prefs;
  if (!csi_module_settings_nvs::begin_read_only(prefs)) return;
  const int32_t raw = read_privacy_ceiling(prefs);
  prefs.end();
  csi_privacy_class_t ceiling;
  switch (raw) {
    case (int32_t)CSI_PRIVACY_P1: ceiling = CSI_PRIVACY_P1; break;
    case (int32_t)CSI_PRIVACY_P2: ceiling = CSI_PRIVACY_P2; break;
    default:                      ceiling = CSI_PRIVACY_P0; break;
  }
  csi_event_set_privacy_ceiling(ceiling);
}

/* ── The boot's first read of the namespace (sweep F150) ─────────────── */

bool read_event_id_floor_rows(const char* floor_key, const char* ceiling_key,
                              uint32_t* floor, uint32_t* ceiling) {
  *floor = 0;
  *ceiling = 0;
  Preferences prefs;
  if (!csi_module_settings_nvs::begin_read_only(prefs)) return false;
  *floor = (uint32_t)prefs.getULong(floor_key, 0);
  *ceiling = (uint32_t)prefs.getULong(ceiling_key, 0);
  prefs.end();
  return true;
}
