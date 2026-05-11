/*
 * SecuraCV Canary — Mesh transport (ESP-NOW) — Implementation
 *
 * Strategy:
 *   • Bounded peer table (8 slots, matches MESH_MAX_PEERS).
 *   • Send: synchronous esp_now_send on the main loop; success/failure
 *     counted by return code. The tx-done callback is NOT registered
 *     here — failures are surfaced via the unicast/broadcast counters
 *     and the eventual peer-stale transition.
 *   • Receive: ESP-NOW recv callback (WiFi task) deposits into a small
 *     SPSC ring. The main loop's process() drains the ring, invokes
 *     the user-registered callback, and updates the peer's last_seen +
 *     RSSI. Frames from unknown senders are dropped (NOT registered as
 *     peers — pairing is a separate concern handled in PR 2b).
 *   • Aging: every process() tick walks the peer table and transitions
 *     ACTIVE → STALE (after PEER_STALE_AFTER_MS) → OFFLINE (after
 *     PEER_OFFLINE_AFTER_MS). The state-change callback fires on each
 *     transition (and only on transitions).
 *
 * Threading model:
 *   • esp_now_register_recv_cb installs csi_recv_cb (WiFi task context).
 *     The callback copies the frame into an SPSC ring and returns fast.
 *   • process() runs on the main loop, drains the ring synchronously,
 *     invokes user callbacks, and ages peers.
 *   • All public state mutations happen on the main loop side. The WiFi
 *     side only writes the ring + atomic counters.
 *
 * Host build:
 *   • CSI_TEST_HOST_BUILD stubs ESP-NOW + millis. test::inject_recv()
 *     drives the recv ring directly so tests cover the full path.
 */

#include "mesh_transport.h"

#include <string.h>
#include <atomic>

#ifdef CSI_TEST_HOST_BUILD
  #include <stdio.h>
#else
  #include <Arduino.h>
  extern "C" {
    #include <esp_now.h>
    #include <esp_wifi.h>
    #include <esp_err.h>
  }
#endif

namespace mesh_transport {

/* ──────────────────────────────────────────────────────────────────────────
 * STATE
 * ────────────────────────────────────────────────────────────────────────── */

static bool   s_initialized = false;
static bool   s_running = false;
static Config s_cfg = Config::defaults();

static Peer   s_peers[MESH_TRANSPORT_MAX_PEERS];

static RecvCallback      s_recv_cb = nullptr;
static PeerStateCallback s_peer_state_cb = nullptr;

/* Cumulative counters. */
static std::atomic<uint32_t> s_bytes_sent{0};
static std::atomic<uint32_t> s_bytes_received{0};
static std::atomic<uint32_t> s_unicasts_sent{0};
static std::atomic<uint32_t> s_unicasts_failed{0};
static std::atomic<uint32_t> s_broadcasts_sent{0};
static std::atomic<uint32_t> s_broadcasts_failed{0};
static std::atomic<uint32_t> s_recv_dropped_no_peer{0};
static std::atomic<uint32_t> s_recv_dropped_ring_full{0};

/* ──────────────────────────────────────────────────────────────────────────
 * RECV RING (single-producer WiFi task, single-consumer main loop)
 * ────────────────────────────────────────────────────────────────────────── */

struct RingSlot {
  uint8_t  mac[MESH_TRANSPORT_MAC_LEN];
  uint16_t len;
  int8_t   rssi_dbm;
  uint8_t  data[MESH_TRANSPORT_PAYLOAD_MAX];
};

/* 8 slots × ~210 bytes ≈ 1.7 KB. Enough to absorb a brief main-loop
 * stall at expected 20 Hz peer probes × 8 peers = 160 Hz aggregate Rx. */
static constexpr size_t RING_CAP = 8;
static RingSlot               s_ring[RING_CAP];
static std::atomic<uint32_t>  s_ring_head{0};   /* WiFi task writes */
static std::atomic<uint32_t>  s_ring_tail{0};   /* main loop reads  */

/* ──────────────────────────────────────────────────────────────────────────
 * CLOCK + DRIVER SHIMS (host vs device)
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

static inline bool driver_send(const uint8_t* mac, const uint8_t* data, size_t len) {
  if (s_test_send_hook) return s_test_send_hook(mac, data, len);
  return true;
}
static inline bool driver_add_peer(const uint8_t* mac) {
  if (s_test_peer_add_hook) return s_test_peer_add_hook(mac);
  return true;
}
static inline bool driver_del_peer(const uint8_t* /*mac*/) { return true; }
#else
static inline uint32_t now_ms() { return millis(); }

