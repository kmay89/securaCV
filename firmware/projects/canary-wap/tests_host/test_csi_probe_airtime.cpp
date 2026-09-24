/*
 * The CSI active probe under the airtime governor, end to end on the host:
 * the REAL csi_probe scheduler (the sketch's staged copy under
 * CSI_TEST_HOST_BUILD: virtual clock, send hook), the REAL airtime_governor,
 * and the gate body the WAP installs (probe_airtime.h). An independent
 * ledger of every send the governor recorded is the oracle for the window.
 *
 * Every percentage here is the governor's ESTIMATE of airtime (192 us +
 * 8 us a byte, plus the probe's 59 B of framing), not a measurement of the
 * air. What the cases hold:
 *   - ensure_governor() makes a mesh-less build's gate real (the governor
 *     otherwise fails open: every reservation passes);
 *   - the WAP today (idle 10 Hz broadcast) is never denied;
 *   - the gate starts a frame at 159 x100 and none at 160 x100;
 *   - one peer at the full 20 Hz stays steady under the 1.60 % ceiling;
 *   - eight peers asking 160 frames/s are held at the ceiling, the window
 *     never reads below the ledger, and the 30 s mesh heartbeat keeps its
 *     room (without the ceiling: 0 of 6);
 *   - probe_pump installs exactly this gate — read from the real
 *     csi_integration.cpp (beacon_source_scan.h; its path is passed as
 *     CSI_INTEGRATION_CPP, so the pin fails closed).
 */
#include "probe_airtime.h"
#include "csi_probe.h"
#include "beacon_source_scan.h"

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

/* probe_pump assigns this function to the probe's hook: same signature. */
static_assert(std::is_same<decltype(&probe_airtime::reserve_probe_frame),
                           decltype(csi_probe::Config::airtime_gate)>::value,
              "reserve_probe_frame must fit csi_probe::Config::airtime_gate");
/* The number FEATURES.md, docs/network_coexistence.md and
 * docs/esp32_mesh_sensing_design.md state. Raising it also moves the probe
 * onto the Beacon's airtime_saturated trouble line (beacon_channel.cpp,
 * > 160 x100): change the docs and that line with it, never this alone. */
static_assert(probe_airtime::PROBE_CEILING_PCT_X100 == 160,
              "the probe's ceiling is the documented 1.60 %");

static int g_failures = 0;
#define EXPECT(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)
/* A case's summary line says PASS only when none of its EXPECTs failed. */
#define VERDICT(before) (g_failures == (before) ? "PASS" : "FAIL")

