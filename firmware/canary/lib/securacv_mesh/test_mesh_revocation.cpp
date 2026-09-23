/**
 * @file test_mesh_revocation.cpp
 * @brief Host test for the Opera revocation deny-list (mesh_revocation,
 *        spec §5.6 REVOCATION_GRACE_MS — F33 part 6). Shared by both mesh
 *        trees: canary-wap compiles a byte-identical staged copy.
 *
 * Verifies:
 *   1. add() lists a fingerprint for exactly REVOCATION_GRACE_MS (7 days):
 *      listed at grace - 1 ms, gone at grace; expire() drops it; the
 *      grace crosses the u32 millis wrap correctly.
 *   2. A second add() of the same device re-arms the full grace.
 *   3. A full list evicts the entry with the least grace left, so the
 *      removal the user just made always fits.
 *   4. encode()/decode(): the stored remaining time restarts from the
 *      new clock (power-off time does not count down), a longer grace
 *      already held wins, expired entries are not written, a stored value
 *      above the spec's grace is clamped, and a malformed length is refused.
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_mesh/test_mesh_revocation.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_revocation.cpp \
 *       -I firmware/canary/lib/securacv_mesh/src -o /tmp/t && /tmp/t
 */

#include "mesh_revocation.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_mesh_revocation_run() { return 0; }
#else

