/*
 * SecuraCV Canary — Familiar-device recognizer — Implementation
 *
 * See familiar.h for design + privacy invariants.
 *
 * This module never sees a MAC, a token, or a raw RSSI sample. rf_presence
 * computes a coarse 11-bit fingerprint from its aggregate state and passes
 * it in; we hash it into Bloom filters with a device-specific salt so a
 * flash dump alone cannot replay which fingerprints were recorded.
 */

#include "familiar.h"
#include "nvs_store.h"
#include "health_log.h"

#include <string.h>
#include <esp_system.h>      // esp_fill_random
#include <mbedtls/sha256.h>

namespace familiar {

// ────────────────────────────────────────────────────────────────────────────
// SECURITY PRIMITIVES
// ────────────────────────────────────────────────────────────────────────────

static void secure_wipe(void* ptr, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (len--) { *p++ = 0; }
  asm volatile("" ::: "memory");
}

static inline uint32_t elapsed_ms(uint32_t start, uint32_t now) {
  return now - start;  /* unsigned subtraction handles wrap */
}

// ────────────────────────────────────────────────────────────────────────────
// NVS KEYS
// ────────────────────────────────────────────────────────────────────────────

static const char* NVS_KEY_SALT       = "fm_salt";      // 32-byte blob
static const char* NVS_KEY_IGNORE     = "fm_ignore";    // IGNORE_BYTES blob
static const char* NVS_KEY_YESTERDAY  = "fm_yest";      // BLOOM_BYTES blob
static const char* NVS_KEY_ROT_UP_MS  = "fm_rot_up";    // last rotation uptime

// ────────────────────────────────────────────────────────────────────────────
// STATE
// ────────────────────────────────────────────────────────────────────────────

static bool    s_initialized = false;
static uint8_t s_salt[32];

// Bloom filters
static uint8_t s_today    [BLOOM_BYTES];
static uint8_t s_yesterday[BLOOM_BYTES];
static bool    s_yesterday_valid = false;   // false on first boot

// Always-ignore filter (persisted)
static uint8_t s_ignore[IGNORE_BYTES];

// Rotation tracking (based on device uptime, not wall clock)
static uint32_t s_last_rotation_ms = 0;

// Stats
static uint32_t s_total_notes = 0;
static uint32_t s_total_rotations = 0;
static uint32_t s_total_ambient_queries = 0;
static uint32_t s_total_ambient_matches = 0;
static uint32_t s_total_always_ignored_queries = 0;
static uint32_t s_total_always_ignored_matches = 0;

// ────────────────────────────────────────────────────────────────────────────
// HASH FUNCTIONS — SHA-256 of (salt || fp_LE || hash_index), sliced into
// enough 11-bit indices to feed all the k hashes a filter needs.
//
// Why SHA? Tamper-resistant and salt-mixed; same ~30 µs regardless of k.
// Why salt? Prevents flash-dump recovery of fingerprints across the 2048-
// entry search space.
// ────────────────────────────────────────────────────────────────────────────

// Generate k indices in [0, bit_width) from (fp, hash_index_base).
// Each index is extracted from the SHA output as a 16-bit slice masked
// to bit_width (which must be a power of two).
static void hash_indices(uint16_t fp, size_t bit_width, uint8_t k,
                         uint16_t* out_indices) {
  // bit_width must be a power of two for the mask to work cleanly.
  const uint16_t mask = (uint16_t)(bit_width - 1);

  // Compose input: salt || fp (LE 2 bytes) || zero byte (reserved).
  uint8_t in[32 + 3];
  memcpy(in, s_salt, 32);
  in[32] = (uint8_t)(fp & 0xFF);
  in[33] = (uint8_t)((fp >> 8) & 0xFF);
  in[34] = 0;

  uint8_t digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  // mbedtls_sha256_starts_ret returns 0 on success; we proceed regardless
  // since a failure here would be fatal for the whole module (and SHA on
  // ESP32 is hardware-backed — failure is extremely unlikely).
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, in, sizeof(in));
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);

  // SHA-256 gives 32 bytes = 16 × 16-bit slices. k ≤ 8 for us, so one
  // digest is plenty. If a future increase in k needed more, concatenate
  // additional digests with hash_index_base in `in[34]`.
  for (uint8_t i = 0; i < k && i < 16; i++) {
    const uint16_t raw = (uint16_t)digest[2 * i]
                       | ((uint16_t)digest[2 * i + 1] << 8);
    out_indices[i] = raw & mask;
  }

  secure_wipe(in, sizeof(in));
  secure_wipe(digest, sizeof(digest));
}

