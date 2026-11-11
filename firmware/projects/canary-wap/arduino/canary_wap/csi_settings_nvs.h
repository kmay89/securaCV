/**
 * @file csi_settings_nvs.h
 * @brief The canary-wap's CSI settings in NVS: the modules' boot init
 *        (sweep F93), the stored Quiet Hours (sweeps F123, F128), and the
 *        reader POST /api/settings finds its keys with (sweep F462).
 */

#ifndef SECURACV_WAP_CSI_SETTINGS_NVS_H
#define SECURACV_WAP_CSI_SETTINGS_NVS_H

#include <stddef.h>
#include <stdint.h>

class Preferences;

/* ── Reading a POST /api/settings body (sweep F462) ───────────────────────
 * The handler parses its small JSON body by hand (ArduinoJson is kept out
 * of csi_integration.cpp). Each key used to be found with strstr() of the
 * quoted key anywhere in the body, the value read after the next ':', so a
 * value spelled like a key was read as the key:
 * {"label":"privacy_ceiling","x":"p2"} stored the privacy ceiling P2,
 * {"label":"pet_mode","x":true} turned pet mode on, and
 * {"note":"preset","pet_mode":true,"preset":"quiet"} stored no preset. Every
 * key the route reads is found here instead, where a JSON parser finds it.
 * Pure: no NVS, no Arduino; test_wap_tune_lab.cpp runs it. */

/** One member of a JSON object, as json_object_member() found it. */
struct JsonMember {
  const char* key;     /* the key's opening quote */
  const char* value;   /* the value's first byte */
  size_t value_len;    /* the value's bytes: a string with both quotes, an
                          object or array to its closing bracket, a literal
                          or number whole */
};

/** Deepest nesting json_object_member() reads; a deeper body is refused. */
constexpr size_t kJsonMaxDepth = 16;

/** Find the member `key` (spelled bare: "pet_mode") of the one JSON object
 *  text[0, len) holds, whitespace around it and nothing else. Only the
 *  object's own members count: a key spelled inside a string (escaped
 *  quotes and all) or a member of an object or array nested in it is not
 *  one. The whole text is read, never a byte past `len`, and it must be
 *  JSON: strings closed, no raw control byte in one, each escape whole
 *  (\" \\ \/ \b \f \n \r \t \uXXXX), brackets matched at most
 *  kJsonMaxDepth deep, every value a string, an object, an array, true,
 *  false, null or a number, commas and colons where they belong. False,
 *  with *out untouched, when it is not, when no member has the key, and
 *  when two do (parsers disagree on which one wins, so neither does). An
 *  own member's key that holds an escape makes the text unreadable too:
 *  none of the keys read here needs one, and the time zone's reader
 *  (tz_rule::json_string_field()) takes its key as it is spelled. */
bool json_object_member(const char* text, size_t len, const char* key, JsonMember* out);

/** The member's value as a bool: true or false, or the same in quotes (a
 *  value sent as a string reads as the bare one, as it always has). */
bool json_member_bool(const JsonMember& m, bool* out);

/** The member's value as an integer: a number with no fraction or
 *  exponent, or the same digits in quotes, saturated at 999999999 either
 *  side (every reader here clamps far inside that). */
bool json_member_int(const JsonMember& m, int32_t* out);

/** True when the member's value is the string `word`, exactly (a string
 *  that holds an escape is none of the words read here). */
bool json_member_is(const JsonMember& m, const char* word);

/** The household time zone a POST /api/settings body names (sweep F28):
 *  its "tz" (a POSIX rule) and "tz_iana" (a zone name), each a member of
 *  the body's own object, found by json_object_member() and read by
 *  tz_rule::json_string_field() from where that member's key stands, so a
 *  value spelled "tz" is not the key. *tz_key / *iana_key say which of the
 *  two the body has; each buffer holds its string, "" when absent. False
 *  when one is there but its value is not a string json_string_field()
 *  takes (not a string, an escape, longer than its buffer): the handler's
 *  400 "bad time zone", before anything is stored. `body` is
 *  NUL-terminated (http_body::read_all()'s) and read to its terminator. */
bool read_tz_from_settings(const char* body, char* tz, size_t tz_cap, char* tz_iana,
                           size_t iana_cap, bool* tz_key, bool* iana_key);

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
 *  Quiet Hours, and the object is the body's own "quiet_hours" member
 *  (json_object_member(), sweep F462), not a key spelled in a value. Each
 *  field lands on the row read_quiet_hours() reads, through the shared key
 *  map. `prefs` is open read-write on the "csi" namespace; `body` is
 *  NUL-terminated and only read. Returns true when it stored any row (the
 *  caller then re-applies with apply_quiet_hours_from_nvs()). It lived in
 *  csi_integration.cpp's handle_settings_post(), which no host suite
 *  compiles, with the keys spelled by hand; test_wap_tune_lab.cpp runs it. */
bool store_quiet_hours_from_settings(Preferences& prefs, const char* body);

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
 *  a member of the body's own object (json_object_member(), sweep F462).
 *  `prefs` is open read-write on the "csi" namespace; `body` is
 *  NUL-terminated. Returns true when it stored any row (the caller re-runs
 *  core.presence's init()). */
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

/** Store a POST /api/settings body's "privacy_ceiling" ("p0"|"p1"|"p2"), a
 *  member of the body's own object (json_object_member(), sweep F462);
 *  anything else stores nothing. `body` is NUL-terminated. True when it
 *  stored the row (the caller then re-applies with
 *  apply_privacy_ceiling_from_nvs()). */
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
