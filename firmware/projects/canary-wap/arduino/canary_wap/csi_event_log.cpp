/**
 * @file csi_event_log.cpp
 * @brief SD-backed event persistence — see csi_event_log.h for contract.
 *
 * Threading: loop task only (csi_event_log.h). The events egress
 * (csi_event_egress.cpp) is the log's writer and its backfill's reader, and
 * runs in its pump on the loop task; the commit hook only queues rows for
 * it. Dismissal lines and the boot reload run on the loop task too. Only
 * queue_dismissal() is called from another task (the HTTP handler), and it
 * touches nothing but an atomic queue.
 *
 * Failure model: every SD operation is best-effort. The dashboard's
 * Today sheet still reads from the in-memory ring, and the MQTT
 * bridge still publishes rows the card could not keep (the egress holds
 * them in RAM, in order); persistence failures degrade history coverage
 * but don't break functionality. Errors are logged via Serial only when
 * SD is supposed to be available — silent when no card is mounted.
 */

#include "csi_event_log.h"
#include "csi_event_log_line.h"  // the line format, shared with the canary PIO tree

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <esp_task_wdt.h>   // fed while the boot reload reads the tail
#include <atomic>
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

/* Scratch path for the atomic-rewrite dance head_truncate
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

/* True when an SD card is mounted, the directory exists, and the card
 * is not a canary base's (no owner file). We re-check on every call
 * rather than caching because SD can hot-unplug; the cost is one
 * cardType() lookup + two exists() per event, negligible vs the
 * write. SD.cardType() returns CARD_NONE
 * when nothing is mounted, so it doubles as the readiness check
 * without us needing to peek at hardware_state's globals.
 *
 * The s_reconciled latch is the hot-plug recovery hook: it flips
 * back to false whenever we see CARD_NONE (here, or in poll(), which looks
 * at the card every pump pass without coming here), then on the next ready
 * transition we run reconcile_truncate_remnants once before
 * returning true. So a card inserted long after boot, or pulled and
 * reinserted, still gets a cleanup pass before any append can call
 * head_truncate. */
bool s_reconciled = false;
bool s_foreign_said = false;

