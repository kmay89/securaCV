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

/* One exception to the no-coupling rule, by declaration only: while the
 * background mount worker (hardware_state.h) is inside SD.begin(), the SD
 * object's card struct is mid-initialization and SD.cardType() can read a
 * garbage non-CARD_NONE value — proceeding to SD.open() would race f_mount
 * on the worker. External-linkage declaration; the definition lives in the
 * sketch TU and resolves at link time. */
bool sd_mount_in_flight();

namespace csi_event_log {

namespace {

constexpr const char* DIR_PATH = "/EVENTS";

/* Scratch path for the atomic-rewrite dance head_truncate_if_oversized
 * uses. Living next to LOG_PATH (same directory) keeps both files on
 * the same FAT cluster chain so SD.rename() stays a single directory-
 * entry update — the only operation FAT guarantees as atomic against
 * power loss. We never read this file from any other context; init()
 * resolves whatever state a crash mid-rewrite leaves behind.
 *
 * Naming: the .tmp suffix is deliberately not part of the
 * dashboard's served filename (which today only ever lists
 * today.ndjson via the GET /api/events/today handler), so a stray
 * temp file is invisible to the dashboard and to MQTT backfill. */
constexpr const char* TMP_PATH = "/EVENTS/today.ndjson.tmp";

/* Forward decl so sd_path_ready can run reconcile_truncate_remnants
 * lazily on the first ready check after a mount transition (PR #400
 * review r3214219273 — init() runs before the user inserts the SD
 * card, so the cleanup needs to run on the actual first-ready edge). */
void reconcile_truncate_remnants();

/* True when an SD card is mounted AND the directory exists. We
 * re-check on every call rather than caching because SD can
 * hot-unplug; the cost is one cardType() lookup + one exists() per
 * event, negligible vs the write. SD.cardType() returns CARD_NONE
 * when nothing is mounted, so it doubles as the readiness check
 * without us needing to peek at hardware_state's globals.
 *
 * The s_reconciled latch is the hot-plug recovery hook: it flips
 * back to false whenever we see CARD_NONE, then on the next ready
 * transition we run reconcile_truncate_remnants once before
 * returning true. So a card inserted long after boot still gets a
 * cleanup pass before any append() can call head_truncate. */
bool sd_path_ready() {
  static bool s_reconciled = false;
  /* A background mount attempt owns the global SD object (hardware_state.h
   * mount worker): the card struct is mid-initialization, so SD.cardType()
   * can read a garbage non-CARD_NONE value and the SD.open below would race
   * f_mount on the worker. Not ready until the attempt concludes. */
  if (sd_mount_in_flight()) {
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    s_reconciled = false;
    return false;
  }
  if (!SD.exists(DIR_PATH)) {
    if (!SD.mkdir(DIR_PATH)) return false;
  }
  if (!s_reconciled) {
    reconcile_truncate_remnants();
    s_reconciled = true;
  }
  return true;
}

/* Drop the oldest ~25% of the file when it crosses MAX_BYTES so the
 * caller's append still succeeds.
 *
 * Streaming copy through a fixed STREAM_BUF_SZ buffer so retention
 * scales with MAX_BYTES rather than free heap (PR #400 review
 * r3214219271). The previous cut allocated the entire survivor span
 * in RAM and capped at 32 KB — which on a 256 KB log meant truncation
 * routinely fell back to "drop the file entirely" and forfeited up
 * to 192 KB of legitimate history. The streaming version reads and
 * writes one buffer at a time, so even a 1 MB log truncates with the
 * same ~1 KB stack-resident buffer.
 *
 * Crash safety: PR #400 made this atomic by writing survivors to a
 * sibling .tmp file, then committing via SD.remove(LOG_PATH) +
 * SD.rename(TMP→LOG). A power cut leaves three possible states, all
 * of which reconcile_truncate_remnants() resolves on next mount:
 *
 *   1. Crash during the .tmp write — LOG_PATH untouched with the
 *      original (still-oversized) contents; TMP_PATH partial. The
 *      reconcile pass deletes TMP_PATH and the next append re-runs
 *      this function from scratch.
 *
 *   2. Crash between SD.remove(LOG_PATH) and SD.rename — LOG_PATH
 *      gone but TMP_PATH holds the complete survivors. Reconcile
 *      promotes TMP_PATH to LOG_PATH so no events are lost.
 *
 *   3. Crash inside SD.rename — FAT directory-entry rename is a
 *      single sector update on Arduino-ESP32's SD library; either
 *      both names point at the cluster chain or only the source does,
 *      so the outcome is observably state #2 or a fully-committed
 *      state. Reconcile handles both.
 *
 * The narrow window where data CAN be lost is now ~one FAT sector
 * write, vs. the prior "duration of the entire rewrite loop" (tens to
 * hundreds of ms with a busy 24KB log). Dwell-time of an in-progress
 * truncate inside the kill-by-watchdog window is negligible. */
constexpr size_t STREAM_BUF_SZ = 1024u;

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

