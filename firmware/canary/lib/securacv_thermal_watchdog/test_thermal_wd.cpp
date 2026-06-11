/**
 * @file test_thermal_wd.cpp
 * @brief Host-build conformance test for the thermal watchdog.
 *
 * Verifies the properties the module exists to guarantee:
 *   1. Hysteresis ladder walks normal -> throttled (>=70) -> paused (>=80)
 *      and recovers at 65 / 75 — one clean step each way.
 *   2. No oscillation: temperature bouncing across an entry threshold
 *      inside the hysteresis band produces exactly one transition.
 *   3. Critical (>=85 °C) advisory fires once per episode, persists to
 *      NVS immediately, clears below 82 °C, and re-arms.
 *   4. Sensor-failure episode: 4 consecutive misses -> one ERROR +
 *      sensor_ok=false; recovery -> one INFO + sensor_ok=true.
 *   5. Cold advisory (<5 °C) fires once, clears at >=7 °C.
 *   6. Time accounting carries raw milliseconds (no truncation drift)
 *      and attributes minutes to the correct shadow state.
 *   7. Env-limited advisory: >=4 throttle entries inside 30 min sets it;
 *      it ages out once entries leave the window.
 *   8. History survives a reboot via NVS round-trip.
 *   9. NVS wear: steady dirty state writes at most once per 10 min.
 *
 * Build (mirrors the CI host-test job):
 *   g++ -std=c++17 -DCSI_TEST_HOST_BUILD -Wall -Wextra -Wno-unused-parameter \
 *       firmware/canary/lib/securacv_thermal_watchdog/test_thermal_wd.cpp \
 *       firmware/canary/lib/securacv_thermal_watchdog/src/securacv_thermal_watchdog.cpp \
 *       -I firmware/canary/lib/securacv_thermal_watchdog/src \
 *       -o /tmp/test_thermal_wd && /tmp/test_thermal_wd
 */

#ifndef CSI_TEST_HOST_BUILD
extern "C" int test_thermal_wd_run() { return 0; }
#else

#include "thermal_wd_host_stubs.h"
#include "securacv_thermal_watchdog.h"

#include <cassert>
#include <cstdio>

/* Host-only reset hook defined in securacv_thermal_watchdog.cpp. */
extern "C" void thermal_wd_test_reset(void);

/* ── Stub backing state (declared in thermal_wd_host_stubs.h) ─────────── */

static uint32_t g_now_ms = 0;
uint32_t millis() { return g_now_ms; }

static float g_die_temp_c = 40.0f;
static bool  g_sensor_ok  = true;
extern "C" bool thermal_read_die_c(float* out_c) {
  if (!g_sensor_ok) return false;
  *out_c = g_die_temp_c;
  return true;
}

std::vector<HostLogEntry> g_host_health_log;
void log_health(LogLevel level, LogCategory /*category*/,
                const char* message, const char* detail) {
  g_host_health_log.push_back({level, message, detail ? detail : ""});
}

HostSerial Serial;
std::map<std::string, int32_t> g_host_nvs;
uint32_t g_host_nvs_write_count = 0;

/* ── Helpers ──────────────────────────────────────────────────────────── */

