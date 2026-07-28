#include "canary/sense_config.h"

#include <Arduino.h>
#include <Preferences.h>

#include "canary/config.h"
#include "canary/log.h"

namespace canary::cfg {

namespace {

SenseConfig g_sense{};
bool g_sense_loaded = false;
bool g_nvs_ok = false;

// Same NVS namespace as runtime_config (identity/credentials) and the
// flasher's WiFi seed; reflex keys carry a sns_ prefix so no family collides.
constexpr const char* NVS_NS = "securacv";

// The wellbeing build compiles the vitals macros in; the presence-only build
// doesn't — seed its (unused) vitals slots with the wellbeing defaults so the
// struct is never garbage and a later flavor switch inherits sane numbers.
#if defined(CS_VITALS_LOCK_MS)
constexpr uint32_t SEED_VLOCK = CS_VITALS_LOCK_MS;
constexpr uint32_t SEED_VLOST = CS_VITALS_LOST_MS;
#else
constexpr uint32_t SEED_VLOCK = 4000;
constexpr uint32_t SEED_VLOST = 6000;
#endif
#if defined(CS_BREATH_MIN_BPM)
constexpr uint32_t SEED_BMIN = CS_BREATH_MIN_BPM;
constexpr uint32_t SEED_BMAX = CS_BREATH_MAX_BPM;
constexpr uint32_t SEED_HMIN = CS_HEART_MIN_BPM;
constexpr uint32_t SEED_HMAX = CS_HEART_MAX_BPM;
#else
constexpr uint32_t SEED_BMIN = 6;
constexpr uint32_t SEED_BMAX = 30;
constexpr uint32_t SEED_HMIN = 40;
constexpr uint32_t SEED_HMAX = 130;
#endif

uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

void load() {
  if (g_sense_loaded) return;

  // Compiled defaults seed the very first boot.
  g_sense.present_debounce_ms = CS_PRESENT_DEBOUNCE_MS;
  g_sense.clear_timeout_ms    = CS_CLEAR_TIMEOUT_MS;
  g_sense.stall_timeout_ms    = CS_RADAR_STALL_MS;
  g_sense.near_cm             = CS_RANGE_NEAR_CM;
  g_sense.mid_cm              = CS_RANGE_MID_CM;
  g_sense.vitals_lock_ms      = SEED_VLOCK;
  g_sense.vitals_lost_ms      = SEED_VLOST;
  g_sense.breath_min_bpm      = SEED_BMIN;
  g_sense.breath_max_bpm      = SEED_BMAX;
  g_sense.heart_min_bpm       = SEED_HMIN;
  g_sense.heart_max_bpm       = SEED_HMAX;

  // Read-only open first (hot path); fall back to read/write so a
  // factory-fresh unit creates the namespace — detect_config.cpp precedent.
  Preferences prefs;
  bool opened = prefs.begin(NVS_NS, /*readOnly=*/true);
  if (!opened) opened = prefs.begin(NVS_NS, /*readOnly=*/false);
  if (!opened) {
    g_sense_loaded = true;
    log_line("CFG", "NVS unavailable — using compiled radar reflexes.");
    return;
  }
  g_nvs_ok = true;

  g_sense.present_debounce_ms = clamp_u32(
      prefs.getULong("sns_debounce", g_sense.present_debounce_ms),
      SENSE_DEBOUNCE_MS_LO, SENSE_DEBOUNCE_MS_HI);
  g_sense.clear_timeout_ms = clamp_u32(
      prefs.getULong("sns_clear", g_sense.clear_timeout_ms),
      SENSE_CLEAR_MS_LO, SENSE_CLEAR_MS_HI);
  g_sense.stall_timeout_ms = clamp_u32(
      prefs.getULong("sns_stall", g_sense.stall_timeout_ms),
      SENSE_STALL_MS_LO, SENSE_STALL_MS_HI);
  g_sense.near_cm = clamp_u32(
      prefs.getULong("sns_near", g_sense.near_cm),
      SENSE_NEAR_CM_LO, SENSE_NEAR_CM_HI);
  g_sense.mid_cm = clamp_u32(
      prefs.getULong("sns_mid", g_sense.mid_cm),
      SENSE_MID_CM_LO, SENSE_MID_CM_HI);
  g_sense.vitals_lock_ms = clamp_u32(
      prefs.getULong("sns_vlock", g_sense.vitals_lock_ms),
      SENSE_VLOCK_MS_LO, SENSE_VLOCK_MS_HI);
  g_sense.vitals_lost_ms = clamp_u32(
      prefs.getULong("sns_vlost", g_sense.vitals_lost_ms),
      SENSE_VLOST_MS_LO, SENSE_VLOST_MS_HI);
  g_sense.breath_min_bpm = clamp_u32(
      prefs.getULong("sns_bmin", g_sense.breath_min_bpm),
      SENSE_BREATH_MIN_LO, SENSE_BREATH_MIN_HI);
  g_sense.breath_max_bpm = clamp_u32(
      prefs.getULong("sns_bmax", g_sense.breath_max_bpm),
      SENSE_BREATH_MAX_LO, SENSE_BREATH_MAX_HI);
  g_sense.heart_min_bpm = clamp_u32(
      prefs.getULong("sns_hmin", g_sense.heart_min_bpm),
      SENSE_HEART_MIN_LO, SENSE_HEART_MIN_HI);
  g_sense.heart_max_bpm = clamp_u32(
      prefs.getULong("sns_hmax", g_sense.heart_max_bpm),
      SENSE_HEART_MAX_LO, SENSE_HEART_MAX_HI);

  prefs.end();
  g_sense_loaded = true;
}

bool persist_ulong(const char* key, uint32_t v) {
  if (!g_nvs_ok) return true;  // applied in RAM for this boot
  Preferences prefs;
  if (!prefs.begin(NVS_NS, /*readOnly=*/false)) return false;
  prefs.putULong(key, v);
  prefs.end();
  return true;
}

bool set_field(uint32_t& field, const char* key, uint32_t v,
               uint32_t lo, uint32_t hi) {
  load();
  v = clamp_u32(v, lo, hi);
  if (field == v) return false;
  field = v;
  persist_ulong(key, v);
  return true;
}

}  // namespace

const SenseConfig& sense() {
  load();
  return g_sense;
}

bool sense_set_present_debounce_ms(uint32_t v) {
  return set_field(g_sense.present_debounce_ms, "sns_debounce", v,
                   SENSE_DEBOUNCE_MS_LO, SENSE_DEBOUNCE_MS_HI);
}
bool sense_set_clear_timeout_ms(uint32_t v) {
  return set_field(g_sense.clear_timeout_ms, "sns_clear", v,
                   SENSE_CLEAR_MS_LO, SENSE_CLEAR_MS_HI);
}
bool sense_set_stall_timeout_ms(uint32_t v) {
  return set_field(g_sense.stall_timeout_ms, "sns_stall", v,
                   SENSE_STALL_MS_LO, SENSE_STALL_MS_HI);
}
bool sense_set_near_cm(uint32_t v) {
  return set_field(g_sense.near_cm, "sns_near", v,
                   SENSE_NEAR_CM_LO, SENSE_NEAR_CM_HI);
}
bool sense_set_mid_cm(uint32_t v) {
  return set_field(g_sense.mid_cm, "sns_mid", v,
                   SENSE_MID_CM_LO, SENSE_MID_CM_HI);
}
bool sense_set_vitals_lock_ms(uint32_t v) {
  return set_field(g_sense.vitals_lock_ms, "sns_vlock", v,
                   SENSE_VLOCK_MS_LO, SENSE_VLOCK_MS_HI);
}
bool sense_set_vitals_lost_ms(uint32_t v) {
  return set_field(g_sense.vitals_lost_ms, "sns_vlost", v,
                   SENSE_VLOST_MS_LO, SENSE_VLOST_MS_HI);
}
bool sense_set_breath_min_bpm(uint32_t v) {
  return set_field(g_sense.breath_min_bpm, "sns_bmin", v,
                   SENSE_BREATH_MIN_LO, SENSE_BREATH_MIN_HI);
}
bool sense_set_breath_max_bpm(uint32_t v) {
  return set_field(g_sense.breath_max_bpm, "sns_bmax", v,
                   SENSE_BREATH_MAX_LO, SENSE_BREATH_MAX_HI);
}
bool sense_set_heart_min_bpm(uint32_t v) {
  return set_field(g_sense.heart_min_bpm, "sns_hmin", v,
                   SENSE_HEART_MIN_LO, SENSE_HEART_MIN_HI);
}
bool sense_set_heart_max_bpm(uint32_t v) {
  return set_field(g_sense.heart_max_bpm, "sns_hmax", v,
                   SENSE_HEART_MAX_LO, SENSE_HEART_MAX_HI);
}

} // namespace canary::cfg