static inline void bloom_set(uint8_t* filter, size_t bit_width,
                             const uint16_t* indices, uint8_t k) {
  (void)bit_width;
  for (uint8_t i = 0; i < k; i++) {
    const uint16_t bit = indices[i];
    filter[bit >> 3] |= (uint8_t)(1u << (bit & 7));
  }
}

static inline bool bloom_contains(const uint8_t* filter, size_t bit_width,
                                  const uint16_t* indices, uint8_t k) {
  (void)bit_width;
  for (uint8_t i = 0; i < k; i++) {
    const uint16_t bit = indices[i];
    if ((filter[bit >> 3] & (uint8_t)(1u << (bit & 7))) == 0) return false;
  }
  return true;
}

static uint32_t bloom_popcount(const uint8_t* filter, size_t bytes) {
  uint32_t n = 0;
  for (size_t i = 0; i < bytes; i++) {
    uint8_t b = filter[i];
    // Kernighan's popcount; fine for 512 byte filters on S3.
    while (b) { b &= (uint8_t)(b - 1); n++; }
  }
  return n;
}

// ────────────────────────────────────────────────────────────────────────────
// DIFFERENTIAL-PRIVACY NOISE
//
// At rotation time, flip each bit of the yesterday snapshot with probability
// DP_NOISE_BASIS_POINTS / 10000. This makes an attacker who extracts the
// filter (with the salt) see a probabilistic view of the previous day.
// ────────────────────────────────────────────────────────────────────────────

static void add_dp_noise(uint8_t* filter, size_t bytes) {
  // We implement Bernoulli(p) using 16-bit random draws.
  for (size_t i = 0; i < bytes; i++) {
    uint8_t noise = 0;
    for (uint8_t b = 0; b < 8; b++) {
      uint32_t r;
      esp_fill_random(&r, sizeof(r));
      if ((r % 10000) < DP_NOISE_BASIS_POINTS) {
        noise |= (uint8_t)(1u << b);
      }
    }
    filter[i] ^= noise;
  }
}

// ────────────────────────────────────────────────────────────────────────────
// PERSISTENCE
// ────────────────────────────────────────────────────────────────────────────

static void persist_ignore() {
  nvs_store::set_blob(NVS_KEY_IGNORE, s_ignore, sizeof(s_ignore));
}

static void persist_yesterday() {
  nvs_store::set_blob(NVS_KEY_YESTERDAY, s_yesterday, sizeof(s_yesterday));
  nvs_store::set_u32(NVS_KEY_ROT_UP_MS, s_last_rotation_ms);
  // Store validity as a sentinel in yesterday's popcount. If all bytes
  // are zero we treat it as invalid on next load.
}

static void load_or_init_salt() {
  if (!nvs_store::get_blob(NVS_KEY_SALT, s_salt, sizeof(s_salt))) {
    esp_fill_random(s_salt, sizeof(s_salt));
    nvs_store::set_blob(NVS_KEY_SALT, s_salt, sizeof(s_salt));
  }
  // Validate non-zero
  uint8_t acc = 0;
  for (size_t i = 0; i < sizeof(s_salt); i++) acc |= s_salt[i];
  if (acc == 0) {
    esp_fill_random(s_salt, sizeof(s_salt));
    nvs_store::set_blob(NVS_KEY_SALT, s_salt, sizeof(s_salt));
  }
}

// ────────────────────────────────────────────────────────────────────────────
// LIFECYCLE
// ────────────────────────────────────────────────────────────────────────────

bool init() {
  if (s_initialized) return true;

  memset(s_today, 0, sizeof(s_today));
  memset(s_yesterday, 0, sizeof(s_yesterday));
  memset(s_ignore, 0, sizeof(s_ignore));
  s_last_rotation_ms = millis();
  s_yesterday_valid = false;

  load_or_init_salt();

  // Restore persisted filters if present.
  if (nvs_store::get_blob(NVS_KEY_IGNORE, s_ignore, sizeof(s_ignore))) {
    // OK
  }
  if (nvs_store::get_blob(NVS_KEY_YESTERDAY, s_yesterday, sizeof(s_yesterday))) {
    // Treat as valid only if non-zero.
    s_yesterday_valid = bloom_popcount(s_yesterday, sizeof(s_yesterday)) > 0;
  }
  s_last_rotation_ms = nvs_store::get_u32(NVS_KEY_ROT_UP_MS, s_last_rotation_ms);

  s_initialized = true;
  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Familiar: init (yesterday=%s, always_ignore bits=%u)",
    s_yesterday_valid ? "valid" : "cold",
    (unsigned)bloom_popcount(s_ignore, sizeof(s_ignore)));
  return true;
}

