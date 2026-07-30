// canary/tincan/warmer_colder.h — hide and seek, played off the household's
// own Canaries. Pure, host-testable.
//
// Design: docs/design/canary_tincan_kids_watch.md §6.5. A hider stands
// somewhere and records how loudly it hears each household Canary. A seeker
// compares its own reading and gets warmer or colder. Every extra Canary a
// house owns makes the game better, which is the nicest incentive alignment in
// the whole product line.
//
// THE REFUSAL THAT DEFINES THIS FILE: a fingerprint is keyed by household
// Canary `fp4` ids and NOTHING ELSE. The obvious implementation — "every
// access point it can hear" — is the wrong one and is refused structurally
// here, not in review:
//
//   An RSSI vector keyed by BSSIDs, or by stable hashes of BSSIDs, is
//   precisely the input commercial Wi-Fi geolocation databases consume.
//   Emitting one would manufacture reusable location metadata on a device
//   whose headline claim is that it holds none, and would break Invariant III
//   (Metadata Minimization: zone ids are local only, correlation tokens are
//   single-use).
//
// So `Fingerprint` has no field an AP identifier could live in. There is no
// "other beacons" list, no generic id type, no escape hatch. A future
// contributor who wants to add APs has to change the struct, which is exactly
// the moment this comment should stop them.
//
// The second refusal: the output is a SIMILARITY SCORE, never a position.
// There is no trilateration here, no map, no coordinates, and no path
// integration. The moment this produced a coordinate it would be a child
// locator and would inherit that entire category's liability.
//
// The third: a fingerprint is GAME-SCOPED. It lives for one round, inside one
// string, and is discarded when the round ends. Never stored, never reused
// across games, never sent anywhere but the one peer.

#ifndef CANARY_TINCAN_WARMER_COLDER_H
#define CANARY_TINCAN_WARMER_COLDER_H

#include <stddef.h>
#include <stdint.h>

namespace canary {
namespace tincan {

// A household is not a warehouse. Eight Canaries is a generous ceiling and
// keeps a fingerprint inside one small frame.
static constexpr size_t HUNT_MAX_ANCHORS = 8;

// Fewer than this in common and the comparison is noise, not a game.
static constexpr size_t HUNT_MIN_OVERLAP = 2;

// One anchor reading. `fp4` is the household Canary fingerprint the fleet
// already beacons — household-scoped and already coarsened before it ever
// reaches this file.
struct AnchorReading {
  uint16_t fp4 = 0;
  int16_t rssi = 0;  // dBm
};

// A game-scoped snapshot. Note what is absent: no timestamp worth correlating,
// no node id, no AP anything.
struct Fingerprint {
  uint8_t count = 0;
  AnchorReading anchors[HUNT_MAX_ANCHORS];

  void clear() {
    count = 0;
    for (size_t i = 0; i < HUNT_MAX_ANCHORS; i++) anchors[i] = AnchorReading();
  }

  // Add or update an anchor. Returns false when full — dropping the weakest
  // would bias the fingerprint toward wherever the seeker happens to be
  // standing, so we simply stop at the cap.
  bool observe(uint16_t fp4, int16_t rssi) {
    if (fp4 == 0) return false;  // 0 is the "no such node" sentinel
    for (uint8_t i = 0; i < count; i++) {
      if (anchors[i].fp4 == fp4) {
        anchors[i].rssi = rssi;
        return true;
      }
    }
    if (count >= HUNT_MAX_ANCHORS) return false;
    anchors[count].fp4 = fp4;
    anchors[count].rssi = rssi;
    count++;
    return true;
  }

