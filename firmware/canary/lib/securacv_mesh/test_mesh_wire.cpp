/**
 * @file test_mesh_wire.cpp
 * @brief Host test for the Opera mesh wire registry (mesh_wire.h, spec
 *        §4.5 — v0.4, awaiting crypto review, not bench-verified). Shared
 *        by both mesh trees: canary-wap compiles a byte-identical staged
 *        copy (its Makefile's test_mesh_wire_staged).
 *
 * Pins the one assignment both trees now compile against:
 *   1. Every value, by number — the spec §4.5 table, so a renumber in the
 *      header without the spec (or the other way round) fails here.
 *   2. The first byte is unambiguous: no pairing type equals a version
 *      byte either tree has used (0, 1), the pairing and opera blocks are
 *      disjoint, and no registry value is a Chirp (0xC4) or Beacon (0xB1)
 *      magic — the two other protocols on a canary-wap's radio.
 *   3. The classifiers agree with the blocks, and the values the trees
 *      used to disagree on (TAMPER_ALERT 18 vs 4, CHANNEL_LOCK 23 vs 20,
 *      HUB_ELECTION 24 vs 21, LEAVE_OPERA 25 vs 13) are the PIO ones, with
 *      canary-wap's old values now either reserved or something else.
 *
 * Build:
 *   g++ -std=c++17 firmware/canary/lib/securacv_mesh/test_mesh_wire.cpp \
 *       -I firmware/canary/lib/securacv_mesh/src -o /tmp/t && /tmp/t
 */

/* canary_config.h #defines PROTOCOL_VERSION as a string. The registry must
 * compile in a translation unit that already has it (PlatformIO canary CI
 * failed on exactly this once), so define it first. The envelope must too:
 * its version constant was mesh_envelope::PROTOCOL_VERSION until v0.4's
 * follow-up, which no TU holding the macro could include (the macro turned
 * the declaration into `constexpr uint8_t "pwk:v0.3.0" = ...`); it is
 * OPERA_VERSION now, the registry's name. The canary-wap Makefile builds
 * this file against the STAGED registry, where there is no envelope, and
 * passes MESH_WIRE_STAGED_ONLY; the PIO build never does, so the guard
 * holds there. */
#define PROTOCOL_VERSION "pwk:v0.3.0"
#include "mesh_wire.h"
#ifndef MESH_WIRE_STAGED_ONLY
#include "mesh_envelope.h"
#endif

#include <cassert>
#include <cstdio>

namespace {

void test_values_pinned() {
  using namespace mesh_wire;
  assert(OPERA_VERSION == 1);

  assert(PAIR_DISCOVER == 8  && PAIR_FIRST == 8);
  assert(PAIR_OFFER    == 9);
  assert(PAIR_ACCEPT   == 10);
  assert(PAIR_CONFIRM  == 11);
  assert(PAIR_COMPLETE == 12 && PAIR_LAST == 12);

  assert(HEARTBEAT        == 16 && OPERA_FIRST == 16);
  assert(CSI_FEATURES     == 17);
  assert(TAMPER_ALERT     == 18);
  assert(POWER_ALERT      == 19);
  assert(OFFLINE_IMMINENT == 20);
  assert(WITNESS_RECORD   == 21);
  assert(BEACON_EVENT     == 22);
  assert(CHANNEL_LOCK     == 23);
  assert(HUB_ELECTION     == 24);
  assert(LEAVE_OPERA      == 25);
  assert(REKEY_OFFER      == 26);
  assert(REKEY_ACCEPT     == 27);
  assert(REKEY_SECRET     == 28);
  assert(REKEY_ACK        == 29);

  assert(AUTH_CHALLENGE   == 30);
  assert(AUTH_RESPONSE    == 31);
  assert(AUTH_COMPLETE    == 32);
  assert(PEER_LIST        == 33);
  assert(ENCRYPTED        == 34);
  assert(OPERA_REKEY      == 35);
  assert(OPERA_REKEY_ACK  == 36);
  assert(ASSIGNED_LAST    == 36);

  assert(CHIRP_MAGIC == 0xC4 && BEACON_MAGIC == 0xB1);
  std::printf("PASS test_values_pinned\n");
}

#ifndef MESH_WIRE_STAGED_ONLY
/* The envelope's version byte is the registry's, under its name — and this
 * TU carries canary_config.h's PROTOCOL_VERSION macro, so compiling at all
 * is the check that the envelope can be included from the canary sketch. */
void test_envelope_takes_the_registry_version() {
  static_assert(mesh_envelope::OPERA_VERSION == mesh_wire::OPERA_VERSION,
                "the envelope's version byte is the registry's");
  assert(mesh_envelope::OPERA_VERSION == 1);
  assert(static_cast<uint8_t>(mesh_envelope::MsgType::TAMPER_ALERT) == mesh_wire::TAMPER_ALERT);
  std::printf("PASS test_envelope_takes_the_registry_version\n");
}
#endif

void test_first_byte_is_unambiguous() {
  using namespace mesh_wire;
  /* The two version bytes the trees have used as a first byte. */
  assert(!is_pairing_type(0) && !is_opera_type(0));
  assert(!is_pairing_type(1) && !is_opera_type(1));
  assert(!is_pairing_type(OPERA_VERSION));
  /* Blocks are disjoint and the classifiers say so for every byte. */
  for (int b = 0; b < 256; ++b) {
    const uint8_t v = (uint8_t)b;
    assert(!(is_pairing_type(v) && is_opera_type(v)));
    assert(is_pairing_type(v) == (b >= 8 && b <= 12));
    assert(is_opera_type(v)   == (b >= 16));
    if (v == OPERA_VERSION) assert(!is_pairing_type(v) && !is_opera_type(v));
  }
  /* Nothing assigned collides with the magics of the other two protocols
   * on the radio, and the version byte does not either. */
  assert(ASSIGNED_LAST < CHIRP_MAGIC && ASSIGNED_LAST < BEACON_MAGIC);
  assert(OPERA_VERSION != CHIRP_MAGIC && OPERA_VERSION != BEACON_MAGIC);
  assert(!is_pairing_type(CHIRP_MAGIC) && !is_pairing_type(BEACON_MAGIC));
  std::printf("PASS test_first_byte_is_unambiguous\n");
}

void test_old_disagreements_resolved() {
  using namespace mesh_wire;
  /* canary-wap's old values, and what the byte means now. */
  assert(4  != TAMPER_ALERT  && !is_pairing_type(4)  && !is_opera_type(4));   /* reserved */
  assert(13 != LEAVE_OPERA   && !is_pairing_type(13) && !is_opera_type(13));  /* reserved */
  assert(20 == OFFLINE_IMMINENT && 20 != CHANNEL_LOCK);
  assert(21 == WITNESS_RECORD   && 21 != HUB_ELECTION);
  /* The PIO tree's old pairing values are reserved now, not pairing. */
  for (int b = 0; b <= 4; ++b) assert(!is_pairing_type((uint8_t)b));
  std::printf("PASS test_old_disagreements_resolved\n");
}

}  /* namespace */

int main() {
  test_values_pinned();
  test_first_byte_is_unambiguous();
  test_old_disagreements_resolved();
#ifndef MESH_WIRE_STAGED_ONLY
  test_envelope_takes_the_registry_version();
#endif
  std::printf("\nALL MESH_WIRE TESTS PASSED\n");
  return 0;
}
