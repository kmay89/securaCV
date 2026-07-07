#pragma once
#include <stdint.h>
#include <cstddef>

// Fleet model — the display's single source of truth about the witnesses it
// watches. Pure C++ (no Arduino/JSON dependencies): the MQTT layer parses
// payloads and feeds already-typed values in here; the UI only reads from
// here. Time is injected (millis-domain uint32, wrap-safe signed deltas) so
// the whole model is host-testable.
//
// Design intent (docs/hardware/display_ux_design.md):
//   - Severity is a small ladder aligned with the HA component's taxonomy
//     (const.py: ... warning < error < critical < alert < tamper) collapsed
//     to what a glance can carry.
//   - Alert-fatigue is the enemy: informational events age out on their
//     own; only Alert/Tamper hold attention until acknowledged, and even an
//     ack leaves a residual marker while the underlying condition persists
//     (Nest pattern: quiet it, never silently un-know it).
//   - The trust badge never overclaims: "Verified" strictly means an
//     Ed25519 signature checked out against a TOFU-pinned key on THIS
//     device — same bar as the HA timeline card's strong green tick.

namespace canary::fleet {

// Severity ladder, worst-last. Collapsed from the HA component's taxonomy.
enum class Sev : uint8_t {
  Ok = 0,      // all quiet
  Notice,      // routine witness activity (presence, occupancy)
  Warn,        // needs a look: stale/low battery/restricted-zone/after-hours
  Alert,       // acoustic alarm patterns, chain verify FAILED, witness lost
  Tamper,      // tamper/panic — highest, mirrors const.py's ladder top
};

// Witness-chain trust badge (timeline-card vocabulary; never overclaim).
enum class Badge : uint8_t {
  Unknown = 0, // nothing seen yet
  Unsigned,    // chain head carries no signature fields
  Signed,      // signature present but no pinned key to check against
  Verified,    // Ed25519 verify passed against the TOFU-pinned pubkey
  Failed,      // signature present and did NOT verify — loud red
};

// Liveness, driven by LWT/status plus local staleness deadlines.
enum class Link : uint8_t {
  Unknown = 0,
  Online,
  Stale,       // silent past the stale deadline (amber)
  Lost,        // silent past the lost deadline (red)
  Offline,     // broker said so (retained LWT "offline")
};

struct Witness {
  bool     used = false;
  char     id[48] = {0};
  char     device_type[24] = {0};

  // Rooms & names (trailblazer spec §8; retained securacv/<id>/meta).
  // The glass speaks "Kitchen", never "canary_sense_001", once meta exists.
  char     name[24] = {0};
  char     room[16] = {0};

  // Pubkey fingerprint (16 hex, from the chain payload). Its first 4 hex
  // chars are the 2-byte id Chirp adverts carry — the off-grid correlator.
  char     fp[17] = {0};

  // Wellbeing surface (spec §9; canary-sense wellbeing state topic).
  bool     wb_present = false;    // this witness publishes a breathing lock
  bool     wb_breathing = false;  // lock currently held

  // Liveness
  Link     link = Link::Unknown;
  uint32_t last_seen_ms = 0;      // any topic activity from this device

  // Last event (edge-triggered attention with per-severity decay)
  char     last_event[40] = {0};
  uint32_t last_event_ms = 0;
  bool     has_event = false;
  Sev      event_sev = Sev::Ok;

  // Level-triggered conditions
  bool     tamper = false;
  char     tamper_kind[24] = {0};
  int16_t  battery_pct = -1;      // -1 = unknown/not present
  bool     battery_present = false;

  // Trust surface
  Badge    badge = Badge::Unknown;
  uint32_t chain_length = 0;
  char     fw[16] = {0};
  // Raw retained chain payload, verbatim — the substrate of Proof-on-Glass
  // (trailblazer spec §1): the QR reproduces exactly what the witness
  // published, so any third party can verify it independently.
  char     chain_raw[360] = {0};
};

// One row of the glance event log (newest first via event_at(0)).
struct EventRow {
  char     device[16] = {0};      // truncated id, display-width
  char     name[40] = {0};
  Sev      sev = Sev::Ok;
  uint32_t at_ms = 0;
  bool     signed_flag = false;
};

struct FleetLimits {
  uint32_t stale_after_ms = 180000;
  uint32_t lost_after_ms  = 600000;
  uint32_t ack_hold_ms    = 3600000;
  // Attention decay for edge events that were never acked.
  uint32_t notice_decay_ms = 600000;   // 10 min
  uint32_t warn_decay_ms   = 1800000;  // 30 min
};

// Map an event name from the wire ("presence_detected", "smoke_alarm_t3",
// "silent_panic", ...) onto the display ladder. Substring heuristics on
// purpose: the fleet's variants spell events differently and a display must
// degrade to Notice, not crash, on vocabulary it hasn't met.
Sev classify_event(const char* event_name);

const char* sev_name(Sev s);

template <int MAX_DEVICES, int EVENT_CAP>
class FleetModel {
 public:
  void set_limits(const FleetLimits& l) { limits_ = l; }