void deinit() {
  if (!s_initialized) return;
  secure_wipe(s_today, sizeof(s_today));
  secure_wipe(s_yesterday, sizeof(s_yesterday));
  secure_wipe(s_ignore, sizeof(s_ignore));
  secure_wipe(s_salt, sizeof(s_salt));
  s_yesterday_valid = false;
  s_initialized = false;
}

// ────────────────────────────────────────────────────────────────────────────
// FINGERPRINT COMPUTATION
//
// 11-bit layout (LSB first):
//   [0..4] time-of-day 45-min bucket    (5 bits)
//   [5..7] rssi_mean level              (3 bits, 8 levels across -90..0 dBm)
//   [8..9] adv density class            (2 bits)
//   [10]   rssi stability flag          (1 bit)
// ────────────────────────────────────────────────────────────────────────────

uint16_t compute_fingerprint(const FingerprintInputs& in) {
  // time_of_day_bucket in the caller is 10-min buckets (0..143). Re-bucket
  // to 45-min buckets: 144 / 32 = 4.5. Integer-divide by 4 gives us 36
  // buckets; mask to 5 bits (32) folds 32..35 back into 0..3, which is
  // fine since fingerprint collisions are deliberately tolerated.
  const uint8_t t_bucket = (uint8_t)((in.time_of_day_bucket / 4) & 0x1F);

  // RSSI mean level: map -90..0 dBm into 8 levels (each 11.25 dBm).
  // Clamp first.
  int32_t rssi = in.rssi_mean_dbm;
  if (rssi < -90) rssi = -90;
  if (rssi >  0)  rssi = 0;
  // 0 = weakest, 7 = strongest.
  const uint8_t rssi_level = (uint8_t)(((rssi + 90) * 8 / 91) & 0x07);

  // Adv density class: 0=quiet (<4/min), 1=low (<16/min), 2=med (<64/min),
  // 3=high (>=64/min).
  uint8_t density = 0;
  const uint8_t apm = in.adv_per_minute;
  if (apm >= 64)      density = 3;
  else if (apm >= 16) density = 2;
  else if (apm >= 4)  density = 1;
  // else 0 (quiet)

  // RSSI stability: 1 if spread > 12 dBm (roughly 4x in power), else 0.
  const uint8_t stability = (in.rssi_spread_dbm > 12) ? 1 : 0;

  const uint16_t fp = (uint16_t)t_bucket
                    | ((uint16_t)rssi_level << 5)
                    | ((uint16_t)density   << 8)
                    | ((uint16_t)stability << 10);
  return (uint16_t)(fp & FINGERPRINT_MASK);
}

// ────────────────────────────────────────────────────────────────────────────
// ROTATING FILTER
// ────────────────────────────────────────────────────────────────────────────

void note_fingerprint(uint16_t fp) {
  if (!s_initialized) return;
  fp &= FINGERPRINT_MASK;

  uint16_t idx[BLOOM_HASHES];
  hash_indices(fp, BLOOM_BITS, BLOOM_HASHES, idx);
  bloom_set(s_today, BLOOM_BITS, idx, BLOOM_HASHES);
  s_total_notes++;
}

bool is_ambient(uint16_t fp) {
  if (!s_initialized || !s_yesterday_valid) return false;
  fp &= FINGERPRINT_MASK;

  s_total_ambient_queries++;

  uint16_t idx[BLOOM_HASHES];
  hash_indices(fp, BLOOM_BITS, BLOOM_HASHES, idx);
  const bool match = bloom_contains(s_yesterday, BLOOM_BITS, idx, BLOOM_HASHES);
  if (match) s_total_ambient_matches++;
  return match;
}

void rotate_now() {
  if (!s_initialized) return;

  // yesterday <- today (snapshot) + DP noise
  memcpy(s_yesterday, s_today, sizeof(s_yesterday));
  add_dp_noise(s_yesterday, sizeof(s_yesterday));
  s_yesterday_valid = true;

  // today <- empty
  memset(s_today, 0, sizeof(s_today));

  s_last_rotation_ms = millis();
  s_total_rotations++;

  persist_yesterday();

  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Familiar: rotated (yesterday bits=%u)",
    (unsigned)bloom_popcount(s_yesterday, sizeof(s_yesterday)));
}

void tick(uint32_t now_ms) {
  if (!s_initialized) return;
  if (elapsed_ms(s_last_rotation_ms, now_ms) >= ROTATION_PERIOD_MS) {
    rotate_now();
  }
}

// ────────────────────────────────────────────────────────────────────────────
// ALWAYS-IGNORE
// ────────────────────────────────────────────────────────────────────────────

