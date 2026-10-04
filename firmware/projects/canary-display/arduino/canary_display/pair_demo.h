// First Light pair demo — the pure core (no Arduino, no LVGL, no radio).
//
// The boxed-pair experience: a Canary Vision and a Canary Nightlight, powered
// on a desk with nothing else — no WiFi, no hub, no app — and the glass shows
// what the camera's NPU sees the moment it sees it. This header owns every
// decision in that experience that can be tested on a host:
//
//   * which witness the demo follows (adopt-first-heard, then an explicit
//     lock the owner confirms on the button),
//   * what counts as a detection EDGE (alert rising, or the class changing
//     while alert holds — the same edge rule the sender's beacon payload
//     uses, so the two ends of the wire agree on what "now" means), with
//     the followed witness's FIRST status observation as a silent baseline:
//     a camera already holding presence when the card opens must never be
//     counted — or timed — as a fresh trigger (review catch: the refresh
//     that happened to arrive first was producing a fabricated near-zero
//     react number, which is the one dishonesty this surface cannot carry),
//   * the trigger-timing bookkeeping (edge count, when, and the receive
//     timestamp the UI measures its radio-to-glass react time against),
//   * the open gesture (BOOT held 5 s — the classifier's grammar is full,
//     so the demo takes the one slot nothing else claims: keep holding),
//   * and the boxed-pair auto-open rule (unprovisioned glass + a beacon on
//     the router-free band = the demo IS the out-of-box experience).
//
// What this deliberately is NOT: a trust surface. The fleet-link beacon is
// unsigned and promiscuous, and trust on it never rises above presence — the
// demo renders a hint channel and says so on the glass. The verification
// badge machinery is untouched; nothing here can make a witness look
// verified (the demo_mode rule: the ✓ is never faked, even here).
//
// Runtime shell: src/ui/pair_demo_ui.cpp (LVGL surface + NVS persistence).
// Host test: tests_host/test_pair_demo.cpp.

#ifndef CANARY_PAIR_PAIR_DEMO_H
#define CANARY_PAIR_PAIR_DEMO_H

#include <stdint.h>
#include <string.h>

#include "canary/fleet/fleet_model.h"  // BeaconStatus + Via (pure)

namespace canary {
namespace pair {

// A followed witness is stale after three missed 5 s refreshes — the same
// reading the fleet model gives a band (VIA_FRESH_MS), so the demo card and
// the fleet row can never disagree about whether the camera is "there."
constexpr uint32_t PAIR_STALE_MS = canary::fleet::VIA_FRESH_MS;

// BOOT held this long opens (or closes) the demo. Long-press (900 ms,
// acknowledge) has long since fired by then; any release before this keeps
// its meaning, so the half-asleep grammar survives intact.
constexpr uint32_t PAIR_HOLD_OPEN_MS = 5000;

// How long the glass wash rides after an edge — long enough to see, short
// enough that the card (words, always) is what the eye settles on.
constexpr uint32_t PAIR_PULSE_MS = 600;

enum class PairStage : uint8_t {
  Idle,       // demo not open
  Listening,  // open, nobody adopted yet ("power the camera nearby")
  Found,      // a witness is on stage, not yet locked ("tap to keep it")
  Live,       // locked to one witness — the demo card proper
};

class PairDemo {
 public:
  // Open the demo surface. remembered_fp4 is the NVS-persisted lock from a
  // previous session (nullptr or "" = none): with one, the demo re-follows
  // that witness immediately and skips the adopt step.
  void open(uint32_t now, const char* remembered_fp4) {
    active_ = true;
    opened_ms_ = now;
    locked_ = false;
    fp4_[0] = '\0';
    if (remembered_fp4 && remembered_fp4[0]) {
      copy_fp(remembered_fp4);
      locked_ = true;
    }
    heard_ = false;
    baselined_ = false;
    alert_ = false;
    prev_alert_ = false;
    detect_class_ = 0;
    detect_score_ = -1;
    edges_ = 0;
    last_edge_ms_ = 0;
    edge_rx_us_ = 0;
    react_us_ = 0;
    last_seen_ms_ = 0;
  }

  void close() { active_ = false; }
  bool active() const { return active_; }

  PairStage stage() const {
    if (!active_) return PairStage::Idle;
    if (fp4_[0] == '\0') return PairStage::Listening;
    return locked_ ? PairStage::Live : PairStage::Found;
  }

  // Feed one received beacon in (from the ESP-NOW / LAN drains). rx_us is
  // the receiver's own micros() at drain time — the start of the honest
  // radio-to-glass react measurement (it can only over-count, never under).
  // Returns true when this observation is a fresh detection EDGE from the
  // followed witness: the UI's cue to pulse and to start the react clock.
  bool observe(const char* fp4, const canary::fleet::BeaconStatus& s,
               bool have_status, uint32_t now, canary::fleet::Via via,
               uint32_t rx_us) {
    if (!active_ || !fp4 || !fp4[0]) return false;

    if (fp4_[0] == '\0') {
      // Adopt the first witness heard. First wins — a second camera in
      // range stays ignored until the owner forgets this one; predictable
      // beats clever on a demo table.
      copy_fp(fp4);
    } else if (strncmp(fp4, fp4_, sizeof(fp4_)) != 0) {
      return false;  // foreign witness — not the one on stage
    }

    heard_ = true;
    last_seen_ms_ = now;
    via_ = via;

    if (!have_status) return false;  // v1 beacon: freshness only

    if (!baselined_) {
      // The FIRST status observation is a silent baseline, never an edge: a
      // camera already holding presence when the card opens (or when this
      // witness is adopted) reports state the demo did not witness change.
      // Counting it — or worse, timing it — would print a react number for
      // a routine 5 s refresh, and a fabricated timing figure is the one
      // dishonesty this surface cannot carry (review catch).
      baselined_ = true;
      prev_alert_ = s.alert;
      alert_ = s.alert;
      detect_class_ = s.detect_class;
      detect_score_ = s.detect_score;
      return false;
    }

    const bool edge =
        s.alert && (!prev_alert_ || s.detect_class != detect_class_);
    prev_alert_ = s.alert;
    alert_ = s.alert;
    detect_class_ = s.detect_class;
    detect_score_ = s.detect_score;

    if (edge) {
      edges_++;
      last_edge_ms_ = now;
      edge_rx_us_ = rx_us;
      react_us_ = 0;  // pending — the UI reports it once the paint lands
    }
    return edge;
  }

