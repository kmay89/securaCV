/*
 * SecuraCV Canary — Mesh-layer NVS persistence — Implementation
 *
 * Device build (ESP32): wraps Arduino Preferences on the canonical
 * "securacv" NVS namespace. Same pattern as ble_scout_key.cpp.
 *
 * Host build (CSI_TEST_HOST_BUILD): deterministic stubs that don't
 * touch any storage. load returns false; save/clear are no-op success.
 */

#include "mesh_state.h"

#include <string.h>

#ifndef CSI_TEST_HOST_BUILD
  #include <Arduino.h>
  #include <Preferences.h>
#endif

namespace mesh_state {

namespace {

#ifndef CSI_TEST_HOST_BUILD
/* Mirrors ble_scout_key.cpp + securacv_audio.cpp — single canonical
 * "securacv" namespace so the same NVS partition entry holds all
 * canary keys. 15-char key budget per ESP32 NVS rules; "opera_secret"
 * is exactly 12. */
constexpr const char* NVS_NAMESPACE = "securacv";
constexpr const char* NVS_KEY_OPERA = "opera_secret";
#endif

}  /* namespace */

bool save_opera_secret(const uint8_t secret[mesh_crypto::OPERA_SECRET_LEN]) {
  if (secret == nullptr) return false;

#ifdef CSI_TEST_HOST_BUILD
  /* Host build: succeed silently — tests use in-memory state, not NVS. */
  return true;
#else
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    return false;
  }
  const size_t put = prefs.putBytes(NVS_KEY_OPERA, secret,
                                    mesh_crypto::OPERA_SECRET_LEN);
  prefs.end();
  return put == mesh_crypto::OPERA_SECRET_LEN;
#endif
}

bool load_opera_secret(uint8_t out[mesh_crypto::OPERA_SECRET_LEN]) {
  if (out == nullptr) return false;

#ifdef CSI_TEST_HOST_BUILD
  /* Host build: always report "no secret persisted" so tests start
   * in a clean unpaired state. */
  return false;
#else
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
    return false;
  }
  const size_t got = prefs.getBytes(NVS_KEY_OPERA, out,
                                    mesh_crypto::OPERA_SECRET_LEN);
  prefs.end();
  return got == mesh_crypto::OPERA_SECRET_LEN;
#endif
}

bool clear_opera_secret() {
#ifdef CSI_TEST_HOST_BUILD
  return true;
#else
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return false;
  /* Preferences::remove returns true on success OR on "key absent" —
   * we treat both as success (idempotent factory reset). */
  prefs.remove(NVS_KEY_OPERA);
  prefs.end();
  return true;
#endif
}

}  /* namespace mesh_state */