namespace {

constexpr uint32_t STEP_MS = 30000;  /* watchdog sample cadence */

/* Fresh boot: module state cleared, clock at t0, logs/NVS as requested. */
void boot(bool wipe_nvs = true) {
  thermal_wd_test_reset();
  g_host_health_log.clear();
  if (wipe_nvs) g_host_nvs.clear();
  g_host_nvs_write_count = 0;
  g_now_ms = 1000;   /* nonzero so millis()==0 edge cases don't mask bugs */
  g_die_temp_c = 40.0f;
  g_sensor_ok = true;
  assert(thermal_wd_init());
}

/* Advance one sample period and feed a reading. */
void step(float temp_c, bool sensor_ok = true) {
  g_die_temp_c = temp_c;
  g_sensor_ok = sensor_ok;
  g_now_ms += STEP_MS;
  thermal_wd_process();
}

thermal_wd_state_t st() {
  thermal_wd_state_t s;
  assert(thermal_wd_get_state(&s));
  return s;
}

thermal_wd_history_t hist() {
  thermal_wd_history_t h;
  assert(thermal_wd_get_history(&h));
  return h;
}

size_t log_count(LogLevel level) {
  size_t n = 0;
  for (const auto& e : g_host_health_log)
    if (e.level == level) n++;
  return n;
}

/* ── Tests ────────────────────────────────────────────────────────────── */

void test_hysteresis_walk() {
  boot();
  thermal_wd_process();              /* first sample at 40 °C */
  assert(st().shadow_state == 0);

  step(71.0f);                       /* >=70: enter throttled */
  assert(st().shadow_state == 1);
  step(67.0f);                       /* inside band: hold */
  assert(st().shadow_state == 1);
  step(64.0f);                       /* <65: recover */
  assert(st().shadow_state == 0);

  step(81.0f);                       /* normal -> paused directly */
  assert(st().shadow_state == 2);
  step(76.0f);                       /* >=75: hold paused */
  assert(st().shadow_state == 2);
  step(74.0f);                       /* <75: step down to throttled */
  assert(st().shadow_state == 1);
  step(69.0f);                       /* not <65: hold */
  assert(st().shadow_state == 1);
  step(64.0f);
  assert(st().shadow_state == 0);

  thermal_wd_history_t h = hist();
  assert(h.throttle_events == 2);    /* two departures from normal */
  assert(h.pause_events == 1);
  assert(h.alltime_max_c == 81);
  assert(h.alltime_min_c == 40);
  std::printf("PASS test_hysteresis_walk\n");
}

void test_no_oscillation() {
  boot();
  thermal_wd_process();
  for (int i = 0; i < 20; i++) {
    step(71.0f);
    step(69.0f);                     /* inside hysteresis band: must hold */
  }
  assert(st().shadow_state == 1);
  assert(hist().throttle_events == 1);
  std::printf("PASS test_no_oscillation\n");
}

void test_critical_episode_once() {
  boot();
  thermal_wd_process();
  for (int i = 0; i < 5; i++) step(86.0f);
  assert(log_count(LOG_LEVEL_CRITICAL) == 1);   /* once per episode */
  assert(hist().critical_events == 1);
  assert(st().advisories & (1u << 1));          /* THERMAL_ADV_CRITICAL */
  /* Critical persists immediately, not on the 10-min cadence. */
  assert(g_host_nvs.count("th_crit_cnt") == 1);
  assert(g_host_nvs["th_crit_cnt"] == 1);

  step(81.0f);                                  /* <82: episode clears */
  assert(!(st().advisories & (1u << 1)));
  for (int i = 0; i < 3; i++) step(86.0f);      /* re-arm: new episode */
  assert(log_count(LOG_LEVEL_CRITICAL) == 2);
  assert(hist().critical_events == 2);
  std::printf("PASS test_critical_episode_once\n");
}

void test_sensor_fail_episode() {
  boot();
  thermal_wd_process();
  step(0, false); step(0, false); step(0, false);
  assert(st().sensor_ok);                       /* threshold is 4 */
  assert(log_count(LOG_LEVEL_ERROR) == 0);

  step(0, false);                               /* 4th miss: episode */
  assert(!st().sensor_ok);
  assert(st().advisories & (1u << 0));          /* THERMAL_ADV_SENSOR_FAULT */
  assert(log_count(LOG_LEVEL_ERROR) == 1);
  assert(hist().sensor_fail_events == 1);

  step(0, false); step(0, false);               /* still down: no spam */
  assert(log_count(LOG_LEVEL_ERROR) == 1);

  size_t infos_before = log_count(LOG_LEVEL_INFO);
  step(42.0f);                                  /* recovery */
  assert(st().sensor_ok);
  assert(!(st().advisories & (1u << 0)));
  assert(log_count(LOG_LEVEL_INFO) == infos_before + 1);
  std::printf("PASS test_sensor_fail_episode\n");
}

void test_cold_advisory() {
  boot();
  thermal_wd_process();
  step(4.0f);                                   /* <5: cold episode */
  assert(st().advisories & (1u << 4));          /* THERMAL_ADV_COLD */
  assert(log_count(LOG_LEVEL_WARNING) == 1);
  assert(hist().cold_events == 1);
  step(6.0f);                                   /* <7: hold (hysteresis) */
  assert(st().advisories & (1u << 4));
  step(8.0f);                                   /* >=7: clears */
  assert(!(st().advisories & (1u << 4)));
  assert(log_count(LOG_LEVEL_WARNING) == 1);    /* no extra warning */
  std::printf("PASS test_cold_advisory\n");
}

void test_time_accounting() {
  boot();
  thermal_wd_process();                /* elapsed 0 on first sample */
  for (int i = 0; i < 20; i++) step(40.0f);   /* 20 * 30 s = 600 s normal */
  thermal_wd_history_t h = hist();
  assert(h.total_runtime_min == 10);
  assert(h.throttled_min == 0 && h.paused_min == 0);

  /* 10 throttled samples. The 30 s leading into the transition belongs
   * to the previous (normal) state; the 9 after it are throttled. */
  for (int i = 0; i < 10; i++) step(72.0f);
  h = hist();
  assert(h.total_runtime_min == 15);   /* 900 s total */
  assert(h.throttled_min == 4);        /* 270 s carried, 30 s in accumulator */
  std::printf("PASS test_time_accounting\n");
}

void test_env_limited_window() {
  boot();
  thermal_wd_process();
  /* Each cycle: one sample at 71 (enter throttled), one at 60 (recover).
   * 4 entries land within 4 minutes — well inside the 30 min window. */
  for (int i = 0; i < 4; i++) { step(71.0f); step(60.0f); }
  assert(st().advisories & (1u << 3));          /* THERMAL_ADV_ENV_LIMITED */
  assert(hist().throttle_events == 4);

  /* Stay cool for >30 min: entries age out and the advisory clears. */
  for (int i = 0; i < 62; i++) step(40.0f);
  assert(!(st().advisories & (1u << 3)));
  std::printf("PASS test_env_limited_window\n");
}

void test_nvs_roundtrip() {
  boot();
  thermal_wd_process();
  step(71.0f); step(81.0f); step(60.0f);
  thermal_wd_persist();                          /* deliberate-reboot path */

  thermal_wd_test_reset();                       /* "reboot", keep NVS */
  g_host_health_log.clear();
  assert(thermal_wd_init());
  thermal_wd_history_t h = hist();
  assert(h.throttle_events == 1);
  assert(h.pause_events == 1);
  assert(h.alltime_max_c == 81);
  assert(h.alltime_min_c == 40);
  std::printf("PASS test_nvs_roundtrip\n");
}

void test_nvs_wear_cadence() {
  boot();
  thermal_wd_process();
  /* One hour at a steadily-warm 72 °C: the minute counters go dirty
   * every minute, but writes may land at most once per 10 min cadence
   * (11 keys per save). */
  for (int i = 0; i < 120; i++) step(72.0f);
  uint32_t saves = g_host_nvs_write_count / 11;
  assert(saves >= 1);
  assert(saves <= 7);
  std::printf("PASS test_nvs_wear_cadence (saves=%u in 60 min)\n", saves);
}

}  /* namespace */

int main() {
  test_hysteresis_walk();
  test_no_oscillation();
  test_critical_episode_once();
  test_sensor_fail_episode();
  test_cold_advisory();
  test_time_accounting();
  test_env_limited_window();
  test_nvs_roundtrip();
  test_nvs_wear_cadence();
  std::printf("\nALL THERMAL_WD TESTS PASSED\n");
  return 0;
}

#endif /* CSI_TEST_HOST_BUILD */
