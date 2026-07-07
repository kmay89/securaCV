// LittleFS persistence for the time-machine journal (spec §7).
//
// Compiled whenever FEATURE_TIME_MACHINE is on, so CI exercises the LittleFS +
// JSON paths (no bench-only typos). Whether it actually MOUNTS and writes is
// gated by FEATURE_TIME_MACHINE_PERSIST — off by default, flipped on after a
// flash-write validation at bench, exactly the "engine compiled, inert until
// enabled" pattern the chime engine uses.
#include "canary/fleet/journal_store.h"

#include "canary/config.h"

#if defined(FEATURE_TIME_MACHINE) && FEATURE_TIME_MACHINE

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#include "canary/fleet/journal_instance.h"
#include "canary/log.h"

namespace canary::fleet {

namespace {

constexpr const char* PATH = "/journal.jsonl";
bool s_mounted = false;

// One record <-> one JSONL line. chain_raw is itself JSON, so it is stored as
// a STRING value ("ch") — ArduinoJson escapes the inner quotes on write and
// hands the original bytes back on read, which is exactly why serialization
// lives here (with the JSON library) rather than in the host-testable core.
size_t line_of(const JournalRecord& r, char* out, size_t cap) {
  JsonDocument doc;
  doc["ts"] = r.epoch;
  doc["id"] = r.id;
  doc["nm"] = r.name;
  doc["sv"] = r.sev;
  doc["bd"] = r.badge;
  doc["ev"] = r.ev;
  doc["fp"] = r.fp;
  doc["ln"] = r.chain_len;
  doc["ch"] = r.chain_raw;
  return serializeJson(doc, out, cap);
}

bool record_of(const char* line, JournalRecord& r) {
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return false;
  r = JournalRecord{};
  r.epoch = doc["ts"] | 0UL;
  strlcpy(r.id, doc["id"] | "", sizeof(r.id));
  strlcpy(r.name, doc["nm"] | "", sizeof(r.name));
  r.sev = doc["sv"] | 0;
  r.badge = doc["bd"] | 0;
  strlcpy(r.ev, doc["ev"] | "", sizeof(r.ev));
  strlcpy(r.fp, doc["fp"] | "", sizeof(r.fp));
  r.chain_len = doc["ln"] | 0UL;
  strlcpy(r.chain_raw, doc["ch"] | "", sizeof(r.chain_raw));
  return r.id[0] != '\0';
}

// Rewrite the file from the RAM ring (newest CD_JOURNAL_CAP records), oldest
// first so a reload reconstructs the same order. Bounds flash to the RAM cap —
// the review UI never shows more than the ring anyway. Rare: only when an
// append would push the file past its byte budget.
void rewrite_from_ring() {
  File f = LittleFS.open(PATH, "w");
  if (!f) return;
  const EventJournal& j = the_journal();
  char buf[900];
  for (int i = j.count() - 1; i >= 0; i--) {
    const JournalRecord* r = j.at(i);
    if (!r) continue;
    const size_t n = line_of(*r, buf, sizeof(buf));
    if (n == 0) continue;  // pathological oversize record: skip, never truncate
    f.write(reinterpret_cast<const uint8_t*>(buf), n);
    f.write('\n');
  }
  f.close();
}

}  // namespace

bool journal_store_init() {
#if defined(FEATURE_TIME_MACHINE_PERSIST) && FEATURE_TIME_MACHINE_PERSIST
  // format-on-fail is safe: LittleFS only ever touches its own `spiffs` data
  // partition, never the app/OTA slots.
  if (!LittleFS.begin(/*formatOnFail=*/true)) {
    log_line("JRNL", "LittleFS mount failed - history is RAM-only this boot");
    s_mounted = false;
    return false;
  }
  s_mounted = true;
  return true;
#else
  s_mounted = false;   // compiled but inert until persistence is bench-enabled
  return false;
#endif
}

void journal_store_load() {
  if (!s_mounted) return;
  File f = LittleFS.open(PATH, "r");
  if (!f) return;  // no history yet — fine
  char line[900];
  while (f.available()) {
    const size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    if (n == 0) continue;
    line[n] = '\0';
    JournalRecord r;
    if (record_of(line, r)) the_journal().append(r);  // ring keeps newest CAP
  }
  f.close();
}

void journal_store_append(const JournalRecord& r) {
  if (!s_mounted) return;
  char buf[900];
  const size_t n = line_of(r, buf, sizeof(buf));
  if (n == 0) return;  // record wouldn't serialize (oversize) — skip silently
  File f = LittleFS.open(PATH, "a");
  if (!f) return;
  const size_t projected = f.size() + n + 1;
  f.write(reinterpret_cast<const uint8_t*>(buf), n);
  f.write('\n');
  f.close();
  // Bounded flash: once the log outgrows its budget, compact to the RAM ring.
  if (projected > (size_t)CD_JOURNAL_MAX_BYTES) rewrite_from_ring();
}

void journal_store_wipe() {
  if (!s_mounted) return;
  LittleFS.remove(PATH);
}

uint32_t journal_store_bytes() {
  if (!s_mounted) return 0;
  File f = LittleFS.open(PATH, "r");
  if (!f) return 0;
  const uint32_t sz = (uint32_t)f.size();
  f.close();
  return sz;
}

}  // namespace canary::fleet

#else  // FEATURE_TIME_MACHINE off entirely — no-op stubs so the journal links.

namespace canary::fleet {
bool journal_store_init() { return false; }
void journal_store_load() {}
void journal_store_append(const JournalRecord&) {}
void journal_store_wipe() {}
uint32_t journal_store_bytes() { return 0; }
}  // namespace canary::fleet

#endif  // FEATURE_TIME_MACHINE
