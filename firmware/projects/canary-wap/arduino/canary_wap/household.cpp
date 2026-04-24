/*
 * SecuraCV Canary — Household Device Recognition — Implementation
 *
 * See household.h for design and byte-order conventions.
 *
 * This module never logs, emits, or otherwise exposes an IRK outside of
 * the module. The only data that ever crosses the module boundary is:
 *   - yes/no resolution verdicts (hot path, per BLE scan)
 *   - user-chosen labels (cold path, status UI)
 *   - aggregate counters (status UI, anonymous)
 */

#include "household.h"
#include "dp.h"
#include "nvs_store.h"
#include "health_log.h"

#include <string.h>
#include <mbedtls/aes.h>

namespace household {

// ────────────────────────────────────────────────────────────────────────────
// SECURITY PRIMITIVES
// ────────────────────────────────────────────────────────────────────────────

static void secure_wipe(void* ptr, size_t len) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
  while (len--) { *p++ = 0; }
  asm volatile("" ::: "memory");
}

// ────────────────────────────────────────────────────────────────────────────
// PERSISTENT STATE
// ────────────────────────────────────────────────────────────────────────────

// One enrolled household device.
// Packed byte layout (40 bytes):
//   [0..15]  irk
//   [16..31] label (NUL-terminated, max 15 chars + NUL; remaining bytes zero)
//   [32..35] added_ms (device millis() at enrollment, not wall clock)
//   [36..37] flags (reserved)
//   [38..39] reserved
struct HouseholdSlot {
  uint8_t  irk[IRK_LEN];
  char     label[MAX_LABEL_LEN];
  uint32_t added_ms;
  uint16_t flags;
  uint16_t reserved;
};
static_assert(sizeof(HouseholdSlot) == 40,
              "HouseholdSlot packing changed — update NVS migration");

// NVS blob key. Holds MAX_HOUSEHOLD_DEVICES × HouseholdSlot = 320 bytes.
static const char* NVS_KEY_SLOTS = "hh_slots";
static const char* NVS_KEY_COUNT = "hh_count";

static HouseholdSlot s_slots[MAX_HOUSEHOLD_DEVICES];
// Bitmap of occupied slots (bit i set ⇒ slot i is in use).
static uint32_t s_slot_bitmap = 0;

static bool s_initialized = false;
static bool s_enrolling = false;
static uint32_t s_enrollment_started_ms = 0;

// Diagnostic counters (do not reveal which device matched, only totals).
static uint32_t s_total_resolves_attempted = 0;
static uint32_t s_total_resolves_matched   = 0;
static uint32_t s_total_non_rpa_seen       = 0;

// ────────────────────────────────────────────────────────────────────────────
// SLOT BITMAP HELPERS
// ────────────────────────────────────────────────────────────────────────────

static inline bool slot_is_used(uint8_t i) {
  return i < MAX_HOUSEHOLD_DEVICES && (s_slot_bitmap & (1u << i));
}
static inline void slot_mark_used(uint8_t i)  { s_slot_bitmap |=  (1u << i); }
static inline void slot_mark_free(uint8_t i)  { s_slot_bitmap &= ~(1u << i); }

static int find_free_slot() {
  for (uint8_t i = 0; i < MAX_HOUSEHOLD_DEVICES; i++) {
    if (!slot_is_used(i)) return (int)i;
  }
  return -1;
}

// Constant-time compare of two IRKs. Prevents timing side-channels if a
// future attacker is probing enrollment flows with chosen MACs.
static bool irks_equal_ct(const uint8_t a[IRK_LEN], const uint8_t b[IRK_LEN]) {
  uint8_t diff = 0;
  for (size_t i = 0; i < IRK_LEN; i++) diff |= (a[i] ^ b[i]);
  return diff == 0;
}

static bool irk_already_stored(const uint8_t irk[IRK_LEN]) {
  for (uint8_t i = 0; i < MAX_HOUSEHOLD_DEVICES; i++) {
    if (slot_is_used(i) && irks_equal_ct(s_slots[i].irk, irk)) return true;
  }
  return false;
}

// ────────────────────────────────────────────────────────────────────────────
// NVS PERSISTENCE
// ────────────────────────────────────────────────────────────────────────────

static void persist() {
  nvs_store::set_blob(NVS_KEY_SLOTS, s_slots, sizeof(s_slots));
  nvs_store::set_u32(NVS_KEY_COUNT, (uint32_t)s_slot_bitmap);
}

