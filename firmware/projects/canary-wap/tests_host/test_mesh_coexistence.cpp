// Host-side unit tests for the mesh channel policy and the airtime governor.
//
// These modules are intentionally Arduino-free in their core logic so they
// can be exercised on a plain g++ build, without an ESP32 in the loop. The
// firmware build path includes the same .h/.cpp files; this test compiles
// them in the non-ESP32 branch.
//
// Build + run:
//   make -C firmware/projects/canary-wap/tests_host
//
// Returns 0 on success, non-zero on failure.

#include "../arduino/canary_wap/mesh_channel_policy.h"
#include "../arduino/canary_wap/airtime_governor.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace mesh_channel_policy;

static int g_failures = 0;

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      g_failures++;                                                        \
    }                                                                      \
  } while (0)

// ─────────────────────────────────────────────────────────────────────────
// mesh_channel_policy::decide()
// ─────────────────────────────────────────────────────────────────────────

static void test_decide_sta_wins() {
  // STA connected on ch 11 + AP on ch 6 → STA wins, locked_to_sta=true.
  RadioState s{};
  s.sta_connected = true;
  s.sta_channel = 11;
  s.ap_active = true;
  s.ap_channel = 6;
  ChannelDecision d = decide(s);
  EXPECT(d.channel == 11);
  EXPECT(d.locked_to_sta);
  EXPECT(!d.locked_to_ap);
  EXPECT(!d.fallback);
}

static void test_decide_ap_when_sta_off() {
  RadioState s{};
  s.sta_connected = false;
  s.ap_active = true;
  s.ap_channel = 1;
  ChannelDecision d = decide(s);
  EXPECT(d.channel == 1);
  EXPECT(!d.locked_to_sta);
  EXPECT(d.locked_to_ap);
  EXPECT(!d.fallback);
}

static void test_decide_fallback() {
  RadioState s{};   // both off
  ChannelDecision d = decide(s);
  EXPECT(d.channel == MESH_FALLBACK_CHANNEL);
  EXPECT(d.fallback);
  EXPECT(!d.locked_to_sta);
  EXPECT(!d.locked_to_ap);
}

static void test_decide_sta_connected_but_zero_channel_falls_through() {
  // Defensive: if STA reports connected but channel == 0, we must not pick
  // channel 0; fall through to AP / fallback.
  RadioState s{};
  s.sta_connected = true;
  s.sta_channel = 0;
  s.ap_active = true;
  s.ap_channel = 6;
  ChannelDecision d = decide(s);
  EXPECT(d.channel == 6);
  EXPECT(d.locked_to_ap);
}

// ─────────────────────────────────────────────────────────────────────────
// mesh_channel_policy::register_listener + poll_radio
// ─────────────────────────────────────────────────────────────────────────

static int g_change_count = 0;
static uint8_t g_last_old = 0;
static uint8_t g_last_new = 0;
static void capture_change(uint8_t old_ch, uint8_t new_ch) {
  g_change_count++;
  g_last_old = old_ch;
  g_last_new = new_ch;
}

static void test_listener_fires_on_change_only() {
  g_change_count = 0;
  register_listener(capture_change);

  RadioState s{};
  s.sta_connected = true;
  s.sta_channel = 6;
  set_state_for_tests(s);
  poll_radio();
  EXPECT(g_change_count == 1);
  EXPECT(g_last_new == 6);

  // Same state → no fire
  poll_radio();
  EXPECT(g_change_count == 1);

  // Switch to channel 11
  s.sta_channel = 11;
  set_state_for_tests(s);
  poll_radio();
  EXPECT(g_change_count == 2);
  EXPECT(g_last_old == 6);
  EXPECT(g_last_new == 11);

  // STA drops → fallback (channel 6) — change fires
  RadioState off{};
  set_state_for_tests(off);
  poll_radio();
  EXPECT(g_change_count == 3);
  EXPECT(g_last_new == MESH_FALLBACK_CHANNEL);
}

// ─────────────────────────────────────────────────────────────────────────
// airtime_governor: cap routine traffic, allow urgent through
// ─────────────────────────────────────────────────────────────────────────

static void test_governor_caps_routine() {
  airtime_governor::init(2);  // 2% cap

  // 2% of 10 s = 200 ms = 200_000 us of allowed routine airtime.
  // One 250-byte packet @ 1 Mbps + 192 us preamble = 2192 us.
  // Cap allows roughly 200_000 / 2192 ≈ 91 packets before denying.
  uint32_t now = 1000;
  int allowed = 0;
  int denied = 0;
  for (int i = 0; i < 200; i++) {
    if (airtime_governor::try_reserve_routine(now, 250)) allowed++;
    else denied++;
    now += 5;   // 5 ms apart → all stay inside the 10 s window
  }
  EXPECT(allowed >= 80 && allowed <= 100);
  EXPECT(denied >= 100);
}

static void test_governor_window_decay() {
  airtime_governor::init(2);

  // Saturate at t=0
  for (int i = 0; i < 200; i++) {
    airtime_governor::try_reserve_routine(0, 250);
  }
  // At t=15 s the entire window has aged out; we should be able to send again.
  EXPECT(airtime_governor::try_reserve_routine(15000, 250));
}

static void test_governor_urgent_bypasses() {
  airtime_governor::init(2);

  // Burn the routine budget
  for (int i = 0; i < 200; i++) {
    airtime_governor::try_reserve_routine(0, 250);
  }
  // Routine now denied
  EXPECT(!airtime_governor::try_reserve_routine(100, 250));
  // Urgent still goes through; force_reserve_urgent is void so we just
  // assert it doesn't crash and the snapshot reflects the send.
  airtime_governor::force_reserve_urgent(100, 250);
  airtime_governor::Stats s = airtime_governor::snapshot(100);
  EXPECT(s.urgent_sends == 1);
  EXPECT(s.routine_denied >= 1);
}

