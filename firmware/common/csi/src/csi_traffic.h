/*
 * SecuraCV Canary — CSI frame supply: router echo (ICMP) traffic source
 *
 * WHY
 * ---
 * The CSI receiver only measures frames it RECEIVES. A solo Canary joined to
 * a home network sees the access point's beacons (~10 Hz) and whatever
 * happens to be addressed to it — and a device cannot receive its own
 * ESP-NOW probes (csi_probe lights up OTHER Canaries, not this one). So a
 * single device sits permanently below the 20 frames/s the 1 s feature
 * window is designed for: every window is "degraded", the breathing ring
 * fills slowly, and the dashboard's supply chip says "starved".
 *
 * WHAT
 * ----
 * The same remedy espressif/esp-csi ships in every router-driven example
 * (csi_recv_router, esp-radar console_test, esp_wifi_sensing's
 * ping_router_start): ping the gateway. Each ICMP echo reply is a frame
 * addressed to this station, so the reply rate IS the CSI frame rate, and
 * the router's reply path is the same room-spanning link the sensing is
 * about. 20 Hz × ~70 B is ~1.4 kB/s — invisible on any Wi-Fi network.
 *
 * PRIVACY
 * -------
 * The payload is `payload_bytes` zero bytes (default 1). Nothing about the
 * room, the device, or the household is in the packet; the gateway learns
 * only that a station on its LAN pings it, which every OS does. No
 * identifiers are stored here: the gateway address is read from the
 * station netif at run time and never logged or exported. The module never
 * touches association or channel — it only speaks while the caller says
 * the link is up.
 *
 * COUPLING
 * --------
 * Standalone. On the device the STA link state and gateway are polled from
 * the esp_netif every second (auto_link); a host build receives them via
 * set_link() and drives the ping session through injectable hooks, so the
 * start/stop policy is unit-tested without hardware
 * (tests_host/test_csi_traffic.cpp).
 *
 * License: MIT (matches the library).
 */
#ifndef SECURACV_CSI_TRAFFIC_H
#define SECURACV_CSI_TRAFFIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace csi_traffic {

struct Config {
  /* Echo requests per second. Matches csi_hal's default max_frame_rate_hz
   * so the receiver's rate limiter never throws away what we paid airtime
   * for. Clamped to 1..100 (esp-csi's examples run 100 Hz; the HAL only
   * wants 20). */
  uint16_t rate_hz;
  /* ICMP payload bytes. 1 is the smallest lwIP accepts; the reply is what
   * carries the CSI, and its size does not change the L-LTF. */
  uint16_t payload_bytes;
  /* Wait this long after the link comes up before the first request, so
   * DHCP/ARP settle and the first replies are not lost to a cold cache. */
  uint32_t start_delay_ms;
  /* After a failed session start, wait this long before retrying. */
  uint32_t retry_ms;
  /* Device only: poll the STA netif for link state + gateway. Off in host
   * builds, where set_link() is the test's steering wheel. */
  bool     auto_link;

  static Config defaults() {
    Config c;
    c.rate_hz        = 20;
    c.payload_bytes  = 1;
    c.start_delay_ms = 3000;
    c.retry_ms       = 5000;
    c.auto_link      = true;
    return c;
  }
};

/* Lifecycle. init() stores the config and does no radio work. */
bool init(const Config& cfg);
void deinit();

/* Policy gate from the integration layer: false stops the session on the
 * next process() (power gate, CSI stopped, quiet hours). */
void set_enabled(bool enabled);

/* Link state. `gateway_ipv4` is the gateway as lwIP stores it (the raw
 * u32 from ip4_addr_get_u32 / Arduino's (uint32_t)WiFi.gatewayIP()); 0
 * means "no gateway", which also stops the session. A gateway CHANGE while
 * pinging restarts the session at the new address. */
void set_link(bool sta_connected, uint32_t gateway_ipv4);

/* Main-loop pump. Starts/stops the echo session so that it runs exactly
 * when enabled && link up && gateway known && start_delay elapsed. */
void process();

bool is_pinging();

struct Stats {
  uint32_t sessions_started;
  uint32_t sessions_stopped;
  uint32_t start_failures;
  uint32_t replies;     /* echo replies seen (≈ CSI frames we caused) */
  uint32_t timeouts;    /* requests with no reply */
};
void get_stats(Stats* out);

#ifdef CSI_TEST_HOST_BUILD
namespace test {
  /* Virtual clock. */
  uint32_t set_now_ms(uint32_t now_ms);
  /* Session hooks. start returns false to simulate a driver failure. */
  using StartHook = bool (*)(uint16_t rate_hz, uint16_t payload_bytes, uint32_t gateway_ipv4);
  using StopHook  = void (*)();
  void set_start_hook(StartHook h);
  void set_stop_hook(StopHook h);
  /* Feed a reply / timeout as the ping task would. */
  void feed_reply();
  void feed_timeout();
}
#endif

}  /* namespace csi_traffic */

#endif /* SECURACV_CSI_TRAFFIC_H */
