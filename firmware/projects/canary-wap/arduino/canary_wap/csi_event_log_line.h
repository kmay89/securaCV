/**
 * @file csi_event_log_line.h
 * @brief The on-card line format of the SD event log — one committed
 *        csi_event per line of /EVENTS/today.ndjson — shared by the
 *        canary-wap sketch (csi_event_log.cpp) and the canary PIO tree
 *        (src/csi_event_log.cpp), so a tool reads either card the same way.
 *
 * Header-only and pure: snprintf into a caller buffer, a hand-rolled field
 * scanner, no allocation, no Arduino, no SD. Staged into the canary-wap
 * sketch by setup.sh with the rest of this directory; check_csi_sync.sh
 * byte-gates the copy. Host-tested by
 * firmware/tests_host/test_csi_event_log_line.cpp (byte-exact goldens).
 *
 * Line format (one record, terminated by '\n'; the canary-wap's since
 * PR #395, moved here unchanged by backlog F37):
 *
 *   {"id":12345,"first":<ms>,"last":<ms>,"cat":"event","priv":"p0",
 *    "module":"core.presence","type":"presence_changed","bundled":1,
 *    "state":"active","conf":"observed","motion":62,"breathing":18,
 *    "bpm":0,"dur":7,"tb":54,"dom":"motion","dismissed":0}
 *
 * `first` / `last` are device-monotonic milliseconds of the boot that wrote
 * the line. Strings are chokepoint-sanitized ASCII (state names, confidence
 * words, module/type ids), so no JSON escaping is needed. Only fields the
 * chokepoint already cleared for export land here; raw feature vectors
 * never do.
 *
 * What parse() refuses: a line that is not exactly one record — one that
 * does not start with `{"id":`, does not end with `}`, or holds a second
 * `{`. A power cut can leave a torn line at the end of the file, and the
 * next append then glues a whole record onto it; the field scanner alone
 * reads that pair as the torn record's id carrying the next record's
 * fields, and a torn line on its own as a record with most fields zero.
 */

#ifndef SECURACV_CSI_EVENT_LOG_LINE_H
#define SECURACV_CSI_EVENT_LOG_LINE_H

#include "csi_event.h"   /* csi_event_record_t, categories, privacy classes */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace csi_event_log_line {

/* Where both trees keep the log (sibling of /WITNESS, /HEALTH, /CHAIN). */
constexpr const char* kDirPath = "/EVENTS";
constexpr const char* kLogPath = "/EVENTS/today.ndjson";
/* The log's owner: one line naming the witness-key fingerprint of the
 * canary base that claimed it. The canary replays its log signed with that
 * key, so it uses only a log that names it. The canary-wap writes no owner
 * file, and leaves alone any card that has one. Without that, its rows
 * would land in a canary's log, and it would replay the canary's history as
 * its own. */
constexpr const char* kOwnerPath = "/EVENTS/owner";

/* One line, '\n' and NUL included, always fits this many bytes: the widest
 * record (every name at CSI_EVENT_NAME_MAX - 1, every number at its type's
 * width) is under 400. Readers size their line buffers with it. */
constexpr size_t kLineMax = 512;

/* Marshal one record into `out`. Returns the byte count including the
 * trailing '\n' (no NUL counted), or 0 on overflow or a null argument. */
inline size_t marshal(const csi_event_record_t* rec, char* out, size_t cap) {
  if (!rec || !out || cap < 32) return 0;
  const char* cat = (rec->category == CSI_CATEGORY_AMBIENT) ? "ambient"
                  : (rec->category == CSI_CATEGORY_ANOMALY) ? "anomaly" : "event";
  const char* priv = (rec->privacy == CSI_PRIVACY_P2) ? "p2"
                   : (rec->privacy == CSI_PRIVACY_P1) ? "p1" : "p0";
  const int n = snprintf(out, cap,
    "{\"id\":%lu,\"first\":%lu,\"last\":%lu,"
     "\"cat\":\"%s\",\"priv\":\"%s\","
     "\"module\":\"%s\",\"type\":\"%s\","
     "\"bundled\":%u,\"state\":\"%s\",\"conf\":\"%s\","
     "\"motion\":%u,\"breathing\":%u,\"bpm\":%u,"
     "\"dur\":%u,\"tb\":%u,\"dom\":\"%s\",\"dismissed\":%u}\n",
    (unsigned long)rec->event_id,
    (unsigned long)rec->first_seen_ms,
    (unsigned long)rec->last_seen_ms,
    cat, priv,
    rec->module_id, rec->type_name,
    (unsigned)rec->bundled_count,
    rec->values.state_name[0]      ? rec->values.state_name      : "",
    rec->values.confidence[0]      ? rec->values.confidence      : "",
    (unsigned)rec->values.motion_score,
    (unsigned)rec->values.breathing_score,
    (unsigned)rec->values.breathing_rate_bpm,
    (unsigned)rec->values.duration_sec,
    (unsigned)rec->values.time_bucket,
    rec->values.dominant_signal[0] ? rec->values.dominant_signal : "",
    (unsigned)rec->values.dismissed);
  if (n <= 0 || (size_t)n >= cap) return 0;
  return (size_t)n;
}

