/*
 * SecuraCV Canary — CSI active probe (ESP-NOW)
 * Version 0.1.0
 *
 * Sends low-payload ESP-NOW unicasts to a small set of paired peers at a
 * configurable rate (default 20 Hz per peer). The transmission itself is
 * the point: each unicast triggers a CSI rx callback on the receiver with
 * a known sender on a pinned channel, so the receiver's 1 Hz feature
 * window (csi_hal/csi_features) becomes deterministic instead of depending
 * on ambient WiFi traffic.
 *
 * PRIVACY:
 *   The packet payload carries no identifiers — a 3-byte magic, a version
 *   byte, a monotonic sequence number, and zero padding. The receiver's
 *   csi_hal already scrubs the source MAC at the ISR boundary before any
 *   feature accumulation. This module sends; it does not store peers
 *   beyond a small RAM table, and it does not log peer MACs.
 *
 * COUPLING:
 *   Standalone — does not depend on csi_hal. ESP-NOW is initialized
 *   lazily on first start(); if some other component already initialized
 *   ESP-NOW, that's fine (init is idempotent). Nothing fills the peer
 *   table today: the canary-wap runs the probe broadcast-only
 *   (csi_integration.cpp probe_pump), the canary compiles this file but
 *   never calls it, and each tree's mesh keeps its own ESP-NOW peer list
 *   without driving this one. A sketch can drive add_peer()/remove_peer()
 *   directly.
 *
 * BUILD:
 *   Compiles for ESP32-S3 against arduino-esp32 (esp_now.h). Host build
 *   (CSI_TEST_HOST_BUILD) stubs ESP-NOW with an injectable send hook so
 *   the scheduler is testable without hardware.
 */

#ifndef SECURACV_CSI_PROBE_H
#define SECURACV_CSI_PROBE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace csi_probe {

/* ──────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ────────────────────────────────────────────────────────────────────────── */

/* Max peers the probe will fan out to. The same 8 as the canary mesh
 * transport's MESH_TRANSPORT_MAX_PEERS (securacv_mesh/src/mesh_transport.h);
 * the two tables are separate, and no mesh fills this one (see COUPLING). */
constexpr size_t CSI_PROBE_MAX_PEERS = 8;

/* MAC address length (no Bluetooth — this is an ESP-NOW peer MAC). */
constexpr size_t CSI_PROBE_MAC_LEN = 6;

/* Maximum payload bytes. ESP-NOW's MTU is 250 but our probe is
 * intentionally tiny — we want predictable airtime, not bulk transfer. */
constexpr size_t CSI_PROBE_PAYLOAD_MAX = 32;

/* Wire format of a probe frame. Packed so it serializes byte-identically
 * on big- and little-endian builds (the host build runs on x86; the
 * device build runs on Xtensa LE). The fields here are NOT identifiers —
 * see file header. */
struct __attribute__((packed)) csi_probe_pkt_t {
  uint8_t  magic[3];        /* {'C','V','P'} */
  uint8_t  version;         /* protocol version, currently 1 */
  uint32_t seq;             /* monotonic sequence, wraps freely */
  uint8_t  padding[8];      /* zero — pads to 16 bytes total */
};

static_assert(sizeof(csi_probe_pkt_t) == 16,
              "csi_probe_pkt_t must be exactly 16 bytes on the wire");

/* ──────────────────────────────────────────────────────────────────────────
 * CONFIGURATION
 * ────────────────────────────────────────────────────────────────────────── */

struct Config {
  /* Per-peer send rate ceiling. 20 Hz matches the CSI HAL's rate-limiter
   * so we never spend airtime on frames the receiver will drop. The
   * effective per-peer rate is min(rate_hz, aggregate_cap_hz / max(1,
   * peer_count)) so the cost stays bounded as the peer table fills. */
  uint16_t rate_hz;

  /* Aggregate Tx cap across all peers + idle broadcasts, in frames/sec.
   *
   * Honest airtime math (this comment once claimed 0.6 %, which counted
   * only the preamble, and then ≈ 0.66 ms a frame, which left out the
   * payload's bytes): ESP-NOW sends at the 1 Mbps long-preamble rate
   * unless esp_wifi_config_espnow_rate() says otherwise, and nothing in
   * this firmware does. One frame is then 192 µs PLCP + 75 bytes on air
   * (~59 bytes of MAC/action-frame framing and the 16-byte payload) at
   * 8 µs a byte: 192 + 75 × 8 = 792 µs. So a single peer at rate_hz=20
   * costs ~1.6 % of the 2.4 GHz channel; a full table of eight peers at
   * the default 20 Hz asks for 160 frames/s, ~12.7 %; and the 200 Hz
   * ceiling below, which only a raised rate_hz reaches, would cost
   * ~15.8 %. That is well over airtime_governor's 2 % routine cap (#442),
   * which is why every send also asks `airtime_gate` below (the
   * integration layer wires it to airtime_governor::try_reserve_routine —
   * see csi_integration.cpp's probe_pump). This cap remains the
   * scheduler's own ceiling: in a build that leaves the gate null
   * (standalone sketches, host tests), it is the only limit, and 25 Hz is
   * the most that fits the 2 % budget at 1 Mbps (25 × 792 µs is 1.98 %;
   * 30 Hz would be ~2.4 %). These are arithmetic at the fallback rate,
   * not a measurement of the air. */
  uint16_t aggregate_cap_hz;