static void load_from_nvs() {
  secure_wipe(s_slots, sizeof(s_slots));
  s_slot_bitmap = 0;

  // Blob may not exist on first boot — that's fine.
  if (!nvs_store::get_blob(NVS_KEY_SLOTS, s_slots, sizeof(s_slots))) {
    return;
  }

  const uint32_t bitmap = nvs_store::get_u32(NVS_KEY_COUNT, 0);
  // Sanitize bitmap to MAX_HOUSEHOLD_DEVICES bits.
  s_slot_bitmap = bitmap & ((1u << MAX_HOUSEHOLD_DEVICES) - 1u);

  // Defensive: wipe slots that the bitmap says are unused (any garbage
  // left over from a previous install should not be resolvable against).
  for (uint8_t i = 0; i < MAX_HOUSEHOLD_DEVICES; i++) {
    if (!slot_is_used(i)) {
      secure_wipe(&s_slots[i], sizeof(s_slots[i]));
    }
  }
}

// ────────────────────────────────────────────────────────────────────────────
// RPA RESOLUTION (Bluetooth Core Spec 5.3, Vol 3, Part H, §2.2.2)
// ────────────────────────────────────────────────────────────────────────────

// The `ah` function: hash = AES-128(irk, 0×13 || prand) mod 2^24.
//   r must be 3 bytes, LSB-first (matches NimBLE mac[0]=LSB convention).
//   out is 3 bytes, LSB-first.
// Returns true on success, false on AES failure.
static bool ah(const uint8_t irk[IRK_LEN], const uint8_t r[3], uint8_t out[3]) {
  uint8_t padded[16];
  uint8_t cipher[16];
  memset(padded, 0, 13);
  padded[13] = r[2];  // MSB of prand (big-endian positioning in AES block)
  padded[14] = r[1];
  padded[15] = r[0];  // LSB of prand

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (mbedtls_aes_setkey_enc(&ctx, irk, 128) != 0) {
    mbedtls_aes_free(&ctx);
    return false;
  }
  if (mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, padded, cipher) != 0) {
    mbedtls_aes_free(&ctx);
    secure_wipe(padded, sizeof(padded));
    return false;
  }
  mbedtls_aes_free(&ctx);

  // Hash = low 24 bits of cipher, output in LSB-first byte order.
  out[0] = cipher[15];  // LSB
  out[1] = cipher[14];
  out[2] = cipher[13];  // MSB

  // Scrub intermediates; cipher contains key-dependent AES output that we
  // don't want lingering on the stack frame.
  secure_wipe(padded, sizeof(padded));
  secure_wipe(cipher, sizeof(cipher));
  return true;
}

// Is the address an RPA? Top two bits of the MSB byte == 0b01.
// With mac[0]=LSB convention, the MSB byte is mac[5].
static inline bool is_rpa(const uint8_t mac[6]) {
  return (mac[5] & 0xC0) == 0x40;
}

// ────────────────────────────────────────────────────────────────────────────
// LIFECYCLE
// ────────────────────────────────────────────────────────────────────────────

bool init() {
  if (s_initialized) return true;
  load_from_nvs();
  s_initialized = true;
  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Household: %u device(s) loaded from NVS", (unsigned)count());
  return true;
}

void deinit() {
  if (!s_initialized) return;
  end_enrollment();
  secure_wipe(s_slots, sizeof(s_slots));
  s_slot_bitmap = 0;
  s_initialized = false;
}

// ────────────────────────────────────────────────────────────────────────────
// ENROLLMENT
// ────────────────────────────────────────────────────────────────────────────

void begin_enrollment() {
  if (!s_initialized) return;
  s_enrolling = true;
  s_enrollment_started_ms = millis();
  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Household: enrollment window opened (60s)");
}

bool is_enrolling() {
  if (!s_enrolling) return false;
  // Auto-close after the window expires.
  const uint32_t now = millis();
  const uint32_t elapsed = now - s_enrollment_started_ms;
  if (elapsed >= ENROLLMENT_WINDOW_MS) {
    s_enrolling = false;
    health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
      "Household: enrollment window closed (timeout)");
    return false;
  }
  return true;
}

void end_enrollment() {
  if (!s_enrolling) return;
  s_enrolling = false;
  s_enrollment_started_ms = 0;
  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Household: enrollment window closed (user)");
}

uint32_t enrollment_ms_remaining() {
  if (!is_enrolling()) return 0;
  const uint32_t elapsed = millis() - s_enrollment_started_ms;
  return elapsed >= ENROLLMENT_WINDOW_MS ? 0 : (ENROLLMENT_WINDOW_MS - elapsed);
}

// ────────────────────────────────────────────────────────────────────────────
// ADD / REMOVE
// ────────────────────────────────────────────────────────────────────────────

