/**
 * @file csi_module_settings_nvs.h
 * @brief The one rule both firmware trees read a CSI module's stored
 *        settings by (sweep F93).
 *
 * A module reads its tunables in init() through csi_module_settings_int /
 * _bool / _float (csi_module.h), by dotted key ("core.presence.preset").
 * The library's definitions are weak and return the caller's default; each
 * host overrides all three to read NVS. The canary (PlatformIO,
 * canary/src/csi_modules_integration.cpp) and the canary-wap (Arduino
 * sketch, csi_settings_nvs.cpp) both override them with the readers below,
 * so a key means the same NVS row on both, read the same way:
 *
 *   - namespace "csi", short key from kKeys (ESP32 Preferences keys are at
 *     most 15 characters, so "core.presence.preset" is stored as
 *     "cp.preset");
 *   - a dotted key not in kKeys reads as the caller's default, and no NVS
 *     handle is opened for it;
 *   - a namespace that will not open, or a row that is absent, reads as the
 *     caller's default;
 *   - at boot, one read-only handle for every module's init(): the host
 *     derives its csi_module_settings from Session below and hands one to
 *     csi_module_init_all(), and a namespace that will not open makes every
 *     read of that boot the default, after one attempt (Arduino's
 *     Preferences logs each failed open, and a canary's NVS has no "csi"
 *     namespace unless a canary-wap image made one);
 *   - outside a session (a NULL handle: the canary-wap's re-init after a
 *     settings change), one read-only handle per read, closed before it
 *     returns;
 *   - the value is returned as stored, typed as asked (getInt / getBool /
 *     getFloat); range checks are the module's own (anomaly.baseline clamps
 *     on read, for one).
 *
 * The canary-wap writes these rows (/api/settings, the threshold
 * calibration, the Tuning Lab); the canary has no surface that writes them,
 * so on a canary they hold only what a canary-wap image stored on the same
 * board. The namespace also holds host keys that are not module settings
 * (the canary-wap's time zone, transmitter filter and event-id floor); they
 * are not listed here.
 *
 * Header-only and Arduino-free: the readers take the Preferences type as a
 * template parameter, so the host tests instantiate them over a fake store.
 * C++11 (the canary's core-2.x envs).
 *
 * A host wires it up in three pieces (both trees do exactly this):
 *
 *   struct csi_module_settings : csi_module_settings_nvs::Session<Preferences> {};
 *
 *   extern "C" int32_t csi_module_settings_int(const csi_module_settings_t* s,
 *                                              const char* key, int32_t d) {
 *     return csi_module_settings_nvs::read_int<Preferences>(s, key, d);
 *   }   // and _bool, _float
 *
 *   csi_module_settings boot;                 // the boot init
 *   csi_module_settings_nvs::begin(boot);
 *   csi_module_init_all(&boot);
 *   csi_module_settings_nvs::end(boot);
 */

#ifndef SECURACV_CSI_MODULE_SETTINGS_NVS_H
#define SECURACV_CSI_MODULE_SETTINGS_NVS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace csi_module_settings_nvs {

/** The NVS namespace every module setting lives in. */
constexpr const char* kNamespace = "csi";

/** A module setting's dotted key and the NVS key it is stored under. */
struct Key {
  const char* full;   /* "core.presence.pet_mode" */
  const char* nvs;    /* "cp.pet_mode" (15 characters at most) */
};