static inline bool driver_send(const uint8_t* mac, const uint8_t* data, size_t len) {
  return esp_now_send(mac, data, len) == ESP_OK;
}
static inline bool driver_add_peer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t pi = {};
  memcpy(pi.peer_addr, mac, MESH_TRANSPORT_MAC_LEN);
  pi.channel = 0;          /* follow current WiFi channel */
  pi.ifidx   = WIFI_IF_STA;
  pi.encrypt = false;
  esp_err_t err = esp_now_add_peer(&pi);
  return err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST;
}
static inline bool driver_del_peer(const uint8_t* mac) {
  return esp_now_del_peer(mac) == ESP_OK;
}
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * INTERNAL HELPERS
 * ────────────────────────────────────────────────────────────────────────── */

static inline bool mac_eq(const uint8_t a[MESH_TRANSPORT_MAC_LEN],
                          const uint8_t b[MESH_TRANSPORT_MAC_LEN]) {
  return memcmp(a, b, MESH_TRANSPORT_MAC_LEN) == 0;
}

static Peer* find_peer_by_mac_internal(const uint8_t mac[MESH_TRANSPORT_MAC_LEN]) {
  for (size_t i = 0; i < MESH_TRANSPORT_MAX_PEERS; ++i) {
    if (s_peers[i].in_use && mac_eq(s_peers[i].mac, mac)) return &s_peers[i];
  }
  return nullptr;
}

/* Internal state-transition with optional callback fire. */
static void transition_peer(Peer* peer, PeerState new_state) {
  if (peer->state == new_state) return;
  const PeerState old_state = peer->state;
  peer->state = new_state;
  if (s_peer_state_cb) s_peer_state_cb(peer->mac, old_state, new_state);
}

/* ──────────────────────────────────────────────────────────────────────────
 * ESP-NOW RECV CALLBACK (WiFi task)
 * ────────────────────────────────────────────────────────────────────────── */