/* Pull one integer field by name; `dflt` on a miss. The needle carries the
 * quotes and the colon, so a key never matches inside another key. */
inline long json_int(const char* line, const char* key, long dflt) {
  char needle[32];
  const int kn = snprintf(needle, sizeof(needle), "\"%s\":", key);
  if (kn <= 0 || (size_t)kn >= sizeof(needle)) return dflt;
  const char* k = strstr(line, needle);
  if (!k) return dflt;
  const char* p = k + kn;
  while (*p == ' ' || *p == '\t' || *p == '"') p++;
  char* end = nullptr;
  const long v = strtol(p, &end, 10);
  return (end == p) ? dflt : v;
}

/* Pull one string field by name into `out` ("" on a miss). */
inline void json_str(const char* line, const char* key, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  char needle[32];
  const int kn = snprintf(needle, sizeof(needle), "\"%s\":\"", key);
  if (kn <= 0 || (size_t)kn >= sizeof(needle)) return;
  const char* k = strstr(line, needle);
  if (!k) return;
  const char* p = k + kn;
  size_t i = 0;
  while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
  out[i] = '\0';
}

/* Exactly one record: starts `{"id":`, ends `}`, no second `{`. */
inline bool well_formed(const char* line) {
  if (!line || strncmp(line, "{\"id\":", 6) != 0) return false;
  const size_t n = strlen(line);
  if (line[n - 1] != '}') return false;
  return strchr(line + 1, '{') == nullptr;
}

/* Parse one line (without its '\n') into `out`. False for a line that is
 * not one well-formed record, or whose id is 0 (no such event). */
inline bool parse(const char* line, csi_event_record_t* out) {
  if (!line || !out) return false;
  memset(out, 0, sizeof(*out));
  if (!well_formed(line)) return false;
  out->event_id      = (uint32_t)json_int(line, "id",       0);
  if (out->event_id == 0) return false;
  out->first_seen_ms = (uint32_t)json_int(line, "first",    0);
  out->last_seen_ms  = (uint32_t)json_int(line, "last",     0);
  out->bundled_count = (uint16_t)json_int(line, "bundled",  1);
  char cat[12]  = {};
  char priv[4]  = {};
  json_str(line, "cat",  cat,  sizeof(cat));
  json_str(line, "priv", priv, sizeof(priv));
  out->category = (strcmp(cat, "ambient") == 0) ? CSI_CATEGORY_AMBIENT
                : (strcmp(cat, "anomaly") == 0) ? CSI_CATEGORY_ANOMALY
                                                : CSI_CATEGORY_EVENT;
  out->privacy = (strcmp(priv, "p2") == 0) ? CSI_PRIVACY_P2
               : (strcmp(priv, "p1") == 0) ? CSI_PRIVACY_P1 : CSI_PRIVACY_P0;
  json_str(line, "module", out->module_id, sizeof(out->module_id));
  json_str(line, "type",   out->type_name, sizeof(out->type_name));
  json_str(line, "state",  out->values.state_name, sizeof(out->values.state_name));
  json_str(line, "conf",   out->values.confidence, sizeof(out->values.confidence));
  json_str(line, "dom",    out->values.dominant_signal, sizeof(out->values.dominant_signal));
  out->values.motion_score        = (uint8_t)json_int(line, "motion",    0);
  out->values.breathing_score     = (uint8_t)json_int(line, "breathing", 0);
  out->values.breathing_rate_bpm  = (uint8_t)json_int(line, "bpm",       0);
  out->values.duration_sec        = (uint16_t)json_int(line, "dur",      0);
  out->values.time_bucket         = (uint8_t)json_int(line, "tb",        0);
  out->values.dismissed           = (uint8_t)json_int(line, "dismissed", 0);
  return true;
}

}  // namespace csi_event_log_line

#endif  // SECURACV_CSI_EVENT_LOG_LINE_H
