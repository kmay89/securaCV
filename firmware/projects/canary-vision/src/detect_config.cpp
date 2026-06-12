#include "canary/detect_config.h"

#include <Arduino.h>
#include <Preferences.h>

#include "canary/config.h"
#include "canary/log.h"

namespace canary::cfg {

namespace {

DetectConfig g_det{};
bool g_det_loaded = false;
bool g_nvs_ok = false;

// Same NVS namespace as runtime_config (identity/credentials); detection
// keys carry a det_ prefix so the two families can't collide.
constexpr const char* NVS_NS = "securacv";

template <typename T>
T clamp_val(T v, T lo, T hi) {
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

void load() {
  if (g_det_loaded) return;

  // Compiled defaults seed the very first boot.
  g_det.person_target   = (uint8_t)PERSON_TARGET;
  g_det.score_min       = (uint8_t)SCORE_MIN;
  g_det.lost_timeout_ms = LOST_TIMEOUT_MS;
  g_det.dwell_start_ms  = DWELL_START_MS;

  // Read-only open avoids write-locking on the hot boot path — but it
  // fails on a factory-fresh unit where the namespace doesn't exist yet
  // (and detect() can run before runtime_config creates it), so fall back
  // to a read/write open that creates the namespace.
  Preferences prefs;
  bool opened = prefs.begin(NVS_NS, /*readOnly=*/true);
  if (!opened) opened = prefs.begin(NVS_NS, /*readOnly=*/false);
  if (!opened) {
    g_det_loaded = true;
    log_line("CFG", "NVS unavailable — using compiled detection defaults.");
    return;
  }
  g_nvs_ok = true;

  g_det.person_target = prefs.getUChar("det_target", g_det.person_target);
  g_det.score_min = clamp_val<uint8_t>(
      prefs.getUChar("det_score", g_det.score_min),
      DETECT_SCORE_MIN_LO, DETECT_SCORE_MIN_HI);
  g_det.lost_timeout_ms = clamp_val<uint32_t>(
      prefs.getULong("det_lost", g_det.lost_timeout_ms),
      DETECT_LOST_MS_LO, DETECT_LOST_MS_HI);
  g_det.dwell_start_ms = clamp_val<uint32_t>(
      prefs.getULong("det_dwell", g_det.dwell_start_ms),
      DETECT_DWELL_MS_LO, DETECT_DWELL_MS_HI);

  prefs.end();
  g_det_loaded = true;
}

bool persist_uchar(const char* key, uint8_t v) {
  if (!g_nvs_ok) return true;  // applied in RAM for this boot
  Preferences prefs;
  if (!prefs.begin(NVS_NS, /*readOnly=*/false)) return false;
  prefs.putUChar(key, v);
  prefs.end();
  return true;
}

bool persist_ulong(const char* key, uint32_t v) {
  if (!g_nvs_ok) return true;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, /*readOnly=*/false)) return false;
  prefs.putULong(key, v);
  prefs.end();
  return true;
}

}  // namespace

const DetectConfig& detect() {
  load();
  return g_det;
}

bool detect_set_person_target(uint8_t v) {
  load();
  if (g_det.person_target == v) return false;
  g_det.person_target = v;
  persist_uchar("det_target", v);
  return true;
}

bool detect_set_score_min(uint8_t v) {
  load();
  v = clamp_val<uint8_t>(v, DETECT_SCORE_MIN_LO, DETECT_SCORE_MIN_HI);
  if (g_det.score_min == v) return false;
  g_det.score_min = v;
  persist_uchar("det_score", v);
  return true;
}

bool detect_set_lost_timeout_ms(uint32_t v) {
  load();
  v = clamp_val<uint32_t>(v, DETECT_LOST_MS_LO, DETECT_LOST_MS_HI);
  if (g_det.lost_timeout_ms == v) return false;
  g_det.lost_timeout_ms = v;
  persist_ulong("det_lost", v);
  return true;
}

bool detect_set_dwell_start_ms(uint32_t v) {
  load();
  v = clamp_val<uint32_t>(v, DETECT_DWELL_MS_LO, DETECT_DWELL_MS_HI);
  if (g_det.dwell_start_ms == v) return false;
  g_det.dwell_start_ms = v;
  persist_ulong("det_dwell", v);
  return true;
}

} // namespace canary::cfg