#ifndef CSI_TEST_HOST_BUILD
static void espnow_recv_cb(const esp_now_recv_info_t* info,
                           const uint8_t* data, int len) {
  if (info == nullptr || data == nullptr || len <= 0) return;
  if ((size_t)len > MESH_TRANSPORT_PAYLOAD_MAX) return;

  const uint32_t head = s_ring_head.load(std::memory_order_relaxed);
  const uint32_t tail = s_ring_tail.load(std::memory_order_acquire);
  if ((head - tail) >= RING_CAP) {
    s_recv_dropped_ring_full.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  RingSlot* slot = &s_ring[head % RING_CAP];
  memcpy(slot->mac, info->src_addr, MESH_TRANSPORT_MAC_LEN);
  memcpy(slot->data, data, (size_t)len);
  slot->len = (uint16_t)len;
  /* rx_ctrl.rssi available via the recv-info struct in IDF 5.x. */
  slot->rssi_dbm = (info->rx_ctrl ? info->rx_ctrl->rssi : 0);

  s_ring_head.store(head + 1, std::memory_order_release);
}
#endif

#ifdef CSI_TEST_HOST_BUILD
namespace test {
  void inject_recv(const uint8_t mac[MESH_TRANSPORT_MAC_LEN],
                   const uint8_t* data, size_t len, int8_t rssi_dbm) {
    if (data == nullptr || len == 0 || len > MESH_TRANSPORT_PAYLOAD_MAX) return;
    const uint32_t head = s_ring_head.load(std::memory_order_relaxed);
    const uint32_t tail = s_ring_tail.load(std::memory_order_acquire);
    if ((head - tail) >= RING_CAP) {
      s_recv_dropped_ring_full.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    RingSlot* slot = &s_ring[head % RING_CAP];
    memcpy(slot->mac, mac, MESH_TRANSPORT_MAC_LEN);
    memcpy(slot->data, data, len);
    slot->len = (uint16_t)len;
    slot->rssi_dbm = rssi_dbm;
    s_ring_head.store(head + 1, std::memory_order_release);
  }
}
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * LIFECYCLE
 * ────────────────────────────────────────────────────────────────────────── */

bool init(const Config& cfg) {
  if (s_initialized) return true;
  s_cfg = cfg;
  memset(s_peers, 0, sizeof(s_peers));
  s_ring_head.store(0);
  s_ring_tail.store(0);
  s_bytes_sent.store(0);
  s_bytes_received.store(0);
  s_unicasts_sent.store(0);
  s_unicasts_failed.store(0);
  s_broadcasts_sent.store(0);
  s_broadcasts_failed.store(0);
  s_recv_dropped_no_peer.store(0);
  s_recv_dropped_ring_full.store(0);

#ifndef CSI_TEST_HOST_BUILD
  /* esp_now_init is idempotent across components (csi_probe may also
   * have called it). Treat already-init returns as success. */
  esp_err_t err = esp_now_init();
  if (err != ESP_OK && err != ESP_ERR_ESPNOW_INTERNAL) {
    return false;
  }
  esp_now_register_recv_cb(espnow_recv_cb);
#endif

  s_initialized = true;
  return true;
}

void deinit() {
  if (!s_initialized) return;
  stop();
  for (size_t i = 0; i < MESH_TRANSPORT_MAX_PEERS; ++i) {
    if (s_peers[i].in_use) driver_del_peer(s_peers[i].mac);
  }
  memset(s_peers, 0, sizeof(s_peers));
#ifndef CSI_TEST_HOST_BUILD
  esp_now_unregister_recv_cb();
  /* Do NOT call esp_now_deinit() — csi_probe may still be using it. */
#endif
  s_recv_cb = nullptr;
  s_peer_state_cb = nullptr;
  s_initialized = false;
}

bool start() {
  if (!s_initialized) return false;
  s_running = true;
  return true;
}

void stop() {
  s_running = false;
  /* Drain the ring so a subsequent start() begins clean. */
  s_ring_tail.store(s_ring_head.load(std::memory_order_acquire));
}

bool is_running() { return s_running; }

/* ──────────────────────────────────────────────────────────────────────────
 * PEER TABLE
 * ────────────────────────────────────────────────────────────────────────── */

bool has_peer(const uint8_t mac[MESH_TRANSPORT_MAC_LEN]) {
  return find_peer_by_mac_internal(mac) != nullptr;
}

bool add_peer(const uint8_t mac[MESH_TRANSPORT_MAC_LEN]) {
  if (!s_initialized) return false;
  if (has_peer(mac)) return false;
  /* Reject broadcast MAC — broadcast() is the API for that. */
  for (size_t i = 0; i < MESH_TRANSPORT_MAC_LEN; ++i) {
    if (mac[i] != 0xFF) goto not_broadcast;
  }
  return false;
not_broadcast:
  for (size_t i = 0; i < MESH_TRANSPORT_MAX_PEERS; ++i) {
    if (s_peers[i].in_use) continue;
    /* Register with the ESP-NOW driver FIRST so a driver-side failure
     * doesn't leave an orphan slot — same pattern as csi_probe. */
    if (!driver_add_peer(mac)) return false;
    memcpy(s_peers[i].mac, mac, MESH_TRANSPORT_MAC_LEN);
    s_peers[i].last_seen_ms = now_ms();
    s_peers[i].last_sent_ms = 0;
    s_peers[i].rssi_dbm = 0;
    s_peers[i].state = PeerState::ACTIVE;
    s_peers[i].in_use = true;
    if (s_peer_state_cb) s_peer_state_cb(mac, PeerState::UNKNOWN, PeerState::ACTIVE);
    return true;
  }
  return false;
}

bool remove_peer(const uint8_t mac[MESH_TRANSPORT_MAC_LEN]) {
  Peer* p = find_peer_by_mac_internal(mac);
  if (p == nullptr) return false;
  const PeerState old_state = p->state;
  driver_del_peer(p->mac);
  memset(p, 0, sizeof(*p));
  if (s_peer_state_cb) s_peer_state_cb(mac, old_state, PeerState::UNKNOWN);
  return true;
}

void clear_peers() {
  for (size_t i = 0; i < MESH_TRANSPORT_MAX_PEERS; ++i) {
    if (s_peers[i].in_use) {
      driver_del_peer(s_peers[i].mac);
      const PeerState old_state = s_peers[i].state;
      uint8_t mac_copy[MESH_TRANSPORT_MAC_LEN];
      memcpy(mac_copy, s_peers[i].mac, MESH_TRANSPORT_MAC_LEN);
      memset(&s_peers[i], 0, sizeof(s_peers[i]));
      if (s_peer_state_cb) s_peer_state_cb(mac_copy, old_state, PeerState::UNKNOWN);
    }
  }
}

size_t peer_count() {
  size_t n = 0;
  for (size_t i = 0; i < MESH_TRANSPORT_MAX_PEERS; ++i) {
    if (s_peers[i].in_use) ++n;
  }
  return n;
}

bool get_peer(const uint8_t mac[MESH_TRANSPORT_MAC_LEN], Peer* out) {
  Peer* p = find_peer_by_mac_internal(mac);
  if (p == nullptr || out == nullptr) return false;
  *out = *p;
  return true;
}

size_t list_peers(Peer* out, size_t max) {
  if (out == nullptr || max == 0) return 0;
  size_t n = 0;
  for (size_t i = 0; i < MESH_TRANSPORT_MAX_PEERS && n < max; ++i) {
    if (s_peers[i].in_use) out[n++] = s_peers[i];
  }
  return n;
}

/* ──────────────────────────────────────────────────────────────────────────
 * SEND
 * ────────────────────────────────────────────────────────────────────────── */

bool send_to_peer(const uint8_t mac[MESH_TRANSPORT_MAC_LEN],
                  const uint8_t* data, size_t len) {
  if (!s_running || data == nullptr || len == 0 || len > MESH_TRANSPORT_PAYLOAD_MAX) {
    return false;
  }
  Peer* peer = find_peer_by_mac_internal(mac);
  if (peer == nullptr) return false;
  if (driver_send(mac, data, len)) {
    s_unicasts_sent.fetch_add(1, std::memory_order_relaxed);
    s_bytes_sent.fetch_add((uint32_t)len, std::memory_order_relaxed);
    peer->last_sent_ms = now_ms();
    return true;
  }
  s_unicasts_failed.fetch_add(1, std::memory_order_relaxed);
  return false;
}

size_t broadcast(const uint8_t* data, size_t len) {
  if (!s_running || data == nullptr || len == 0 || len > MESH_TRANSPORT_PAYLOAD_MAX) {
    return 0;
  }
  const uint32_t now = now_ms();
  size_t sent = 0;
  for (size_t i = 0; i < MESH_TRANSPORT_MAX_PEERS; ++i) {
    if (!s_peers[i].in_use) continue;
    if (driver_send(s_peers[i].mac, data, len)) {
      ++sent;
      s_peers[i].last_sent_ms = now;
      s_bytes_sent.fetch_add((uint32_t)len, std::memory_order_relaxed);
    } else {
      s_broadcasts_failed.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (sent > 0) {
    s_broadcasts_sent.fetch_add((uint32_t)sent, std::memory_order_relaxed);
  }
  return sent;
}

/* ──────────────────────────────────────────────────────────────────────────
 * RECEIVE + AGING
 * ────────────────────────────────────────────────────────────────────────── */

void set_recv_callback(RecvCallback cb) { s_recv_cb = cb; }
void set_peer_state_callback(PeerStateCallback cb) { s_peer_state_cb = cb; }

static void drain_ring() {
  for (;;) {
    const uint32_t tail = s_ring_tail.load(std::memory_order_relaxed);
    const uint32_t head = s_ring_head.load(std::memory_order_acquire);
    if (tail == head) break;
    RingSlot* slot = &s_ring[tail % RING_CAP];

    Peer* peer = find_peer_by_mac_internal(slot->mac);
    if (peer != nullptr) {
      peer->last_seen_ms = now_ms();
      peer->rssi_dbm = slot->rssi_dbm;
      transition_peer(peer, PeerState::ACTIVE);
      s_bytes_received.fetch_add((uint32_t)slot->len, std::memory_order_relaxed);
      if (s_recv_cb) {
        s_recv_cb(slot->mac, slot->data, slot->len, slot->rssi_dbm);
      }
    } else {
      /* Frame from an unknown sender. PR 2b's pairing layer registers
       * peers; raw transport just counts the drop. */
      s_recv_dropped_no_peer.fetch_add(1, std::memory_order_relaxed);
    }
    /* Wipe slot before advancing tail so a producer wrap doesn't leak
     * stale data into the next iteration. */
    memset(slot, 0, sizeof(*slot));
    s_ring_tail.store(tail + 1, std::memory_order_release);
  }
}

static void age_peers() {
  const uint32_t now = now_ms();
  for (size_t i = 0; i < MESH_TRANSPORT_MAX_PEERS; ++i) {
    Peer* p = &s_peers[i];
    if (!p->in_use) continue;
    const uint32_t age = now - p->last_seen_ms;
    if (age >= PEER_OFFLINE_AFTER_MS) {
      transition_peer(p, PeerState::OFFLINE);
    } else if (age >= PEER_STALE_AFTER_MS) {
      transition_peer(p, PeerState::STALE);
    } else {
      transition_peer(p, PeerState::ACTIVE);
    }
  }
}

void process() {
  if (!s_running) return;
  drain_ring();
  age_peers();
}

bool get_stats(Stats* out) {
  if (!out) return false;
  out->bytes_sent               = s_bytes_sent.load(std::memory_order_relaxed);
  out->bytes_received           = s_bytes_received.load(std::memory_order_relaxed);
  out->unicasts_sent            = s_unicasts_sent.load(std::memory_order_relaxed);
  out->unicasts_failed          = s_unicasts_failed.load(std::memory_order_relaxed);
  out->broadcasts_sent          = s_broadcasts_sent.load(std::memory_order_relaxed);
  out->broadcasts_failed        = s_broadcasts_failed.load(std::memory_order_relaxed);
  out->recv_dropped_no_peer     = s_recv_dropped_no_peer.load(std::memory_order_relaxed);
  out->recv_dropped_ring_full   = s_recv_dropped_ring_full.load(std::memory_order_relaxed);
  /* Live peer-state counters. */
  out->peers_active = 0;
  out->peers_stale = 0;
  out->peers_offline = 0;
  for (size_t i = 0; i < MESH_TRANSPORT_MAX_PEERS; ++i) {
    if (!s_peers[i].in_use) continue;
    switch (s_peers[i].state) {
      case PeerState::ACTIVE:  ++out->peers_active;  break;
      case PeerState::STALE:   ++out->peers_stale;   break;
      case PeerState::OFFLINE: ++out->peers_offline; break;
      default: break;
    }
  }
  return true;
}

}  /* namespace mesh_transport */
