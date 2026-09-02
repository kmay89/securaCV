// Host test for csi_traffic — the router-echo CSI frame supply's start/stop
// policy, driven through the CSI_TEST_HOST_BUILD hooks.
//
// Covered:
//   1. Nothing happens while disabled, or while enabled with the link down.
//   2. Enabled + link up → ONE session after start_delay, at the clamped
//      rate, against the gateway that was reported.
//   3. Link down → the session stops; link back up → a fresh session after
//      the delay again.
//   4. Gateway change while pinging → stop + start against the new address.
//   5. A driver failure to start is retried no sooner than retry_ms.
//   6. Disable while pinging → stop; re-enable → start on the next tick (the
//      link has been up the whole time and nothing failed, so no back-off).
//   7. Reply/timeout counters flow through get_stats.
//   8. deinit() while pinging stops the session.
//
// Build/run: make (this dir).

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "csi_traffic.h"

namespace {

struct Start { uint16_t rate_hz; uint16_t payload; uint32_t gw; uint32_t at_ms; };
std::vector<Start> g_starts;
std::vector<uint32_t> g_stops;
bool g_start_ok = true;
uint32_t g_now = 0;

bool start_hook(uint16_t rate_hz, uint16_t payload, uint32_t gw) {
  g_starts.push_back({rate_hz, payload, gw, g_now});
  return g_start_ok;
}
void stop_hook() { g_stops.push_back(g_now); }

void tick(uint32_t step_ms, uint32_t total_ms) {
  for (uint32_t t = 0; t < total_ms; t += step_ms) {
    g_now += step_ms;
    csi_traffic::test::set_now_ms(g_now);
    csi_traffic::process();
  }
}

void fresh() {
  csi_traffic::deinit();
  g_starts.clear(); g_stops.clear(); g_start_ok = true;
  g_now = 1000; csi_traffic::test::set_now_ms(g_now);
  csi_traffic::TrafficConfig c = csi_traffic::TrafficConfig::defaults();
  c.rate_hz = 20; c.start_delay_ms = 3000; c.retry_ms = 5000;
  assert(csi_traffic::init(c));
  csi_traffic::test::set_start_hook(start_hook);
  csi_traffic::test::set_stop_hook(stop_hook);
}

void test_idle_when_disabled_or_link_down() {
  fresh();
  csi_traffic::set_link(true, 0xC0A80101u);
  tick(100, 10000);                        // enabled? no
  assert(g_starts.empty());
  csi_traffic::set_enabled(true);
  csi_traffic::set_link(false, 0);
  tick(100, 10000);
  assert(g_starts.empty() && !csi_traffic::is_pinging());
  // Link up but no gateway → still idle.
  csi_traffic::set_link(true, 0);
  tick(100, 10000);
  assert(g_starts.empty());
  printf("  idle while disabled / link down / no gateway: ok\n");
}

void test_starts_once_after_delay() {
  fresh();
  csi_traffic::set_enabled(true);
  csi_traffic::set_link(true, 0xC0A80101u);
  tick(100, 2900);
  assert(g_starts.empty());                // still inside start_delay
  tick(100, 200);
  assert(g_starts.size() == 1);
  assert(g_starts[0].rate_hz == 20 && g_starts[0].payload == 1 && g_starts[0].gw == 0xC0A80101u);
  assert(csi_traffic::is_pinging());
  tick(100, 30000);
  assert(g_starts.size() == 1);            // one session, not one per tick
  printf("  one session after start_delay: ok\n");
}

void test_link_down_stops_and_up_restarts() {
  fresh();
  csi_traffic::set_enabled(true);
  csi_traffic::set_link(true, 0xC0A80101u);
  tick(100, 3200);
  assert(csi_traffic::is_pinging());
  csi_traffic::set_link(false, 0);
  tick(100, 100);
  assert(!csi_traffic::is_pinging() && g_stops.size() == 1);
  csi_traffic::set_link(true, 0xC0A80101u);
  tick(100, 2900);
  assert(g_starts.size() == 1);            // delay applies again
  tick(100, 200);
  assert(g_starts.size() == 2 && csi_traffic::is_pinging());
  printf("  link down → stop, link up → restart after delay: ok\n");
}

void test_gateway_change_restarts() {
  fresh();
  csi_traffic::set_enabled(true);
  csi_traffic::set_link(true, 0xC0A80101u);
  tick(100, 3200);
  assert(g_starts.size() == 1);
  csi_traffic::set_link(true, 0x0A000001u);
  tick(100, 100);
  assert(g_stops.size() == 1);
  // Link never dropped and nothing failed: the restart is immediate.
  tick(100, 100);
  assert(g_starts.size() == 2 && g_starts[1].gw == 0x0A000001u);
  printf("  gateway change → restart at the new address: ok\n");
}

void test_start_failure_retries_after_retry_ms() {
  fresh();
  g_start_ok = false;
  csi_traffic::set_enabled(true);
  csi_traffic::set_link(true, 0xC0A80101u);
  tick(100, 3200);
  assert(g_starts.size() == 1 && !csi_traffic::is_pinging());
  tick(100, 4700);
  assert(g_starts.size() == 1);            // < retry_ms since the failure
  tick(100, 400);
  assert(g_starts.size() == 2);
  csi_traffic::Stats s; csi_traffic::get_stats(&s);
  assert(s.start_failures == 2 && s.sessions_started == 0);
  g_start_ok = true;
  tick(100, 5100);
  assert(csi_traffic::is_pinging());
  csi_traffic::get_stats(&s);
  assert(s.sessions_started == 1);
  printf("  start failure → retry after retry_ms: ok\n");
}

void test_disable_stops_enable_resumes() {
  fresh();
  csi_traffic::set_enabled(true);
  csi_traffic::set_link(true, 0xC0A80101u);
  tick(100, 3200);
  assert(csi_traffic::is_pinging());
  csi_traffic::set_enabled(false);
  tick(100, 100);
  assert(!csi_traffic::is_pinging() && g_stops.size() == 1);
  csi_traffic::set_enabled(true);
  tick(100, 100);                          // nothing failed: no back-off
  assert(csi_traffic::is_pinging() && g_starts.size() == 2);
  printf("  disable → stop, enable → resume: ok\n");
}

void test_stats_counters() {
  fresh();
  csi_traffic::test::feed_reply(); csi_traffic::test::feed_reply();
  csi_traffic::test::feed_timeout();
  csi_traffic::Stats s; csi_traffic::get_stats(&s);
  assert(s.replies >= 2 && s.timeouts >= 1);
  printf("  reply/timeout counters: ok\n");
}

void test_deinit_stops() {
  fresh();
  csi_traffic::set_enabled(true);
  csi_traffic::set_link(true, 0xC0A80101u);
  tick(100, 3200);
  assert(csi_traffic::is_pinging());
  csi_traffic::deinit();
  assert(!csi_traffic::is_pinging() && g_stops.size() == 1);
  // A deinit'd module ignores process().
  tick(100, 10000);
  assert(g_starts.size() == 1);
  printf("  deinit while pinging → stop: ok\n");
}

}  // namespace

int main() {
  printf("test_csi_traffic\n");
  test_idle_when_disabled_or_link_down();
  test_starts_once_after_delay();
  test_link_down_stops_and_up_restarts();
  test_gateway_change_restarts();
  test_start_failure_retries_after_retry_ms();
  test_disable_stops_enable_resumes();
  test_stats_counters();
  test_deinit_stops();
  printf("test_csi_traffic: ALL PASSED\n");
  return 0;
}
