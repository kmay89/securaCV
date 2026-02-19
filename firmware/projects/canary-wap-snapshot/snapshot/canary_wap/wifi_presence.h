/*
 * SecuraCV Canary — WiFi Presence Detection (Probe Request Monitoring)
 *
 * Privacy-preserving device counting using WiFi probe request sniffing.
 * The ESP32 runs promiscuous mode alongside AP mode to passively listen
 * for probe requests from nearby phones and devices.
 *
 * Core Privacy Invariants:
 * - MAC addresses are hashed immediately — never stored raw
 * - Hashes include a per-bucket salt so they can't be correlated across time
 * - Only unique device COUNTS are stored, never identifiers
 * - No SSID names are stored
 * - Result: {"nearby_devices": 5, "bucket": 42} — that's it
 *
 * Hardware Note:
 * Promiscuous mode works alongside AP mode, but ESP32 can only listen
 * on one channel at a time. Since we're running an AP on a fixed channel,
 * we only hear probes on that channel. This is fine for presence estimation.
 */

#ifndef SECURACV_WIFI_PRESENCE_H
#define SECURACV_WIFI_PRESENCE_H

#include "build_config.h"

#if FEATURE_WIFI_PRESENCE

#include <Arduino.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "mbedtls/sha256.h"

namespace wifi_presence {

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

static const uint32_t BUCKET_DURATION_MS     = 60000;  // 1-minute buckets
static const uint8_t  MAX_MACS_PER_BUCKET    = 64;     // Max unique MACs tracked per bucket
static const uint8_t  HASH_LEN               = 4;      // Truncated hash bytes (enough for counting, not tracking)
static const uint8_t  HISTORY_BUCKETS        = 10;     // Keep last N bucket counts for sparkline

// ════════════════════════════════════════════════════════════════════════════
// STATE
// ════════════════════════════════════════════════════════════════════════════

struct PresenceState {
  uint32_t bucket_start_ms;
  uint8_t  mac_hashes[MAX_MACS_PER_BUCKET][HASH_LEN];
  uint8_t  count;                          // Current bucket count
  uint8_t  last_count;                     // Previous bucket count
  uint8_t  history[HISTORY_BUCKETS];       // Ring buffer of past bucket counts
  uint8_t  history_idx;                    // Next write index in history
  uint8_t  peak_count;                     // All-time peak in a single bucket
  uint32_t total_probes_seen;              // Total probe requests received
  bool     enabled;
  bool     initialized;
};

static volatile PresenceState g_state = {};

// ════════════════════════════════════════════════════════════════════════════
// MAC HASHING (privacy-preserving)
// ════════════════════════════════════════════════════════════════════════════

// Domain-separated hash: only 4 bytes kept. Cannot reverse to MAC.
// Changes every bucket because bucket_start is mixed in.
static void hash_mac(const uint8_t mac[6], uint32_t bucket_salt,
                     uint8_t out[HASH_LEN]) {
  uint8_t input[32];
  // Domain separation: "scv:wp:" + bucket_salt + MAC
  memcpy(input, "scv:wp:", 7);
  memcpy(input + 7, &bucket_salt, 4);
  memcpy(input + 11, mac, 6);

  uint8_t hash[32];
  mbedtls_sha256(input, 17, hash, 0);
  memcpy(out, hash, HASH_LEN);
}

// Check if MAC hash already seen this bucket
static bool mac_seen(const uint8_t hash[HASH_LEN]) {
  for (uint8_t i = 0; i < g_state.count; i++) {
    if (memcmp(g_state.mac_hashes[i], hash, HASH_LEN) == 0) {
      return true;
    }
  }
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// PROMISCUOUS MODE CALLBACK
// ════════════════════════════════════════════════════════════════════════════

// Called by ESP32 WiFi driver for every received frame in promiscuous mode.
// IRAM_ATTR: runs from IRAM for speed since it's called from WiFi ISR context.
static void IRAM_ATTR promiscuous_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!g_state.enabled) return;
  if (type != WIFI_PKT_MGMT) return;  // Only management frames

  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  const uint8_t* frame = pkt->payload;