bool sd_path_ready() {
  /* A background mount attempt owns the global SD object (hardware_state.h
   * mount worker): the card struct is mid-initialization, so SD.cardType()
   * can read a garbage non-CARD_NONE value and the SD.open below would race
   * f_mount on the worker. Not ready until the attempt concludes. */
  if (sd_mount_in_flight()) {
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    s_reconciled = false;
    s_foreign_said = false;
    return false;
  }
  /* A canary base's card: its /EVENTS log is bound to that canary's witness
   * key by the owner file (csi_event_log_line.h). This device writes none,
   * so the log is not its own. Leave it alone: no append, since the canary
   * would sign these rows as its own, and no backfill, since this device
   * would replay the canary's history as its own. Checked before the
   * truncate reconcile, so the canary's scratch file is never touched. */
  if (SD.exists(csi_event_log_line::kOwnerPath)) {
    if (!s_foreign_said) {
      Serial.println("[EVT-LOG] /EVENTS belongs to a canary base (owner file) - event log off for this card");
      s_foreign_said = true;
    }
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

/* The log's size after a truncation (head_truncate sets it; the adapter
 * below keeps it). */
uint32_t s_size = 0;

/* Returns the bytes dropped from the head (0 = nothing dropped). `*broken`
 * = the old log is gone but the rename failed: the survivors sit in the
 * .tmp file, which only the next mount's reconcile may promote, so the log
 * must not be written again until the card is pulled (an append would
 * create a fresh log, and the reconcile would then drop the survivors). */
uint32_t head_truncate(bool* broken) {
  File f = SD.open(LOG_PATH, FILE_READ);
  if (!f) return 0;  /* nothing to truncate */
  const size_t sz = f.size();
  if (sz < MAX_BYTES) { f.close(); return 0; }

  const size_t drop = sz / 4;
  /* Walk to the first line break at or after `drop` so we don't
   * leave a half-line at the new head. */
  if (!f.seek(drop)) { f.close(); return 0; }
  size_t cut = drop;
  while (f.available()) {
    cut++;
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
  if (!w) { f.close(); return 0; }

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
    return 0;
  }

  /* Step 2: commit. The remove + rename pair is the irreducible non-
   * atomic window. Arduino-ESP32's SD lib's rename() fails when the
   * destination exists, hence the explicit remove first. If rename
   * fails after the remove succeeded, reconcile_truncate_remnants
   * will see TMP_PATH alone on the next mount and promote it. */
  SD.remove(LOG_PATH);
  if (!SD.rename(TMP_PATH, LOG_PATH)) {
    Serial.println("[EVT-LOG] truncate rename failed — log closed; reconcile will recover on next mount");
    *broken = true;
    return 0;
  }
  s_size = (uint32_t)(sz - cut);
  return (uint32_t)cut;
}

/* Boot-time reconciliation for the atomic-rewrite states described in
 * head_truncate's comment. Runs on each mount (sd_path_ready). The
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

/* Walk the whole lines of the log's last LOAD_TAIL_BYTES, oldest first,
 * handing each (without its '\n', NUL-terminated) to fn. Starting mid-file,
 * the first line is a fragment and is dropped, unless the window starts
 * exactly on a line boundary (the byte before it is a '\n'), when it is a
 * whole record and is kept; an over-long line is not one of ours and is
 * dropped; a last line without its '\n' (the torn tail of a power cut, or a
 * whole record missing only the newline) is handed over and left to
 * parse(). False when the log could not be opened, seeked or read at the
 * window's edge.
 *
 * Chunked reads instead of one read() per byte, since this runs from setup()
 * over up to LOAD_TAIL_BYTES; the chunk is small because the caller
 * (csi_integration::init, inside start_http_server) is already deep in the
 * loop task's stack.
 *
 * The task watchdog (canary_wap.ino setup: idle tasks of BOTH cores plus the
 * loop task, which this runs on) is kept fed every WDT_FEED_CHUNKS chunks:
 * esp_task_wdt_reset() for the loop task's own subscription, and
 * vTaskDelay(1) so this core's idle task gets to run. yield() would do
 * neither: it only yields to tasks of equal or higher priority, and the idle
 * tasks sit below the loop task. A few ms over a whole 128 KB tail. (Where
 * FEATURE_WATCHDOG leaves the loop task unsubscribed, the reset is a logged
 * no-op, as it is for hardware_state.h's mount wait.) */
typedef void (*line_fn_t)(const char* line, void* user);

constexpr size_t TAIL_CHUNK = 256u;
constexpr unsigned WDT_FEED_CHUNKS = 16u;   /* every 4 KB */

bool for_each_tail_line(line_fn_t fn, void* user) {
  File f = SD.open(LOG_PATH, FILE_READ);
  if (!f) return false;
  const size_t sz = f.size();
  bool skipping = false;
  if (sz > LOAD_TAIL_BYTES) {
    /* Read the byte just before the window: a '\n' there means the window
     * opens on a whole line. Leaves the file positioned at the window. */
    const size_t start = sz - LOAD_TAIL_BYTES;
    if (!f.seek(start - 1)) { f.close(); return false; }
    const int before = f.read();
    if (before < 0) { f.close(); return false; }
    skipping = (before != '\n');
  }
  char line[csi_event_log_line::kLineMax];
  size_t li = 0;
  bool overlong = false;
  uint8_t buf[TAIL_CHUNK];
  unsigned chunks = 0;
  for (;;) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; ++i) {
      const char c = (char)buf[i];
      if (c == '\n') {
        if (!skipping && !overlong && li > 0) {
          line[li] = '\0';
          fn(line, user);
        }
        skipping = false;
        overlong = false;
        li = 0;
      } else if (!skipping && !overlong) {
        if (li < sizeof(line) - 1) {
          line[li++] = c;
        } else {
          overlong = true;   /* no record is this long: not one of ours */
        }
      }
    }
    if (++chunks % WDT_FEED_CHUNKS == 0) {
      esp_task_wdt_reset();   /* the loop task's own TWDT subscription */
      vTaskDelay(1);          /* let this core's idle task run */
    }
  }
  f.close();
  if (!skipping && !overlong && li > 0) {
    line[li] = '\0';
    fn(line, user);
  }
  return true;
}

