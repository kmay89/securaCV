/*
 * SecuraCV Canary — WiFi Presence Detection (Probe Request Monitoring)
 *
 * Privacy-preserving device counting using WiFi probe request sniffing.
 * The ESP32 runs promiscuous mode alongside AP mode to passively listen
 * for probe requests from nearby phones and devices.
 *
 * Architecture:
 * - ISR callback (promiscuous_cb) captures only the raw MAC + timestamp
 *   and pushes it into a FreeRTOS queue. No heavy work in ISR context.
 * - process_queue() runs from loop() and performs hashing, dedup, bucket
 *   rotation — all safely outside ISR context.
 *
 * Core Privacy Invariants:
 * - MAC addresses are hashed immediately in process_queue — never stored raw
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
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace wifi_presence {

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

static const uint32_t BUCKET_DURATION_MS     = 60000;  // 1-minute buckets
static const uint8_t  MAX_MACS_PER_BUCKET    = 64;     // Max unique MACs tracked per bucket
static const uint8_t  HASH_LEN               = 4;      // Truncated hash bytes (enough for counting, not tracking)
static const uint8_t  HISTORY_BUCKETS        = 10;     // Keep last N bucket counts for sparkline
static const uint8_t  QUEUE_DEPTH            = 32;     // Max queued MACs from ISR before drop

// Alert threshold — devices per bucket to trigger witness record
static const uint8_t  DEFAULT_ALERT_THRESHOLD = 10;

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

// Lightweight struct pushed from ISR → queue (no heap, no heavy ops)
struct ProbeCapture {
  uint8_t  mac[6];
  uint32_t timestamp_ms;
};

struct PresenceState {
  uint32_t bucket_start_ms;
  uint8_t  mac_hashes[MAX_MACS_PER_BUCKET][HASH_LEN];
  uint8_t  count;                          // Current bucket count
  uint8_t  last_count;                     // Previous bucket count
  uint8_t  history[HISTORY_BUCKETS];       // Ring buffer of past bucket counts
  uint8_t  history_idx;                    // Next write index in history
  uint8_t  peak_count;                     // All-time peak in a single bucket
  uint32_t total_probes_seen;              // Total probe requests received
  uint32_t queue_drops;                    // Probes dropped because queue was full
  bool     enabled;
  bool     initialized;
};

// ════════════════════════════════════════════════════════════════════════════
// STATE
// ════════════════════════════════════════════════════════════════════════════

// Protected by portMUX for reads from main context.
// Only modified by process_queue() (main context) — never from ISR.
static PresenceState g_state = {};
static portMUX_TYPE  g_state_mux = portMUX_INITIALIZER_UNLOCKED;

// FreeRTOS queue: ISR pushes ProbeCapture → main loop pops & processes
static QueueHandle_t g_probe_queue = nullptr;

// ISR-safe counters (updated atomically from ISR)
static volatile uint32_t g_isr_probe_count = 0;
static volatile uint32_t g_isr_drop_count = 0;

// ════════════════════════════════════════════════════════════════════════════
// MAC HASHING (privacy-preserving) — runs in main context only
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

// Check if MAC hash already seen this bucket — main context only
static bool mac_seen(const uint8_t hash[HASH_LEN]) {
  for (uint8_t i = 0; i < g_state.count; i++) {
    if (memcmp(g_state.mac_hashes[i], hash, HASH_LEN) == 0) {
      return true;
    }
  }
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// PROMISCUOUS MODE CALLBACK (ISR context — minimal work)
// ════════════════════════════════════════════════════════════════════════════

// Only extracts the source MAC and pushes to queue. No hashing, no loops.
static void IRAM_ATTR promiscuous_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;  // Only management frames

  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  const uint8_t* frame = pkt->payload;

  // Check if it's a probe request (type=0, subtype=4)
  uint8_t frame_type = (frame[0] >> 2) & 0x03;
  uint8_t frame_subtype = (frame[0] >> 4) & 0x0F;
  if (frame_type != 0 || frame_subtype != 4) return;

  g_isr_probe_count++;

  // Source MAC is at offset 10 in management frames
  ProbeCapture cap;
  memcpy(cap.mac, frame + 10, 6);
  cap.timestamp_ms = millis();

  // Non-blocking push to queue — drop if full (acceptable for counting)
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (xQueueSendFromISR(g_probe_queue, &cap, &xHigherPriorityTaskWoken) != pdTRUE) {
    g_isr_drop_count++;
  }

  if (xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR();
  }
}

// ════════════════════════════════════════════════════════════════════════════
// QUEUE PROCESSING — call from loop() (main context)
// ════════════════════════════════════════════════════════════════════════════

// Drains the probe queue and does all heavy work (hashing, dedup, bucket
// rotation) safely outside ISR context.
static void process_queue() {
  if (!g_state.enabled || !g_probe_queue) return;

  ProbeCapture cap;
  uint32_t now = millis();

  // Sync ISR counters
  g_state.total_probes_seen = g_isr_probe_count;
  g_state.queue_drops = g_isr_drop_count;

  // Rotate bucket if needed (before processing new MACs)
  if (now - g_state.bucket_start_ms >= BUCKET_DURATION_MS) {
    portENTER_CRITICAL(&g_state_mux);
    g_state.history[g_state.history_idx] = g_state.count;
    g_state.history_idx = (g_state.history_idx + 1) % HISTORY_BUCKETS;
    if (g_state.count > g_state.peak_count) {
      g_state.peak_count = g_state.count;
    }
    g_state.last_count = g_state.count;
    g_state.count = 0;
    g_state.bucket_start_ms = now;
    memset(g_state.mac_hashes, 0, sizeof(g_state.mac_hashes));
    portEXIT_CRITICAL(&g_state_mux);
  }

  // Process up to QUEUE_DEPTH items per call to avoid starving loop()
  uint8_t processed = 0;
  while (processed < QUEUE_DEPTH && xQueueReceive(g_probe_queue, &cap, 0) == pdTRUE) {
    processed++;

    // Hash and deduplicate
    uint8_t mac_hash[HASH_LEN];
    hash_mac(cap.mac, g_state.bucket_start_ms, mac_hash);

    portENTER_CRITICAL(&g_state_mux);
    if (!mac_seen(mac_hash) && g_state.count < MAX_MACS_PER_BUCKET) {
      memcpy(g_state.mac_hashes[g_state.count], mac_hash, HASH_LEN);
      g_state.count++;
    }
    portEXIT_CRITICAL(&g_state_mux);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

static bool start() {
  if (g_state.enabled) return true;  // Already running

  // Create queue if needed
  if (!g_probe_queue) {
    g_probe_queue = xQueueCreate(QUEUE_DEPTH, sizeof(ProbeCapture));
    if (!g_probe_queue) {
      Serial.println("[WIFI-PRESENCE] Failed to create probe queue");
      return false;
    }
  }

  esp_wifi_set_promiscuous_rx_cb(promiscuous_cb);
  esp_err_t err = esp_wifi_set_promiscuous(true);
  if (err == ESP_OK) {
    portENTER_CRITICAL(&g_state_mux);
    g_state.enabled = true;
    g_state.initialized = true;
    g_state.bucket_start_ms = millis();
    g_state.count = 0;
    g_isr_probe_count = 0;
    g_isr_drop_count = 0;
    portEXIT_CRITICAL(&g_state_mux);
    Serial.println("[WIFI-PRESENCE] Probe monitoring started");
    return true;
  }
  Serial.printf("[WIFI-PRESENCE] Failed to start: %s\n", esp_err_to_name(err));
  return false;
}

static void stop() {
  if (!g_state.enabled) return;
  esp_wifi_set_promiscuous(false);
  portENTER_CRITICAL(&g_state_mux);
  g_state.enabled = false;
  g_state.last_count = g_state.count;
  portEXIT_CRITICAL(&g_state_mux);
  Serial.println("[WIFI-PRESENCE] Probe monitoring stopped");
}

// All getters snapshot under critical section for consistency
static bool is_enabled() {
  portENTER_CRITICAL(&g_state_mux);
  bool v = g_state.enabled;
  portEXIT_CRITICAL(&g_state_mux);
  return v;
}

static uint8_t get_current_count() {
  portENTER_CRITICAL(&g_state_mux);
  uint8_t v = g_state.count;
  portEXIT_CRITICAL(&g_state_mux);
  return v;
}

static uint8_t get_last_count() {
  portENTER_CRITICAL(&g_state_mux);
  uint8_t v = g_state.last_count;
  portEXIT_CRITICAL(&g_state_mux);
  return v;
}

static uint8_t get_peak_count() {
  portENTER_CRITICAL(&g_state_mux);
  uint8_t v = g_state.peak_count;
  portEXIT_CRITICAL(&g_state_mux);
  return v;
}

static uint32_t get_total_probes() { return g_isr_probe_count; }
static uint32_t get_queue_drops() { return g_isr_drop_count; }

static uint32_t get_bucket_elapsed_ms() {
  portENTER_CRITICAL(&g_state_mux);
  bool en = g_state.enabled;
  uint32_t start = g_state.bucket_start_ms;
  portEXIT_CRITICAL(&g_state_mux);
  if (!en) return 0;
  return millis() - start;
}

// Get history as array (oldest first) — snapshot under critical section
static void get_history(uint8_t* out, uint8_t* out_len) {
  *out_len = HISTORY_BUCKETS;
  portENTER_CRITICAL(&g_state_mux);
  uint8_t start_idx = g_state.history_idx;
  for (uint8_t i = 0; i < HISTORY_BUCKETS; i++) {
    uint8_t idx = (start_idx + i) % HISTORY_BUCKETS;
    out[i] = g_state.history[idx];
  }
  portEXIT_CRITICAL(&g_state_mux);
}

// Check if threshold crossed (for witness record generation)
static bool threshold_crossed(uint8_t threshold) {
  portENTER_CRITICAL(&g_state_mux);
  bool result = g_state.count >= threshold && g_state.last_count < threshold;
  portEXIT_CRITICAL(&g_state_mux);
  return result;
}

// Check if presence dropped to zero
static bool presence_cleared() {
  portENTER_CRITICAL(&g_state_mux);
  bool result = g_state.count == 0 && g_state.last_count > 0;
  portEXIT_CRITICAL(&g_state_mux);
  return result;
}

} // namespace wifi_presence

#else // !FEATURE_WIFI_PRESENCE

// No-op stubs
namespace wifi_presence {
  static inline bool start() { return false; }
  static inline void stop() {}
  static inline void process_queue() {}
  static inline bool is_enabled() { return false; }
  static inline uint8_t get_current_count() { return 0; }
  static inline uint8_t get_last_count() { return 0; }
  static inline uint8_t get_peak_count() { return 0; }
  static inline uint32_t get_total_probes() { return 0; }
  static inline uint32_t get_queue_drops() { return 0; }
  static inline uint32_t get_bucket_elapsed_ms() { return 0; }
  static inline void get_history(uint8_t* out, uint8_t* out_len) { *out_len = 0; }
  static inline bool threshold_crossed(uint8_t) { return false; }
  static inline bool presence_cleared() { return false; }
}

#endif // FEATURE_WIFI_PRESENCE

#endif // SECURACV_WIFI_PRESENCE_H
