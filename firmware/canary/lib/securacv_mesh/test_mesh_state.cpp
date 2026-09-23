/**
 * @file test_mesh_state.cpp
 * @brief Host-build test for the mesh_state NVS persistence stubs.
 *
 * The host build deliberately implements load/save/clear as
 * deterministic stubs (load → false, save/clear → true) so the
 * mesh_session and integration tests can run without an NVS
 * backend. This file pins that contract: a future contributor who
 * adds host-side persistence (e.g. file-backed) must also update
 * these expectations.
 *
 * On-device behavior is exercised by manual hardware tests; the host
 * build cannot meaningfully test Preferences without a fake.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_mesh/test_mesh_state.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_state.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_crypto.cpp \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_mesh_state && /tmp/test_mesh_state
 */

#include "mesh_state.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_mesh_state_run() { return 0; }
#else

namespace {

void test_load_returns_false_on_host() {
  /* Host stub semantics: no NVS, no persisted secret — load always
   * reports false so callers treat the device as "not paired yet". */
  uint8_t buf[mesh_crypto::OPERA_SECRET_LEN];
  std::memset(buf, 0xCC, sizeof(buf));   /* poison */
  assert(!mesh_state::load_opera_secret(buf));
  /* The poison bytes must be intact — load_opera_secret MUST NOT
   * touch the output buffer when it returns false. */
  for (size_t i = 0; i < sizeof(buf); ++i) {
    assert(buf[i] == 0xCC);
  }
  std::printf("PASS test_load_returns_false_on_host\n");
}

void test_save_and_clear_return_true_on_host() {
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)i;
  assert(mesh_state::save_opera_secret(secret));   /* host stub success */
  assert(mesh_state::clear_opera_secret());        /* host stub success */
  /* And load STILL returns false — the host stub is stateless by
   * design (no file-backing). Tests that need a populated opera_id
   * use mesh_session::set_opera_secret() in-memory. */
  uint8_t buf[mesh_crypto::OPERA_SECRET_LEN] = {0};
  assert(!mesh_state::load_opera_secret(buf));
  std::printf("PASS test_save_and_clear_return_true_on_host\n");
}

void test_null_pointer_handling() {
  /* save/load reject null pointers regardless of build. */
  assert(!mesh_state::save_opera_secret(nullptr));
  assert(!mesh_state::load_opera_secret(nullptr));
  /* clear has no pointer argument; nothing to test for null. */
  std::printf("PASS test_null_pointer_handling\n");
}

void test_trusted_peers_load_empty_on_host() {
  /* Host stub: load_trusted_peers always reports zero peers and
   * succeeds. Callers iterate i=0..count and skip when count==0. */
  uint8_t buf[mesh_state::MAX_TRUSTED_PEERS * mesh_crypto::PUBKEY_LEN];
  std::memset(buf, 0xCC, sizeof(buf));
  size_t count = 99;   /* poison */
  assert(mesh_state::load_trusted_peers(buf, sizeof(buf), &count));
  assert(count == 0);
  std::printf("PASS test_trusted_peers_load_empty_on_host\n");
}

void test_trusted_peers_save_and_clear_on_host() {
  uint8_t pubkey[mesh_crypto::PUBKEY_LEN];
  for (size_t i = 0; i < sizeof(pubkey); ++i) pubkey[i] = (uint8_t)(0x40 + i);
  assert(mesh_state::save_trusted_peer(pubkey));     /* host: no-op success */
  assert(mesh_state::clear_trusted_peers());          /* host: no-op success */
  /* Host stub stays empty after save — same stateless design as
   * opera_secret. Tests that need a populated peer set use
   * mesh_session::register_trusted_peer() directly. */
  uint8_t buf[mesh_state::MAX_TRUSTED_PEERS * mesh_crypto::PUBKEY_LEN];
  size_t count = 0;
  assert(mesh_state::load_trusted_peers(buf, sizeof(buf), &count));
  assert(count == 0);
  std::printf("PASS test_trusted_peers_save_and_clear_on_host\n");
}