  // Check if it's a probe request (type=0, subtype=4)
  uint8_t frame_type = (frame[0] >> 2) & 0x03;
  uint8_t frame_subtype = (frame[0] >> 4) & 0x0F;
  if (frame_type != 0 || frame_subtype != 4) return;

  g_state.total_probes_seen++;

  // Source MAC is at offset 10 in management frames
  const uint8_t* src_mac = frame + 10;

  // Rotate bucket if needed
  uint32_t now = millis();
  if (now - g_state.bucket_start_ms >= BUCKET_DURATION_MS) {
    // Save current count to history
    g_state.history[g_state.history_idx] = g_state.count;
    g_state.history_idx = (g_state.history_idx + 1) % HISTORY_BUCKETS;

    // Track peak
    if (g_state.count > g_state.peak_count) {
      g_state.peak_count = g_state.count;
    }

    g_state.last_count = g_state.count;
    g_state.count = 0;
    g_state.bucket_start_ms = now;
    memset((void*)g_state.mac_hashes, 0, sizeof(g_state.mac_hashes));
  }

  // Hash and deduplicate
  uint8_t mac_hash[HASH_LEN];
  hash_mac(src_mac, g_state.bucket_start_ms, mac_hash);

  if (!mac_seen(mac_hash) && g_state.count < MAX_MACS_PER_BUCKET) {
    memcpy((void*)g_state.mac_hashes[g_state.count], mac_hash, HASH_LEN);
    g_state.count++;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

static bool start() {
  if (g_state.enabled) return true;  // Already running

  esp_wifi_set_promiscuous_rx_cb(promiscuous_cb);
  esp_err_t err = esp_wifi_set_promiscuous(true);
  if (err == ESP_OK) {
    g_state.enabled = true;
    g_state.initialized = true;
    g_state.bucket_start_ms = millis();
    g_state.count = 0;
    Serial.println("[WIFI-PRESENCE] Probe monitoring started");
    return true;
  }
  Serial.printf("[WIFI-PRESENCE] Failed to start: %s\n", esp_err_to_name(err));
  return false;
}

static void stop() {
  if (!g_state.enabled) return;
  esp_wifi_set_promiscuous(false);
  g_state.enabled = false;
  g_state.last_count = g_state.count;
  Serial.println("[WIFI-PRESENCE] Probe monitoring stopped");
}

static bool is_enabled() { return g_state.enabled; }
static uint8_t get_current_count() { return g_state.count; }
static uint8_t get_last_count() { return g_state.last_count; }
static uint8_t get_peak_count() { return g_state.peak_count; }
static uint32_t get_total_probes() { return g_state.total_probes_seen; }

static uint32_t get_bucket_elapsed_ms() {
  if (!g_state.enabled) return 0;
  return millis() - g_state.bucket_start_ms;
}

// Get history as array (oldest first)
static void get_history(uint8_t* out, uint8_t* out_len) {
  *out_len = HISTORY_BUCKETS;
  for (uint8_t i = 0; i < HISTORY_BUCKETS; i++) {
    uint8_t idx = (g_state.history_idx + i) % HISTORY_BUCKETS;
    out[i] = g_state.history[idx];
  }
}

// Check if threshold crossed (for witness record generation)
static bool threshold_crossed(uint8_t threshold) {
  return g_state.count >= threshold && g_state.last_count < threshold;
}

// Check if presence dropped to zero
static bool presence_cleared() {
  return g_state.count == 0 && g_state.last_count > 0;
}

} // namespace wifi_presence

#else // !FEATURE_WIFI_PRESENCE

// No-op stubs
namespace wifi_presence {
  static inline bool start() { return false; }
  static inline void stop() {}
  static inline bool is_enabled() { return false; }
  static inline uint8_t get_current_count() { return 0; }
  static inline uint8_t get_last_count() { return 0; }
  static inline uint8_t get_peak_count() { return 0; }
  static inline uint32_t get_total_probes() { return 0; }
  static inline uint32_t get_bucket_elapsed_ms() { return 0; }
  static inline void get_history(uint8_t* out, uint8_t* out_len) { *out_len = 0; }
  static inline bool threshold_crossed(uint8_t) { return false; }
  static inline bool presence_cleared() { return false; }
}

#endif // FEATURE_WIFI_PRESENCE

#endif // SECURACV_WIFI_PRESENCE_H
