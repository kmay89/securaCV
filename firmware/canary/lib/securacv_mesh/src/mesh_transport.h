/*
 * SecuraCV Canary — Mesh transport (ESP-NOW)
 * Version 0.1.0
 *
 * Raw ESP-NOW transport with a bounded peer table. This is PR 2a of the
 * mesh port from `firmware/projects/canary-wap/arduino/canary_wap/
 * mesh_network.cpp` — JUST the unencrypted transport + peer table.
 * Crypto (Ed25519/ChaCha20-Poly1305), pairing, and message-type dispatch
 * arrive in PR 2b/2c.
 *
 * Boundaries:
 *   • No identifiers persist: MACs in the peer table are runtime-only.
 *     A future NVS persistence layer can re-hydrate the table at boot;
 *     it lives outside this module so the persistence policy stays one
 *     place instead of leaking into the transport.
 *   • This module owns the ESP-NOW init/teardown for the peer-data
 *     channel. csi_probe also uses ESP-NOW; both call esp_now_init()
 *     which is idempotent across components.
 *   • PR 2b's crypto layer wraps `send_to_peer()` / receive callback to
 *     add encryption. Raw send/recv stay accessible for diagnostics.
 *
 * Threading:
 *   • The ESP-NOW recv callback runs in the WiFi task. We copy the
 *     payload into a small ring and surface it from process() on the
 *     main loop. Send paths run on the main loop (esp_now_send returns
 *     immediately; tx completion comes via the send callback).
 *
 * Host build:
 *   • CSI_TEST_HOST_BUILD swaps ESP-NOW with an injectable send hook
 *     and millis() with a virtual clock so the scheduler + peer-table
 *     state machine are unit-testable without hardware.
 */

#ifndef SECURACV_MESH_TRANSPORT_H
#define SECURACV_MESH_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_transport {

/* ──────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ────────────────────────────────────────────────────────────────────────── */

/* Maximum peers. Matches MESH_MAX_PEERS in firmware/common/network/
 * mesh_network.h and CSI_PROBE_MAX_PEERS so the three peer lists are
 * one-to-one. */
constexpr size_t MESH_TRANSPORT_MAX_PEERS = 8;

/* MAC address length (ESP-NOW unicast/broadcast addresses). */
constexpr size_t MESH_TRANSPORT_MAC_LEN = 6;

/* Maximum ESP-NOW payload bytes. ESP-NOW's hard MTU is 250 bytes;
 * we leave headroom for a future encryption frame so the post-crypto
 * payload still fits. */
constexpr size_t MESH_TRANSPORT_PAYLOAD_MAX = 200;

/* Peer aging thresholds (milliseconds). Matches the canary-wap
 * mesh_network constants so a future merge of the two lanes is a
 * mechanical edit. */
constexpr uint32_t PEER_STALE_AFTER_MS   = 90  * 1000;   /* 90 s */
constexpr uint32_t PEER_OFFLINE_AFTER_MS = 300 * 1000;   /* 5 min */

/* ──────────────────────────────────────────────────────────────────────────
 * STATES + TYPES
 * ────────────────────────────────────────────────────────────────────────── */

/* Transport-layer peer state. Crypto/auth state machine in PR 2b sits
 * above this — those states are independent. */
enum class PeerState : uint8_t {
  UNKNOWN = 0,    /* slot empty */
  ACTIVE,         /* last_seen within PEER_STALE_AFTER_MS */
  STALE,          /* between PEER_STALE_AFTER_MS and PEER_OFFLINE_AFTER_MS */
  OFFLINE         /* exceeded PEER_OFFLINE_AFTER_MS */
};

struct Peer {
  uint8_t   mac[MESH_TRANSPORT_MAC_LEN];
  uint32_t  last_seen_ms;       /* updated by recv callback */
  uint32_t  last_sent_ms;       /* updated by send call */
  int8_t    rssi_dbm;           /* most recent rx_ctrl.rssi from this peer */
  PeerState state;
  bool      in_use;
};

struct Config {
  /* Reserved. Empty for PR 2a; PR 2b will add crypto-mode flags. */
  uint8_t  _reserved;

  static Config defaults() {
    return Config{ /* _reserved */ 0 };
  }
};

struct Stats {
  uint32_t bytes_sent;
  uint32_t bytes_received;
  uint32_t unicasts_sent;
  uint32_t unicasts_failed;
  uint32_t broadcasts_sent;
  uint32_t broadcasts_failed;
  uint32_t recv_dropped_no_peer;  /* recv from an unknown sender */
  uint32_t recv_dropped_ring_full;
  uint32_t peers_active;
  uint32_t peers_stale;
  uint32_t peers_offline;
};