void test_trusted_peers_null_handling() {
  uint8_t buf[mesh_state::MAX_TRUSTED_PEERS * mesh_crypto::PUBKEY_LEN];
  size_t count = 0;
  assert(!mesh_state::save_trusted_peer(nullptr));
  assert(!mesh_state::load_trusted_peers(nullptr, sizeof(buf), &count));
  assert(!mesh_state::load_trusted_peers(buf, sizeof(buf), nullptr));
  std::printf("PASS test_trusted_peers_null_handling\n");
}

void test_trusted_peers_load_buffer_too_small() {
  /* The buffer must hold MAX_TRUSTED_PEERS * PUBKEY_LEN bytes minimum.
   * A smaller buffer must be rejected so a caller can't accidentally
   * truncate the loaded list. */
  uint8_t small[mesh_crypto::PUBKEY_LEN];   /* room for ONE peer */
  size_t count = 0;
  assert(!mesh_state::load_trusted_peers(small, sizeof(small), &count));
  std::printf("PASS test_trusted_peers_load_buffer_too_small\n");
}

void test_elected_hub_load_returns_false_on_host() {
  /* Host stub: no NVS — load always reports false (no persisted hub)
   * and MUST NOT touch the output buffer on that false path. */
  uint8_t buf[mesh_crypto::FINGERPRINT_LEN];
  std::memset(buf, 0xCC, sizeof(buf));   /* poison */
  assert(!mesh_state::load_elected_hub(buf));
  for (size_t i = 0; i < sizeof(buf); ++i) {
    assert(buf[i] == 0xCC);
  }
  std::printf("PASS test_elected_hub_load_returns_false_on_host\n");
}

void test_elected_hub_save_and_clear_on_host() {
  uint8_t fp[mesh_crypto::FINGERPRINT_LEN];
  for (size_t i = 0; i < sizeof(fp); ++i) fp[i] = (uint8_t)(0x10 + i);
  assert(mesh_state::save_elected_hub(fp));   /* host stub success */
  assert(mesh_state::clear_elected_hub());     /* host stub success */
  /* Stateless host stub: load still reports nothing persisted. */
  uint8_t buf[mesh_crypto::FINGERPRINT_LEN] = {0};
  assert(!mesh_state::load_elected_hub(buf));
  std::printf("PASS test_elected_hub_save_and_clear_on_host\n");
}

void test_elected_hub_null_handling() {
  assert(!mesh_state::save_elected_hub(nullptr));
  assert(!mesh_state::load_elected_hub(nullptr));
  /* clear has no pointer argument; nothing to test for null. */
  std::printf("PASS test_elected_hub_null_handling\n");
}

void test_mesh_enabled_host_stub() {
  /* F10: load reports "nothing stored" (callers default to enabled) and
   * leaves the output untouched; save is a no-op success. */
  bool v = false;
  assert(!mesh_state::load_mesh_enabled(&v));
  assert(v == false);
  v = true;
  assert(!mesh_state::load_mesh_enabled(&v));
  assert(v == true);
  assert(!mesh_state::load_mesh_enabled(nullptr));
  assert(mesh_state::save_mesh_enabled(false));
  assert(mesh_state::save_mesh_enabled(true));
  std::printf("PASS test_mesh_enabled_host_stub\n");
}

void test_opera_name_host_stub() {
  /* F10: load → false with out untouched; save/clear → true; save
   * still validates its input on the host (null / empty / over-long). */
  char out[mesh_state::MAX_OPERA_NAME_BYTES + 1];
  std::memset(out, 0x5A, sizeof(out));
  assert(!mesh_state::load_opera_name(out, sizeof(out)));
  for (size_t i = 0; i < sizeof(out); ++i) assert(out[i] == 0x5A);
  assert(!mesh_state::load_opera_name(nullptr, sizeof(out)));
  assert(!mesh_state::load_opera_name(out, mesh_state::MAX_OPERA_NAME_BYTES));

  assert(mesh_state::save_opera_name("Home"));
  assert(!mesh_state::save_opera_name(nullptr));
  assert(!mesh_state::save_opera_name(""));
  char longname[mesh_state::MAX_OPERA_NAME_BYTES + 2];
  std::memset(longname, 'A', sizeof(longname) - 1);
  longname[sizeof(longname) - 1] = '\0';          /* 33 chars */
  assert(!mesh_state::save_opera_name(longname));
  longname[mesh_state::MAX_OPERA_NAME_BYTES] = '\0';  /* exactly 32 */
  assert(mesh_state::save_opera_name(longname));
  assert(mesh_state::clear_opera_name());
  std::printf("PASS test_opera_name_host_stub\n");
}