  const AnchorReading* get(uint16_t fp4) const {
    for (uint8_t i = 0; i < count; i++) {
      if (anchors[i].fp4 == fp4) return &anchors[i];
    }
    return nullptr;
  }
};

// How the seeker is doing. Not a distance — a feeling.
enum class HuntHeat : uint8_t {
  Unknown = 0,  // not enough anchors in common to say anything honest
  Cold,
  Cool,
  Warm,
  Hot,
  Found,
};

inline const char* hunt_heat_token(HuntHeat h) {
  switch (h) {
    case HuntHeat::Cold:  return "cold";
    case HuntHeat::Cool:  return "cool";
    case HuntHeat::Warm:  return "warm";
    case HuntHeat::Hot:   return "hot";
    case HuntHeat::Found: return "found";
    case HuntHeat::Unknown:
    default:              return "unknown";
  }
}

// Mean absolute RSSI difference across the anchors both fingerprints share,
// in dB. Returns false when they share too few to be meaningful — reporting
// "unknown" is the honest answer and keeps a kid from chasing noise.
inline bool hunt_distance_db(const Fingerprint& hider, const Fingerprint& seeker,
                             int32_t& out_db, size_t& out_overlap) {
  int32_t sum = 0;
  size_t n = 0;
  for (uint8_t i = 0; i < hider.count; i++) {
    const AnchorReading* s = seeker.get(hider.anchors[i].fp4);
    if (!s) continue;
    int32_t d = (int32_t)hider.anchors[i].rssi - (int32_t)s->rssi;
    if (d < 0) d = -d;
    sum += d;
    n++;
  }
  out_overlap = n;
  if (n < HUNT_MIN_OVERLAP) {
    out_db = 0;
    return false;
  }
  out_db = sum / (int32_t)n;
  return true;
}

// Thresholds, in mean dB of difference. Wide bands on purpose: RSSI on a wrist
// swings ~20 dB just from a turning body, and a heat readout that flickers
// between Warm and Cold as a child rotates is worse than a coarse one that
// holds still.
static constexpr int32_t HUNT_FOUND_DB = 4;
static constexpr int32_t HUNT_HOT_DB = 9;
static constexpr int32_t HUNT_WARM_DB = 16;
static constexpr int32_t HUNT_COOL_DB = 26;

// Hysteresis: once Found, stay Found until clearly wrong. Kids stop moving the
// instant they win, and a readout that immediately un-wins them is cruel.
static constexpr int32_t HUNT_FOUND_RELEASE_DB = 8;

inline HuntHeat hunt_heat(int32_t db, bool was_found) {
  if (was_found && db <= HUNT_FOUND_RELEASE_DB) return HuntHeat::Found;
  if (db <= HUNT_FOUND_DB) return HuntHeat::Found;
  if (db <= HUNT_HOT_DB) return HuntHeat::Hot;
  if (db <= HUNT_WARM_DB) return HuntHeat::Warm;
  if (db <= HUNT_COOL_DB) return HuntHeat::Cool;
  return HuntHeat::Cold;
}

// One round. Holds the hider's snapshot and the current verdict; `end()` is
// what enforces the game-scoped rule.
struct Hunt {
  bool active = false;
  Fingerprint target;
  HuntHeat heat = HuntHeat::Unknown;
  int32_t last_db = 0;
  size_t last_overlap = 0;

  void begin(const Fingerprint& hider_snapshot) {
    active = true;
    target = hider_snapshot;
    heat = HuntHeat::Unknown;
    last_db = 0;
    last_overlap = 0;
  }

  HuntHeat update(const Fingerprint& mine) {
    if (!active) return HuntHeat::Unknown;
    int32_t db = 0;
    size_t overlap = 0;
    if (!hunt_distance_db(target, mine, db, overlap)) {
      last_overlap = overlap;
      heat = HuntHeat::Unknown;
      return heat;
    }
    last_db = db;
    last_overlap = overlap;
    heat = hunt_heat(db, heat == HuntHeat::Found);
    return heat;
  }

  // The round is over. The snapshot is destroyed here, not merely marked
  // inactive — a fingerprint that outlives its game is the reusable
  // correlation token this design refuses to hold.
  void end() {
    active = false;
    target.clear();
    heat = HuntHeat::Unknown;
    last_db = 0;
    last_overlap = 0;
  }
};

// ---------------------------------------------------------------------------
// Wire
//
// 1 byte count, then (fp4 LE, rssi) triples. 3 bytes per anchor, 25 bytes at
// the cap — small enough that a fingerprint is one frame.
// ---------------------------------------------------------------------------

static constexpr size_t HUNT_WIRE_MAX = 1 + HUNT_MAX_ANCHORS * 3;

inline size_t hunt_encode(const Fingerprint& f, uint8_t* out, size_t cap) {
  if (!out) return 0;
  if (f.count > HUNT_MAX_ANCHORS) return 0;
  const size_t need = 1 + (size_t)f.count * 3;
  if (cap < need) return 0;
  out[0] = f.count;
  for (uint8_t i = 0; i < f.count; i++) {
    out[1 + i * 3] = (uint8_t)(f.anchors[i].fp4 & 0xFF);
    out[2 + i * 3] = (uint8_t)((f.anchors[i].fp4 >> 8) & 0xFF);
    // RSSI is negative dBm; store as a signed byte.
    int16_t r = f.anchors[i].rssi;
    if (r < -128) r = -128;
    if (r > 127) r = 127;
    out[3 + i * 3] = (uint8_t)(int8_t)r;
  }
  return need;
}

inline bool hunt_decode(const uint8_t* in, size_t len, Fingerprint& out) {
  if (!in || len < 1) return false;
  const uint8_t n = in[0];
  if (n > HUNT_MAX_ANCHORS) return false;
  if (len != 1 + (size_t)n * 3) return false;
  out.clear();
  for (uint8_t i = 0; i < n; i++) {
    const uint16_t fp4 =
        (uint16_t)((uint16_t)in[1 + i * 3] | ((uint16_t)in[2 + i * 3] << 8));
    const int16_t rssi = (int16_t)(int8_t)in[3 + i * 3];
    if (!out.observe(fp4, rssi)) return false;  // fp4 == 0 or duplicate overflow
  }
  return true;
}

}  // namespace tincan
}  // namespace canary

#endif  // CANARY_TINCAN_WARMER_COLDER_H
