/**
 * @file csi_event_log.cpp
 * @brief SD-backed event persistence — see csi_event_log.h for contract.
 *
 * Threading: append() and iterate_since() can fire from the same main
 * loop / HTTP-handler context that already serializes through the
 * httpd worker pool, so no extra mutex is needed. The MQTT task
 * calls iterate_since on reconnect — that path is also serialized
 * (we drain on the main loop, not in the MQTT event callback) to
 * avoid contending with append().
 *
 * Failure model: every SD operation is best-effort. The dashboard's
 * Today sheet still reads from the in-memory ring, and the MQTT
 * bridge still publishes live events through csi_event_on_committed;
 * persistence failures degrade history coverage but don't break
 * functionality. Errors are logged via Serial only when SD is
 * supposed to be available — silent when no card is mounted.
 */

#include "csi_event_log.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* We deliberately do NOT pull in hardware_state.h: that header carries
 * a non-inline file-scope definition of HardwareState g_hw and is
 * meant to be included in exactly one TU (canary_wap.ino's). Pulling
 * it in here would produce a duplicate-definition link error on the
 * arduino-cli build path (PlatformIO builds a different sketch and
 * wouldn't catch it). Going through Arduino-ESP32's SD object
 * directly via SD.cardType() gives us the same readiness check
 * without the cross-TU coupling. */

namespace csi_event_log {

namespace {

constexpr const char* DIR_PATH = "/EVENTS";

/* True when an SD card is mounted AND the directory exists. We
 * re-check on every call rather than caching because SD can
 * hot-unplug; the cost is one cardType() lookup + one exists() per
 * event, negligible vs the write. SD.cardType() returns CARD_NONE
 * when nothing is mounted, so it doubles as the readiness check
 * without us needing to peek at hardware_state's globals. */
bool sd_path_ready() {
  if (SD.cardType() == CARD_NONE) return false;
  if (!SD.exists(DIR_PATH)) {
    if (!SD.mkdir(DIR_PATH)) return false;
  }
  return true;
}

/* Drop the oldest ~25% of the file when it crosses MAX_BYTES so the
 * caller's append still succeeds. Cheap-but-not-free: reads the whole
 * file, finds a line break past the 25%-mark, rewrites everything
 * after it. Good enough for a 256 KB cap; daily rotation is the next
 * step if this ever shows up in profiles. */
bool head_truncate_if_oversized() {
  File f = SD.open(LOG_PATH, FILE_READ);
  if (!f) return true;  /* nothing to truncate */
  const size_t sz = f.size();
  if (sz < MAX_BYTES) { f.close(); return true; }

  const size_t drop = sz / 4;
  /* Walk to the first line break at or after `drop` so we don't
   * leave a half-line at the new head. */
  if (!f.seek(drop)) { f.close(); return false; }
  while (f.available()) {
    if (f.read() == '\n') break;
  }
  /* Read the survivors into RAM. ESP32-S3 has plenty of heap; cap at
   * MAX_BYTES so a malformed file that's somehow huge can't OOM. */
  const size_t remaining = sz - f.position();
  char* buf = (char*)malloc(remaining + 1);
  if (!buf) { f.close(); return false; }
  const size_t got = f.read((uint8_t*)buf, remaining);
  f.close();
  buf[got] = '\0';

  File w = SD.open(LOG_PATH, FILE_WRITE);  /* FILE_WRITE truncates */
  if (!w) { free(buf); return false; }
  const size_t wrote = w.write((const uint8_t*)buf, got);
  w.close();
  free(buf);
  return wrote == got;
}

/* Marshal one record to the line-delimited JSON shape documented in
 * csi_event_log.h. Returns the byte count (including trailing '\n'),
 * or 0 on overflow. The shape mirrors what csi_mqtt::publish_event
 * produces so a future "publish from log" path is one snprintf. */
size_t marshal_line(const csi_event_record_t* rec, char* out, size_t cap) {
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

/* Mini scanner to pull one int field by name. Returns dflt on miss.
 * Hand-rolled rather than ArduinoJson — same shape as the parser
 * pattern used by csi_mqtt and /api/settings. */
long json_int(const char* line, const char* key, long dflt) {
  /* Build "key" with surrounding quotes so we don't accidentally
   * match a substring of another field name. */
  char needle[32];
  const int kn = snprintf(needle, sizeof(needle), "\"%s\":", key);
  if (kn <= 0 || (size_t)kn >= sizeof(needle)) return dflt;
  const char* k = strstr(line, needle);
  if (!k) return dflt;
  const char* p = k + kn;
  while (*p == ' ' || *p == '\t' || *p == '"') p++;
  char* end = nullptr;
  long v = strtol(p, &end, 10);
  return (end == p) ? dflt : v;
}

void json_str(const char* line, const char* key, char* out, size_t cap) {
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

bool parse_line(const char* line, csi_event_record_t* out) {
  if (!line || !out) return false;
  memset(out, 0, sizeof(*out));
  out->event_id      = (uint32_t)json_int(line, "id",       0);
  if (out->event_id == 0) return false;  /* unparseable / blank line */
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
  out->values.time_bucket         = (uint16_t)json_int(line, "tb",       0);
  out->values.dismissed           = (uint8_t)json_int(line, "dismissed", 0);
  return true;
}

}  /* namespace */

/* ──────────────────────────────────────────────────────────────────────────
 * Public API
 * ────────────────────────────────────────────────────────────────────────── */

bool init() {
  if (SD.cardType() == CARD_NONE) return true;   /* deferred; not an error */
  if (!SD.exists(DIR_PATH)) {
    if (!SD.mkdir(DIR_PATH)) {
      Serial.println("[EVT-LOG] mkdir /EVENTS failed");
      return false;
    }
  }
  return true;
}

bool append(const csi_event_record_t* rec) {
  if (!sd_path_ready()) return false;
  /* Pre-truncate so the next append doesn't blow past MAX_BYTES.
   * Cheap when below cap (single stat). */
  head_truncate_if_oversized();

  char line[512];
  const size_t n = marshal_line(rec, line, sizeof(line));
  if (n == 0) return false;

  /* FILE_APPEND on the Arduino-ESP32 SD library opens for write and
   * seeks to the end. SD.h's flush() is implicit on close(); we
   * close after every write so a power cut at most loses the
   * in-flight line and not the file structure. */
  File f = SD.open(LOG_PATH, FILE_APPEND);
  if (!f) return false;
  const size_t wrote = f.write((const uint8_t*)line, n);
  f.close();
  return wrote == n;
}

/* load_into_ring() deferred — see csi_event_log.h. The csi_event_inject
 * helper it needs in the canonical library would touch firmware/common/csi
 * AND its staged copy in lockstep, which is its own scope. The MQTT
 * backfill path below does NOT depend on it and works as-is. */

size_t iterate_since(uint32_t since_event_id, iterate_cb_t cb, void* user) {
  if (!cb || !sd_path_ready()) return 0;
  File f = SD.open(LOG_PATH, FILE_READ);
  if (!f) return 0;

  size_t emitted = 0;
  char line[512];
  size_t li = 0;
  while (f.available() && emitted < BACKFILL_MAX) {
    const int c = f.read();
    if (c < 0) break;
    if (c == '\n') {
      line[li] = '\0';
      csi_event_record_t rec;
      if (parse_line(line, &rec) && rec.event_id > since_event_id) {
        if (!cb(&rec, user)) { f.close(); return emitted; }
        emitted++;
      }
      li = 0;
    } else if (li < sizeof(line) - 1) {
      line[li++] = (char)c;
    }
  }
  if (li > 0 && emitted < BACKFILL_MAX) {
    line[li] = '\0';
    csi_event_record_t rec;
    if (parse_line(line, &rec) && rec.event_id > since_event_id) {
      if (cb(&rec, user)) emitted++;
    }
  }
  f.close();
  return emitted;
}

}  /* namespace csi_event_log */
