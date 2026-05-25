/*
 * SecuraCV Canary — CSI active probe — Implementation
 *
 * Strategy:
 *   • Per-peer next-send timestamp. process() walks the peer table and
 *     sends to any peer whose next-send time has elapsed, then schedules
 *     the peer's next send (now + 1000/rate_hz).
 *   • Round-robin in-order so the radio queues don't get hit with a burst.
 *   • If the peer list is empty and broadcast_when_no_peers is true, send
 *     a broadcast (FF:FF:FF:FF:FF:FF) at idle_rate_hz instead.
 *   • All state is module-static; allocation-free.
 *
 * Threading:
 *   process() and the peer-registry calls run on the main loop. ESP-NOW's
 *   tx-done callback runs in the LwIP task and only updates the
 *   *_failed atomic counters via the unicast/broadcast send paths' return
 *   codes — we do not register the tx callback here.
 *
 * Host build:
 *   When CSI_TEST_HOST_BUILD is defined, the ESP-NOW symbols are replaced
 *   by a single injectable function pointer (`test::set_send_hook`) and
 *   millis() is replaced by `test::set_now_ms`. The scheduler logic is
 *   identical in both builds.
 */

#include "csi_probe.h"

#include <string.h>

#ifdef CSI_TEST_HOST_BUILD
  /* Host build: no Arduino, no ESP-NOW. We provide minimal shims. */
  #include <stdio.h>
#else
  #include <Arduino.h>
  extern "C" {
    #include <esp_now.h>
    #include <esp_wifi.h>
    #include <esp_err.h>
  }
#endif

