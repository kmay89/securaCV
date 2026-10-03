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
 * written by POST /api/settings (store_quiet_hours_from_settings()) and by
 * the Tuning Lab, both through the key map. Devices hold them under those
 * names, so the names stay (test_wap_tune_lab.cpp). No module reads them:
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

/** Store the "quiet_hours" object of a POST /api/settings body (the
 *  dashboard's Quiet Hours panel): {"enabled": true|false, "start_min": M,
 *  "end_min": M}, each field optional, minutes clamped to 0..1439, a value
 *  sent as a string read as the bare one. Only the object's own fields
 *  count, so a top-level "enabled" (or any other object's) is not read as
 *  Quiet Hours. Each field lands on the row read_quiet_hours() reads, through
 *  the shared key map. `prefs` is open read-write on the "csi" namespace;
 *  `body` is NUL-terminated, and is written to while the object is parsed
 *  and given back unchanged. Returns true when it stored any row (the caller
 *  then re-applies with apply_quiet_hours_from_nvs()). It lived in
 *  csi_integration.cpp's handle_settings_post(), which no host suite
 *  compiles, with the keys spelled by hand; test_wap_tune_lab.cpp runs it. */
bool store_quiet_hours_from_settings(Preferences& prefs, char* body);

/** Push the stored Quiet Hours into the chokepoint
 *  (csi_event_set_quiet_window, a pure state update the HTTP server task
 *  may make; the next emit on the loop task flushes any held summary). A
 *  namespace that will not open changes nothing. Called at boot
 *  (register_v1_modules), after a Quiet Hours change through
 *  POST /api/settings, and after a Tuning Lab or bundle POST stores a
 *  core.quiet_hours.* knob (tune_post(), sweep F128). */
void apply_quiet_hours_from_nvs(void);

/* ── core.presence: the dashboard and the calibration (sweep F151) ────────
 * The dashboard's pet mode, preset and sensitivity (POST /api/settings) and
 * the calibration's three thresholds (POST /api/csi/calibrate/apply) are
 * core.presence's own settings, which its init() reads through the shared
 * key map: core.presence.pet_mode, .preset, .sensitivity,
 * .motion_threshold, .active_threshold and .breathing_threshold (cp.pet_mode,
 * cp.preset, cp.sens, cp.mt, cp.at, cp.bt). The handlers wrote and read them
 * by literal key in csi_integration.cpp, which no host suite compiles, so a
 * key spelled differently there saved a value no module read (a renamed
 * "cp.sens" passed every check). These write and read them by the map;
 * test_wap_tune_lab.cpp runs them against the module's own reads and holds
 * the handlers to them. */

struct PresenceSettings {
  bool    pet_mode;
  int32_t preset;        /* 0 sensitive, 1 balanced, 2 quiet */
  int32_t sensitivity;   /* 0..100 */
};

/** What core.presence's init() reads for each of them when no row stores
 *  it: off, balanced, 50. read_presence_settings()'s defaults. */
PresenceSettings presence_settings_defaults(void);

/** The stored pet mode, preset and sensitivity, each absent row
 *  core.presence's own default (presence_settings_defaults()), as
 *  GET /api/settings reports them. `prefs` is open on the "csi" namespace. */
PresenceSettings read_presence_settings(Preferences& prefs);

/** Store a POST /api/settings body's "pet_mode" (true|false), "preset"
 *  ("sensitive"|"balanced"|"quiet", stored 0 / 1 / 2) and "sensitivity"
 *  (0..100, clamped; a value sent as a string read as the bare one), each
 *  key matched with its quotes. `prefs` is open read-write on the "csi"
 *  namespace. Returns true when it stored any row (the caller re-runs
 *  core.presence's init()). The parse is the handler's, moved unchanged. */
bool store_presence_from_settings(Preferences& prefs, const char* body);

struct PresenceThresholds {
  int32_t motion;
  int32_t active;
  int32_t breathing;
};