  // ── Ingestion (called by the MQTT dispatcher) ────────────────────────

  void on_activity(const char* id, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    // Any traffic proves liveness — including a device coming back from a
    // retained-offline state.
    if (w->link != Link::Online) dirty_ = true;
    w->link = Link::Online;
  }

  void on_availability(const char* id, bool online, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    const Link next = online ? Link::Online : Link::Offline;
    if (w->link != next) dirty_ = true;
    w->link = next;
  }

  void on_status(const char* id, const char* device_type, bool online,
                 int battery_soc, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    copy_str(w->device_type, sizeof(w->device_type), device_type);
    if (battery_soc >= 0) { w->battery_pct = (int16_t)battery_soc; w->battery_present = true; }
    on_availability(id, online, now);
  }

  void on_health(const char* id, int battery, bool battery_present,
                 const char* fw, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    if (battery_present && battery >= 0) {
      w->battery_pct = (int16_t)battery;
      w->battery_present = true;
    }
    if (fw && fw[0]) copy_str(w->fw, sizeof(w->fw), fw);
    dirty_ = true;
  }

  void on_event(const char* id, const char* event_name, bool signed_flag,
                uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    w->link = Link::Online;
    copy_str(w->last_event, sizeof(w->last_event), event_name);
    w->last_event_ms = now;
    w->has_event = true;
    w->event_sev = classify_event(event_name);
    push_event(id, event_name, w->event_sev, signed_flag, now);
    dirty_ = true;
  }

  void on_tamper(const char* id, bool active, const char* kind, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    const bool was = w->tamper;
    if (was != active) dirty_ = true;
    w->tamper = active;
    copy_str(w->tamper_kind, sizeof(w->tamper_kind), kind);
    // Edge-only: the tamper topic is retained, so every broker reconnect
    // replays it — pushing on level would spam the log and cancel acks.
    if (active && !was)
      push_event(id, kind && kind[0] ? kind : "tamper", Sev::Tamper, false, now);
  }

  // Rooms & names (spec §8): retained securacv/<id>/meta. Deliberately
  // does NOT touch last_seen_ms: meta is human-authored (HA, companion
  // app) and retained — renaming a dead witness must not revive it.
  void on_meta(const char* id, const char* name, const char* room,
               uint32_t /*now*/) {
    Witness* w = upsert(id);
    if (!w) return;
    copy_str(w->name, sizeof(w->name), name);
    copy_str(w->room, sizeof(w->room), room);
    dirty_ = true;
  }

  // Wellbeing surface (spec §9): breathing lock riding the state topic.
  void on_wellbeing(const char* id, bool breathing, uint32_t now) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    if (!w->wb_present || w->wb_breathing != breathing) dirty_ = true;
    w->wb_present = true;
    w->wb_breathing = breathing;
  }

  // Chirp ingest (spec §6): an off-grid BLE advert — coarse and UNSIGNED,
  // so it feeds liveness and attention but never trust ("via chirp" names,
  // badge untouched). fp4 = 4 hex chars (the advert's 2-byte fingerprint
  // prefix); an unknown prefix becomes a pseudo witness "SCV-XXXX" so the
  // glass still shows *something* screamed in the dark.
  void on_chirp(const char fp4[5], uint8_t chirp_type, uint32_t now) {
    Witness* w = nullptr;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (slots_[i].used && fp_suffix_match(slots_[i].fp, fp4)) { w = &slots_[i]; break; }
    }
    if (!w) {
      char pseudo[12];
      pseudo[0] = 'S'; pseudo[1] = 'C'; pseudo[2] = 'V'; pseudo[3] = '-';
      for (int i = 0; i < 4; i++) pseudo[4 + i] = fp4[i];
      pseudo[8] = '\0';
      w = upsert(pseudo);
      if (!w) return;
    }
    w->last_seen_ms = now;
    if (w->link != Link::Online) dirty_ = true;
    w->link = Link::Online;