namespace {

using mesh_revocation::List;
constexpr uint32_t G = mesh_revocation::REVOCATION_GRACE_MS;

void fp_of(uint8_t n, uint8_t out[8]) {
  for (int i = 0; i < 8; ++i) out[i] = (uint8_t)(n * 16 + i);
}

void test_grace_is_seven_days() {
  assert(G == 7u * 24u * 3600u * 1000u);
  List l;
  mesh_revocation::init(l);
  uint8_t x[8], y[8];
  fp_of(1, x);
  fp_of(2, y);
  assert(!mesh_revocation::contains(l, x, 0));
  assert(mesh_revocation::add(l, x, 1000));
  assert(mesh_revocation::contains(l, x, 1000));
  assert(mesh_revocation::contains(l, x, 1000 + G - 1));
  assert(!mesh_revocation::contains(l, x, 1000 + G));
  assert(!mesh_revocation::contains(l, y, 1000));
  assert(mesh_revocation::count(l, 1000) == 1);
  assert(mesh_revocation::expire(l, 1000 + G - 1) == 0);
  assert(mesh_revocation::expire(l, 1000 + G) == 1);
  assert(mesh_revocation::count(l, 1000 + G) == 0);
  assert(!mesh_revocation::add(l, nullptr, 0));
  assert(!mesh_revocation::contains(l, nullptr, 0));

  /* Across the u32 millis wrap (every ~49.7 days of uptime). */
  const uint32_t t0 = 0xFFFFFFFFu - 1000u;
  assert(mesh_revocation::add(l, y, t0));
  assert(mesh_revocation::contains(l, y, t0 + 2000u));          /* wrapped */
  assert(mesh_revocation::contains(l, y, t0 + G - 1));
  assert(!mesh_revocation::contains(l, y, t0 + G));
  std::printf("PASS test_grace_is_seven_days\n");
}

void test_readd_rearms_full_grace() {
  List l;
  mesh_revocation::init(l);
  uint8_t x[8];
  fp_of(3, x);
  assert(mesh_revocation::add(l, x, 0));
  assert(mesh_revocation::add(l, x, G / 2));        /* removed again */
  assert(mesh_revocation::contains(l, x, G));       /* past the first grace */
  assert(mesh_revocation::contains(l, x, G / 2 + G - 1));
  assert(!mesh_revocation::contains(l, x, G / 2 + G));
  assert(mesh_revocation::count(l, G) == 1);        /* one entry, not two */
  std::printf("PASS test_readd_rearms_full_grace\n");
}

void test_full_list_evicts_least_grace() {
  List l;
  mesh_revocation::init(l);
  uint8_t f[mesh_revocation::MAX_REVOKED + 1][8];
  for (size_t i = 0; i <= mesh_revocation::MAX_REVOKED; ++i) fp_of((uint8_t)(i + 4), f[i]);
  for (size_t i = 0; i < mesh_revocation::MAX_REVOKED; ++i) {
    assert(mesh_revocation::add(l, f[i], (uint32_t)(i * 1000)));   /* f[0] is the oldest */
  }
  const uint32_t now = mesh_revocation::MAX_REVOKED * 1000u;
  assert(mesh_revocation::count(l, now) == mesh_revocation::MAX_REVOKED);
  assert(mesh_revocation::add(l, f[mesh_revocation::MAX_REVOKED], now));
  assert(mesh_revocation::contains(l, f[mesh_revocation::MAX_REVOKED], now));
  assert(!mesh_revocation::contains(l, f[0], now));                /* evicted */
  for (size_t i = 1; i < mesh_revocation::MAX_REVOKED; ++i) {
    assert(mesh_revocation::contains(l, f[i], now));
  }
  std::printf("PASS test_full_list_evicts_least_grace\n");
}

void test_encode_decode() {
  List l;
  mesh_revocation::init(l);
  uint8_t x[8], y[8], z[8];
  fp_of(13, x);
  fp_of(14, y);
  fp_of(15, z);
  /* Uptime only moves forward: x at 0, y at 3000, z at 4000. */
  assert(mesh_revocation::add(l, x, 0));
  assert(mesh_revocation::add(l, y, 3000));
  assert(mesh_revocation::add(l, z, 4000));
  uint8_t blob[mesh_revocation::BLOB_MAX];
  size_t n = mesh_revocation::encode(l, 5000, blob, sizeof(blob));
  assert(n == 3 * mesh_revocation::ENTRY_LEN);
  assert(mesh_revocation::encode(l, 5000, blob, 2 * mesh_revocation::ENTRY_LEN) == 0);  /* cap */
  /* x: G - 5000 left; y: G - 2000 left; z: G - 1000 left. */
  const uint32_t rx = (uint32_t)blob[8] | ((uint32_t)blob[9] << 8) |
                      ((uint32_t)blob[10] << 16) | ((uint32_t)blob[11] << 24);
  assert(std::memcmp(blob, x, 8) == 0 && rx == G - 5000);

  /* "Reboot": a fresh list, a new clock. The remaining time counts from
   * the restore — time powered off does not count down. */
  List r;
  mesh_revocation::init(r);
  assert(mesh_revocation::decode(r, blob, n, 100));
  assert(mesh_revocation::contains(r, x, 100 + (G - 5000) - 1));
  assert(!mesh_revocation::contains(r, x, 100 + (G - 5000)));
  assert(mesh_revocation::contains(r, y, 100 + (G - 2000) - 1));

  /* Merging keeps the longer grace. */
  assert(mesh_revocation::add(r, x, 200));                  /* fresh: G from 200 */
  assert(mesh_revocation::decode(r, blob, n, 300));          /* stored: shorter */
  assert(mesh_revocation::contains(r, x, 200 + G - 1));

  /* An expired list encodes to nothing. */
  List e;
  mesh_revocation::init(e);
  assert(mesh_revocation::add(e, x, 0));
  assert(mesh_revocation::encode(e, G, blob, sizeof(blob)) == 0);

  /* A stored grace above the spec's is corrupt, clamped; zero is skipped;
   * a malformed length is refused. */
  uint8_t bad[mesh_revocation::ENTRY_LEN * 2];
  std::memcpy(bad, z, 8);
  bad[8] = bad[9] = bad[10] = bad[11] = 0xFF;
  std::memcpy(bad + 12, y, 8);
  bad[20] = bad[21] = bad[22] = bad[23] = 0;
  List c;
  mesh_revocation::init(c);
  assert(mesh_revocation::decode(c, bad, sizeof(bad), 0));
  assert(mesh_revocation::contains(c, z, G - 1) && !mesh_revocation::contains(c, z, G));
  assert(!mesh_revocation::contains(c, y, 0));
  assert(!mesh_revocation::decode(c, bad, 13, 0));
  assert(!mesh_revocation::decode(c, nullptr, 12, 0));
  assert(mesh_revocation::decode(c, nullptr, 0, 0));
  uint8_t huge[mesh_revocation::BLOB_MAX + mesh_revocation::ENTRY_LEN] = {0};
  assert(!mesh_revocation::decode(c, huge, sizeof(huge), 0));
  std::printf("PASS test_encode_decode\n");
}

}  /* namespace */

int main() {
  test_grace_is_seven_days();
  test_readd_rearms_full_grace();
  test_full_list_evicts_least_grace();
  test_encode_decode();
  std::printf("\nALL MESH_REVOCATION TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
