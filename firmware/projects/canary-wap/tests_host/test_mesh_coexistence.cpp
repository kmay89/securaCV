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

// The governor adds ESP-NOW's framing to every frame itself: 192 us
// preamble + (bytes + 59) x 8 us at the 1 Mbps fallback rate.
static_assert(airtime_governor::ESPNOW_FRAME_OVERHEAD_BYTES == 59,
              "the framing docs/network_coexistence.md states");

static void test_governor_estimate_includes_framing() {
  EXPECT(airtime_governor::estimate_airtime_us(0) == 664u);     // framing alone
  EXPECT(airtime_governor::estimate_airtime_us(16) == 792u);    // the CSI probe
  EXPECT(airtime_governor::estimate_airtime_us(250) == 2664u);  // a full frame
}

static void test_governor_caps_routine() {
  airtime_governor::init(2);  // 2% cap

  // 2% of 10 s = 200 ms = 200_000 us of allowed routine airtime.
  // One 250-byte payload framed as 309 B @ 1 Mbps + 192 us preamble =
  // 2664 us, so the cap allows 75 (199 800 us); the 76th would pass it.
  uint32_t now = 1000;
  int allowed = 0;
  int denied = 0;
  for (int i = 0; i < 200; i++) {
    if (airtime_governor::try_reserve_routine(now, 250)) allowed++;
    else denied++;
    now += 5;   // 5 ms apart → all stay inside the 10 s window
  }
  EXPECT(allowed == 75);
  EXPECT(denied == 125);
  EXPECT(airtime_governor::snapshot(now).airtime_us == 75u * 2664u);
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
  // Urgent still goes through, past the cap, and the window counts it:
  // 75 routine sends of 2664 us, then one urgent one.
  airtime_governor::force_reserve_urgent(100, 250);
  airtime_governor::Stats s = airtime_governor::snapshot(100);
  EXPECT(s.urgent_sends == 1);
  EXPECT(s.routine_allowed == 75);
  EXPECT(s.routine_denied == 126);
  EXPECT(s.airtime_us == 76u * 2664u);
}

static void test_governor_airtime_pct() {
  airtime_governor::init(2);

  // 50 routine sends of 250 B each within the window, framed:
  // 50 * 2664 us = 133_200 us out of 10_000_000 us = 1.332 %
  // (pct_x100 = 133).
  for (int i = 0; i < 50; i++) {
    airtime_governor::try_reserve_routine(100, 250);
  }
  uint16_t pct = airtime_governor::airtime_pct_x100(200);
  EXPECT(pct == 133);
}

// ─────────────────────────────────────────────────────────────────────────
// airtime_governor: a fan-out is charged one frame per peer
// ─────────────────────────────────────────────────────────────────────────

// mesh_network's broadcast_message() unicasts one signed frame to each
// connected peer (38 B header + payload + 64 B signature), so a heartbeat
// (12 B HeartbeatPayload) to 3 connected peers puts 3 frames of 114 B on
// the air: 3 x (192 + (114 + 59) x 8) = 3 x 1576 us, each with its own
// preamble and framing — not one 342 B frame, and not one 114 B frame.
static void test_governor_charges_each_frame_of_a_fan_out() {
  const size_t hb_frame = 38 + 12 + 64;
  const uint32_t one = airtime_governor::estimate_airtime_us(hb_frame);
  EXPECT(one == 1576u);

  airtime_governor::init(2);
  EXPECT(airtime_governor::try_reserve_routine(1000, hb_frame, 3));
  airtime_governor::Stats s = airtime_governor::snapshot(1000);
  EXPECT(s.airtime_us == 3u * 1576u);
  EXPECT(s.airtime_us != airtime_governor::estimate_airtime_us(3 * hb_frame));
  EXPECT(s.routine_allowed == 1);        // one reservation, three frames

  // frames defaults to 1; urgent and Beacon reservations multiply alike.
  EXPECT(airtime_governor::try_reserve_routine(1000, hb_frame));
  airtime_governor::force_reserve_urgent(1000, hb_frame, 3);
  airtime_governor::force_reserve_beacon(1000, hb_frame, 2);
  s = airtime_governor::snapshot(1000);
  EXPECT(s.airtime_us == 9u * 1576u);
  EXPECT(s.beacon_airtime_us == 2u * 1576u);
  EXPECT(s.urgent_sends == 1 && s.beacon_sends == 1);

  // No peers connected: nothing goes on the air, nothing is charged.
  airtime_governor::init(2);
  EXPECT(airtime_governor::try_reserve_routine(1000, hb_frame, 0));
  EXPECT(airtime_governor::snapshot(1000).airtime_us == 0);

  // All or nothing: 125 x 1576 = 197 000 us leaves 3000 us, room for one
  // frame (1576) but not three (4728); the fan-out is denied whole and
  // records nothing.
  airtime_governor::init(2);
  EXPECT(airtime_governor::try_reserve_routine(1000, hb_frame, 125));
  EXPECT(!airtime_governor::try_reserve_routine(1000, hb_frame, 3));
  EXPECT(airtime_governor::snapshot(1000).airtime_us == 125u * 1576u);
  EXPECT(airtime_governor::try_reserve_routine(1000, hb_frame, 1));
  s = airtime_governor::snapshot(1000);
  EXPECT(s.airtime_us == 126u * 1576u);
  EXPECT(s.routine_denied == 1);
}

