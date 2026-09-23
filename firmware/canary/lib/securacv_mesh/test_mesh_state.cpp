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

}  /* namespace */

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
  std::printf("\nALL MESH_STATE TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
