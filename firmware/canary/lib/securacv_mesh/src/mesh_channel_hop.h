/*
 * SecuraCV Canary — Mesh channel-hop wire format
 * Version 0.1.0
 *
 * PR 4b. Pins the CHANNEL_LOCK payload encoding for coordinated
 * channel hopping across mesh peers.
 *
 * Wire format (2 bytes total, fits under mesh_envelope::MAX_PAYLOAD_LEN=128):
 *
 *   offset 0  : channel        uint8_t  — 1..14 (2.4 GHz WiFi)
 *   offset 1  : reason         uint8_t  — why the hop was proposed
 *
 * The Hub broadcasts this when channel utilization exceeds a threshold
 * (default 50% for 60 s). Receivers call csi_hal::set_channel_lock()
 * with the proposed channel. The lock is advisory — if WiFi STA is
 * associated to an AP that owns a different channel, the AP wins and
 * is_channel_in_sync() will report false.
 *
 * This TU is pure logic: no Arduino, no mbedtls, no transport. It
 * encodes/decodes bytes and is host-build-clean.
 */

#ifndef SECURACV_MESH_CHANNEL_HOP_H
#define SECURACV_MESH_CHANNEL_HOP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace mesh_channel_hop {

enum class Reason : uint8_t {
  UTILIZATION   = 0,   /* airtime exceeded threshold */
  INTERFERENCE  = 1,   /* external interference detected */
  MANUAL        = 2,   /* operator-initiated */
  INITIAL       = 3,   /* first lock after pairing */
  /* 4..255 reserved. */
};

constexpr size_t  CHANNEL_LEN  = 1;
constexpr size_t  REASON_LEN   = 1;
constexpr size_t  PAYLOAD_LEN  = CHANNEL_LEN + REASON_LEN;   /* = 2 */

constexpr uint8_t MIN_CHANNEL  = 1;
constexpr uint8_t MAX_CHANNEL  = 14;

/* Encode a (channel, reason) pair into a fixed 2-byte payload.
 *
 * Returns false if:
 *   • out_buf is nullptr or out_buf_cap < PAYLOAD_LEN
 *   • channel is outside [MIN_CHANNEL, MAX_CHANNEL] */
bool encode(uint8_t   channel,
            Reason    reason,
            uint8_t*  out_buf,
            size_t    out_buf_cap);

/* Decode a 2-byte payload into (channel, reason).
 *
 * Returns false if:
 *   • any pointer is null
 *   • in_len != PAYLOAD_LEN (strict fixed-size)
 *   • decoded channel is outside [MIN_CHANNEL, MAX_CHANNEL] */
bool decode(const uint8_t*  in_buf,
            size_t          in_len,
            uint8_t*        out_channel,
            Reason*         out_reason);

/* ──────────────────────────────────────────────────────────────────────────
 * CHANNEL SELECTION
 *
 * Picks the next non-overlapping 2.4 GHz channel to hop to: 1 → 6 → 11 → 1.
 * Returns 0 if current_channel is not one of {1, 6, 11} (caller should
 * fall back to channel 6 in that case).
 * ────────────────────────────────────────────────────────────────────────── */

uint8_t next_channel(uint8_t current_channel);

/* ──────────────────────────────────────────────────────────────────────────
 * HOP THRESHOLD TRACKER
 *
 * Pure state machine that tracks whether airtime utilization has exceeded
 * a threshold for a sustained duration. Call tick() from the main loop
 * with the current time and utilization reading; should_propose() returns
 * true exactly once per threshold breach, then resets with a cooldown.
 *
 * Host-testable: no I/O, no timers, no globals.
 * ────────────────────────────────────────────────────────────────────────── */

struct HopTracker {
  uint16_t threshold_pct_x100;   /* e.g. 5000 = 50% */
  uint32_t sustain_ms;           /* how long above threshold before hop */
  uint32_t cooldown_ms;          /* silence after a hop before re-arming */

  uint32_t above_since_ms;       /* valid only when tracking_above==true */
  uint32_t last_hop_ms;          /* 0 = never hopped */
  bool     tracking_above;       /* true = above threshold, timing */
  bool     armed;                /* true = watching; false = in cooldown */
};

HopTracker make_tracker(uint16_t threshold_pct_x100 = 5000,
                        uint32_t sustain_ms          = 60000,
                        uint32_t cooldown_ms         = 120000);

/* Feed a new utilization sample. Returns true if the tracker has just
 * transitioned to "should hop" — caller should broadcast CHANNEL_LOCK
 * and then call reset() to start the cooldown. */
bool tick(HopTracker& t, uint32_t now_ms, uint16_t airtime_pct_x100);

/* Reset the tracker into cooldown after a hop was broadcast. */
void reset(HopTracker& t, uint32_t now_ms);

}  /* namespace mesh_channel_hop */

#endif  /* SECURACV_MESH_CHANNEL_HOP_H */
