/**
 * @file csi_settings_nvs.h
 * @brief The canary-wap's CSI settings in NVS: the modules' boot init
 *        (sweep F93) and the stored Quiet Hours (sweeps F123, F128).
 */

#ifndef SECURACV_WAP_CSI_SETTINGS_NVS_H
#define SECURACV_WAP_CSI_SETTINGS_NVS_H

#include <stddef.h>
#include <stdint.h>

class Preferences;

/**
 * Run every registered module's init() once (csi_module_init_all), reading
 * its stored settings through one read-only NVS handle for the whole boot
 * (csi_module_settings_nvs.h's rule, the canary's too). Returns how many
 * init() calls it made. csi_integration::init() calls it once, right after
 * register_v1_modules() and before the HAL installs the features callback;
 * check_wap_event_egress.py's rule 3 holds that. Loop task, at boot.
 */
size_t csi_settings_nvs_init_modules(void);

/* ── Quiet Hours ─────────────────────────────────────────────────────────
 * Three rows in the "csi" namespace, under the shared key map's keys for
 * core.quiet_hours.enabled / start_min / end_min (qh.en, qh.start, qh.end),
 * written by POST /api/settings and by the Tuning Lab. No module reads them:
 * the chokepoint holds them (csi_event_set_quiet_window).
 *
 * The one default a device that never stored them runs: off, 23:00 to
 * 07:00 (minutes of day). GET /api/settings, the Tuning Lab's declared
 * defaults (and so its reset buttons and the bundle it exports), and the
 * chokepoint at boot all read these constants (sweep F123), and the
 * dashboard's own first values match them (test_wap_tune_lab.cpp). */
constexpr bool    kQuietHoursDefaultEnabled  = false;
constexpr int32_t kQuietHoursDefaultStartMin = 23 * 60;
constexpr int32_t kQuietHoursDefaultEndMin   = 7 * 60;

struct QuietHours {
  bool    enabled;
  int32_t start_min;   /* minutes of day, as stored */
  int32_t end_min;
};

/** The stored Quiet Hours, each absent row its default above. `prefs` is
 *  open on the "csi" namespace (read-only is enough). */
QuietHours read_quiet_hours(Preferences& prefs);

/** Push the stored Quiet Hours into the chokepoint
 *  (csi_event_set_quiet_window, a pure state update the HTTP server task
 *  may make; the next emit on the loop task flushes any held summary). A
 *  namespace that will not open changes nothing. Called at boot
 *  (register_v1_modules), after a Quiet Hours change through
 *  POST /api/settings, and after a Tuning Lab or bundle POST stores a
 *  core.quiet_hours.* knob (tune_post(), sweep F128). */
void apply_quiet_hours_from_nvs(void);

#endif /* SECURACV_WAP_CSI_SETTINGS_NVS_H */