  /* Step 1: stream survivors to TMP_PATH a buffer at a time. The 1 KB
   * buffer lives on the stack so retention never depends on free heap
   * (PR #400 review r3214219271). FILE_WRITE truncates so a leftover
   * .tmp from a previous crash is overwritten. If any write returns
   * short we abort and clean the partial .tmp — the next append
   * retries truncation from scratch, and a reboot triggers reconcile
   * which would also clean a stranded .tmp. */
  File w = SD.open(TMP_PATH, FILE_WRITE);
  if (!w) { f.close(); return false; }

  uint8_t stream_buf[STREAM_BUF_SZ];
  bool stream_ok = true;
  while (f.available()) {
    const int n = f.read(stream_buf, sizeof(stream_buf));
    if (n <= 0) break;  /* EOF or read error; treat as end-of-survivors */
    if (w.write(stream_buf, (size_t)n) != (size_t)n) {
      stream_ok = false;
      break;
    }
  }
  f.close();
  w.close();
  if (!stream_ok) {
    SD.remove(TMP_PATH);
    return false;
  }

  /* Step 2: commit. The remove + rename pair is the irreducible non-
   * atomic window. Arduino-ESP32's SD lib's rename() fails when the
   * destination exists, hence the explicit remove first. If rename
   * fails after the remove succeeded, reconcile_truncate_remnants
   * will see TMP_PATH alone on the next mount and promote it. */
  SD.remove(LOG_PATH);
  if (!SD.rename(TMP_PATH, LOG_PATH)) {
    Serial.println("[EVT-LOG] truncate rename failed — reconcile will recover on next mount");
    return false;
  }
  return true;
}

/* Boot-time reconciliation for the atomic-rewrite states described in
 * head_truncate_if_oversized's comment. Runs once from init(). The
 * three observable shapes after a crash mid-rewrite are:
 *
 *   - Both LOG_PATH and TMP_PATH exist  → rewrite was interrupted
 *     before remove(LOG_PATH); the .tmp is partial / stale, drop it.
 *   - Only TMP_PATH exists              → rewrite committed
 *     remove(LOG_PATH) but crashed before rename; promote TMP_PATH.
 *   - Only LOG_PATH exists (or neither) → no recovery needed.
 *
 * No-op when SD isn't mounted; the next mount triggers init() again
 * via the normal path. */
void reconcile_truncate_remnants() {
  const bool has_log = SD.exists(LOG_PATH);
  const bool has_tmp = SD.exists(TMP_PATH);
  if (has_log && has_tmp) {
    if (SD.remove(TMP_PATH)) {
      Serial.println("[EVT-LOG] cleared stale truncate scratch (.tmp)");
    }
  } else if (!has_log && has_tmp) {
    if (SD.rename(TMP_PATH, LOG_PATH)) {
      Serial.println("[EVT-LOG] recovered events from interrupted truncate");
    } else {
      /* Last-resort cleanup so the .tmp doesn't linger forever and
       * confuse a future reconcile. We forfeit the survivors only
       * when rename fails — extremely rare on a healthy card. */
      SD.remove(TMP_PATH);
      Serial.println("[EVT-LOG] truncate recovery rename failed — .tmp dropped");
    }
  }
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
  if (sd_mount_in_flight()) return true;         /* deferred; not an error */
  if (SD.cardType() == CARD_NONE) return true;   /* deferred; not an error */
  if (!SD.exists(DIR_PATH)) {
    if (!SD.mkdir(DIR_PATH)) {
      Serial.println("[EVT-LOG] mkdir /EVENTS failed");
      return false;
    }
  }
  /* PR #395 truncated the log on cold boot to avoid the
   * event_id-collision bug between previous-boot and current-boot ids.
   * PR #397 fixes the underlying issue by NVS-persisting
   * g_next_event_id (see apply_event_id_floor_from_nvs in
   * csi_integration.cpp), so the log can now survive reboots and
   * cross-reboot MQTT backfill works correctly.
   *
   * Atomic-truncate cleanup (PR #400) lives in sd_path_ready() so
   * it runs on every fresh mount, not just at boot — a card hot-
   * inserted after init still gets a reconcile pass before its
   * first append. */
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