/* A dismissal on the card. The egress logs every original with
 * "dismissed":0, whatever the ring row says by then (a dismissal can land
 * between the commit and the hook's copy of the row, which the egress
 * appends a loop pass later), so a line with
 * "dismissed":1 is never an original: it is the dismissal of the record with
 * that id, a copy of the ring row written by flush_dismissals() through
 * write_record(). load_into_ring() honors it and the egress's backfill does not
 * replay it. The line format stays csi_event_log_line.h's, shared with the
 * canary PIO tree: the rule is who writes the 1, not a new key. */
bool is_dismissal(const csi_event_record_t* rec) { return rec->values.dismissed != 0; }

/* The ids the tail dismisses. Sized to the ring (CSI_EVENT_RING_CAP, 512
 * rows in csi_event.cpp): more dismissals than that, or no heap for the set,
 * and every restored row is restored dismissed, so a dismissal is never
 * undone by running out of room to remember it. */
constexpr size_t kMaxDismissed = 512;
struct DismissedSet {
  uint32_t* ids;
  size_t    n;
  bool      all;
};

bool dismissed_contains(const DismissedSet* set, uint32_t id) {
  if (set->all) return true;
  for (size_t i = 0; i < set->n; ++i) {
    if (set->ids[i] == id) return true;
  }
  return false;
}

void collect_dismissal(const char* line, void* user) {
  DismissedSet* set = (DismissedSet*)user;
  csi_event_record_t rec;
  if (set->all || !csi_event_log_line::parse(line, &rec) || !is_dismissal(&rec)) return;
  if (dismissed_contains(set, rec.event_id)) return;
  if (set->n == kMaxDismissed) { set->all = true; return; }
  set->ids[set->n++] = rec.event_id;
}

struct RestoreCtx {
  const DismissedSet* dismissed;
  size_t restored;
  size_t refused;
};

void restore_line(const char* line, void* user) {
  RestoreCtx* ctx = (RestoreCtx*)user;
  csi_event_record_t rec;
  if (!csi_event_log_line::parse(line, &rec)) { ctx->refused++; return; }
  if (is_dismissal(&rec)) return;   /* honored through the set, not a row of its own */
  if (dismissed_contains(ctx->dismissed, rec.event_id)) rec.values.dismissed = 1;
  if (csi_event_inject(&rec)) {
    ctx->restored++;
  } else {
    ctx->refused++;
  }
}

/* Dismissals queued by queue_dismissal() on whatever task served the
 * request, and written by flush_dismissals() on the loop task, where every
 * other write to the log happens (the egress's append_line(), the head
 * truncate inside it). A zero slot is free; event id 0 is never an event. */
constexpr size_t kPendingDismissals = 8;
std::atomic<uint32_t> s_pending_dismissals[kPendingDismissals];

/* arm_load(): csi_integration::init has restored the id floor and the
 * privacy ceiling from NVS. Until then load_into_ring() neither reads nor
 * latches. */
std::atomic<bool> s_load_armed{false};
bool s_load_latched = false;

/* ── The egress's card adapter (poll / append_line / read_at) ──────────── */

bool     s_open = false;       /* the log is usable now */
bool     s_reported = false;   /* what the last poll told the egress */
bool     s_evaluated = false;  /* this card was looked at since it went in */
bool     s_needs_seal = false; /* the log ends in a torn line: '\n' first */
uint32_t s_tail_id = 0;

/* No mount attempt in flight and a card in the slot: the cheap check every
 * card call makes (the owner file and /EVENTS are checked once, at open). */
