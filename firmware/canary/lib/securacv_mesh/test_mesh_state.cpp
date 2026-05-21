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

}  /* namespace */

int main() {
  test_load_returns_false_on_host();
  test_save_and_clear_return_true_on_host();
  test_null_pointer_handling();
  std::printf("\nALL MESH_STATE TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