bool always_ignore(uint16_t fp) {
  if (!s_initialized) return false;
  fp &= FINGERPRINT_MASK;

  uint16_t idx[IGNORE_HASHES];
  hash_indices(fp, IGNORE_BITS, IGNORE_HASHES, idx);
  bloom_set(s_ignore, IGNORE_BITS, idx, IGNORE_HASHES);

  persist_ignore();
  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Familiar: always-ignore added (bits set now=%u)",
    (unsigned)bloom_popcount(s_ignore, sizeof(s_ignore)));
  return true;
}

bool is_always_ignored(uint16_t fp) {
  if (!s_initialized) return false;
  fp &= FINGERPRINT_MASK;

  s_total_always_ignored_queries++;

  uint16_t idx[IGNORE_HASHES];
  hash_indices(fp, IGNORE_BITS, IGNORE_HASHES, idx);
  const bool match = bloom_contains(s_ignore, IGNORE_BITS, idx, IGNORE_HASHES);
  if (match) s_total_always_ignored_matches++;
  return match;
}

bool forget_always_ignored() {
  if (!s_initialized) return false;
  memset(s_ignore, 0, sizeof(s_ignore));
  persist_ignore();
  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Familiar: always-ignore list cleared");
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// INTROSPECTION
// ────────────────────────────────────────────────────────────────────────────

bool get_stats(Stats* out) {
  if (!out) return false;
  out->total_notes                     = s_total_notes;
  out->total_rotations                 = s_total_rotations;
  out->total_ambient_queries           = s_total_ambient_queries;
  out->total_ambient_matches           = s_total_ambient_matches;
  out->total_always_ignored_queries    = s_total_always_ignored_queries;
  out->total_always_ignored_matches    = s_total_always_ignored_matches;
  out->today_bits_set                  = bloom_popcount(s_today, sizeof(s_today));
  out->yesterday_bits_set              = s_yesterday_valid
                                         ? bloom_popcount(s_yesterday, sizeof(s_yesterday))
                                         : 0;
  out->always_ignored_bits_set         = bloom_popcount(s_ignore, sizeof(s_ignore));

  const uint32_t elapsed = elapsed_ms(s_last_rotation_ms, millis());
  out->ms_until_next_rotation = elapsed >= ROTATION_PERIOD_MS
                                ? 0
                                : (ROTATION_PERIOD_MS - elapsed);
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// CONFORMANCE
// ────────────────────────────────────────────────────────────────────────────

bool conformance_self_test() {
  // Save state so the test doesn't corrupt real data.
  uint8_t saved_today    [BLOOM_BYTES];
  uint8_t saved_yesterday[BLOOM_BYTES];
  bool    saved_yvalid = s_yesterday_valid;
  uint32_t saved_last_rot = s_last_rotation_ms;

  memcpy(saved_today,     s_today,     sizeof(saved_today));
  memcpy(saved_yesterday, s_yesterday, sizeof(saved_yesterday));

  memset(s_today, 0, sizeof(s_today));
  memset(s_yesterday, 0, sizeof(s_yesterday));
  s_yesterday_valid = false;

  // 1. Insert fp into today. Query "ambient" (should be false — yesterday
  //    is empty). Query "always_ignored" (should be false).
  const uint16_t test_fp = 0x2A5;  // arbitrary 11-bit value
  note_fingerprint(test_fp);
  const bool step1a = !is_ambient(test_fp);
  const bool step1b = !is_always_ignored(test_fp);

  // 2. Simulate rotation without DP noise (for determinism). Yesterday
  //    now contains today's fp, so is_ambient should return true.
  memcpy(s_yesterday, s_today, sizeof(s_yesterday));
  s_yesterday_valid = true;
  const bool step2 = is_ambient(test_fp);

  // 3. Simulate a second rotation (today was just emptied). Yesterday
  //    inherits the empty today; is_ambient should return false.
  memset(s_today, 0, sizeof(s_today));
  memcpy(s_yesterday, s_today, sizeof(s_yesterday));
  const bool step3 = !is_ambient(test_fp);

  // Restore state.
  memcpy(s_today,     saved_today,     sizeof(s_today));
  memcpy(s_yesterday, saved_yesterday, sizeof(s_yesterday));
  s_yesterday_valid = saved_yvalid;
  s_last_rotation_ms = saved_last_rot;

  const bool ok = step1a && step1b && step2 && step3;
  if (!ok) {
    health_logging::logf(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Familiar self-test FAIL: s1a=%d s1b=%d s2=%d s3=%d",
      step1a, step1b, step2, step3);
  } else {
    health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
      "Familiar self-test: OK");
  }
  return ok;
}

}  // namespace familiar
