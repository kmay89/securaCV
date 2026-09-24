/*
 * SecuraCV Canary — Airtime Governor
 *
 * Caps how much 2.4 GHz airtime the mesh (Opera ESP-NOW + chirp broadcasts)
 * is allowed to consume on the shared radio, so multi-Canary deployments
 * don't degrade the user's home WiFi.
 *
 * Model
 * ─────
 * Each TX is recorded with its byte count. We estimate airtime per frame as
 *   airtime_us ≈ PHY_PREAMBLE_US
 *                + ((bytes + ESPNOW_FRAME_OVERHEAD_BYTES) * 8) / PHY_BIT_RATE_MBPS
 * and a reservation of `frames` frames costs frames x that. Then we sum
 * airtime over a rolling 10-second window and compare against a
 * configurable cap (default 2%). Sends are summed in 100 ms buckets, so the
 * window holds every send at any rate and reads 10.0-10.1 s (a bucket
 * leaves it with its newest send; the cap errs toward denying).
 *
 * This is an ESTIMATE, not a measurement of the air: it counts the bytes
 * the caller hands esp_now_send at ESP-NOW's default 1 Mbps rate, plus the
 * ~59 B framing allowance (ESPNOW_FRAME_OVERHEAD_BYTES, below) the governor
 * adds to every frame, so no caller adds framing itself. A caller that
 * unicasts one message to several peers passes `frames` (mesh_network's
 * broadcasts: one frame per peer broadcast_message reaches, i.e. every
 * authenticated peer, connected, stale, offline or alerting); the chirp
 * and Beacon broadcasts are one frame each.
 *
 * Two send classes
 * ────────────────
 *   - Routine: heartbeats, gossip, presence, peer-list refresh, chirp
 *     presence beacons, and the WAP's CSI active probe (probe_airtime.h,
 *     which also stops the probe at 1.60 % so the rest keep room). These
 *     call `try_reserve_routine()`. If the projected
 *     airtime would exceed the cap, the send is denied; the caller should
 *     simply skip this tick.
 *   - Urgent:  tamper alerts, OFFLINE_IMMINENT, power-loss notifications.
 *     These call `force_reserve_urgent()`. Always permitted, but still
 *     recorded so telemetry stays honest.
 *
 * Telemetry
 * ─────────
 * `airtime_pct_x100()` returns the rolling-window utilization as percent
 * x 100 (215 = 2.15 %).
 * The firmware publishes this as `mesh_airtime_pct` over MQTT discovery
 * and the BLE console.
 *
 * Pure-logic module: no Arduino dependency. Time is injected via the
 * caller's `now_ms` so it is host-testable.
 */

#ifndef SECURACV_AIRTIME_GOVERNOR_H
#define SECURACV_AIRTIME_GOVERNOR_H

#include <stdint.h>
#include <stddef.h>

namespace airtime_governor {

// PHY parameters for ESP-NOW @ 1 Mbps long preamble. 1 Mbps is ESP-NOW's
// default rate, and nothing in this firmware changes it (no call to
// esp_wifi_config_espnow_rate(); csi_probe.h says the same). The byte count
// is the caller's payload plus the framing allowance below, per frame.
static const uint32_t PHY_PREAMBLE_US = 192;      // long preamble + headers
static const uint32_t PHY_BIT_RATE_KBPS = 1000;   // ESP-NOW's default rate

// The governor's allowance for ESP-NOW's MAC/action-frame framing around
// the payload, added to every frame by estimate_airtime_us() (so a 16 B CSI
// probe payload is charged as 75 B, 792 us). Callers pass only what they
// hand esp_now_send. Conservative: Espressif's ESP-NOW frame format (ESP-IDF
// api-reference/network/esp_now) documents 43 B of fixed fields around the
// body (24 B MAC header, 1 B category, 3 B OUI, 4 B random values, the 7 B
// vendor element header, 4 B FCS), and every caller here sends unencrypted
// (encrypt = false in mesh_network, chirp_channel, beacon_channel and
// csi_probe), so 59 errs high by 16 B, 128 us a frame. Not measured on the
// air; ACK/SIFS time and contention are not in the estimate either.
constexpr size_t ESPNOW_FRAME_OVERHEAD_BYTES = 59;

static const uint32_t WINDOW_MS = 10000;           // rolling 10-second window
static const uint8_t DEFAULT_CAP_PCT = 2;          // 2% airtime cap (routine)

struct Stats {
  uint32_t window_ms;          // active window size
  uint32_t airtime_us;         // total airtime in window
  uint16_t airtime_pct_x100;   // utilization x100 (e.g. 215 = 2.15%)
  uint32_t routine_allowed;    // count of permitted routine sends
  uint32_t routine_denied;     // count of denied routine sends
  uint32_t urgent_sends;       // count of urgent sends (always allowed)
  uint32_t beacon_sends;       // count of urgent sends from the Beacon channel
                               // (distinct telemetry slot per spec/beacon_channel_v0.md §8)
  uint32_t beacon_airtime_us;  // total Beacon airtime in window (subset of airtime_us)
};

// Estimate the airtime cost of one frame carrying `bytes` of ESP-NOW
// payload (microseconds), its ESP-NOW framing included.
uint32_t estimate_airtime_us(size_t bytes);

// Initialize / reset the governor. Optional: pass cap_pct = 0 to keep default.
void init(uint8_t cap_pct);

/* True when the send-window ring allocated (PSRAM diet, csi_mem.h). The
 * governor fails open without it; callers should log the degradation. */
bool ring_ok();

// Attempt to reserve airtime for a routine (non-urgent) send.
//   now_ms: caller's millisecond clock
//   bytes:  ESP-NOW payload of ONE frame (the governor adds the framing)
//   frames: how many such frames the send puts on the air — one per peer
//           when the caller unicasts the same message to each
// The cost is frames x estimate_airtime_us(bytes), all or nothing, in one
// window slot. Returns true and records the send if allowed; returns false
// and records a denial otherwise. The counters count calls, not frames.
// A reservation of 0 frames (no peer to send to) puts nothing on the air:
// it charges nothing and is always allowed, even on a window already over
// the cap, so it counts as an allowed call and never as a denial.
bool try_reserve_routine(uint32_t now_ms, size_t bytes, uint16_t frames = 1);

// Force-reserve airtime for an urgent send (same bytes/frames meaning; 0
// frames charges nothing but still counts the call). Always permitted; the
// cost is still recorded so the rolling window reflects reality.
void force_reserve_urgent(uint32_t now_ms, size_t bytes, uint16_t frames = 1);

// Force-reserve airtime for a Beacon-class urgent send. Same airtime
// accounting as force_reserve_urgent, plus a distinct counter slot so the
// `beacon.airtime_pct` / `beacon.frames` MQTT sensor can be surfaced
// separately from Opera tamper/power alerts.
void force_reserve_beacon(uint32_t now_ms, size_t bytes, uint16_t frames = 1);

// Telemetry — utilization expressed as percent × 100 (i.e. 215 = 2.15%).
uint16_t airtime_pct_x100(uint32_t now_ms);

// Full snapshot — for the BLE console + HA sensor publish path.
Stats snapshot(uint32_t now_ms);

} // namespace airtime_governor

#endif // SECURACV_AIRTIME_GOVERNOR_H