/* The thresholds core.presence runs, and where each comes from (sweep
 * F166). Its init() reads each core.presence.*_threshold row with the
 * preset and sensitivity baseline as the read's default
 * (core_presence_baseline_thresholds()), so a stored row wins and a
 * threshold no row stores is that baseline. The calibration's status used
 * to report every absent row as the balanced 35 / 75 / 30 whatever the
 * preset, so on a device that saved "sensitive" or "quiet" (or moved the
 * slider) and stores no threshold its before/after showed thresholds the
 * module did not use. */
struct PresenceThresholdsInUse {
  PresenceThresholds thresholds;
  bool motion_stored;     /* a row the module's getInt reads, so it wins */
  bool active_stored;
  bool breathing_stored;
};

/** What core.presence's init() derives from `prefs` (open on the "csi"
 *  namespace): each threshold a row stores, as stored, and each other one
 *  from the stored preset and sensitivity (read_presence_settings()). A row
 *  counts as stored when getInt reads it, as the module's read does (a row
 *  of another type reads as absent there too). Not the runtime nudge a
 *  dismissal gives the module (on_dismiss(), lost at the next init()). */
PresenceThresholdsInUse read_presence_thresholds_in_use(Preferences& prefs);

/** The same with nothing readable (NVS does not open, as on a first boot
 *  after an erase, sweep F150): the baseline of
 *  presence_settings_defaults(), nothing stored. What init() runs then. */
PresenceThresholdsInUse presence_thresholds_in_use_unread(void);

/** "stored" when every threshold is a stored row (a calibration, the
 *  Tuning Lab), "preset" when none is (the preset and sensitivity
 *  baseline), "mixed" otherwise. */
const char* presence_thresholds_source(const PresenceThresholdsInUse& in_use);

/** Store a calibration's thresholds. `prefs` is open read-write on the "csi"
 *  namespace. True when all three rows were stored. */
bool store_presence_thresholds(Preferences& prefs, const PresenceThresholds& thresholds);

/* ── The privacy ceiling ───────────────────────────────────────────────────
 * core.privacy_ceiling in the shared key map (cp.pc): the persisted
 * P0 / P1 / P2 choice (0 / 1 / 2), which the host reads and no module does.
 * Written and read by the map too (sweep F151), not by "cp.pc" spelled in
 * three places. */

/** The stored ceiling as stored (0 when absent: P0, privacy-first). */
int32_t read_privacy_ceiling(Preferences& prefs);

/** Store a POST /api/settings body's "privacy_ceiling" ("p0"|"p1"|"p2");
 *  anything else stores nothing. True when it stored the row (the caller
 *  then re-applies with apply_privacy_ceiling_from_nvs()). */
bool store_privacy_ceiling_from_settings(Preferences& prefs, const char* body);

/* ── The boot's first read of the namespace (sweep F150) ──────────────────
 * csi_integration::init() restores the event-id floor before anything else
 * touches "csi". On the first boot after an NVS erase nothing has created
 * the namespace yet (the events egress's first delivery-ceiling record,
 * right after, is what does), and a read-only Preferences::begin() of it
 * logged "nvs_open failed: NOT_FOUND" at error level (on a build that keeps
 * Arduino's error log: canary-wap-debug; the release image compiles it
 * out). */

/** Read the persisted event-id floor (`floor_key`) and the events egress's
 *  delivery ceiling (`ceiling_key`), each 0 when absent, through one
 *  read-only handle opened by csi_module_settings_nvs::begin_read_only().
 *  False when NVS was not read: the namespace would not open (a fault, which
 *  Preferences logs) or does not exist (which logs nothing); both are left
 *  0. */
bool read_event_id_floor_rows(const char* floor_key, const char* ceiling_key,
                              uint32_t* floor, uint32_t* ceiling);

/** Push the stored ceiling into the chokepoint (csi_event_set_privacy_ceiling);
 *  a value that is not 1 or 2 is P0, never a more permissive level. Called at
 *  boot (csi_integration::init()) and after POST /api/settings stores it. A
 *  namespace that will not open changes nothing. */
void apply_privacy_ceiling_from_nvs(void);

#endif /* SECURACV_WAP_CSI_SETTINGS_NVS_H */