void test_remove_trusted_peer_host_stub() {
  uint8_t pubkey[mesh_crypto::PUBKEY_LEN];
  for (size_t i = 0; i < sizeof(pubkey); ++i) pubkey[i] = (uint8_t)(0x60 + i);
  assert(mesh_state::remove_trusted_peer(pubkey));   /* host: no-op success */
  assert(!mesh_state::remove_trusted_peer(nullptr));
  std::printf("PASS test_remove_trusted_peer_host_stub\n");
}

void test_persist_rotation_host_stub() {
  /* F10-rekey: the integration layer's re-persist through the FE gate.
   * Host stub validates arguments and succeeds. */
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x70 + i);
  uint8_t pks[2][mesh_crypto::PUBKEY_LEN] = {{1}, {2}};
  assert(mesh_state::persist_rotation(secret, nullptr, 0));
  assert(mesh_state::persist_rotation(secret, pks, 2));
  assert(!mesh_state::persist_rotation(nullptr, pks, 2));
  assert(!mesh_state::persist_rotation(secret, nullptr, 1));
  std::printf("PASS test_persist_rotation_host_stub\n");
}

void test_persist_rotation_fails_closed() {
  /* Review finding (fw-mesh #5): the secret used to be saved BEFORE the
   * forgotten pubkeys were removed, so a power cut between the writes
   * booted a survivor with the new secret AND the removed device still
   * trusted — and opera_id is cleartext, so the removed device could copy
   * the new one off the air. Order is now remove, remove, …, save. */
  uint8_t secret[mesh_crypto::OPERA_SECRET_LEN];
  for (size_t i = 0; i < sizeof(secret); ++i) secret[i] = (uint8_t)(0x71 + i);
  uint8_t pks[2][mesh_crypto::PUBKEY_LEN] = {{1}, {2}};

  mesh_state::test::reset_journal();
  assert(mesh_state::persist_rotation(secret, pks, 2));
  assert(std::strcmp(mesh_state::test::journal(), "RRS") == 0);

  mesh_state::test::reset_journal();
  assert(mesh_state::persist_rotation(secret, nullptr, 0));
  assert(std::strcmp(mesh_state::test::journal(), "S") == 0);

  /* A removal that fails: every removal is still tried, the new secret is
   * NOT saved, and the rotated-away one is cleared — the next boot comes
   * up with no opera rather than the new secret beside a stale member. */
  mesh_state::test::reset_journal();
  mesh_state::test::fail_remove_trusted_peer(true);
  assert(!mesh_state::persist_rotation(secret, pks, 2));
  assert(std::strcmp(mesh_state::test::journal(), "RRC") == 0);
  mesh_state::test::reset_journal();   /* also clears the failure switch */
  std::printf("PASS test_persist_rotation_fails_closed\n");
}

}  /* namespace */

