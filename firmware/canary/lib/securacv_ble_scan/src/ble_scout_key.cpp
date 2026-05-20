/*
 * SecuraCV Canary — BLE Scout per-device key store — Implementation
 *
 * Device build (ESP32): uses Arduino Preferences (wraps nvs_flash)
 * under the "securacv" namespace, key "scout_key".
 *
 * Host build (CSI_TEST_HOST_BUILD): provides a deterministic
 * 32-byte test key (0x00..0x1F) so tests are reproducible.
 */

#include "ble_scout_key.h"

#include <string.h>

#ifndef CSI_TEST_HOST_BUILD
  #include <Arduino.h>
  #include <Preferences.h>
  #include <esp_system.h>   /* esp_fill_random */
#endif

namespace ble_scout {

namespace {

#ifndef CSI_TEST_HOST_BUILD
/* Reuse the existing "securacv" namespace (same as mic-mute /
 * opera_secret) so the NVS layout doesn't require a new partition
 * entry. 15-char key budget — "scout_key" is well under that. */
constexpr const char* NVS_NAMESPACE = "securacv";
constexpr const char* NVS_KEY       = "scout_key";
#endif

uint8_t s_key[ble_scan::PER_DEVICE_KEY_LEN];
bool    s_loaded = false;

/* DCE-safe wipe — same pattern as mesh_crypto::secure_zero. */
void secure_zero(void* p, size_t n) {
  volatile uint8_t* v = (volatile uint8_t*)p;
  for (size_t i = 0; i < n; ++i) v[i] = 0;
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" ::: "memory");
#endif
}

}  /* namespace */

bool ble_scout_key_init() {
  if (s_loaded) return true;

#ifdef CSI_TEST_HOST_BUILD
  /* Deterministic test key: 0x00, 0x01, ..., 0x1F. */
  for (size_t i = 0; i < ble_scan::PER_DEVICE_KEY_LEN; ++i) {
    s_key[i] = (uint8_t)i;
  }
  s_loaded = true;
  return true;
#else
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    return false;
  }

  /* Try to read an existing key. getBytes returns the number of bytes
   * read; 0 means the key is absent. */
  size_t got = prefs.getBytes(NVS_KEY, s_key, sizeof(s_key));
  if (got == sizeof(s_key)) {
    prefs.end();
    s_loaded = true;
    return true;
  }

  /* Absent (or partial/corrupt — same handling): generate a fresh
   * 32-byte key from the hardware RNG and persist it. */
  esp_fill_random(s_key, sizeof(s_key));
  const size_t put = prefs.putBytes(NVS_KEY, s_key, sizeof(s_key));
  prefs.end();
  if (put != sizeof(s_key)) {
    /* Persistence failed — wipe and bail. Don't half-trust an
     * un-persisted key; the next boot would generate a different one
     * and silently invalidate every paired beacon. */
    secure_zero(s_key, sizeof(s_key));
    return false;
  }
  s_loaded = true;
  return true;
#endif
}

bool ble_scout_key_loaded() {
  return s_loaded;
}

bool ble_scout_key_hash_beacon(const uint8_t mac[ble_scan::MAC_LEN],
                               uint8_t       out[ble_scan::HASHED_ID_LEN]) {
  if (!s_loaded) return false;
  return ble_scan::hash_beacon_id(s_key, mac, out);
}

void ble_scout_key_deinit() {
  secure_zero(s_key, sizeof(s_key));
  s_loaded = false;
}

bool ble_scout_key_factory_reset() {
#ifdef CSI_TEST_HOST_BUILD
  ble_scout_key_deinit();
  return true;
#else
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return false;
  const bool ok = prefs.remove(NVS_KEY);
  prefs.end();
  ble_scout_key_deinit();
  return ok;
#endif
}

}  /* namespace ble_scout */