namespace {

struct Rec { uint32_t ts, us; };
std::vector<Rec> g_ledger;
uint32_t g_now = 0;

/* The ledger's window is exactly 10 s of sends; the governor's may read up
 * to one 100 ms bucket more, never less. */
uint32_t ledger_window(uint32_t now) {
  uint64_t s = 0;
  for (const Rec& r : g_ledger)
    if ((uint32_t)(now - r.ts) <= airtime_governor::WINDOW_MS) s += r.us;
  return (uint32_t)s;
}
bool gate(uint32_t now, size_t payload) {
  const bool ok = probe_airtime::reserve_probe_frame(now, payload);
  if (ok) g_ledger.push_back({now, airtime_governor::estimate_airtime_us(
                                       payload + probe_airtime::PROBE_FRAME_OVERHEAD_BYTES)});
  return ok;
}
bool heartbeat(uint32_t now) {                 /* mesh_network: 60 B every 30 s */
  const bool ok = airtime_governor::try_reserve_routine(now, 60);
  if (ok) g_ledger.push_back({now, airtime_governor::estimate_airtime_us(60)});
  return ok;
}
bool send_ok(const uint8_t*, const uint8_t*, size_t) { return true; }

struct Run { uint32_t hb_ok, hb_tries, max_x100, max_under_us; csi_probe::Stats st; };

Run run(int peers, uint32_t seconds) {
  g_ledger.clear();
  airtime_governor::init(airtime_governor::DEFAULT_CAP_PCT);
  csi_probe::deinit();
  g_now = 1000;
  csi_probe::test::set_now_ms(g_now);
  csi_probe::test::set_send_hook(send_ok);
  csi_probe::test::set_peer_add_hook(nullptr);
  csi_probe::Config c = csi_probe::Config::defaults();
  c.broadcast_when_no_peers = true;
  c.idle_rate_hz = 10;                         /* CSI_PROBE_BROADCAST_HZ */
  c.airtime_gate = gate;
  EXPECT(csi_probe::init(c));
  EXPECT(csi_probe::start());
  for (int i = 0; i < peers; ++i) {
    const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, (uint8_t)i};
    EXPECT(csi_probe::add_peer(mac));
  }
  Run r{};
  uint32_t next_hb = g_now + 5000;
  for (const uint32_t end = g_now + seconds * 1000u; g_now < end;) {
    ++g_now;
    csi_probe::test::set_now_ms(g_now);
    csi_probe::process();
    if (g_now >= next_hb) { ++r.hb_tries; if (heartbeat(g_now)) ++r.hb_ok; next_hb += 30000; }
    if (g_now % 100 == 0) {
      const uint32_t truth = ledger_window(g_now);
      const uint32_t seen = airtime_governor::snapshot(g_now).airtime_us;
      if (truth > seen && truth - seen > r.max_under_us) r.max_under_us = truth - seen;
      const uint32_t x100 = (uint32_t)((uint64_t)truth * 10000u / (airtime_governor::WINDOW_MS * 1000u));
      if (x100 > r.max_x100) r.max_x100 = x100;
    }
  }
  csi_probe::get_stats(&r.st);
  return r;
}

/* The ring is still unallocated when this runs first: ensure_governor()
 * is what makes a mesh-less build's gate real. */
void test_ensure_governor_brings_up_a_mesh_less_window() {
  const int f0 = g_failures;
  EXPECT(!airtime_governor::ring_ok());
  probe_airtime::ensure_governor();
  EXPECT(airtime_governor::ring_ok());
  EXPECT(probe_airtime::reserve_probe_frame(1000, 16));
  EXPECT(airtime_governor::snapshot(1000).airtime_us ==
         airtime_governor::estimate_airtime_us(16 + 59));   /* 792 us, framed */
  /* A second call on a live ring must not reset the window. */
  probe_airtime::ensure_governor();
  EXPECT(airtime_governor::snapshot(1000).airtime_us == 792u);
  std::printf("%s ensure_governor + framed cost\n", VERDICT(f0));
}

/* The gate itself, at its line: a window reading 159 x100 starts one more
 * frame and a window reading 160 x100 starts none. Estimates are 192 us +
 * 8 us a byte, so one urgent send of 19 975 B puts 159 992 us in the
 * window and one of 19 976 B puts exactly 160 000 us. */
void test_ceiling_is_one_point_six_zero_exactly() {
  const int f0 = g_failures;
  airtime_governor::init(airtime_governor::DEFAULT_CAP_PCT);
  airtime_governor::force_reserve_urgent(50000, 19975);
  EXPECT(airtime_governor::snapshot(50000).airtime_us == 159992u);
  EXPECT(airtime_governor::airtime_pct_x100(50000) == 159);
  EXPECT(probe_airtime::reserve_probe_frame(50000, 16));    /* under: sends */
  EXPECT(airtime_governor::snapshot(50000).airtime_us == 159992u + 792u);
  EXPECT(!probe_airtime::reserve_probe_frame(50000, 16));   /* 160: stops */

  airtime_governor::init(airtime_governor::DEFAULT_CAP_PCT);
  airtime_governor::force_reserve_urgent(50000, 19976);
  EXPECT(airtime_governor::snapshot(50000).airtime_us == 160000u);
  EXPECT(airtime_governor::airtime_pct_x100(50000) == 160);
  EXPECT(!probe_airtime::reserve_probe_frame(50000, 16));   /* at the line */
  /* ...while the routine cap still has 40 000 us for everyone else. */
  EXPECT(airtime_governor::try_reserve_routine(50000, 60));
  std::printf("%s ceiling: 159 x100 sends, 160 x100 stops\n", VERDICT(f0));
}