    const char* name = nullptr;
    Sev sev = Sev::Ok;
    switch (chirp_type) {
      case 0x01: name = "alert (chirp)";  sev = Sev::Alert;  break;
      case 0x03: name = "tamper (chirp)"; sev = Sev::Tamper; break;
      default: return;  // heartbeat/witness/boot = liveness only
    }
    // Chirp timestamps are hour-coarse; dedupe locally instead — one
    // attention edge per (witness, type) per minute, so a 2 s broadcast
    // burst can't spam the log or re-cancel acks.
    if (str_eq(w->last_event, name) &&
        (int32_t)(now - w->last_event_ms) < 60000) {
      return;
    }
    copy_str(w->last_event, sizeof(w->last_event), name);
    w->last_event_ms = now;
    w->has_event = true;
    w->event_sev = sev;
    push_event(w->id, name, sev, false, now);
  }

  void on_chain(const char* id, uint32_t length, Badge verdict, uint32_t now,
                const char* raw = nullptr, size_t raw_len = 0,
                const char* fp = nullptr) {
    Witness* w = upsert(id);
    if (!w) return;
    w->last_seen_ms = now;
    if (w->badge != verdict || w->chain_length != length) dirty_ = true;
    const Badge was = w->badge;
    w->chain_length = length;
    w->badge = verdict;
    if (fp && fp[0]) {
      copy_str(w->fp, sizeof(w->fp), fp);
      // An off-grid chirp may have created a pseudo witness ("SCV-XXXX",
      // named by the fp's last-4 suffix like the canary's own BLE name)
      // for this very device before its fingerprint was known. Now that a
      // real identity owns the suffix, retire the ghost — left behind it
      // would go Lost once scanning stops and raise a false alarm.
      size_t n = 0;
      while (fp[n]) n++;
      if (n >= 4) {
        const char* s = fp + n - 4;
        const char pseudo[9] = {'S', 'C', 'V', '-', s[0], s[1], s[2], s[3],
                                '\0'};
        for (int i = 0; i < MAX_DEVICES; i++) {
          Witness& g = slots_[i];
          if (g.used && &g != w && str_eq(g.id, pseudo)) {
            g.used = false;
            dirty_ = true;
          }
        }
      }
    }
    if (raw && raw_len > 0 && raw_len < sizeof(w->chain_raw)) {
      memcpy_str(w->chain_raw, raw, raw_len);
    } else {
      // Missing/oversize payload: drop the cached proof rather than let the
      // proof sheet present a stale chain head as current state — the sheet
      // then honestly says "no signed chain to prove". (review catch)
      w->chain_raw[0] = '\0';
    }
    // Edge-only, same retained-replay reasoning as on_tamper.
    if (verdict == Badge::Failed && was != Badge::Failed) {
      push_event(id, "chain_verify_failed", Sev::Alert, false, now);
    }
  }

  // ── Time ─────────────────────────────────────────────────────────────

  // Staleness deadlines. Call every loop pass; cheap.
  void tick(uint32_t now) {
    for (int i = 0; i < MAX_DEVICES; i++) {
      Witness& w = slots_[i];
      if (!w.used) continue;
      if (w.link == Link::Offline || w.link == Link::Unknown) continue;
      const int32_t silent = (int32_t)(now - w.last_seen_ms);
      Link next = Link::Online;
      if (silent >= (int32_t)limits_.lost_after_ms)       next = Link::Lost;
      else if (silent >= (int32_t)limits_.stale_after_ms) next = Link::Stale;
      if (w.link != next) {
        // Crossing into Lost is a new Alert-grade condition: it must cancel
        // any standing ack so the display re-demands attention (G5/G8).
        if (next == Link::Lost) acked_ = false;
        w.link = next;
        dirty_ = true;
      }
    }
  }

  // ── Acknowledge (long-press) ─────────────────────────────────────────
  // Quiets the current emphasis; conditions stay visible as residual chips
  // until they actually clear. The ack itself expires after ack_hold_ms so
  // a persisting Alert re-demands attention rather than rotting silently.

  void acknowledge(uint32_t now) { ack_ms_ = now; acked_ = true; dirty_ = true; }

  bool ack_active(uint32_t now) const {
    return acked_ && (int32_t)(now - ack_ms_) < (int32_t)limits_.ack_hold_ms;
  }

  // ── Queries (UI) ─────────────────────────────────────────────────────

  int count() const {
    int n = 0;
    for (int i = 0; i < MAX_DEVICES; i++) if (slots_[i].used) n++;
    return n;
  }

  const Witness* at(int idx) const {
    int n = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (!slots_[i].used) continue;
      if (n == idx) return &slots_[i];
      n++;
    }
    return nullptr;
  }

  // Effective severity of one witness right now (link + conditions + the
  // decaying edge event).
  Sev witness_sev(const Witness& w, uint32_t now) const {
    Sev s = Sev::Ok;
    if (w.tamper) s = worst_of(s, Sev::Tamper);
    if (w.badge == Badge::Failed) s = worst_of(s, Sev::Alert);
    if (w.link == Link::Lost) {
      s = worst_of(s, Sev::Alert);
    } else if (w.link == Link::Offline) {
      // A retained LWT is an explicit "died on the wire" — it follows the
      // same amber-then-red ladder as plain silence instead of parking at
      // Warn forever (#843 review catch). last_seen_ms was stamped when
      // the LWT arrived, so the clock starts at the drop.
      const int32_t gone = (int32_t)(now - w.last_seen_ms);
      s = worst_of(s, gone >= (int32_t)limits_.lost_after_ms ? Sev::Alert
                                                             : Sev::Warn);
    } else if (w.link == Link::Stale) {
      s = worst_of(s, Sev::Warn);
    }
    if (w.battery_present && w.battery_pct >= 0) {
      if (w.battery_pct < 10)      s = worst_of(s, Sev::Warn);
      else if (w.battery_pct < 25) s = worst_of(s, Sev::Notice);
    }
    if (w.has_event) {
      const int32_t age = (int32_t)(now - w.last_event_ms);
      Sev es = w.event_sev;
      // Informational attention decays; Alert/Tamper only clear via their
      // level-triggered condition or the fleet-wide ack residual rules.
      if (es == Sev::Notice && age >= (int32_t)limits_.notice_decay_ms) es = Sev::Ok;
      if (es == Sev::Warn   && age >= (int32_t)limits_.warn_decay_ms)   es = Sev::Ok;
      s = worst_of(s, es);
    }
    return s;
  }

  Sev worst(uint32_t now) const {
    Sev s = Sev::Ok;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (!slots_[i].used) continue;
      s = worst_of(s, witness_sev(slots_[i], now));
    }
    return s;
  }

  // True when every tracked witness chain shows the strong tick.
  bool all_verified() const {
    bool any = false;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (!slots_[i].used) continue;
      any = true;
      if (slots_[i].badge != Badge::Verified) return false;
    }
    return any;
  }

  int events_count() const { return ev_count_ < EVENT_CAP ? ev_count_ : EVENT_CAP; }

  // idx 0 = newest.
  const EventRow* event_at(int idx) const {
    if (idx < 0 || idx >= events_count()) return nullptr;
    const int pos = (ev_head_ - 1 - idx + 2 * EVENT_CAP) % EVENT_CAP;
    return &events_[pos];
  }

  // What the glass calls a witness: its meta name once one exists, its id
  // until then (spec §8).
  static const char* display_name(const Witness& w) {
    return w.name[0] ? w.name : w.id;
  }

  // ── Time machine v1 (spec §7): the day's story, in RAM ────────────────
  // 24 hour-of-day buckets binned at push_event time using the wall hour
  // main feeds in (unknown clock = unbinned). The SD-backed verifiable
  // journal remains the bench-phase §7 follow-up; this is the honest
  // subset: "Today: 14 events · worst was warn".

  void set_wall_hour(int hour) {
    if (hour < 0 || hour > 23) { wall_hour_ = -1; return; }
    if (wall_hour_ == -1) {
      // Coming from "clock unknown" (boot, or SNTP lost for an unknowable
      // span): the buckets' ages are unknowable too. Start the day fresh
      // rather than present stale counts as "Past 24h" — no guessed history.
      for (int h = 0; h < 24; h++) {
        hist_count_[h] = 0;
        hist_worst_[h] = (uint8_t)Sev::Ok;
      }
      wall_hour_ = hour;
      return;
    }
    if (wall_hour_ != hour) {
      // Entering a new hour reclaims that bucket (rolling 24 h window).
      hist_count_[hour] = 0;
      hist_worst_[hour] = (uint8_t)Sev::Ok;
      wall_hour_ = hour;
    }
  }

  uint8_t history_count(int hour) const {
    return (hour >= 0 && hour < 24) ? hist_count_[hour] : 0;
  }
  Sev history_worst(int hour) const {
    return (hour >= 0 && hour < 24) ? (Sev)hist_worst_[hour] : Sev::Ok;
  }
  int history_total() const {
    int t = 0;
    for (int h = 0; h < 24; h++) t += hist_count_[h];
    return t;
  }
  Sev history_worst_day() const {
    uint8_t w = (uint8_t)Sev::Ok;
    for (int h = 0; h < 24; h++) if (hist_worst_[h] > w) w = hist_worst_[h];
    return (Sev)w;
  }

  // Render-needed flag; consuming it resets it.
  bool take_dirty() { const bool d = dirty_; dirty_ = false; return d; }
  void mark_dirty() { dirty_ = true; }

 private:
  static Sev worst_of(Sev a, Sev b) { return (uint8_t)a >= (uint8_t)b ? a : b; }

  static void memcpy_str(char* dst, const char* src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = '\0';
  }

  static void copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
  }

  static bool str_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
  }

  // Chirp fingerprint match: the advert carries the LAST 4 hex chars of
  // the witness's 16-hex fingerprint (ble_chirp.h encodes the deviceIdHash
  // *suffix* — the same 4 chars as its "SCV-XXXX" BLE name), so compare
  // suffix-to-suffix. A short/absent fp never matches and never reads out
  // of bounds. (review catch: this originally compared the prefix, which
  // would have filed every known witness's chirps under a pseudo twin.)
  static bool fp_suffix_match(const char* fp, const char* fp4) {
    if (!fp || !fp4) return false;
    size_t n = 0;
    while (fp[n]) n++;
    if (n < 4) return false;
    const char* s = fp + n - 4;
    for (int i = 0; i < 4; i++) {
      if (!fp4[i] || s[i] != fp4[i]) return false;
    }
    return true;
  }

  Witness* upsert(const char* id) {
    if (!id || !id[0]) return nullptr;
    for (int i = 0; i < MAX_DEVICES; i++)
      if (slots_[i].used && str_eq(slots_[i].id, id)) return &slots_[i];
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (slots_[i].used) continue;
      slots_[i] = Witness{};
      slots_[i].used = true;
      copy_str(slots_[i].id, sizeof(slots_[i].id), id);
      dirty_ = true;
      return &slots_[i];
    }
    return nullptr;  // fleet full — over-cap devices are dropped, not rotated
  }

  void push_event(const char* id, const char* name, Sev sev, bool signed_flag,
                  uint32_t now) {
    // Time machine v1: bin into the current wall hour when the clock is known.
    if (wall_hour_ >= 0) {
      if (hist_count_[wall_hour_] < 255) hist_count_[wall_hour_]++;
      if ((uint8_t)sev > hist_worst_[wall_hour_]) hist_worst_[wall_hour_] = (uint8_t)sev;
    }
    EventRow& r = events_[ev_head_];
    // The log speaks the witness's friendly name once meta gave it one.
    const Witness* w = nullptr;
    for (int i = 0; i < MAX_DEVICES; i++)
      if (slots_[i].used && str_eq(slots_[i].id, id)) { w = &slots_[i]; break; }
    copy_str(r.device, sizeof(r.device), w ? display_name(*w) : id);
    copy_str(r.name, sizeof(r.name), name);
    r.sev = sev;
    r.at_ms = now;
    r.signed_flag = signed_flag;
    ev_head_ = (ev_head_ + 1) % EVENT_CAP;
    if (ev_count_ < EVENT_CAP) ev_count_++;
    // A newly ingested Alert/Tamper cancels a standing ack: the ack covered
    // what was known when the user pressed, not whatever comes next. This
    // is what lets a fresh 3 a.m. tamper punch through the night floor even
    // inside the ack-hold window (G5/G8; #843 review catch). Callers only
    // push on edges (see on_tamper/on_chain), so retained replays at broker
    // reconnect cannot cancel acks.
    if ((uint8_t)sev >= (uint8_t)Sev::Alert) acked_ = false;
    dirty_ = true;
  }

  Witness  slots_[MAX_DEVICES] = {};
  EventRow events_[EVENT_CAP] = {};
  int      wall_hour_ = -1;
  uint8_t  hist_count_[24] = {0};
  uint8_t  hist_worst_[24] = {0};
  int      ev_head_ = 0;
  int      ev_count_ = 0;
  bool     dirty_ = true;
  bool     acked_ = false;
  uint32_t ack_ms_ = 0;
  FleetLimits limits_{};
};

}  // namespace canary::fleet
