// Time-machine journal — the durable, proof-carrying history (spec §7).
//
// Wave 2 gave the fleet an in-RAM 24h *histogram* (counts per hour). This is
// the next layer: an epoch-stamped, proof-carrying *record* of each event, so
// the home's story survives longer than the glance log and — the headline —
// stays VERIFIABLE. Each record keeps the verbatim signed chain head that was
// current when the event fired, so the Proof-on-Glass QR works on a week-old
// event exactly as it does on a live one. History never becomes hearsay.
//
// This header is deliberately dependency-free and host-testable, exactly like
// fleet_model.h: it holds the ring + the day-narrative math. The firmware
// injects the wall clock (epoch) and the persistence backend; the template
// itself never touches Arduino, time, or storage. Serialization lives in the
// (firmware-only) journal_store so the JSON-escaping of chain_raw has a single
// owner that can lean on ArduinoJson.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace canary::fleet {

// One durable event. Unlike the live EventRow (millis-domain, name truncated
// to display width), this carries wall time and enough to re-prove:
//   - id      keys the TOFU-pinned pubkey in the trust store
//   - chain_raw is the exact signed payload the witness published
// Given (id, pinned_pubkey, chain_raw) the display rebuilds the identical QR
// body it would show live — no cloud, no account, years later.
struct JournalRecord {
  uint32_t epoch = 0;            // wall-clock seconds; 0 = logged before SNTP
  char     id[48] = {0};         // full witness id (pubkey-pin key)
  char     name[24] = {0};       // friendly name at capture ("Kitchen") or ""
  uint8_t  sev = 0;              // Sev at capture (Ok..Tamper)
  uint8_t  badge = 0;            // Badge/verdict at capture (Unknown..Failed)
  char     ev[40] = {0};         // event name ("smoke_alarm_t3", "tamper (chirp)")
  char     fp[17] = {0};         // 16-hex fingerprint (chirp correlator)
  uint32_t chain_len = 0;        // chain length at capture
  char     chain_raw[360] = {0}; // verbatim signed chain payload -> re-provable
};

// Fixed-capacity, newest-first ring. Host-testable: no time, no I/O.
template <int CAP>
class Journal {
 public:
  void clear() { head_ = 0; count_ = 0; }
  int  count() const { return count_ < CAP ? count_ : CAP; }
  int  capacity() const { return CAP; }

  void append(const JournalRecord& r) {
    recs_[head_] = r;
    head_ = (head_ + 1) % CAP;
    if (count_ < CAP) count_++;
  }

  // idx 0 = newest, count()-1 = oldest retained.
  const JournalRecord* at(int idx) const {
    if (idx < 0 || idx >= count()) return nullptr;
    const int pos = (head_ - 1 - idx + 2 * CAP) % CAP;
    return &recs_[pos];
  }

  // ── Day-narrative math (the honest summary the glass speaks) ──────────
  // Only records with a known clock (epoch != 0) inside the window count —
  // an event logged before SNTP has no place on a wall-clock timeline, the
  // same "no guessed history" rule the histogram enforces.

  int count_since(uint32_t now_epoch, uint32_t window_s) const {
    int n = 0;
    const uint32_t floor = window_floor(now_epoch, window_s);
    for (int i = 0; i < count(); i++) {
      const JournalRecord* r = at(i);
      if (r->epoch != 0 && r->epoch >= floor && r->epoch <= now_epoch) n++;
    }
    return n;
  }

  uint8_t worst_since(uint32_t now_epoch, uint32_t window_s) const {
    uint8_t w = 0;
    const uint32_t floor = window_floor(now_epoch, window_s);
    for (int i = 0; i < count(); i++) {
      const JournalRecord* r = at(i);
      if (r->epoch != 0 && r->epoch >= floor && r->epoch <= now_epoch &&
          r->sev > w)
        w = r->sev;
    }
    return w;
  }

  // Oldest retained record with a known clock (0 if none) — for a
  // "history since <date>" affordance and the retention story.
  uint32_t oldest_known_epoch() const {
    for (int i = count() - 1; i >= 0; i--) {
      const JournalRecord* r = at(i);
      if (r->epoch != 0) return r->epoch;
    }
    return 0;
  }

 private:
  // Saturating subtract: never wrap below zero into a huge unsigned floor.
  static uint32_t window_floor(uint32_t now_epoch, uint32_t window_s) {
    return (now_epoch > window_s) ? (now_epoch - window_s) : 0;
  }

  JournalRecord recs_[CAP];
  int head_ = 0;
  int count_ = 0;
};

}  // namespace canary::fleet
