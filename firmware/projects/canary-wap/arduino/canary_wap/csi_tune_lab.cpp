/**
 * @file csi_tune_lab.cpp
 * @brief The Tuning Lab's knobs and its POST's store-and-apply (sweeps F123,
 *        F128). The HTTP handlers stay in csi_integration.cpp; see
 *        csi_tune_lab.h.
 */

#include "csi_tune_lab.h"

#include <Preferences.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csi_module_settings_nvs.h"
#include "csi_settings_nvs.h"

using csi_module_settings_nvs::nvs_key_for;

const TuneCoeff TUNE_COEFFS[] = {
  /* Presence — preset (0=sensitive,1=balanced,2=quiet) + sensitivity slider
   * map onto the three direct thresholds; exposing all five lets a
   * tuner pin individual values without the preset overriding them. */
  { "core.presence.preset",              "core.presence",  "Preset (0=sensitive 1=balanced 2=quiet)", TK_INT,     0,    2,    1,  TA_REINIT_PRESENCE },
  { "core.presence.sensitivity",         "core.presence",  "Sensitivity (0..100)",                     TK_INT,     0,    100,  50, TA_REINIT_PRESENCE },
  { "core.presence.motion_threshold",    "core.presence",  "Motion threshold",                          TK_INT,     5,    120,  35, TA_REINIT_PRESENCE },
  { "core.presence.active_threshold",    "core.presence",  "Active threshold",                          TK_INT,     5,    120,  75, TA_REINIT_PRESENCE },
  { "core.presence.breathing_threshold", "core.presence",  "Breathing threshold",                       TK_INT,     5,    120,  30, TA_REINIT_PRESENCE },
  { "core.presence.pet_mode",            "core.presence",  "Pet mode",                                  TK_BOOL,    0,    1,    0,  TA_REINIT_PRESENCE },
  { "core.presence.pet_mode_seconds",    "core.presence",  "Pet-mode confirm window (sec)",             TK_INT,     5,    120,  30, TA_REINIT_PRESENCE },
  /* Multipath shimmer rejection — large RSSI swing without Doppler is
   * reflection noise, not motion. Defaults mirror core_presence.cpp. */
  { "core.presence.shimmer_enabled",     "core.presence",  "Shimmer rejection enabled",                 TK_BOOL,    0,    1,    1,  TA_REINIT_PRESENCE },
  { "core.presence.shimmer_rssi_swing",  "core.presence",  "Shimmer RSSI swing threshold (dB)",         TK_INT,     1,    50,   8,  TA_REINIT_PRESENCE },
  { "core.presence.shimmer_doppler_floor","core.presence", "Shimmer Doppler floor",                     TK_INT,     1,    120,  30, TA_REINIT_PRESENCE },

  /* Breathing — Goertzel band lock parameters. */
  { "core.breathing.lock_threshold",     "core.breathing", "Lock threshold",                            TK_INT,     5,    120,  30, TA_REINIT_BREATHING },
  { "core.breathing.confirm_seconds",    "core.breathing", "Confirm window (sec)",                      TK_INT,     5,    60,   20, TA_REINIT_BREATHING },

  /* Quiet hours — minutes-of-day window the chokepoint holds non-anomaly
   * rows in. Declared with the device's own defaults (sweep F123: they said
   * 00:00 to 08:00 while the device ran 23:00 to 07:00), and applied at once
   * (sweep F128: they applied only from the next boot). */
  { "core.quiet_hours.enabled",          "core.quiet_hours","Enabled",                                  TK_BOOL,    0,    1,    kQuietHoursDefaultEnabled ? 1 : 0, TA_QUIET_HOURS },
  { "core.quiet_hours.start_min",        "core.quiet_hours","Start",                                    TK_MINUTES, 0,    1439, kQuietHoursDefaultStartMin,        TA_QUIET_HOURS },
  { "core.quiet_hours.end_min",          "core.quiet_hours","End",                                      TK_MINUTES, 0,    1439, kQuietHoursDefaultEndMin,          TA_QUIET_HOURS },

  /* Anomaly baseline — out-of-pattern detector envelope. The runtime
   * clamps these inside the module on read; the UI mirrors the same
   * envelope so a tuner can't accidentally pick a value the runtime
   * will silently round off. */
  { "anomaly.baseline.spike_ratio",      "anomaly.baseline","Spike ratio (× baseline, 100 = 1.0×)",    TK_INT,     110,  1000, 250,TA_REINIT_ANOMALY },
  { "anomaly.baseline.min_motion",       "anomaly.baseline","Motion floor",                            TK_INT,     1,    100,  60, TA_REINIT_ANOMALY },
  { "anomaly.baseline.min_breathing",    "anomaly.baseline","Breathing floor",                         TK_INT,     1,    100,  50, TA_REINIT_ANOMALY },
  { "anomaly.baseline.cooldown_sec",     "anomaly.baseline","Per-channel cooldown (sec)",              TK_INT,     30,   3600, 600,TA_REINIT_ANOMALY },
};