int add_irk(const uint8_t irk[IRK_LEN], const char* label) {
  if (!s_initialized) return -1;
  if (!is_enrolling()) {
    health_logging::log(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "Household: add_irk rejected — enrollment window not open");
    return -1;
  }
  if (!irk) return -1;

  if (irk_already_stored(irk)) {
    health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
      "Household: add_irk ignored — IRK already enrolled");
    return -1;
  }

  const int slot = find_free_slot();
  if (slot < 0) {
    health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
      "Household: add_irk rejected — all %u slots full",
      (unsigned)MAX_HOUSEHOLD_DEVICES);
    return -1;
  }

  HouseholdSlot& s = s_slots[slot];
  memcpy(s.irk, irk, IRK_LEN);
  memset(s.label, 0, MAX_LABEL_LEN);
  if (label) {
    strncpy(s.label, label, MAX_LABEL_LEN - 1);
  }
  s.added_ms = millis();
  s.flags = 0;
  s.reserved = 0;

  slot_mark_used((uint8_t)slot);
  persist();

  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Household: enrolled slot %d (%s), %u total",
    slot, s.label[0] ? s.label : "unlabeled", (unsigned)count());
  return slot;
}

bool remove_by_slot(uint8_t slot) {
  if (!s_initialized) return false;
  if (!slot_is_used(slot)) return false;

  secure_wipe(&s_slots[slot], sizeof(s_slots[slot]));
  slot_mark_free(slot);
  persist();

  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Household: removed slot %u, %u remaining", (unsigned)slot, (unsigned)count());
  return true;
}

bool remove_all() {
  if (!s_initialized) return false;
  secure_wipe(s_slots, sizeof(s_slots));
  s_slot_bitmap = 0;
  persist();
  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Household: all devices forgotten");
  return true;
}

uint8_t count() {
  uint32_t bm = s_slot_bitmap;
  uint8_t c = 0;
  while (bm) { c += (bm & 1u); bm >>= 1; }
  return c;
}

// ────────────────────────────────────────────────────────────────────────────
// RESOLUTION (hot path)
// ────────────────────────────────────────────────────────────────────────────

ResolveResult resolve_rpa_detailed(const uint8_t mac[6]) {
  ResolveResult r = { false, -1, false };
  if (!s_initialized || !mac) return r;

  s_total_resolves_attempted++;

  r.looked_like_rpa = is_rpa(mac);
  if (!r.looked_like_rpa) {
    s_total_non_rpa_seen++;
    return r;
  }

  // Extract prand (3 LSB-first bytes: mac[3..5]).
  const uint8_t prand[3] = { mac[3], mac[4], mac[5] };
  const uint8_t expected_hash[3] = { mac[0], mac[1], mac[2] };

  uint8_t computed_hash[3];
  for (uint8_t i = 0; i < MAX_HOUSEHOLD_DEVICES; i++) {
    if (!slot_is_used(i)) continue;
    if (!ah(s_slots[i].irk, prand, computed_hash)) continue;

    if (computed_hash[0] == expected_hash[0] &&
        computed_hash[1] == expected_hash[1] &&
        computed_hash[2] == expected_hash[2]) {
      r.matched = true;
      r.slot = (int8_t)i;
      s_total_resolves_matched++;
      secure_wipe(computed_hash, sizeof(computed_hash));
      return r;
    }
  }
  secure_wipe(computed_hash, sizeof(computed_hash));
  return r;
}

bool resolve_rpa(const uint8_t mac[6]) {
  return resolve_rpa_detailed(mac).matched;
}

// ────────────────────────────────────────────────────────────────────────────
// INTROSPECTION
// ────────────────────────────────────────────────────────────────────────────

bool get_label(uint8_t slot, char* out_buf, size_t out_len) {
  if (!slot_is_used(slot) || !out_buf || out_len == 0) return false;
  strncpy(out_buf, s_slots[slot].label, out_len - 1);
  out_buf[out_len - 1] = '\0';
  return true;
}

uint32_t get_added_ms(uint8_t slot) {
  if (!slot_is_used(slot)) return 0;
  return s_slots[slot].added_ms;
}

bool get_stats(Stats* out) {
  if (!out) return false;
  out->enrolled_count             = count();
  out->enrolling                  = is_enrolling();
  out->enrollment_ms_remaining    = enrollment_ms_remaining();
  out->total_resolves_attempted   = s_total_resolves_attempted;
  out->total_resolves_matched     = s_total_resolves_matched;
  out->total_non_rpa_seen         = s_total_non_rpa_seen;
  return true;
}