void test_wap_today_is_in_budget() {           /* broadcast-only at 10 Hz */
  const int f0 = g_failures;
  Run r = run(0, 60);
  EXPECT(r.st.sends_denied_airtime == 0);
  EXPECT(r.st.broadcasts_sent >= 599);
  EXPECT(r.max_x100 >= 78 && r.max_x100 <= 80);   /* 10 x 792 us per s */
  EXPECT(r.max_under_us == 0);
  EXPECT(r.hb_ok == r.hb_tries);
  std::printf("%s WAP today: 10 Hz broadcast, %u x100, never denied\n", VERDICT(f0), r.max_x100);
}

void test_one_peer_at_full_rate_is_steady() {  /* 1.58 %: under the ceiling */
  const int f0 = g_failures;
  Run r = run(1, 60);
  EXPECT(r.st.sends_denied_airtime <= 3);
  EXPECT(r.st.unicasts_sent >= 1190);
  EXPECT(r.max_under_us == 0);
  std::printf("%s one peer x 20 Hz steady (%u denied)\n", VERDICT(f0), r.st.sends_denied_airtime);
}

void test_over_budget_probe_leaves_heartbeats_their_room() {
  const int f0 = g_failures;
  Run r = run(8, 180);                         /* 160 frames/s = 12.7 % asked */
  EXPECT(r.st.sends_denied_airtime > 0);
  /* The probe starts no frame at 160 x100, and one frame is 792 us, under
   * 1 x100, so its frames stop at 160; a heartbeat (672 us) landing after
   * the last frame makes 161. Absolute, not relative to the constant. */
  EXPECT(r.max_x100 <= 161);
  EXPECT(r.hb_tries == 6 && r.hb_ok == r.hb_tries);
  EXPECT(r.max_under_us == 0);
  std::printf("%s 8 peers: held at %u x100, heartbeats %u/%u\n", VERDICT(f0), r.max_x100, r.hb_ok, r.hb_tries);
}

void test_probe_pump_installs_this_gate() {
  const int f0 = g_failures;
  bool ok = false;
  const std::string code = beacon_source_scan::strip_comments(
      beacon_source_scan::read_source(CSI_INTEGRATION_CPP, &ok));
  EXPECT(ok);
  const std::string body = beacon_source_scan::function_body(code, "probe_pump");
  EXPECT(!body.empty());
  EXPECT(beacon_source_scan::count(body, "pc.airtime_gate = probe_airtime::reserve_probe_frame;") == 1);
  EXPECT(beacon_source_scan::before(body, "probe_airtime::ensure_governor();", "csi_probe::init(pc)"));
  EXPECT(beacon_source_scan::count(code, "try_reserve_routine") == 0);  /* one gate, in one place */
  std::printf("%s probe_pump installs probe_airtime::reserve_probe_frame\n", VERDICT(f0));
}

}  // namespace

int main() {
  test_ensure_governor_brings_up_a_mesh_less_window();   /* must run first */
  test_ceiling_is_one_point_six_zero_exactly();
  test_wap_today_is_in_budget();
  test_one_peer_at_full_rate_is_steady();
  test_over_budget_probe_leaves_heartbeats_their_room();
  test_probe_pump_installs_this_gate();
  if (g_failures) { std::fprintf(stderr, "FAIL  %d assertion(s) failed\n", g_failures); return 1; }
  std::printf("test_csi_probe_airtime: ALL PASSED\n");
  return 0;
}
