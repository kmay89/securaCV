/**
 * @file test_mesh_alert.cpp
 * @brief Host-build test for the mesh_alert TAMPER_ALERT wire format (F10).
 *
 * Verifies:
 *   1. encode/decode round-trips every defined kind, severity and a
 *      witness_seq that exercises all four LE bytes.
 *   2. The byte layout is pinned: kind @0, severity @1, witness_seq LE @2..5.
 *   3. encode rejects severity > 7, a null buffer and a short buffer.
 *   4. decode rejects any length other than PAYLOAD_LEN (both shorter and
 *      LONGER — fixed-size is the smuggling defense), null pointers, and
 *      a severity byte > 7.
 *   5. An unknown kind byte decodes (forward compat) and kind_name()
 *      renders it "unknown"; the three defined kinds render the
 *      dictionary's tamper `kind` strings.
 *   6. type_name() is "TAMPER" (the canary-wap string the UI shows).
 *
 * Build:
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
 *       firmware/canary/lib/securacv_mesh/test_mesh_alert.cpp \
 *       firmware/canary/lib/securacv_mesh/src/mesh_alert.cpp \
 *       -I firmware/canary/lib/securacv_mesh/src \
 *       -o /tmp/test_mesh_alert && /tmp/test_mesh_alert
 */

#include "mesh_alert.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_mesh_alert_run() { return 0; }
#else

namespace {

void test_roundtrip_all_kinds() {
  const mesh_alert::Kind kinds[] = {
    mesh_alert::Kind::ENCLOSURE_TAMPER,
    mesh_alert::Kind::TEMP_DRIFT,
    mesh_alert::Kind::CAMERA_TAMPER,
  };
  for (mesh_alert::Kind k : kinds) {
    for (uint8_t sev = 0; sev <= mesh_alert::MAX_SEVERITY; ++sev) {
      uint8_t buf[mesh_alert::PAYLOAD_LEN];
      assert(mesh_alert::encode(k, sev, 0xA1B2C3D4u, buf, sizeof(buf)));
      mesh_alert::Kind out_k;
      uint8_t  out_sev = 0xFF;
      uint32_t out_seq = 0;
      assert(mesh_alert::decode(buf, sizeof(buf), &out_k, &out_sev, &out_seq));
      assert(out_k == k);
      assert(out_sev == sev);
      assert(out_seq == 0xA1B2C3D4u);
    }
  }
  std::printf("PASS test_roundtrip_all_kinds\n");
}

void test_layout_pinned() {
  uint8_t buf[mesh_alert::PAYLOAD_LEN];
  std::memset(buf, 0xEE, sizeof(buf));
  assert(mesh_alert::PAYLOAD_LEN == 6);
  assert(mesh_alert::encode(mesh_alert::Kind::CAMERA_TAMPER, 6, 0x04030201u,
                            buf, sizeof(buf)));
  static const uint8_t expected[6] = {0x02, 0x06, 0x01, 0x02, 0x03, 0x04};
  assert(std::memcmp(buf, expected, sizeof(expected)) == 0);
  std::printf("PASS test_layout_pinned\n");
}

void test_encode_rejects() {
  uint8_t buf[mesh_alert::PAYLOAD_LEN];
  assert(!mesh_alert::encode(mesh_alert::Kind::TEMP_DRIFT, 8, 1, buf, sizeof(buf)));
  assert(!mesh_alert::encode(mesh_alert::Kind::TEMP_DRIFT, 0xFF, 1, buf, sizeof(buf)));
  assert(!mesh_alert::encode(mesh_alert::Kind::TEMP_DRIFT, 3, 1, nullptr, sizeof(buf)));
  assert(!mesh_alert::encode(mesh_alert::Kind::TEMP_DRIFT, 3, 1, buf, sizeof(buf) - 1));
  std::printf("PASS test_encode_rejects\n");
}

void test_decode_rejects() {
  uint8_t buf[mesh_alert::PAYLOAD_LEN + 1];
  assert(mesh_alert::encode(mesh_alert::Kind::ENCLOSURE_TAMPER, 6, 9, buf, sizeof(buf)));
  buf[mesh_alert::PAYLOAD_LEN] = 0x00;
  mesh_alert::Kind k;
  uint8_t  sev = 0;
  uint32_t seq = 0;
  /* Short and long both refused — fixed-size format. */
  assert(!mesh_alert::decode(buf, mesh_alert::PAYLOAD_LEN - 1, &k, &sev, &seq));
  assert(!mesh_alert::decode(buf, mesh_alert::PAYLOAD_LEN + 1, &k, &sev, &seq));
  assert(!mesh_alert::decode(buf, 0, &k, &sev, &seq));
  /* Null pointers. */
  assert(!mesh_alert::decode(nullptr, mesh_alert::PAYLOAD_LEN, &k, &sev, &seq));
  assert(!mesh_alert::decode(buf, mesh_alert::PAYLOAD_LEN, nullptr, &sev, &seq));
  assert(!mesh_alert::decode(buf, mesh_alert::PAYLOAD_LEN, &k, nullptr, &seq));
  assert(!mesh_alert::decode(buf, mesh_alert::PAYLOAD_LEN, &k, &sev, nullptr));
  /* A severity byte outside the LogLevel scale is a malformed frame. */
  buf[1] = 8;
  assert(!mesh_alert::decode(buf, mesh_alert::PAYLOAD_LEN, &k, &sev, &seq));
  std::printf("PASS test_decode_rejects\n");
}

void test_unknown_kind_decodes_as_unknown() {
  const uint8_t buf[mesh_alert::PAYLOAD_LEN] = {0x7F, 4, 0, 0, 0, 0};
  mesh_alert::Kind k;
  uint8_t  sev = 0;
  uint32_t seq = 99;
  assert(mesh_alert::decode(buf, sizeof(buf), &k, &sev, &seq));
  assert(static_cast<uint8_t>(k) == 0x7F);
  assert(sev == 4);
  assert(seq == 0);
  assert(std::strcmp(mesh_alert::kind_name(k), "unknown") == 0);
  std::printf("PASS test_unknown_kind_decodes_as_unknown\n");
}

void test_names() {
  /* The dictionary's firmware tamper `kind` vocabulary — the same
   * strings main.cpp's MQTT tamper drain publishes. */
  assert(std::strcmp(mesh_alert::kind_name(mesh_alert::Kind::ENCLOSURE_TAMPER),
                     "enclosure_tamper") == 0);
  assert(std::strcmp(mesh_alert::kind_name(mesh_alert::Kind::TEMP_DRIFT),
                     "temp_drift") == 0);
  assert(std::strcmp(mesh_alert::kind_name(mesh_alert::Kind::CAMERA_TAMPER),
                     "camera_tamper") == 0);
  assert(std::strcmp(mesh_alert::type_name(), "TAMPER") == 0);
  std::printf("PASS test_names\n");
}

}  /* namespace */

int main() {
  test_roundtrip_all_kinds();
  test_layout_pinned();
  test_encode_rejects();
  test_decode_rejects();
  test_unknown_kind_decodes_as_unknown();
  test_names();
  std::printf("\nALL MESH_ALERT TESTS PASSED\n");
  return 0;
}

#endif  /* CSI_TEST_HOST_BUILD */