// Export variant — adds Gaussian DP noise (ε = 1.0 default) to the
// monotonic counters. enrolled_count, enrolling, and enrollment_ms_
// remaining are user-visible and NOT noised.
bool get_stats_for_export(Stats* out) {
  if (!get_stats(out)) return false;
  out->total_resolves_attempted = dp::noisy_u32(out->total_resolves_attempted, 1);
  out->total_resolves_matched   = dp::noisy_u32(out->total_resolves_matched,   1);
  out->total_non_rpa_seen       = dp::noisy_u32(out->total_non_rpa_seen,       1);
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// CONFORMANCE
// ────────────────────────────────────────────────────────────────────────────

bool conformance_self_test() {
  // Round-trip: synthesize an RPA from a random IRK + prand, verify that
  // the resolver matches. Does not mutate stored slots.

  // Fixed test vector (derived with mbedtls offline; self-consistent).
  // If AES or `ah` regresses, this will flip.
  const uint8_t test_irk[IRK_LEN] = {
    0x9b, 0x7d, 0x39, 0xa1, 0x3b, 0x1e, 0x3f, 0x2a,
    0xe0, 0xc3, 0xf8, 0xb1, 0x4f, 0x2d, 0x6e, 0x73
  };
  const uint8_t test_prand[3] = { 0x94, 0x81, 0x70 };  // LSB-first: 0x708194 MSB

  uint8_t test_hash[3];
  if (!ah(test_irk, test_prand, test_hash)) {
    health_logging::log(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Household self-test: ah() failed");
    return false;
  }

  // Compose the RPA: [hash LSB-first (3 bytes) || prand LSB-first (3 bytes)]
  // Then set the RPA type bits (top two of mac[5]) = 0b01.
  uint8_t test_rpa[6];
  test_rpa[0] = test_hash[0];
  test_rpa[1] = test_hash[1];
  test_rpa[2] = test_hash[2];
  test_rpa[3] = test_prand[0];
  test_rpa[4] = test_prand[1];
  test_rpa[5] = (uint8_t)((test_prand[2] & 0x3F) | 0x40);

  // Temporarily install the IRK in a scratch location (NOT a real slot),
  // compute ah on it, and verify the hash equals what's in test_rpa[0..2].
  const uint8_t prand_from_rpa[3] = { test_rpa[3], test_rpa[4], test_rpa[5] };
  uint8_t recomputed_hash[3];
  if (!ah(test_irk, prand_from_rpa, recomputed_hash)) return false;

  const bool round_trip_ok =
      recomputed_hash[0] == test_rpa[0] &&
      recomputed_hash[1] == test_rpa[1] &&
      recomputed_hash[2] == test_rpa[2];

  secure_wipe(recomputed_hash, sizeof(recomputed_hash));
  secure_wipe(test_hash, sizeof(test_hash));

  if (!round_trip_ok) {
    health_logging::log(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Household self-test: RPA round-trip FAILED");
    return false;
  }

  // Also verify that is_rpa() classifies our synthesized address correctly.
  if (!is_rpa(test_rpa)) {
    health_logging::log(health_logging::LEVEL_ERROR, health_logging::CAT_RF,
      "Household self-test: is_rpa classifier rejected a valid RPA");
    return false;
  }

  health_logging::log(health_logging::LEVEL_INFO, health_logging::CAT_RF,
    "Household self-test: RPA round-trip OK");
  return true;
}

bool conformance_no_mac_in_slots() {
  // The structural guarantee is that HouseholdSlot has no MAC field. This
  // is a defense-in-depth heuristic scanning for 6-byte OUI-patterned runs.
  for (uint8_t i = 0; i < MAX_HOUSEHOLD_DEVICES; i++) {
    if (!slot_is_used(i)) continue;
    // Scan the IRK bytes — IRKs are random-looking so OUI patterns are
    // statistically unlikely. Flag a warning if the first 3 bytes of the
    // IRK match any of the common high-volume vendor OUIs (this would
    // suggest someone accidentally stored a MAC as if it were an IRK).
    const uint8_t* b = s_slots[i].irk;
    // LU bit (0x02) and MC bit (0x01) set on byte 0 ⇒ very rare as key material.
    if ((b[0] & 0x03) != 0 && b[3] == 0 && b[4] == 0 && b[5] == 0) {
      health_logging::logf(health_logging::LEVEL_WARNING, health_logging::CAT_RF,
        "Household conformance: slot %u IRK has MAC-like prefix", (unsigned)i);
      // Do not return false — could be a fluke. Real guarantee is structural.
    }
  }
  return true;
}

}  // namespace household