  /* If true, when no peers are registered the probe falls back to ESP-NOW
   * broadcast (FF:FF:FF:FF:FF:FF) at idle_rate_hz so CSI stays warm.
   * Useful during pairing and for the very first feature windows. */
  bool     broadcast_when_no_peers;

  /* Low-rate broadcast cadence (used only when broadcast_when_no_peers
   * is true and the peer list is empty). 2 Hz keeps CSI alive without
   * meaningful airtime cost. */
  uint16_t idle_rate_hz;

  /* Payload size in bytes (sizeof(csi_probe_pkt_t) padded to this). The
   * minimum useful value is sizeof(csi_probe_pkt_t)=16; larger payloads
   * give more subcarriers per frame (LLTF is fixed but the rate at which
   * the WiFi MAC schedules sends is amortized over a larger MPDU). */
  uint8_t  payload_len;

  /* Airtime reservation gate. When non-null, every probe send — unicast
   * and idle broadcast alike — asks this hook to reserve its cost first,
   * passing the caller's clock and the ESP-NOW payload length (the hook
   * owns adding MAC/action-frame framing before charging a budget — see
   * the honest airtime math above). Return false to deny: the send is
   * skipped, counted in Stats::sends_denied_airtime, and the slot's
   * cadence is kept so it retries one period later. The integration
   * layer wires this to airtime_governor::try_reserve_routine (probe
   * frames are routine traffic — never urgent); null means ungated,
   * bounded only by aggregate_cap_hz. */
  bool (*airtime_gate)(uint32_t now_ms, size_t payload_bytes);

  static Config defaults() {
    return Config{
      /* rate_hz */                 20,
      /* aggregate_cap_hz */        200,
      /* broadcast_when_no_peers */ true,
      /* idle_rate_hz */            2,
      /* payload_len */             sizeof(csi_probe_pkt_t),
      /* airtime_gate */            nullptr
    };
  }
};

/* ──────────────────────────────────────────────────────────────────────────
 * STATS
 * ────────────────────────────────────────────────────────────────────────── */

struct Stats {
  uint32_t unicasts_sent;
  uint32_t unicasts_failed;
  uint32_t broadcasts_sent;
  uint32_t broadcasts_failed;
  uint32_t ticks_skipped_rate;    /* process() called but rate gate not met */
  uint32_t ticks_skipped_idle;    /* no peers + broadcast disabled */
  uint32_t peers_registered;      /* current peer count (snapshot) */
  uint32_t sends_denied_airtime;  /* sends refused by Config::airtime_gate */
};

/* ──────────────────────────────────────────────────────────────────────────
 * LIFECYCLE
 * ────────────────────────────────────────────────────────────────────────── */

bool init(const Config& cfg);
void deinit();
bool start();
void stop();
bool is_running();

/* Pause/resume probe sends without tearing down state. While paused,
 * process() skips all sends. Used during channel-hop recovery to avoid
 * probing on the wrong channel. */
void set_paused(bool paused);
bool is_paused();

/* ──────────────────────────────────────────────────────────────────────────
 * PEER REGISTRY
 *
 * Small RAM-only table of up to CSI_PROBE_MAX_PEERS MACs. No persistence,
 * and no caller fills it in either tree today (see COUPLING); the durable
 * peer lists are the meshes' own.
 * ────────────────────────────────────────────────────────────────────────── */

bool   add_peer(const uint8_t mac[CSI_PROBE_MAC_LEN]);
bool   remove_peer(const uint8_t mac[CSI_PROBE_MAC_LEN]);
void   clear_peers();
size_t peer_count();
bool   has_peer(const uint8_t mac[CSI_PROBE_MAC_LEN]);

/* ──────────────────────────────────────────────────────────────────────────
 * MAIN LOOP
 *
 * Call from the main loop at any reasonable cadence (>= 10 Hz). Internal
 * rate gates determine whether sends actually occur. Non-blocking.
 * ────────────────────────────────────────────────────────────────────────── */

void process();

/* Snapshot of internal counters. Cumulative since init(). */
bool get_stats(Stats* out);

/* ──────────────────────────────────────────────────────────────────────────
 * TEST HOOKS (host build only)
 *
 * Host-build entry points so the scheduler logic can be exercised in CI
 * without ESP-NOW or millis(). See csi_probe_test.cpp for usage.
 * ────────────────────────────────────────────────────────────────────────── */

#ifdef CSI_TEST_HOST_BUILD
namespace test {
  /* Replace the internal millis() with a virtual clock. Returns the old
   * value. Setting now_ms=0 resets the clock. */
  uint32_t set_now_ms(uint32_t now_ms);
  uint32_t get_now_ms();

  /* Hook called instead of esp_now_send() under CSI_TEST_HOST_BUILD.
   * Return true to count as success. The MAC is FF:FF:FF:FF:FF:FF for
   * broadcasts, otherwise the peer's stored MAC. */
  using SendHook = bool (*)(const uint8_t* mac,
                            const uint8_t* payload,
                            size_t          len);
  void set_send_hook(SendHook hook);

  /* Hook called instead of esp_now_add_peer() under CSI_TEST_HOST_BUILD.
   * Return false to simulate a driver-side failure (table full, OOM); the
   * tests use this to verify add_peer() rolls back the slot when the
   * driver rejects the peer. Pass nullptr to clear and revert to the
   * default success behavior. */
  using PeerAddHook = bool (*)(const uint8_t* mac);
  void set_peer_add_hook(PeerAddHook hook);
}
#endif

}  /* namespace csi_probe */

#endif  /* SECURACV_CSI_PROBE_H */