// ─────────────────────────────────────────────────────────────────────────
// airtime_governor: the window holds every send, at any reservation rate
// ─────────────────────────────────────────────────────────────────────────

// 2000 per-frame 8 B reservations inside one 10 s window (200 Hz) may
// spend the 2 % cap and no more: framed as 67 B, a frame is 728 us, and
// 274 x 728 = 199 472 us (the 275th would pass 200 000). A ring of 256
// SENDS saw at most 256 x 728 = 186 368 us, under the cap, so it allowed
// all 2000 (host-measured before the bucket ring, unframed 16 B: all 2000
// allowed, read 0.82 %, true 6.41 %). The payload is 8 B, not the probe's
// 16 B, because a framed 16 B frame (792 us) clears the per-send ring's
// 781.25 us-a-slot line at 2 % and would not catch it.
static void test_governor_ring_covers_window_at_any_rate() {
  airtime_governor::init(2);
  int allowed = 0;
  for (int i = 0; i < 2000; i++) {
    if (airtime_governor::try_reserve_routine(1000u + 5u * i, 8)) allowed++;
  }
  airtime_governor::Stats s = airtime_governor::snapshot(1000u + 5u * 1999u);
  EXPECT(allowed == 274);
  EXPECT(s.airtime_us == 199472u);
  EXPECT(s.airtime_pct_x100 == 199);
  EXPECT(s.routine_denied == 2000u - 274u);
}

// The cap is init()'s to choose, and it must hold for the CSI probe's
// 16 B frames (framed as 75 B, 792 us) at 160 frames/s: 300 000 / 792 =
// 378. A ring of 256 sends only ever saw 256 x 792 = 202 752 us of them,
// so a 3 % cap never denied at all.
static void test_governor_cap_holds_above_default() {
  airtime_governor::init(3);
  int allowed = 0;
  for (int i = 0; i < 1600; i++) {
    if (airtime_governor::try_reserve_routine(1000u + (10000u * i) / 1600u, 16)) allowed++;
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
// then snapshot(now) runs. Ten 16 B probe frames at 20000..20090 and a
// one-peer heartbeat (one 114 B signed frame) at 20095 share one bucket
// stamped 20095; a reader at 20050 must see at least the six frames sent
// by then. A bucket stamped after `now` was skipped whole, so the window
// read 0.
static void test_governor_trailing_reader_keeps_the_newest_bucket() {
  airtime_governor::init(2);
  const uint32_t frame = airtime_governor::estimate_airtime_us(16);   // 792 us
  const uint32_t hb = airtime_governor::estimate_airtime_us(114);     // 1576 us
  for (uint32_t t = 20000; t <= 20090; t += 10) {
    EXPECT(airtime_governor::try_reserve_routine(t, 16));
  }
  EXPECT(airtime_governor::try_reserve_routine(20095, 114));
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

  test_governor_estimate_includes_framing();
  test_governor_caps_routine();
  test_governor_window_decay();
  test_governor_urgent_bypasses();
  test_governor_airtime_pct();
  test_governor_charges_each_frame_of_a_fan_out();
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
