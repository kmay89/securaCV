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
 *
 * Every key POST /api/settings reads is found by json_object_member()
 * below (sweep F462), the stores' and the handler's own (filter_foreign,
 * the time zone through read_tz_from_settings()), never by a strstr() of
 * the quoted key; test_wap_tune_lab.cpp pins that.
 */

#include "csi_settings_nvs.h"

#include <string.h>

#include <Preferences.h>

#include "core_presence.h"   /* core_presence_baseline_thresholds() (F166) */
#include "csi_event.h"
#include "csi_module.h"
#include "csi_module_settings_nvs.h"
#include "tz_rule.h"         /* json_string_field(), the time zone's value (F462) */

/* ── Reading a POST /api/settings body (sweep F462) ──────────────────── */

namespace {

bool json_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool json_digit(char c) { return c >= '0' && c <= '9'; }
bool json_hex(char c) {
  return json_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* The string that opens at t[*i] == '"': on true *i is one past its closing
 * quote and *escaped says whether it holds an escape. False for one that
 * does not close inside t[0, len), holds a raw control byte, or an escape
 * JSON does not have. */
bool json_skip_string(const char* t, size_t len, size_t* i, bool* escaped) {
  *escaped = false;
  size_t j = *i + 1;
  while (j < len) {
    const unsigned char c = (unsigned char)t[j];
    if (c == '"') {
      *i = j + 1;
      return true;
    }
    if (c < 0x20) return false;
    if (c == '\\') {
      *escaped = true;
      if (++j >= len) return false;
      switch (t[j]) {
        case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
          break;
        case 'u':
          for (int k = 0; k < 4; ++k) {
            if (++j >= len || !json_hex(t[j])) return false;
          }
          break;
        default:
          return false;
      }
    }
    ++j;
  }
  return false;
}

/* The literal or number at t[*i]: on true *i is one past it. */
bool json_skip_scalar(const char* t, size_t len, size_t* i) {
  static const char* const kLiterals[] = {"true", "false", "null"};
  for (const char* lit : kLiterals) {
    const size_t n = strlen(lit);
    if (len - *i >= n && memcmp(t + *i, lit, n) == 0) {
      *i += n;
      return true;
    }
  }
  size_t j = *i;
  if (j < len && t[j] == '-') ++j;
  if (j >= len || !json_digit(t[j])) return false;
  if (t[j] == '0') {
    ++j;
  } else {
    while (j < len && json_digit(t[j])) ++j;
  }
  if (j < len && t[j] == '.') {
    ++j;
    if (j >= len || !json_digit(t[j])) return false;
    while (j < len && json_digit(t[j])) ++j;
  }
  if (j < len && (t[j] == 'e' || t[j] == 'E')) {
    ++j;
    if (j < len && (t[j] == '+' || t[j] == '-')) ++j;
    if (j >= len || !json_digit(t[j])) return false;
    while (j < len && json_digit(t[j])) ++j;
  }
  *i = j;
  return true;
}

/* A value's bare bytes: a string's contents, anything else as it is. False
 * for a string that holds an escape: none of the values read here needs
 * one. */
bool json_member_text(const JsonMember& m, const char** p, size_t* n) {
  if (m.value == nullptr || m.value_len == 0) return false;
  if (m.value[0] == '"') {
    if (m.value_len < 2) return false;
    *p = m.value + 1;
    *n = m.value_len - 2;
    return memchr(*p, '\\', *n) == nullptr;
  }
  *p = m.value;
  *n = m.value_len;
  return true;
}

}  // namespace

bool json_object_member(const char* text, size_t len, const char* key, JsonMember* out) {
  if (text == nullptr || key == nullptr || out == nullptr) return false;
  const size_t key_len = strlen(key);
  enum Expect { kValue, kKeyOrClose, kKey, kColon, kValueOrClose, kAfterValue };
  char open[kJsonMaxDepth];   /* '{' or '[' at each depth */
  size_t depth = 0;
  size_t i = 0;
  while (i < len && json_ws(text[i])) ++i;
  if (i >= len || text[i] != '{') return false;
  open[depth++] = '{';
  ++i;
  Expect expect = kKeyOrClose;
  bool this_member = false;   /* the own member being read has the key */
  size_t key_at = 0;
  size_t value_at = 0;
  size_t found = 0;
  JsonMember hit = {nullptr, nullptr, 0};
  for (;;) {
    while (i < len && json_ws(text[i])) ++i;
    if (i >= len) return false;           /* the text ends inside the object */
    const char c = text[i];
    bool value_done = false;
    if (expect == kKeyOrClose || expect == kKey) {
      if (c == '}' && expect == kKeyOrClose) {
        ++i;
        --depth;
        if (depth == 0) break;
        value_done = true;
      } else if (c == '"') {
        const size_t at = i;
        bool escaped = false;
        if (!json_skip_string(text, len, &i, &escaped)) return false;
        if (depth == 1) {
          if (escaped) return false;      /* an own key with an escape: unreadable */
          this_member = (i - at - 2 == key_len) && memcmp(text + at + 1, key, key_len) == 0;
          key_at = at;
        }
        expect = kColon;
        continue;
      } else {
        return false;
      }
    } else if (expect == kColon) {
      if (c != ':') return false;
      ++i;
      expect = kValue;
      continue;
    } else if (expect == kValue || expect == kValueOrClose) {
      if (c == ']' && expect == kValueOrClose) {
        ++i;
        --depth;
        value_done = true;
      } else {
        if (depth == 1) value_at = i;
        if (c == '"') {
          bool escaped = false;
          if (!json_skip_string(text, len, &i, &escaped)) return false;
          value_done = true;
        } else if (c == '{' || c == '[') {
          if (depth == kJsonMaxDepth) return false;
          open[depth++] = c;
          ++i;
          expect = (c == '{') ? kKeyOrClose : kValueOrClose;
          continue;
        } else {
          if (!json_skip_scalar(text, len, &i)) return false;
          value_done = true;
        }
      }
    } else {   /* kAfterValue */
      if (c == ',') {
        ++i;
        expect = (open[depth - 1] == '{') ? kKey : kValue;
        continue;
      }
      if ((c == '}' && open[depth - 1] == '{') || (c == ']' && open[depth - 1] == '[')) {
        ++i;
        --depth;
        if (depth == 0) break;
        value_done = true;
      } else {
        return false;
      }
    }
    if (value_done) {
      if (depth == 1 && this_member) {
        ++found;
        hit.key = text + key_at;
        hit.value = text + value_at;
        hit.value_len = i - value_at;
        this_member = false;
      }
      expect = kAfterValue;
    }
  }
  /* The object closed: nothing but whitespace may follow it. */
  while (i < len && json_ws(text[i])) ++i;
  if (i != len || found != 1) return false;
  *out = hit;
  return true;
}

bool json_member_bool(const JsonMember& m, bool* out) {
  const char* p = nullptr;
  size_t n = 0;
  if (!json_member_text(m, &p, &n)) return false;
  if (n == 4 && memcmp(p, "true", 4) == 0) {
    *out = true;
    return true;
  }
  if (n == 5 && memcmp(p, "false", 5) == 0) {
    *out = false;
    return true;
  }
  return false;
}

bool json_member_int(const JsonMember& m, int32_t* out) {
  const char* p = nullptr;
  size_t n = 0;
  if (!json_member_text(m, &p, &n)) return false;
  size_t i = 0;
  const bool negative = (n > 0 && p[0] == '-');
  if (negative) i = 1;
  if (i >= n) return false;
  /* Saturated at kJsonIntMax: once another digit would carry the value
   * past it, it is kJsonIntMax, and the digits after are still read, to
   * refuse one that is not a digit. It kept the first nine digits before
   * (1234567890 read as 123456789, a different number; the F462 review). */
  int32_t v = 0;
  for (; i < n; ++i) {
    if (!json_digit(p[i])) return false;
    const int32_t d = (int32_t)(p[i] - '0');
    v = (v > (kJsonIntMax - d) / 10) ? kJsonIntMax : v * 10 + d;
  }
  *out = negative ? -v : v;
  return true;
}

bool json_member_is(const JsonMember& m, const char* word) {
  if (word == nullptr || m.value == nullptr || m.value_len < 2 || m.value[0] != '"') return false;
  const char* p = nullptr;
  size_t n = 0;
  if (!json_member_text(m, &p, &n)) return false;
  return n == strlen(word) && memcmp(p, word, n) == 0;
}

bool read_tz_from_settings(const char* body, char* tz, size_t tz_cap, char* tz_iana,
                           size_t iana_cap, bool* tz_key, bool* iana_key) {
  if (tz_cap > 0) tz[0] = '\0';
  if (iana_cap > 0) tz_iana[0] = '\0';
  const size_t len = strlen(body);
  JsonMember zone = {nullptr, nullptr, 0};
  JsonMember iana = {nullptr, nullptr, 0};
  *tz_key = json_object_member(body, len, "tz", &zone);
  *iana_key = json_object_member(body, len, "tz_iana", &iana);
  /* json_string_field() finds its key with strstr() from where it starts:
   * handed the member's own key, that is the key it reads. */
  if (*tz_key && !tz_rule::json_string_field(zone.key, "\"tz\"", tz, tz_cap)) return false;
  if (*iana_key && !tz_rule::json_string_field(iana.key, "\"tz_iana\"", tz_iana, iana_cap)) return false;
  return true;
}

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

bool store_quiet_hours_from_settings(Preferences& prefs, const char* body) {
  using csi_module_settings_nvs::nvs_key_for;
  /* The body's own "quiet_hours" member, an object, and only that object's
   * own fields: a top-level "enabled" (or any other object's) is not Quiet
   * Hours, and neither is a key spelled inside a value (sweep F462: the
   * object was found by a strstr() of the quoted key and the first '{'
   * after it, the fields by strstr() inside it). */
  JsonMember qh = {nullptr, nullptr, 0};
  if (!json_object_member(body, strlen(body), "quiet_hours", &qh) || qh.value[0] != '{') return false;

  bool stored = false;
  JsonMember m = {nullptr, nullptr, 0};
  bool on = false;
  if (json_object_member(qh.value, qh.value_len, "enabled", &m) && json_member_bool(m, &on)) {
    prefs.putBool(nvs_key_for("core.quiet_hours.enabled"), on);
    stored = true;
  }
  auto put_minute = [&](const char* field, const char* full_key) {
    JsonMember f = {nullptr, nullptr, 0};
    int32_t n = 0;
    if (!json_object_member(qh.value, qh.value_len, field, &f) || !json_member_int(f, &n)) return;
    if (n < 0)    n = 0;
    if (n > 1439) n = 1439;
    prefs.putInt(nvs_key_for(full_key), n);
    stored = true;
  };
  put_minute("start_min", "core.quiet_hours.start_min");
  put_minute("end_min",   "core.quiet_hours.end_min");
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
  /* Each a member of the body's own object (sweep F462): a value spelled
   * like a key, or a key of a nested object, is not one. */
  const size_t len = strlen(body);
  JsonMember m = {nullptr, nullptr, 0};

  /* "pet_mode": true|false */
  bool pet = false;
  if (json_object_member(body, len, "pet_mode", &m) && json_member_bool(m, &pet)) {
    prefs.putBool(nvs_key_for("core.presence.pet_mode"), pet);
    stored = true;
  }

  /* "preset": "sensitive" | "balanced" | "quiet". Stored as int 0/1/2
   * so core_presence.cpp's switch is fast and the NVS row is small. */
  if (json_object_member(body, len, "preset", &m)) {
    int32_t idx = -1;
    if      (json_member_is(m, "sensitive")) idx = 0;
    else if (json_member_is(m, "balanced"))  idx = 1;
    else if (json_member_is(m, "quiet"))     idx = 2;
    if (idx >= 0) {
      prefs.putInt(nvs_key_for("core.presence.preset"), idx);
      stored = true;
    }
  }

  /* "sensitivity": 0..100 (clamped). A value sent as a string
   * ({"sensitivity":"75"}) reads the same as a bare number. */
  int32_t n = 0;
  if (json_object_member(body, len, "sensitivity", &m) && json_member_int(m, &n)) {
    if (n < 0)   n = 0;
    if (n > 100) n = 100;
    prefs.putInt(nvs_key_for("core.presence.sensitivity"), (int32_t)n);
    stored = true;
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
   * existing persisted value (or P0 default) survives. The body's own
   * member only (sweep F462): {"label":"privacy_ceiling","x":"p2"} has
   * none, and stored P2 when the key was found by strstr(). */
  JsonMember m = {nullptr, nullptr, 0};
  if (!json_object_member(body, strlen(body), "privacy_ceiling", &m)) return false;
  int32_t val = -1;
  if      (json_member_is(m, "p0")) val = (int32_t)CSI_PRIVACY_P0;
  else if (json_member_is(m, "p1")) val = (int32_t)CSI_PRIVACY_P1;
  else if (json_member_is(m, "p2")) val = (int32_t)CSI_PRIVACY_P2;
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