constexpr Key kKeys[] = {
  /* core.presence: the dashboard's preset (0 sensitive, 1 balanced,
   * 2 quiet) and sensitivity slider (0..100) set the three thresholds; a
   * threshold stored directly (the calibration, the Tuning Lab) wins over
   * them. */
  { "core.presence.pet_mode",             "cp.pet_mode" },
  { "core.presence.preset",               "cp.preset"   },
  { "core.presence.sensitivity",          "cp.sens"     },
  { "core.presence.motion_threshold",     "cp.mt"       },
  { "core.presence.active_threshold",     "cp.at"       },
  { "core.presence.breathing_threshold",  "cp.bt"       },
  { "core.presence.pet_mode_seconds",     "cp.ps"       },
  /* core.presence's multipath shimmer filter */
  { "core.presence.shimmer_rssi_swing",   "cp.srs"      },
  { "core.presence.shimmer_doppler_floor","cp.sdf"      },
  { "core.presence.shimmer_enabled",      "cp.se"       },
  /* core.breathing */
  { "core.breathing.lock_threshold",      "cb.lt"       },
  { "core.breathing.confirm_seconds",     "cb.cs"       },
  /* Quiet Hours (minutes of day). The canary-wap's chokepoint reads these
   * rows directly; listed so the Tuning Lab can name them. */
  { "core.quiet_hours.enabled",           "qh.en"       },
  { "core.quiet_hours.start_min",         "qh.start"    },
  { "core.quiet_hours.end_min",           "qh.end"      },
  /* The canary-wap's privacy ceiling (0/1/2), which its host reads
   * directly; no module reads it. */
  { "core.privacy_ceiling",               "cp.pc"       },
  /* anomaly.baseline */
  { "anomaly.baseline.spike_ratio",       "ab.sr"       },
  { "anomaly.baseline.min_motion",        "ab.mm"       },
  { "anomaly.baseline.min_breathing",     "ab.mb"       },
  { "anomaly.baseline.cooldown_sec",      "ab.cd"       },
};

constexpr size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

/** The NVS key a dotted module key is stored under, or nullptr. */
inline const char* nvs_key_for(const char* full_key) {
  if (full_key == nullptr) return nullptr;
  for (size_t i = 0; i < kKeyCount; ++i) {
    if (strcmp(kKeys[i].full, full_key) == 0) return kKeys[i].nvs;
  }
  return nullptr;
}

/** One read-only handle for a run of reads: a boot's every module init().
 *  `open` is false until begin() opens it, and stays false when the
 *  namespace will not open; every read in the run then returns its
 *  default. `Prefs` is Arduino-ESP32's Preferences, or a test fake with the
 *  same begin / end / getInt / getBool / getFloat. */
template <class Prefs>
struct Session {
  mutable Prefs prefs;
  bool open = false;
};

template <class Prefs>
void begin(Session<Prefs>& session) {
  session.open = session.prefs.begin(kNamespace, /*readOnly=*/true);
}

template <class Prefs>
void end(Session<Prefs>& session) {
  if (session.open) session.prefs.end();
  session.open = false;
}

/* One read: through `session` when there is one (opened or not), else
 * through a handle of its own. */
template <class Prefs, class T, class Get>
T read(const Session<Prefs>* session, const char* full_key, T default_value, Get get) {
  const char* nvs_key = nvs_key_for(full_key);
  if (nvs_key == nullptr) return default_value;
  if (session != nullptr) {
    return session->open ? get(session->prefs, nvs_key, default_value) : default_value;
  }
  Prefs prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/true)) return default_value;
  const T v = get(prefs, nvs_key, default_value);
  prefs.end();
  return v;
}

template <class Prefs>
int32_t read_int(const Session<Prefs>* session, const char* full_key, int32_t default_value) {
  return read(session, full_key, default_value,
              [](Prefs& p, const char* k, int32_t d) -> int32_t { return p.getInt(k, d); });
}

template <class Prefs>
bool read_bool(const Session<Prefs>* session, const char* full_key, bool default_value) {
  return read(session, full_key, default_value,
              [](Prefs& p, const char* k, bool d) -> bool { return p.getBool(k, d); });
}

template <class Prefs>
float read_float(const Session<Prefs>* session, const char* full_key, float default_value) {
  return read(session, full_key, default_value,
              [](Prefs& p, const char* k, float d) -> float { return p.getFloat(k, d); });
}

}  /* namespace csi_module_settings_nvs */

#endif /* SECURACV_CSI_MODULE_SETTINGS_NVS_H */