/* F33 part 1 — the "peer_macs" blob edits (pure, run on the device too). */
void test_peer_mac_blob() {
  using namespace mesh_state;
  assert(PEER_MAC_ENTRY_LEN == 14 && PEER_MACS_BLOB_MAX == 112);
  uint8_t blob[PEER_MACS_BLOB_MAX] = {0};
  size_t len = 0;
  uint8_t fp[9][8];
  uint8_t mac[9][6];
  for (int i = 0; i < 9; ++i) {
    for (int b = 0; b < 8; ++b) fp[i][b] = (uint8_t)(0x10 * i + b);
    for (int b = 0; b < 6; ++b) mac[i][b] = (uint8_t)(0xA0 + i);
  }
  /* Insert up to the table size; the ninth fingerprint is refused. */
  for (int i = 0; i < 8; ++i) assert(peer_mac_blob::upsert(blob, &len, fp[i], mac[i]));
  assert(len == 8 * PEER_MAC_ENTRY_LEN);
  uint8_t before[PEER_MACS_BLOB_MAX];
  std::memcpy(before, blob, sizeof(blob));
  assert(!peer_mac_blob::upsert(blob, &len, fp[8], mac[8]));
  assert(len == 8 * PEER_MAC_ENTRY_LEN && std::memcmp(before, blob, sizeof(blob)) == 0);
  /* Replacing a known fingerprint's address is fine when full. */
  assert(peer_mac_blob::upsert(blob, &len, fp[3], mac[8]));
  assert(len == 8 * PEER_MAC_ENTRY_LEN);
  PeerMac out[MAX_TRUSTED_PEERS];
  size_t n = 0;
  assert(peer_mac_blob::decode(blob, len, out, MAX_TRUSTED_PEERS, &n) && n == 8);
  assert(std::memcmp(out[3].fingerprint, fp[3], 8) == 0 && std::memcmp(out[3].mac, mac[8], 6) == 0);
  /* Remove keeps the others in order. */
  assert(peer_mac_blob::remove(blob, &len, fp[0]));
  assert(!peer_mac_blob::remove(blob, &len, fp[0]));
  assert(len == 7 * PEER_MAC_ENTRY_LEN);
  assert(peer_mac_blob::decode(blob, len, out, MAX_TRUSTED_PEERS, &n) && n == 7);
  assert(std::memcmp(out[0].fingerprint, fp[1], 8) == 0 && std::memcmp(out[6].fingerprint, fp[7], 8) == 0);
  /* A malformed length is refused everywhere: a torn write is not a table. */
  size_t bad = 13;
  assert(!peer_mac_blob::valid_len(bad));
  assert(!peer_mac_blob::upsert(blob, &bad, fp[8], mac[8]));
  assert(!peer_mac_blob::remove(blob, &bad, fp[1]));
  assert(!peer_mac_blob::decode(blob, bad, out, MAX_TRUSTED_PEERS, &n) && n == 0);
  assert(!peer_mac_blob::valid_len(PEER_MACS_BLOB_MAX + PEER_MAC_ENTRY_LEN));
  assert(!peer_mac_blob::decode(blob, len, out, 2, &n));   /* out too small */
  assert(peer_mac_blob::decode(nullptr, 0, nullptr, 0, &n) && n == 0);
  /* Host NVS stubs: nothing stored. */
  assert(save_peer_mac(fp[1], mac[1]));
  assert(load_peer_macs(out, MAX_TRUSTED_PEERS, &n) && n == 0);
  assert(!load_peer_macs(out, MAX_TRUSTED_PEERS - 1, &n));
  assert(remove_peer_mac(fp[1]) && clear_peer_macs());
  assert(!save_peer_mac(nullptr, mac[1]) && !save_peer_mac(fp[1], nullptr));
  std::printf("PASS test_peer_mac_blob\n");
}

int main() {
  test_load_returns_false_on_host();
  test_save_and_clear_return_true_on_host();
  test_null_pointer_handling();
  test_trusted_peers_load_empty_on_host();
  test_trusted_peers_save_and_clear_on_host();
  test_trusted_peers_null_handling();
  test_trusted_peers_load_buffer_too_small();
  test_elected_hub_load_returns_false_on_host();
  test_elected_hub_save_and_clear_on_host();
  test_elected_hub_null_handling();
  test_mesh_enabled_host_stub();
  test_opera_name_host_stub();
  test_remove_trusted_peer_host_stub();
  test_persist_rotation_host_stub();
  test_persist_rotation_fails_closed();
  test_peer_mac_blob();
  std::printf("\nALL MESH_STATE TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