bool card_present() {
  return !sd_mount_in_flight() && SD.cardType() != CARD_NONE;
}

/* The id of the last row in buf[0..n): the last whole, well-formed line
 * that is not a dismissal; 0 when there is none. `head_is_line_start` says
 * whether buf[0] starts a line (the file's start) or may be mid-line. */
uint32_t last_row_in(const char* buf, size_t n, bool head_is_line_start) {
  size_t end = n;
  while (end > 0 && buf[end - 1] != '\n') --end;   /* drop a torn fragment */
  while (end > 0) {
    const size_t nl = end - 1;
    size_t start = nl;
    while (start > 0 && buf[start - 1] != '\n') --start;
    if (start == 0 && !head_is_line_start) break;   /* a partial line */
    const size_t len = nl - start;
    if (len > 0 && len < csi_event_log_line::kLineMax) {
      char line[csi_event_log_line::kLineMax];
      memcpy(line, buf + start, len);
      line[len] = '\0';
      csi_event_record_t rec;
      if (csi_event_log_line::parse(line, &rec) && rec.values.dismissed == 0) {
        return rec.event_id;
      }
    }
    end = start;
  }
  return 0;
}

/* The log's last row, read backwards a window at a time, up to
 * TAIL_SCAN_MAX bytes: dismissal lines (copies of older rows) can follow
 * the last row, and the planner must not take an older id for the card's
 * newest. A window is longer than any line, so a line cut by one window's
 * start is whole in the next. */
constexpr size_t TAIL_SCAN_WINDOW = 768;
static_assert(TAIL_SCAN_WINDOW > csi_event_log_line::kLineMax,
              "a line cut by a window's start must fit the next window whole");

uint32_t last_row_id(File& f, uint32_t size) {
  char buf[TAIL_SCAN_WINDOW];
  uint32_t end = size;
  uint32_t scanned = 0;
  while (end > 0 && scanned < TAIL_SCAN_MAX) {
    const uint32_t start = (end > TAIL_SCAN_WINDOW) ? end - (uint32_t)TAIL_SCAN_WINDOW : 0;
    if (!f.seek(start)) return 0;
    const int got = f.read((uint8_t*)buf, end - start);
    if (got <= 0 || (uint32_t)got != end - start) return 0;
    const uint32_t id = last_row_in(buf, (size_t)got, start == 0);
    if (id != 0 || start == 0) return id;
    /* Next window ends just after this one's first line break: the line
     * this window cut is whole there. */
    const char* nl = static_cast<const char*>(memchr(buf, '\n', (size_t)got));
    if (!nl) return 0;   /* a window with no line break: not our format */
    const uint32_t next_end = start + (uint32_t)(nl - buf) + 1;
    scanned += end - next_end;
    end = next_end;
  }
  return 0;
}

/* A card that went in: its log's size, last row and torn tail. False when
 * the card is not usable for the log (a canary base's, /EVENTS missing and
 * not creatable, or the log will not open). */
bool open_log() {
  s_size = 0;
  s_tail_id = 0;
  s_needs_seal = false;
  if (!sd_path_ready()) return false;      /* owner file, /EVENTS, reconcile */
  if (!SD.exists(LOG_PATH)) return true;   /* created by the first append */
  File f = SD.open(LOG_PATH, FILE_READ);
  if (!f) return false;
  s_size = (uint32_t)f.size();
  if (s_size > 0) {
    if (f.seek(s_size - 1)) {
      const int last = f.read();
      s_needs_seal = (last >= 0 && last != '\n');
    }
    s_tail_id = last_row_id(f, s_size);
  }
  f.close();
  return true;
}

/* One line at the end of the log, while it is open. `may_cut`: a committed
 * row's append may drop the oldest quarter first; a dismissal's may not (the
 * egress's planner learns of a cut only from its own appends), so at the cap
 * a dismissal is refused (flush_dismissals() keeps it queued until a
 * committed row's append has cut the log). */
