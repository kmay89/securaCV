/*
 * SecuraCV Canary — CSI frame supply: router echo traffic source — impl
 *
 * Policy (host-tested):
 *   desired  = enabled && link_up && gateway != 0
 *   start    when desired && !pinging && (now - link_up_since) >= start_delay
 *            && (no failed start within the last retry_ms)
 *   stop     when pinging && (!desired || gateway changed)
 *
 * Device side: esp_ping (lwIP apps/ping), the same session API
 * espressif/esp-csi uses — count 0 (infinite), interval 1000/rate_hz ms,
 * data_size payload_bytes, target = the STA netif's gateway.
 */

#include "csi_traffic.h"

#include <string.h>
#include <atomic>

#ifdef CSI_TEST_HOST_BUILD
  #include <stdio.h>
#else
  #include <Arduino.h>
  extern "C" {
    #include <esp_err.h>
    #include <esp_netif.h>
    #include "lwip/ip_addr.h"
    #include "ping/ping_sock.h"
  }
#endif

namespace csi_traffic {

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool     s_initialized = false;
static TrafficConfig s_cfg = TrafficConfig::defaults();
static bool     s_enabled = false;
static bool     s_link_up = false;
static uint32_t s_gateway = 0;
static uint32_t s_link_up_since_ms = 0;
static bool     s_pinging = false;
static uint32_t s_pinging_gateway = 0;
static uint32_t s_last_failure_ms = 0;   /* retry_ms gates only after a FAILED start */

static uint32_t s_sessions_started = 0;
static uint32_t s_sessions_stopped = 0;
static uint32_t s_start_failures = 0;
/* Written from the ping task (device) — atomic. */
static std::atomic<uint32_t> s_replies{0};
static std::atomic<uint32_t> s_timeouts{0};

static inline uint16_t clamp_rate(uint16_t hz) {
  if (hz == 0) return 1;
  if (hz > 100) return 100;
  return hz;
}

/* ──────────────────────────────────────────────────────────────────────────
 * CLOCK + SESSION SHIMS  (host vs device)
 * ────────────────────────────────────────────────────────────────────────── */

#ifdef CSI_TEST_HOST_BUILD

static uint32_t         s_test_now_ms = 0;
static test::StartHook  s_test_start_hook = nullptr;
static test::StopHook   s_test_stop_hook = nullptr;

static inline uint32_t now_ms() { return s_test_now_ms; }

static bool session_start(uint32_t gateway) {
  if (s_test_start_hook) return s_test_start_hook(clamp_rate(s_cfg.rate_hz), s_cfg.payload_bytes, gateway);
  return true;
}
static void session_stop() { if (s_test_stop_hook) s_test_stop_hook(); }
static void poll_link() {}   /* host: set_link() is the only source */

namespace test {
  uint32_t set_now_ms(uint32_t v) { const uint32_t o = s_test_now_ms; s_test_now_ms = v; return o; }
  void set_start_hook(StartHook h) { s_test_start_hook = h; }
  void set_stop_hook(StopHook h)   { s_test_stop_hook = h; }
  void feed_reply()   { s_replies.fetch_add(1, std::memory_order_relaxed); }
  void feed_timeout() { s_timeouts.fetch_add(1, std::memory_order_relaxed); }
}

#else  /* device */

static esp_ping_handle_t s_ping = nullptr;
static uint32_t s_last_link_poll_ms = 0;

static inline uint32_t now_ms() { return millis(); }

static void on_ping_success(esp_ping_handle_t, void*) {
  s_replies.fetch_add(1, std::memory_order_relaxed);
}
static void on_ping_timeout(esp_ping_handle_t, void*) {
  s_timeouts.fetch_add(1, std::memory_order_relaxed);
}

static bool session_start(uint32_t gateway) {
  esp_ping_config_t pc;
  memset(&pc, 0, sizeof(pc));
  pc.count           = 0;                                  /* ESP_PING_COUNT_INFINITE */
  pc.interval_ms     = 1000u / clamp_rate(s_cfg.rate_hz);
  pc.timeout_ms      = 1000;
  pc.data_size       = s_cfg.payload_bytes;
  pc.ttl             = 64;
  pc.task_stack_size = 3072;
  pc.task_prio       = 2;
  pc.interface       = 0;                                  /* any */
  ip_addr_set_ip4_u32(&pc.target_addr, gateway);

  esp_ping_callbacks_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.on_ping_success = on_ping_success;
  cbs.on_ping_timeout = on_ping_timeout;

  esp_ping_handle_t h = nullptr;
  if (esp_ping_new_session(&pc, &cbs, &h) != ESP_OK || h == nullptr) return false;
  if (esp_ping_start(h) != ESP_OK) {
    esp_ping_delete_session(h);
    return false;
  }
  s_ping = h;
  return true;
}

static void session_stop() {
  if (s_ping == nullptr) return;
  esp_ping_stop(s_ping);
  esp_ping_delete_session(s_ping);
  s_ping = nullptr;
}

/* Read the STA netif's address + gateway once a second. A zero IP means no
 * link; the gateway is passed through as lwIP stores it. */
static void poll_link() {
  if (!s_cfg.auto_link) return;
  const uint32_t now = now_ms();
  if (s_last_link_poll_ms != 0 && (now - s_last_link_poll_ms) < 1000u) return;
  s_last_link_poll_ms = now;

  esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta == nullptr) { set_link(false, 0); return; }
  esp_netif_ip_info_t ip;
  memset(&ip, 0, sizeof(ip));
  if (esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0) { set_link(false, 0); return; }
  set_link(true, ip.gw.addr);
}