namespace csi_probe {

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

struct PeerEntry {
  uint8_t  mac[CSI_PROBE_MAC_LEN];
  uint32_t next_send_ms;
  bool     in_use;
};

static bool       s_initialized = false;
static bool       s_running = false;
static bool       s_paused  = false;
static Config     s_cfg = Config::defaults();
static PeerEntry  s_peers[CSI_PROBE_MAX_PEERS];
static uint32_t   s_seq = 0;
static uint32_t   s_next_broadcast_ms = 0;

/* Cumulative counters. */
static uint32_t s_unicasts_sent = 0;
static uint32_t s_unicasts_failed = 0;
static uint32_t s_broadcasts_sent = 0;
static uint32_t s_broadcasts_failed = 0;
static uint32_t s_ticks_skipped_rate = 0;
static uint32_t s_ticks_skipped_idle = 0;

/* Broadcast MAC. */
static const uint8_t BROADCAST_MAC[CSI_PROBE_MAC_LEN] =
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ──────────────────────────────────────────────────────────────────────────
 * CLOCK + SEND SHIMS  (host vs device)
 * ────────────────────────────────────────────────────────────────────────── */

#ifdef CSI_TEST_HOST_BUILD
static uint32_t s_test_now_ms = 0;
static test::SendHook s_test_send_hook = nullptr;
static test::PeerAddHook s_test_peer_add_hook = nullptr;
static inline uint32_t now_ms() { return s_test_now_ms; }
namespace test {
  uint32_t set_now_ms(uint32_t v) { uint32_t o = s_test_now_ms; s_test_now_ms = v; return o; }
  uint32_t get_now_ms() { return s_test_now_ms; }
  void set_send_hook(SendHook h) { s_test_send_hook = h; }
  void set_peer_add_hook(PeerAddHook h) { s_test_peer_add_hook = h; }
}
static inline bool send_raw(const uint8_t* mac, const uint8_t* payload, size_t len) {
  if (s_test_send_hook) return s_test_send_hook(mac, payload, len);
  return true;  /* Default to success when no hook is installed. */
}
#else
static inline uint32_t now_ms() { return millis(); }
static inline bool send_raw(const uint8_t* mac, const uint8_t* payload, size_t len) {
  return esp_now_send(mac, payload, len) == ESP_OK;
}
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * INTERNAL HELPERS
 * ────────────────────────────────────────────────────────────────────────── */

static inline bool mac_eq(const uint8_t a[CSI_PROBE_MAC_LEN],
                          const uint8_t b[CSI_PROBE_MAC_LEN]) {
  return memcmp(a, b, CSI_PROBE_MAC_LEN) == 0;
}

static inline uint16_t clamp_rate(uint16_t rate_hz) {
  if (rate_hz == 0) return 1;     /* avoid div-by-zero */
  if (rate_hz > 200) return 200;  /* hard cap; airtime sanity */
  return rate_hz;
}

static inline uint32_t period_ms_from_rate(uint16_t rate_hz) {
  /* +1 to avoid 0 for >1000Hz rates; clamp_rate keeps us <=200 */
  return 1000u / clamp_rate(rate_hz);
}

/* Effective per-peer rate is the configured ceiling capped by an even
 * share of the aggregate budget. With 1 peer = full ceiling, with 8 peers
 * the budget gets sliced 8 ways. Returns at least 1 Hz so a peer never
 * stops being serviced entirely. */
static inline uint16_t effective_per_peer_rate(size_t peer_count) {
  if (peer_count == 0) return clamp_rate(s_cfg.rate_hz);
  uint32_t share = s_cfg.aggregate_cap_hz / peer_count;
  if (share == 0) share = 1;
  uint32_t r = (share < s_cfg.rate_hz) ? share : s_cfg.rate_hz;
  return clamp_rate((uint16_t)r);
}

/* Build the probe packet. Caller supplies the buffer (sized to
 * s_cfg.payload_len). The bytes after sizeof(csi_probe_pkt_t) are zero
 * padding so the over-the-air length matches the requested payload_len.
 * The seq increments per call so two peers in the same tick get distinct
 * sequence numbers. */
static void build_packet(uint8_t* buf, size_t len) {
  if (len < sizeof(csi_probe_pkt_t)) return;
  csi_probe_pkt_t pkt = {};
  pkt.magic[0] = 'C'; pkt.magic[1] = 'V'; pkt.magic[2] = 'P';
  pkt.version  = 1;
  pkt.seq      = ++s_seq;
  memcpy(buf, &pkt, sizeof(pkt));
  if (len > sizeof(pkt)) {
    memset(buf + sizeof(pkt), 0, len - sizeof(pkt));
  }
}

/* Register a peer with the ESP-NOW driver. On device build only — the
 * host build's send_raw() takes any MAC. Returns true on success or if
 * the peer is already registered (ESP_ERR_ESPNOW_EXIST). */
static bool driver_add_peer(const uint8_t mac[CSI_PROBE_MAC_LEN]) {
#ifdef CSI_TEST_HOST_BUILD
  if (s_test_peer_add_hook) return s_test_peer_add_hook(mac);
  return true;
#else
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t pi = {};
  memcpy(pi.peer_addr, mac, CSI_PROBE_MAC_LEN);
  pi.channel = 0;          /* follow current WiFi channel */
  pi.ifidx   = WIFI_IF_STA; /* ESP-NOW runs over STA in canary/canary-wap */
  pi.encrypt = false;       /* Opera ChaCha20-Poly1305 is layered above */
  esp_err_t err = esp_now_add_peer(&pi);
  return err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST;
#endif
}

static bool driver_del_peer(const uint8_t mac[CSI_PROBE_MAC_LEN]) {
#ifdef CSI_TEST_HOST_BUILD
  (void)mac;
  return true;
#else
  return esp_now_del_peer(mac) == ESP_OK;
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * LIFECYCLE
 * ────────────────────────────────────────────────────────────────────────── */

bool init(const Config& cfg) {
  if (s_initialized) return true;

  s_cfg = cfg;
  s_cfg.rate_hz = clamp_rate(s_cfg.rate_hz);
  if (s_cfg.aggregate_cap_hz == 0) s_cfg.aggregate_cap_hz = 200;
  if (s_cfg.idle_rate_hz == 0) s_cfg.idle_rate_hz = 1;
  if (s_cfg.payload_len < sizeof(csi_probe_pkt_t)) {
    s_cfg.payload_len = sizeof(csi_probe_pkt_t);
  }
  if (s_cfg.payload_len > CSI_PROBE_PAYLOAD_MAX) {
    s_cfg.payload_len = CSI_PROBE_PAYLOAD_MAX;
  }

  memset(s_peers, 0, sizeof(s_peers));
  s_seq = 0;
  s_next_broadcast_ms = 0;
  s_unicasts_sent = s_unicasts_failed = 0;
  s_broadcasts_sent = s_broadcasts_failed = 0;
  s_ticks_skipped_rate = s_ticks_skipped_idle = 0;

#ifndef CSI_TEST_HOST_BUILD
  /* esp_now_init() is safe to call multiple times — if another component
   * has already initialized it, we get ESP_ERR_ESPNOW_INTERNAL or
   * ESP_OK depending on IDF version. Treat both as success. */
  esp_err_t err = esp_now_init();
  if (err != ESP_OK && err != ESP_ERR_ESPNOW_INTERNAL) {
    /* Real failure (WiFi not started, etc.). Caller can retry later. */
    return false;
  }
  /* Register the broadcast peer once so broadcast sends work without a
   * later add_peer(FF:FF:FF:FF:FF:FF) gymnastic. Errors are non-fatal —
   * the send will just fail. */
  driver_add_peer(BROADCAST_MAC);
#endif

  s_initialized = true;
  return true;
}

void deinit() {
  if (!s_initialized) return;
  stop();
  for (size_t i = 0; i < CSI_PROBE_MAX_PEERS; ++i) {
    if (s_peers[i].in_use) {
      driver_del_peer(s_peers[i].mac);
    }
  }
  memset(s_peers, 0, sizeof(s_peers));
#ifndef CSI_TEST_HOST_BUILD
  driver_del_peer(BROADCAST_MAC);
  /* We intentionally do NOT call esp_now_deinit(): another component
   * (e.g. the mesh in PR 2) may also be using ESP-NOW. The peer list
   * cleanup above is sufficient to stop our traffic. */
#endif
  s_initialized = false;
}

bool start() {
  if (!s_initialized) return false;
  s_running = true;
  /* Stagger the initial send time for each peer across one period so a
   * batch of pre-registered peers (e.g. restored from NVS by PR 2 mesh)
   * doesn't all transmit on the same first tick. j=0 fires immediately;
   * subsequent peers fire at evenly-spaced phases. The steady state is
   * preserved by the per-peer next_send_ms = now + period scheduling in
   * process(). */
  const uint32_t t = now_ms();
  const size_t   peers = peer_count();
  const uint32_t period =
      period_ms_from_rate(effective_per_peer_rate(peers));
  size_t j = 0;
  for (size_t i = 0; i < CSI_PROBE_MAX_PEERS; ++i) {
    if (!s_peers[i].in_use) continue;
    const uint32_t phase = peers > 0 ? ((uint32_t)j * period) / (uint32_t)peers : 0;
    s_peers[i].next_send_ms = t + phase;
    ++j;
  }
  s_next_broadcast_ms = t;
  return true;
}

void stop() {
  s_running = false;
}

bool is_running() { return s_running; }

void set_paused(bool paused) { s_paused = paused; }
bool is_paused() { return s_paused; }

/* ──────────────────────────────────────────────────────────────────────────
 * PEER REGISTRY
 * ────────────────────────────────────────────────────────────────────────── */

bool has_peer(const uint8_t mac[CSI_PROBE_MAC_LEN]) {
  for (size_t i = 0; i < CSI_PROBE_MAX_PEERS; ++i) {
    if (s_peers[i].in_use && mac_eq(s_peers[i].mac, mac)) return true;
  }
  return false;
}

bool add_peer(const uint8_t mac[CSI_PROBE_MAC_LEN]) {
  if (!s_initialized) return false;
  if (has_peer(mac)) return false;  /* already present */
  /* Reject broadcast MAC — it's the idle fallback, not a peer. */
  if (mac_eq(mac, BROADCAST_MAC)) return false;
  for (size_t i = 0; i < CSI_PROBE_MAX_PEERS; ++i) {
    if (s_peers[i].in_use) continue;
    /* Register with the ESP-NOW driver FIRST so a driver-side failure
     * (table full, OOM) doesn't leave an orphan slot that the scheduler
     * keeps trying to send to. Only commit our slot on success. */
    if (!driver_add_peer(mac)) {
      return false;
    }
    memcpy(s_peers[i].mac, mac, CSI_PROBE_MAC_LEN);
    s_peers[i].next_send_ms = now_ms();  /* eligible immediately */
    s_peers[i].in_use = true;
    return true;
  }
  return false;  /* table full */
}

bool remove_peer(const uint8_t mac[CSI_PROBE_MAC_LEN]) {
  for (size_t i = 0; i < CSI_PROBE_MAX_PEERS; ++i) {
    if (s_peers[i].in_use && mac_eq(s_peers[i].mac, mac)) {
      driver_del_peer(s_peers[i].mac);
      memset(&s_peers[i], 0, sizeof(s_peers[i]));
      return true;
    }
  }
  return false;
}

void clear_peers() {
  for (size_t i = 0; i < CSI_PROBE_MAX_PEERS; ++i) {
    if (s_peers[i].in_use) {
      driver_del_peer(s_peers[i].mac);
      memset(&s_peers[i], 0, sizeof(s_peers[i]));
    }
  }
}

size_t peer_count() {
  size_t n = 0;
  for (size_t i = 0; i < CSI_PROBE_MAX_PEERS; ++i) {
    if (s_peers[i].in_use) ++n;
  }
  return n;
}

/* ──────────────────────────────────────────────────────────────────────────
 * MAIN LOOP
 * ────────────────────────────────────────────────────────────────────────── */

void process() {
  if (!s_running || s_paused) return;

  const uint32_t t = now_ms();
  /* Compute the effective per-peer rate from the aggregate budget every
   * tick — the peer count can change at runtime (mesh pair / unpair). */
  const size_t   peers = peer_count();
  const uint16_t eff_rate = effective_per_peer_rate(peers);
  const uint32_t period = period_ms_from_rate(eff_rate);

  uint8_t payload[CSI_PROBE_PAYLOAD_MAX];
  const size_t payload_len = s_cfg.payload_len;

  bool any_eligible_peer = false;
  bool any_sent_this_tick = false;

  /* Unicast pass: send to every peer whose next-send time has elapsed.
   * For each, rebuild the packet so the seq is per-send (helps the
   * receiver disambiguate duplicates across links if that ever matters). */
  for (size_t i = 0; i < CSI_PROBE_MAX_PEERS; ++i) {
    if (!s_peers[i].in_use) continue;
    any_eligible_peer = true;
    /* Use signed comparison so wrap-around doesn't starve a peer. */
    if ((int32_t)(t - s_peers[i].next_send_ms) < 0) continue;

    build_packet(payload, payload_len);
    if (send_raw(s_peers[i].mac, payload, payload_len)) {
      ++s_unicasts_sent;
      any_sent_this_tick = true;
    } else {
      ++s_unicasts_failed;
    }
    s_peers[i].next_send_ms = t + period;
  }

  if (any_eligible_peer) {
    if (!any_sent_this_tick) ++s_ticks_skipped_rate;
    return;
  }

  /* Idle path: no peers. Either broadcast at idle_rate_hz, or skip. */
  if (!s_cfg.broadcast_when_no_peers) {
    ++s_ticks_skipped_idle;
    return;
  }

  if ((int32_t)(t - s_next_broadcast_ms) < 0) {
    ++s_ticks_skipped_rate;
    return;
  }
  build_packet(payload, payload_len);
  if (send_raw(BROADCAST_MAC, payload, payload_len)) {
    ++s_broadcasts_sent;
  } else {
    ++s_broadcasts_failed;
  }
  s_next_broadcast_ms = t + period_ms_from_rate(s_cfg.idle_rate_hz);
}

bool get_stats(Stats* out) {
  if (!out) return false;
  out->unicasts_sent      = s_unicasts_sent;
  out->unicasts_failed    = s_unicasts_failed;
  out->broadcasts_sent    = s_broadcasts_sent;
  out->broadcasts_failed  = s_broadcasts_failed;
  out->ticks_skipped_rate = s_ticks_skipped_rate;
  out->ticks_skipped_idle = s_ticks_skipped_idle;
  out->peers_registered   = (uint32_t)peer_count();
  return true;
}

}  /* namespace csi_probe */