static void test_governor_airtime_pct() {
  airtime_governor::init(2);

  // 50 routine sends of 250 B each within the window:
  // 50 * 2192 us = 109_600 us out of 10_000_000 us = ~1.10%
  // (pct_x100 ≈ 109).
  for (int i = 0; i < 50; i++) {
    airtime_governor::try_reserve_routine(100, 250);
  }
  uint16_t pct = airtime_governor::airtime_pct_x100(200);
  EXPECT(pct >= 90 && pct <= 130);
}

// ─────────────────────────────────────────────────────────────────────────
// airtime_governor: the window holds every send, at any reservation rate
// ─────────────────────────────────────────────────────────────────────────

// 2000 per-frame 16 B reservations inside one 10 s window (200 Hz) may
// spend the 2 % cap and no more: 625 x 320 us = 200 000 us. A ring of 256
// SENDS overwrote in-window sends, allowed all 2000 and read 0.82 % (the
// true figure was 6.41 %).
static void test_governor_ring_covers_window_at_any_rate() {
  airtime_governor::init(2);
  int allowed = 0;
  for (int i = 0; i < 2000; i++) {
    if (airtime_governor::try_reserve_routine(1000u + 5u * i, 16)) allowed++;
  }
  airtime_governor::Stats s = airtime_governor::snapshot(1000u + 5u * 1999u);
  EXPECT(allowed == 625);
  EXPECT(s.airtime_us == 200000u);
  EXPECT(s.airtime_pct_x100 == 200);
  EXPECT(s.routine_denied == 2000u - 625u);
}

// The cap is init()'s to choose, and it must hold for the CSI probe's
// framed 75 B frames (792 us) at 160 frames/s: 300 000 / 792 = 378. A ring
// of 256 sends only ever saw 256 x 792 = 202 752 us of them, so a 3 % cap
// never denied at all.
static void test_governor_cap_holds_above_default() {
  airtime_governor::init(3);
  int allowed = 0;
  for (int i = 0; i < 1600; i++) {
    if (airtime_governor::try_reserve_routine(1000u + (10000u * i) / 1600u, 75)) allowed++;
  }
  EXPECT(allowed == 378);
  EXPECT(airtime_governor::airtime_pct_x100(1000u + 9993u) <= 300);
}

// A 100 ms bucket leaves the window with its NEWEST send, routine and
// urgent alike: the window reads 10.0-10.1 s, never shorter, so the cap
// can only err toward denying.
static void test_governor_bucket_ages_out_with_its_newest_send() {
  airtime_governor::init(2);
  const uint32_t c = airtime_governor::estimate_airtime_us(100);
  EXPECT(airtime_governor::try_reserve_routine(1000, 100));
  airtime_governor::force_reserve_urgent(1050, 100);
  EXPECT(airtime_governor::try_reserve_routine(1099, 100));
  EXPECT(airtime_governor::snapshot(11000).airtime_us == 3 * c);
  EXPECT(airtime_governor::snapshot(11099).airtime_us == 3 * c);
  EXPECT(airtime_governor::snapshot(11100).airtime_us == 0);
  airtime_governor::Stats s = airtime_governor::snapshot(11100);
  EXPECT(s.routine_allowed == 2);
  EXPECT(s.urgent_sends == 1);
}

// A reader whose clock trails the newest send still counts the sends
// before its `now`. The WAP's MQTT publish does this: loop() takes
// `now = millis()` early, the mesh, chirp and probe record later sends,
// then snapshot(now) runs. Ten 75 B probe frames at 20000..20090 and a
// 60 B heartbeat at 20095 share one bucket stamped 20095; a reader at
// 20050 must see at least the six frames sent by then. A bucket stamped
// after `now` was skipped whole, so the window read 0.
static void test_governor_trailing_reader_keeps_the_newest_bucket() {
  airtime_governor::init(2);
  const uint32_t frame = airtime_governor::estimate_airtime_us(75);   // 792 us
  const uint32_t hb = airtime_governor::estimate_airtime_us(60);      // 672 us
  for (uint32_t t = 20000; t <= 20090; t += 10) {
    EXPECT(airtime_governor::try_reserve_routine(t, 75));
  }
  EXPECT(airtime_governor::try_reserve_routine(20095, 60));
  const airtime_governor::Stats s = airtime_governor::snapshot(20050);
  EXPECT(s.airtime_us >= 6 * frame);            // never below the truth
  EXPECT(s.airtime_us <= 10 * frame + hb);      // over by at most one bucket
  EXPECT(airtime_governor::airtime_pct_x100(20050) >= (6 * frame) / 1000);
}

// ─────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────

int main() {
  test_decide_sta_wins();
  test_decide_ap_when_sta_off();
  test_decide_fallback();
  test_decide_sta_connected_but_zero_channel_falls_through();
  test_listener_fires_on_change_only();

  test_governor_caps_routine();
  test_governor_window_decay();
  test_governor_urgent_bypasses();
  test_governor_airtime_pct();
  test_governor_ring_covers_window_at_any_rate();
  test_governor_cap_holds_above_default();
  test_governor_bucket_ages_out_with_its_newest_send();
  test_governor_trailing_reader_keeps_the_newest_bucket();

  if (g_failures == 0) {
    std::printf("OK  all mesh coexistence tests passed\n");
    std::printf("ALL MESH COEXISTENCE TESTS PASSED\n");
    return 0;
  }
  std::fprintf(stderr, "FAIL  %d assertion(s) failed\n", g_failures);
  return 1;
}