#endif

/* ──────────────────────────────────────────────────────────────────────────
 * API
 * ────────────────────────────────────────────────────────────────────────── */

bool init(const TrafficConfig& cfg) {
  /* A re-init after deinit() must not inherit the previous run's timers:
   * a stale last-attempt stamp would silently defer the first session by
   * retry_ms (the host test caught exactly that). */
  if (s_pinging) { session_stop(); s_pinging = false; s_sessions_stopped++; }
  s_enabled = false;
  s_link_up = false;
  s_gateway = 0;
  s_link_up_since_ms = 0;
  s_pinging_gateway = 0;
  s_last_failure_ms = 0;
  /* Stats are "since init", like csi_hal's. */
  s_sessions_started = 0;
  s_sessions_stopped = 0;
  s_start_failures = 0;
  s_replies.store(0, std::memory_order_relaxed);
  s_timeouts.store(0, std::memory_order_relaxed);
  s_cfg = cfg;
  s_cfg.rate_hz = clamp_rate(s_cfg.rate_hz);
  if (s_cfg.payload_bytes == 0) s_cfg.payload_bytes = 1;
  if (s_cfg.retry_ms == 0) s_cfg.retry_ms = 1000;
#ifdef CSI_TEST_HOST_BUILD
  s_cfg.auto_link = false;
#endif
  s_initialized = true;
  return true;
}

void deinit() {
  if (!s_initialized) return;
  if (s_pinging) { session_stop(); s_pinging = false; s_sessions_stopped++; }
  s_enabled = false;
  s_link_up = false;
  s_gateway = 0;
  s_initialized = false;
}

void set_enabled(bool enabled) { s_enabled = enabled; }

void set_link(bool sta_connected, uint32_t gateway_ipv4) {
  if (!sta_connected) gateway_ipv4 = 0;
  if (sta_connected && !s_link_up) s_link_up_since_ms = now_ms();
  s_link_up = sta_connected;
  s_gateway = gateway_ipv4;
}

void process() {
  if (!s_initialized) return;
  poll_link();

  const bool desired = s_enabled && s_link_up && s_gateway != 0;
  const uint32_t now = now_ms();

  if (s_pinging && (!desired || s_gateway != s_pinging_gateway)) {
    session_stop();
    s_pinging = false;
    s_sessions_stopped++;
  }
  if (s_pinging || !desired) return;

  if ((now - s_link_up_since_ms) < s_cfg.start_delay_ms) return;
  /* Back off only after a failed start. A deliberate stop (gate closed,
   * gateway moved) restarts on the next tick — nothing went wrong. */
  if (s_last_failure_ms != 0 && (now - s_last_failure_ms) < s_cfg.retry_ms) return;

  if (session_start(s_gateway)) {
    s_pinging = true;
    s_pinging_gateway = s_gateway;
    s_sessions_started++;
    s_last_failure_ms = 0;
  } else {
    s_start_failures++;
    s_last_failure_ms = now;
  }
}

bool is_pinging() { return s_pinging; }

void get_stats(Stats* out) {
  if (!out) return;
  out->sessions_started = s_sessions_started;
  out->sessions_stopped = s_sessions_stopped;
  out->start_failures   = s_start_failures;
  out->replies          = s_replies.load(std::memory_order_relaxed);
  out->timeouts         = s_timeouts.load(std::memory_order_relaxed);
}

}  /* namespace csi_traffic */
