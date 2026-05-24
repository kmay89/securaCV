/*
 * Host-build test for mesh_hub_election wire format + HubMonitor.
 * Compile: g++ -std=c++17 -DCSI_TEST_HOST_BUILD -I src \
 *          src/mesh_hub_election.cpp test_mesh_hub_election.cpp -o test && ./test
 */

#include "mesh_hub_election.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace mesh_hub_election;

/* ── Wire format encode/decode ─────────────────────────────────────────── */

static void test_encode_decode_roundtrip() {
  uint8_t fp[FINGERPRINT_LEN] = {0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF};
  uint8_t buf[PAYLOAD_LEN];
  assert(encode(Event::HUB_ELECTED, fp, buf, sizeof(buf)));
  assert(buf[0] == 1);
  assert(memcmp(buf + 1, fp, FINGERPRINT_LEN) == 0);

  Event   out_event;
  uint8_t out_fp[FINGERPRINT_LEN];
  assert(decode(buf, sizeof(buf), &out_event, out_fp));
  assert(out_event == Event::HUB_ELECTED);
  assert(memcmp(out_fp, fp, FINGERPRINT_LEN) == 0);
  printf("  PASS: encode_decode_roundtrip\n");
}

static void test_encode_hub_absent() {
  uint8_t fp[FINGERPRINT_LEN] = {0};
  uint8_t buf[PAYLOAD_LEN];
  assert(encode(Event::HUB_ABSENT, fp, buf, sizeof(buf)));
  assert(buf[0] == 0);
  printf("  PASS: encode_hub_absent\n");
}

static void test_encode_rejects_null() {
  uint8_t fp[FINGERPRINT_LEN] = {0};
  uint8_t buf[PAYLOAD_LEN];
  assert(!encode(Event::HUB_ELECTED, fp, nullptr, sizeof(buf)));
  assert(!encode(Event::HUB_ELECTED, nullptr, buf, sizeof(buf)));
  printf("  PASS: encode_rejects_null\n");
}

static void test_encode_rejects_small_buffer() {
  uint8_t fp[FINGERPRINT_LEN] = {0};
  uint8_t buf[PAYLOAD_LEN - 1];
  assert(!encode(Event::HUB_ELECTED, fp, buf, sizeof(buf)));
  printf("  PASS: encode_rejects_small_buffer\n");
}

static void test_decode_rejects_wrong_length() {
  uint8_t buf[PAYLOAD_LEN + 1] = {0};
  Event   ev;
  uint8_t fp[FINGERPRINT_LEN];
  assert(!decode(buf, PAYLOAD_LEN + 1, &ev, fp));
  assert(!decode(buf, PAYLOAD_LEN - 1, &ev, fp));
  assert(!decode(buf, 0, &ev, fp));
  printf("  PASS: decode_rejects_wrong_length\n");
}

static void test_decode_rejects_null() {
  uint8_t buf[PAYLOAD_LEN] = {0};
  Event   ev;
  uint8_t fp[FINGERPRINT_LEN];
  assert(!decode(nullptr, PAYLOAD_LEN, &ev, fp));
  assert(!decode(buf, PAYLOAD_LEN, nullptr, fp));
  assert(!decode(buf, PAYLOAD_LEN, &ev, nullptr));
  printf("  PASS: decode_rejects_null\n");
}

/* ── Fingerprint comparison ────────────────────────────────────────────── */

static void test_compare_equal() {
  uint8_t a[FINGERPRINT_LEN] = {1,2,3,4,5,6,7,8};
  uint8_t b[FINGERPRINT_LEN] = {1,2,3,4,5,6,7,8};
  assert(compare_fingerprints(a, b) == 0);
  printf("  PASS: compare_equal\n");
}

static void test_compare_less() {
  uint8_t a[FINGERPRINT_LEN] = {0,0,0,0,0,0,0,1};
  uint8_t b[FINGERPRINT_LEN] = {0,0,0,0,0,0,0,2};
  assert(compare_fingerprints(a, b) < 0);
  printf("  PASS: compare_less\n");
}

static void test_compare_greater() {
  uint8_t a[FINGERPRINT_LEN] = {0xFF,0,0,0,0,0,0,0};
  uint8_t b[FINGERPRINT_LEN] = {0x00,0,0,0,0,0,0,0};
  assert(compare_fingerprints(a, b) > 0);
  printf("  PASS: compare_greater\n");
}

static void test_compare_first_byte_wins() {
  uint8_t a[FINGERPRINT_LEN] = {0x01,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  uint8_t b[FINGERPRINT_LEN] = {0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
  assert(compare_fingerprints(a, b) < 0);
  printf("  PASS: compare_first_byte_wins\n");
}

/* ── HubMonitor ────────────────────────────────────────────────────────── */

static void test_monitor_no_hub_no_fire() {
  HubMonitor m = make_monitor(60000);
  assert(!tick(m, 0));
  assert(!tick(m, 60000));
  assert(!tick(m, 120000));
  printf("  PASS: monitor_no_hub_no_fire\n");
}

static void test_monitor_fires_after_timeout() {
  HubMonitor m = make_monitor(60000);
  on_hub_heartbeat(m, 1000);
  assert(!tick(m, 1000));
  assert(!tick(m, 30000));
  assert(!tick(m, 60999));
  assert( tick(m, 61000));
  printf("  PASS: monitor_fires_after_timeout\n");
}

static void test_monitor_fires_once() {
  HubMonitor m = make_monitor(60000);
  on_hub_heartbeat(m, 0);
  assert(!tick(m, 59999));
  assert( tick(m, 60000));
  assert(!tick(m, 60001));
  assert(!tick(m, 120000));
  printf("  PASS: monitor_fires_once\n");
}

static void test_monitor_heartbeat_resets() {
  HubMonitor m = make_monitor(60000);
  on_hub_heartbeat(m, 0);
  assert(!tick(m, 50000));
  on_hub_heartbeat(m, 50000);
  assert(!tick(m, 100000));
  assert(!tick(m, 109999));
  assert( tick(m, 110000));
  printf("  PASS: monitor_heartbeat_resets\n");
}

static void test_monitor_heartbeat_clears_absence() {
  HubMonitor m = make_monitor(60000);
  on_hub_heartbeat(m, 0);
  assert( tick(m, 60000));
  on_hub_heartbeat(m, 70000);
  assert(!tick(m, 70001));
  assert(!tick(m, 129999));
  assert( tick(m, 130000));
  printf("  PASS: monitor_heartbeat_clears_absence\n");
}

static void test_monitor_reset_election() {
  HubMonitor m = make_monitor(60000);
  on_hub_heartbeat(m, 0);
  assert( tick(m, 60000));
  reset_election(m);
  assert(!tick(m, 60001));
  assert(!tick(m, 120000));
  printf("  PASS: monitor_reset_election\n");
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main() {
  printf("mesh_hub_election tests:\n");

  test_encode_decode_roundtrip();
  test_encode_hub_absent();
  test_encode_rejects_null();
  test_encode_rejects_small_buffer();
  test_decode_rejects_wrong_length();
  test_decode_rejects_null();
  test_compare_equal();
  test_compare_less();
  test_compare_greater();
  test_compare_first_byte_wins();
  test_monitor_no_hub_no_fire();
  test_monitor_fires_after_timeout();
  test_monitor_fires_once();
  test_monitor_heartbeat_resets();
  test_monitor_heartbeat_clears_absence();
  test_monitor_reset_election();

  printf("ALL 16 PASSED\n");
  return 0;
}
