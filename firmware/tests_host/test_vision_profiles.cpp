// Watch profiles (canary-vision detect_profiles.h): pins the per-use-case
// preset table — room presence vs litter box — that the HA "Watch profile"
// select applies to the runtime detection settings. Proves every preset is
// inside the detect_config bounds (a preset the setters would clamp is a
// lie in the UI), the machine keys/labels are unique and well-formed, text
// lookup accepts exactly the key or the label, out-of-range ids degrade to
// the default profile, and the beacon class tokens are real members of the
// fleet beacon's ObjectClass vocabulary (person for room_presence, animal
// for litter_box — never a token the vocabulary doesn't define).
// Pure hosted C++, no Arduino deps.

#include <cassert>
#include <cstdio>
#include <cstring>

#include "canary/config.h"
#include "canary/detect_config.h"
#include "canary/detect_profiles.h"
#include "fleet_beacon.h"

using canary::cfg::WATCH_PROFILES;
using canary::cfg::WATCH_PROFILE_COUNT;
using canary::cfg::watch_profile;
using canary::cfg::watch_profile_from_text;

static bool key_charset_ok(const char* key) {
  for (const char* p = key; *p; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || *p == '_')) return false;
  }
  return key[0] != '\0';
}

int main() {
  // The table exists and profile 0 is the room-presence default whose preset
  // matches the compiled first-boot seeds in config.h — selecting it must be
  // a no-op on a factory-fresh unit.
  assert(WATCH_PROFILE_COUNT >= 2);
  assert(std::strcmp(WATCH_PROFILES[0].key, "room_presence") == 0);
  assert(WATCH_PROFILES[0].target == (uint8_t)PERSON_TARGET);
  assert(WATCH_PROFILES[0].score_min == (uint8_t)SCORE_MIN);
  assert(WATCH_PROFILES[0].lost_timeout_ms == LOST_TIMEOUT_MS);
  assert(WATCH_PROFILES[0].dwell_start_ms == DWELL_START_MS);

  for (uint8_t i = 0; i < WATCH_PROFILE_COUNT; ++i) {
    const auto& p = WATCH_PROFILES[i];

    // Keys are machine identifiers; labels and subjects are human strings.
    assert(key_charset_ok(p.key));
    assert(p.label && p.label[0] != '\0');
    assert(p.subject && p.subject[0] != '\0');

    // Every preset value must survive its setter unclamped — the select's
    // one-step preset may never promise numbers the bounds reject.
    assert(p.score_min >= canary::cfg::DETECT_SCORE_MIN_LO);
    assert(p.score_min <= canary::cfg::DETECT_SCORE_MIN_HI);
    assert(p.lost_timeout_ms >= canary::cfg::DETECT_LOST_MS_LO);
    assert(p.lost_timeout_ms <= canary::cfg::DETECT_LOST_MS_HI);
    assert(p.dwell_start_ms >= canary::cfg::DETECT_DWELL_MS_LO);
    assert(p.dwell_start_ms <= canary::cfg::DETECT_DWELL_MS_HI);

    // Beacon class: a defined ObjectClass token, never NONE, never a value
    // the fleet vocabulary doesn't own (Invariant II).
    assert(p.beacon_class == FLEET_BEACON_DETECT_PERSON ||
           p.beacon_class == FLEET_BEACON_DETECT_VEHICLE ||
           p.beacon_class == FLEET_BEACON_DETECT_ANIMAL ||
           p.beacon_class == FLEET_BEACON_DETECT_PACKAGE);

    // Keys and labels are unique across the table.
    for (uint8_t j = 0; j < i; ++j) {
      assert(std::strcmp(WATCH_PROFILES[j].key, p.key) != 0);
      assert(std::strcmp(WATCH_PROFILES[j].label, p.label) != 0);
    }

    // Both spellings resolve to this profile and to no other.
    assert(watch_profile_from_text(p.key) == (int)i);
    assert(watch_profile_from_text(p.label) == (int)i);
  }

  // The two shipped profiles advertise the class they actually watch for.
  assert(watch_profile((uint8_t)watch_profile_from_text("room_presence"))
             .beacon_class == FLEET_BEACON_DETECT_PERSON);
  assert(watch_profile((uint8_t)watch_profile_from_text("litter_box"))
             .beacon_class == FLEET_BEACON_DETECT_ANIMAL);

  // Unknown text never matches; near-misses (case, whitespace, prefixes) are
  // rejected rather than fuzzy-matched — a mangled payload must not retune.
  assert(watch_profile_from_text(nullptr) == -1);
  assert(watch_profile_from_text("") == -1);
  assert(watch_profile_from_text("Room Presence") == -1);
  assert(watch_profile_from_text("litter") == -1);
  assert(watch_profile_from_text("litter_box ") == -1);

  // Out-of-range ids read as the default profile instead of past the table
  // (the NVS blob may have been written by a newer firmware).
  assert(&watch_profile(WATCH_PROFILE_COUNT) == &WATCH_PROFILES[0]);
  assert(&watch_profile(0xFF) == &WATCH_PROFILES[0]);

  std::puts("PASS test_vision_profiles");
  return 0;
}