/* Receive callback. Called from process() on the main loop, NOT from
 * the WiFi task — so callees can safely use logging/SD/etc. mac points
 * at a 6-byte buffer owned by the caller; data is valid for the
 * duration of the call only. */
using RecvCallback = void (*)(const uint8_t mac[MESH_TRANSPORT_MAC_LEN],
                              const uint8_t* data,
                              size_t          len,
                              int8_t          rssi_dbm);

/* Peer state-change callback (transitions only; no-op when state is
 * unchanged). Useful for the integration layer to mirror peer add/remove
 * into csi_probe's table. */
using PeerStateCallback = void (*)(const uint8_t mac[MESH_TRANSPORT_MAC_LEN],
                                   PeerState old_state,
                                   PeerState new_state);

/* ──────────────────────────────────────────────────────────────────────────
 * LIFECYCLE
 * ────────────────────────────────────────────────────────────────────────── */

bool init(const Config& cfg);
void deinit();
bool start();
void stop();
bool is_running();

/* ──────────────────────────────────────────────────────────────────────────
 * PEER TABLE
 * ────────────────────────────────────────────────────────────────────────── */

bool   add_peer(const uint8_t mac[MESH_TRANSPORT_MAC_LEN]);
bool   remove_peer(const uint8_t mac[MESH_TRANSPORT_MAC_LEN]);
void   clear_peers();
size_t peer_count();
bool   has_peer(const uint8_t mac[MESH_TRANSPORT_MAC_LEN]);

/* Returns false if the peer is not known. */
bool get_peer(const uint8_t mac[MESH_TRANSPORT_MAC_LEN], Peer* out);

/* Snapshot the table — fills `out` up to `max`, returns how many entries
 * are populated. Order is implementation-defined. */
size_t list_peers(Peer* out, size_t max);

/* ──────────────────────────────────────────────────────────────────────────
 * SEND
 * ────────────────────────────────────────────────────────────────────────── */

/* Unicast `data` (len bytes, ≤ MESH_TRANSPORT_PAYLOAD_MAX) to a known
 * peer. Returns false if the peer is unknown, the length is invalid, or
 * the ESP-NOW driver refused the send (the failure counter is bumped). */
bool send_to_peer(const uint8_t mac[MESH_TRANSPORT_MAC_LEN],
                  const uint8_t* data, size_t len);

/* Broadcast to every paired peer (NOT FF:FF:FF:FF:FF:FF). Returns the
 * number of peers that accepted the send. */
size_t broadcast(const uint8_t* data, size_t len);

/* ──────────────────────────────────────────────────────────────────────────
 * RECEIVE + AGING
 * ────────────────────────────────────────────────────────────────────────── */

void set_recv_callback(RecvCallback cb);
void set_peer_state_callback(PeerStateCallback cb);

/* Pump: drains the recv ring (delivering to the recv callback) and ages
 * peers (ACTIVE → STALE → OFFLINE based on PEER_*_AFTER_MS). Call from
 * the main loop at any cadence; 10 Hz is plenty. */
void process();

bool get_stats(Stats* out);

/* ──────────────────────────────────────────────────────────────────────────
 * TEST HOOKS (host build only)
 * ────────────────────────────────────────────────────────────────────────── */

#ifdef CSI_TEST_HOST_BUILD
namespace test {
  /* Virtual clock — replaces millis(). */
  uint32_t set_now_ms(uint32_t v);
  uint32_t get_now_ms();

  /* Replace esp_now_send. Return true = success, false = ESP-NOW refused. */
  using SendHook = bool (*)(const uint8_t* mac,
                            const uint8_t* data,
                            size_t          len);
  void set_send_hook(SendHook h);

  /* Replace esp_now_add_peer. Return false to simulate a driver failure. */
  using PeerAddHook = bool (*)(const uint8_t* mac);
  void set_peer_add_hook(PeerAddHook h);

  /* Inject an incoming frame as if ESP-NOW had delivered it. Drives the
   * recv path including peer last_seen + RSSI updates. */
  void inject_recv(const uint8_t mac[MESH_TRANSPORT_MAC_LEN],
                   const uint8_t* data, size_t len, int8_t rssi_dbm);
}
#endif

}  /* namespace mesh_transport */

#endif  /* SECURACV_MESH_TRANSPORT_H */