const size_t TUNE_COEFF_COUNT = sizeof(TUNE_COEFFS) / sizeof(TUNE_COEFFS[0]);

const TuneCoeff* tune_coeff_for(const char* full_key) {
  if (!full_key) return nullptr;
  for (size_t i = 0; i < TUNE_COEFF_COUNT; ++i) {
    if (strcmp(TUNE_COEFFS[i].full_key, full_key) == 0) return &TUNE_COEFFS[i];
  }
  return nullptr;
}

int32_t tune_clamp(const TuneCoeff& c, int32_t v) {
  if (v < c.min_v) return c.min_v;
  if (v > c.max_v) return c.max_v;
  return v;
}

/* Read the persisted value for one coefficient, or fall back to its
 * declared default. The declared default mirrors what the device runs with
 * the row absent (each module's `csi_module_settings_int default` argument;
 * for Quiet Hours, csi_settings_nvs.h's constants), so GET returns the
 * value the device would actually use and the slider sits there.
 *
 * Defensive guard: if a TuneCoeff is ever added without a matching
 * key-map row, nvs_key_for() returns nullptr and we fall back to
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

/* Find one or more "key":value pairs in the body and write each. The
 * parser is intentionally minimal — it walks the body looking for
 * keys we recognize from TUNE_COEFFS and a numeric or true/false RHS.
 * Any unrecognized key is silently ignored (P2; tinkerers are not
 * expected to need detailed feedback on typos). */
TunePost tune_post(const char* body, void (*reinit)(const char* module_id)) {
  TunePost post;
  memset(&post, 0, sizeof(post));
  if (!body) return post;

  Preferences prefs;
  if (!prefs.begin(csi_module_settings_nvs::kNamespace, /*readOnly=*/false)) return post;
  post.nvs_ok = true;

  /* Track what to apply. A small fixed set keeps us from reinit-spamming
   * when one POST changes several coefficients under the same module, or
   * re-applying Quiet Hours once per knob. */
  for (size_t i = 0; i < TUNE_COEFF_COUNT; ++i) {
    const TuneCoeff& c = TUNE_COEFFS[i];
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
    post.changed++;
    switch (c.apply) {
      case TA_REINIT_PRESENCE:  post.reinit_presence  = true; break;
      case TA_REINIT_BREATHING: post.reinit_breathing = true; break;
      case TA_REINIT_ANOMALY:   post.reinit_anomaly   = true; break;
      case TA_QUIET_HOURS:      post.quiet_hours      = true; break;
    }
  }
  prefs.end();

  /* Apply after NVS is closed: each re-run init() and the Quiet Hours
   * apply open their own read-only handles. */
  if (reinit) {
    if (post.reinit_presence)  reinit("core.presence");
    if (post.reinit_breathing) reinit("core.breathing");
    if (post.reinit_anomaly)   reinit("anomaly.baseline");
  }
  if (post.quiet_hours) apply_quiet_hours_from_nvs();
  return post;
}