csi_event_backfill::AppendResult append_bytes(const char* line, size_t len, bool may_cut) {
  csi_event_backfill::AppendResult r = {false, s_size, 0};
  if (!s_open || !card_present() || !line || len == 0) return r;
  if (s_size >= MAX_BYTES) {
    if (!may_cut) return r;
    bool broken = false;
    const uint32_t cut = head_truncate(&broken);
    if (broken) {
      s_open = false;   /* the next poll reports it closed */
      r.size = s_size;
      return r;
    }
    r.cut = cut;
  }
  /* FILE_APPEND on the Arduino-ESP32 SD library opens for write and
   * seeks to the end. SD.h's flush() is implicit on close(); we
   * close after every write so a power cut at most loses the
   * in-flight line and not the file structure. */
  File f = SD.open(LOG_PATH, FILE_APPEND);
  if (!f) {
    r.size = s_size;
    return r;
  }
  if (s_needs_seal) {
    if (f.write((const uint8_t*)"\n", 1) != 1) {
      f.close();
      r.size = s_size;
      return r;
    }
    s_size += 1;
    s_needs_seal = false;
  }
  const size_t wrote = f.write((const uint8_t*)line, len);
  f.close();
  s_size += (uint32_t)wrote;
  r.size = s_size;
  if (wrote != len) {
    if (wrote > 0) s_needs_seal = true;   /* a torn line now ends the log */
    return r;
  }
  r.ok = true;
  return r;
}

