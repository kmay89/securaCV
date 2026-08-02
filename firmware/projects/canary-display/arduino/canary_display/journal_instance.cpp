// Time-machine journal singleton + the fleet event sink that feeds it.
#include "journal_instance.h"

#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE

#include <Arduino.h>
#include <time.h>

#include "fleet_instance.h"
#include "journal_store.h"
#include "sd_archive.h"
#include "log.h"

namespace canary::fleet {

namespace {

EventJournal s_journal;

void copy_field(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  size_t i = 0;
  for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
  dst[i] = '\0';
}

// The fleet's EventSink: turn one pushed event + its owning witness into a
// durable, proof-carrying record and file it (RAM ring + flash). Reads the
// witness's CURRENT chain head — the proof that was live when the event fired.
void on_fleet_event(const char* id, const char* name, Sev sev,
                    bool /*signed_flag*/, uint32_t /*now_ms*/,
                    const Witness* w) {
  JournalRecord r;
  const time_t t = time(nullptr);
  // Same SNTP floor the histogram uses: below it the clock is a guess, so the
  // record is kept (ordering preserved) but stamped 0 = "time unknown".
  // Coarsen to 10-minute buckets before storing (AGENTS.md Invariant III,
  // Metadata Minimization): the durable history keeps the *shape* of the day,
  // never a precise "someone moved at 02:47:13" trail.
  r.epoch = (t >= 1700000000L) ? (uint32_t)(t - (t % 600)) : 0;
  copy_field(r.id, sizeof(r.id), w ? w->id : id);
  copy_field(r.name, sizeof(r.name), w ? w->name : "");
  copy_field(r.ev, sizeof(r.ev), name);
  r.sev = (uint8_t)sev;
  if (w) {
    copy_field(r.fp, sizeof(r.fp), w->fp);
    copy_field(r.chain_raw, sizeof(r.chain_raw), w->chain_raw);
    r.chain_len = w->chain_length;
    r.badge = (uint8_t)w->badge;
  }
  s_journal.append(r);
  journal_store_append(r);
  // Deep tier last: the RAM ring and LittleFS slice are already safe, so a
  // slow or absent card can never delay what the review UI shows.
  sd_archive_append(r);
}

}  // namespace

EventJournal& the_journal() { return s_journal; }

void journal_begin() {
  const bool persisted = journal_store_init();
  if (persisted) journal_store_load();
  // The SD deep archive is append-only breadth, not a load source: reloads
  // come from the bounded LittleFS slice (the ring never holds more anyway).
  const bool archived = sd_archive_init();
  the_fleet().set_event_sink(&on_fleet_event);
  char msg[80];
  snprintf(msg, sizeof(msg), "time machine up (%s%s, %d loaded)",
           persisted ? "persisted" : "RAM-only",
           archived ? " + SD archive" : "", s_journal.count());
  log_line("JRNL", msg);
}

void journal_wipe_all() {
  journal_store_wipe();
  sd_archive_wipe();  // the deep copy forgets too — sovereignty is all-tier
  s_journal.clear();
  the_fleet().mark_dirty();
  log_line("JRNL", "history erased by user");
}

}  // namespace canary::fleet

#else  // FEATURE_TIME_MACHINE off — provide the singleton so links still resolve.

namespace canary::fleet {
EventJournal& the_journal() {
  static EventJournal s_journal;
  return s_journal;
}
void journal_begin() {}
void journal_wipe_all() {}
}  // namespace canary::fleet

#endif  // FEATURE_TIME_MACHINE
