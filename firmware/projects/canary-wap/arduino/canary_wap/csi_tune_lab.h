/**
 * @file csi_tune_lab.h
 * @brief The Tuning Lab's knobs and its POST (sweeps F123, F128).
 *
 * The Tuning Lab (/tune, privacy class P2) lists every NVS-backed CSI
 * coefficient as a labeled slider with min, max and default, and stores a
 * change through POST /api/tune/coefficients; POST /api/tune/preset (a
 * bundle import) is the same handler. csi_integration.cpp keeps the HTTP
 * handlers; the table and the POST's store-and-apply live here, in a TU
 * tests_host/test_wap_tune_lab.cpp compiles with the staged CSI library.
 *
 * Why a metadata table next to the shared key map
 * (csi_module_settings_nvs.h)? The key map only knows (full_key, nvs_key);
 * it cannot render a slider. This table adds what the UI needs (label,
 * kind, range, default) and what the POST needs (what a stored change
 * applies). Adding a coefficient is a row here and a row in the shared key
 * map (which the canary reads by too).
 */

#ifndef SECURACV_WAP_CSI_TUNE_LAB_H
#define SECURACV_WAP_CSI_TUNE_LAB_H

#include <stddef.h>
#include <stdint.h>

class Preferences;

enum TuneKind { TK_INT, TK_BOOL, TK_MINUTES };

/** What a stored change to a knob applies at once. Every knob applies:
 *  a module knob re-runs that module's init() (reinit_module()), which
 *  re-reads its settings; a Quiet Hours knob re-applies the stored window
 *  to the chokepoint (apply_quiet_hours_from_nvs(), sweep F128). */
enum TuneApply {
  TA_REINIT_PRESENCE,    /* core.presence */
  TA_REINIT_BREATHING,   /* core.breathing */
  TA_REINIT_ANOMALY,     /* anomaly.baseline */
  TA_QUIET_HOURS,        /* the chokepoint's Quiet Hours window */
};

struct TuneCoeff {
  const char* full_key;   /* e.g. "core.presence.preset" */
  const char* group;      /* "core.presence" */
  const char* label;      /* short human label for the slider */
  TuneKind    kind;       /* INT | BOOL | MINUTES (HH:MM render) */
  int32_t     min_v;
  int32_t     max_v;
  int32_t     default_v;  /* what the device runs with the row absent */
  TuneApply   apply;
};

/** Every knob, in the Lab's order. */
extern const TuneCoeff TUNE_COEFFS[];
extern const size_t    TUNE_COEFF_COUNT;

/** The row for `full_key`, or nullptr. */
const TuneCoeff* tune_coeff_for(const char* full_key);

/** `v` clamped into the knob's [min, max]. */
int32_t tune_clamp(const TuneCoeff& c, int32_t v);

/** The stored value of one knob, or its declared default when the row (or
 *  its key-map row) is absent. `prefs` is open on the "csi" namespace. */
int32_t tune_read_value(Preferences& prefs, const TuneCoeff& c);

/** Store one knob, clamped, typed as its kind (putBool / putInt). */
void tune_write_value(Preferences& prefs, const TuneCoeff& c, int32_t v);

/** What one Tuning Lab POST did. */
struct TunePost {
  bool nvs_ok;             /* false: "csi" would not open read-write; nothing stored */
  int  changed;            /* knobs stored */
  bool reinit_presence;    /* applied: core.presence re-ran its init() */
  bool reinit_breathing;   /* applied: core.breathing re-ran its init() */
  bool reinit_anomaly;     /* applied: anomaly.baseline re-ran its init() */
  bool quiet_hours;        /* applied: the stored Quiet Hours re-applied */
};

/**
 * POST /api/tune/coefficients and POST /api/tune/preset: store every
 * "<full_key>": value pair in `body` that names a knob (a number, or
 * true / false / 1 / 0 for a bool; each clamped to the knob's range; any
 * other key ignored), close NVS, then apply what changed: each module
 * touched once through `reinit` (csi_integration.cpp's reinit_module()), and
 * the stored Quiet Hours once through apply_quiet_hours_from_nvs() when a
 * core.quiet_hours.* knob was stored (sweep F128). Nothing is signed or
 * checked beyond that: a bundle is the flat object GET /api/tune/preset
 * streams. HTTP server task, as /api/settings applies its own changes.
 */
TunePost tune_post(const char* body, void (*reinit)(const char* module_id));

#endif /* SECURACV_WAP_CSI_TUNE_LAB_H */