/* One dismissal line, as `rec` says, at the end of the log. */
bool write_record(const csi_event_record_t* rec) {
  char line[csi_event_log_line::kLineMax];
  const size_t n = csi_event_log_line::marshal(rec, line, sizeof(line));
  if (n == 0) return false;
  return append_bytes(line, n, /*may_cut=*/false).ok;
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

CardChange poll(uint32_t* size, uint32_t* tail_id) {
  bool opened_now = false;
  if (!card_present()) {
    s_open = false;
    s_evaluated = false;
    /* The card may come back as another card, or as this one after a
     * rewrite whose rename failed (its survivors in the .tmp file): the next
     * open reconciles first. Without this, only sd_path_ready() saw the card
     * leave, and nothing calls it while the card is out. */
    s_reconciled = false;
    s_foreign_said = false;
  } else if (!s_evaluated) {
    /* A card that went in since the last look (or the first look this
     * boot): looked at once while it stays in, so a refused or broken log
     * stays closed until the card is pulled. */
    s_evaluated = true;
    s_open = open_log();
    opened_now = s_open;
  }
  if (opened_now) {
    s_reported = true;
    if (size) *size = s_size;
    if (tail_id) *tail_id = s_tail_id;
    Serial.printf("[EVT-LOG] %s open: %lu bytes, last row %lu\n", LOG_PATH,
                  (unsigned long)s_size, (unsigned long)s_tail_id);
    return CardChange::kOpened;
  }
  if (s_reported && !s_open) {
    s_reported = false;
    return CardChange::kClosed;
  }
  return CardChange::kUnchanged;
}

csi_event_backfill::AppendResult append_line(const char* line, size_t len) {
  return append_bytes(line, len, /*may_cut=*/true);
}

size_t read_at(uint32_t off, char* buf, size_t cap) {
  if (!s_open || !card_present() || !buf || cap == 0) return 0;
  File f = SD.open(LOG_PATH, FILE_READ);
  if (!f) return 0;
  size_t got = 0;
  if (f.seek(off)) {
    const int n = f.read((uint8_t*)buf, cap);
    got = (n > 0) ? (size_t)n : 0;
  }
  f.close();
  return got;
}

void arm_load() { s_load_armed.store(true); }

size_t load_into_ring() {
  if (s_load_latched) return 0;
  /* Not before csi_integration::init has restored the id floor and the
   * ceiling: without the floor inject refuses every row, and latching then
   * would leave the ring empty for the boot. Not latched, so the call after
   * init (or a later card) still loads. */
  if (!s_load_armed.load()) return 0;
  if (!sd_path_ready()) return 0;   /* no card yet (or not ours): try again later */
  s_load_latched = true;            /* one pass per boot, whatever it finds */

  /* A live event already in the ring: csi_event_inject refuses every row
   * from now on (recency order), so reading up to 2 x LOAD_TAIL_BYTES of a
   * late card would restore nothing. The ring holds nothing but live rows
   * before this, the only injector, has run. */
  {
    csi_event_record_t probe;
    if (csi_event_recent(&probe, 1) > 0) {
      Serial.println("[EVT-LOG] a live event has committed this boot - the earlier boots' log is not reloaded");
      return 0;
    }
  }

  /* Two passes over the tail: first the ids it dismisses (a dismissal is
   * written after its record, so one pass would inject the row before
   * reading that it was dismissed), then the records, oldest first. */
  DismissedSet dismissed = {(uint32_t*)malloc(kMaxDismissed * sizeof(uint32_t)), 0, false};
  if (!dismissed.ids) dismissed.all = true;
  if (!for_each_tail_line(collect_dismissal, &dismissed)) {
    free(dismissed.ids);
    return 0;
  }
  RestoreCtx ctx = {&dismissed, 0, 0};
  const bool read_ok = for_each_tail_line(restore_line, &ctx);
  free(dismissed.ids);
  if (!read_ok) return ctx.restored;
  Serial.printf("[EVT-LOG] re-injected %u event(s) from the log tail (the ring keeps its newest; %u line(s) refused)\n",
                (unsigned)ctx.restored, (unsigned)ctx.refused);
  if (dismissed.all) {
    Serial.println("[EVT-LOG] too many dismissals to track (or no heap): every restored event restored dismissed");
  }
  return ctx.restored;
}

bool queue_dismissal(uint32_t event_id) {
  if (event_id == 0) return false;
  for (size_t i = 0; i < kPendingDismissals; ++i) {
    if (s_pending_dismissals[i].load() == event_id) return true;   /* already queued */
  }
  for (size_t i = 0; i < kPendingDismissals; ++i) {
    uint32_t expected = 0;
    if (s_pending_dismissals[i].compare_exchange_strong(expected, event_id)) return true;
  }
  return false;
}

size_t flush_dismissals() {
  /* A dismissal waits in the queue while the log cannot take it: no open
   * log (before the egress's first pump pass, a card being remounted, no
   * card) or a log at MAX_BYTES (a dismissal never cuts the log; the next
   * committed row's append does). Taking it now would lose it for good.
   * Asked before each one: an earlier dismissal in this flush can be the
   * line that takes the log to the cap. */
  size_t written = 0;
  for (size_t i = 0; i < kPendingDismissals; ++i) {
    if (!s_open || !card_present() || s_size >= MAX_BYTES) break;
    const uint32_t id = s_pending_dismissals[i].exchange(0);
    if (id == 0) continue;
    /* The ring row, as the dismissal left it: the same record, in the same
     * line format, that the egress already logged for this id, with
     * "dismissed":1. */
    csi_event_record_t rec;
    if (!csi_event_find(id, &rec) || !rec.values.dismissed) continue;
    if (write_record(&rec)) {
      written++;
    } else {
      Serial.printf("[EVT-LOG] dismissal of event %lu not written (no card, or a failed write)\n",
                    (unsigned long)id);
    }
  }
  return written;
}

#ifdef CSI_TEST_HOST_BUILD
void test_rearm_load() {
  s_load_latched = false;
  s_load_armed.store(false);
  for (size_t i = 0; i < kPendingDismissals; ++i) s_pending_dismissals[i].store(0);
  s_reconciled = false;
  s_foreign_said = false;
  s_open = false;
  s_reported = false;
  s_evaluated = false;
  s_size = 0;
  s_needs_seal = false;
  s_tail_id = 0;
}
#endif

}  /* namespace csi_event_log */