  // The lock ("keep this camera"): only meaningful while a candidate is on
  // stage. Persistence is the caller's job — the core has no NVS.
  bool can_lock() const { return active_ && fp4_[0] != '\0' && !locked_; }
  void lock() {
    if (can_lock()) locked_ = true;
  }
  // Forget the lock (and the candidate): back to open listening. The next
  // witness adopted starts from its own baseline.
  void forget() {
    locked_ = false;
    fp4_[0] = '\0';
    heard_ = false;
    baselined_ = false;
    alert_ = false;
    prev_alert_ = false;
    detect_class_ = 0;
    detect_score_ = -1;
  }
  bool locked() const { return locked_; }
  const char* fp4() const { return fp4_; }

  // Live-card readings.
  bool heard() const { return heard_; }
  bool stale(uint32_t now) const {
    return heard_ && (int32_t)(now - last_seen_ms_) > (int32_t)PAIR_STALE_MS;
  }
  bool alert_now() const { return alert_; }
  uint8_t detect_class() const { return detect_class_; }
  int16_t detect_score() const { return detect_score_; }
  canary::fleet::Via via() const { return via_; }
  uint32_t edges() const { return edges_; }
  uint32_t last_edge_ms() const { return last_edge_ms_; }
  uint32_t edge_rx_us() const { return edge_rx_us_; }
  uint32_t last_seen_ms() const { return last_seen_ms_; }

  // Radio-to-glass react time, measured by the UI (micros at RX drain to
  // micros after the edge's first paint) and parked here for the card. Zero
  // = not yet measured for the latest edge.
  void note_react_us(uint32_t us) { react_us_ = us; }
  uint32_t react_us() const { return react_us_; }

 private:
  void copy_fp(const char* fp4) {
    strncpy(fp4_, fp4, sizeof(fp4_) - 1);
    fp4_[sizeof(fp4_) - 1] = '\0';
  }

  bool active_ = false;
  uint32_t opened_ms_ = 0;
  bool locked_ = false;
  char fp4_[5] = {0};
  bool heard_ = false;
  bool baselined_ = false;
  bool alert_ = false;
  bool prev_alert_ = false;
  uint8_t detect_class_ = 0;
  int16_t detect_score_ = -1;
  canary::fleet::Via via_ = canary::fleet::Via::Mesh;
  uint32_t edges_ = 0;
  uint32_t last_edge_ms_ = 0;
  uint32_t edge_rx_us_ = 0;
  uint32_t react_us_ = 0;
  uint32_t last_seen_ms_ = 0;
};

// The open gesture: BOOT held PAIR_HOLD_OPEN_MS continuously, fired once
// per hold. Runs BESIDE the ButtonClassifier on the same raw level — the
// classifier's grammar (tap/double/hold/triple) is full on the nightlight,
// and "keep holding" is the one slot left. The 900 ms acknowledge having
// already fired en route is accepted: an ack is idempotent and quiet, and
// costing it would mean breaking the half-asleep rule for everyone else.
//
// No debounce stage on purpose: at a five-second human scale a contact
// bounce that reads as a release simply restarts the clock, and the user —
// already told to keep holding — holds a breath longer. Simpler beats a
// stateful debouncer that can disagree with the classifier's.
class HoldGate {
 public:
  // Step with the RAW pressed level. Returns true exactly once per
  // continuous hold, the moment it matures.
  bool step(bool pressed_raw, uint32_t now) {
    if (!pressed_raw) {
      held_ = false;
      fired_ = false;
      return false;
    }
    if (!held_) {
      held_ = true;
      held_since_ = now;
    }
    if (!fired_ &&
        (int32_t)(now - held_since_) >= (int32_t)PAIR_HOLD_OPEN_MS) {
      fired_ = true;
      return true;
    }
    return false;
  }

 private:
  bool held_ = false;
  bool fired_ = false;
  uint32_t held_since_ = 0;
};

// The boxed-pair moment: auto-open only when the glass would otherwise be an
// onboarding screen (never provisioned), the beacon arrived on the
// router-free band (a camera is actually on the desk — a LAN datagram would
// mean a network already exists and the owner has a real setup underway),
// nothing urgent owns the glass, and the session's ONE auto-open hasn't been
// spent. A provisioned nightlight never auto-opens: a working bedside clock
// must not swap its face because a camera powered up somewhere.
//
// `already_spent` is the review catch: the Vision refreshes every 5 s, so
// without it, closing the card ("hold: leave") lasted five seconds and the
// demo kept burying the onboarding wizard. Any open — automatic or manual —
// spends it; after that, reopening is the explicit hold gesture (or a
// reboot), never the radio's idea.
inline bool pair_should_auto_open(bool unprovisioned, canary::fleet::Via via,
                                  bool already_active, bool urgent,
                                  bool already_spent) {
  return unprovisioned && via == canary::fleet::Via::Mesh && !already_active &&
         !urgent && !already_spent;
}

}  // namespace pair
}  // namespace canary

#endif  // CANARY_PAIR_PAIR_DEMO_H
