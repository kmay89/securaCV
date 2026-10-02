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
  if (!prefs.begin(csi_module_settings_nvs::kNamespace, /*readOnly=*/true)) return;
  const QuietHours qh = read_quiet_hours(prefs);
  prefs.end();
  csi_event_set_quiet_window((uint16_t)qh.start_min, (uint16_t)qh.end_min, qh.enabled);
}
