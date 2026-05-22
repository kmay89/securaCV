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
  #include <esp_flash_encrypt.h>   /* esp_flash_encryption_enabled() */
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

/* Project invariant (AGENTS.md §"NVS write paths"): persisting the
 * household opera_secret to NVS requires flash encryption to be
 * active. canary-wap's mesh_network.cpp:1053 enforces the same check
 * on its opera_config write/load paths. Devices without FE blown
 * simply cannot store an opera_secret — the user must complete
 * pairing on every boot until the eFuse is committed. */
inline bool flash_encryption_enabled() {
  return esp_flash_encryption_enabled();
}
#endif

}  /* namespace */

bool save_opera_secret(const uint8_t secret[mesh_crypto::OPERA_SECRET_LEN]) {
  if (secret == nullptr) return false;

#ifdef CSI_TEST_HOST_BUILD
  /* Host build: succeed silently — tests use in-memory state, not NVS.
   * The flash-encryption gate is a hardware-only concern, not a logic
   * concern; we don't simulate it on host. */
  return true;
#else
  if (!flash_encryption_enabled()) {
    /* AGENTS.md invariant: refuse to persist on FE-off devices.
     * Serial-log so the developer/operator sees WHY pairing won't
     * survive a reboot. log_health would be a circular dep on the
     * witness lib from this lib; Serial is sufficient. */
    Serial.println("[ALERT][mesh_state] refused save_opera_secret — "
                   "flash encryption disabled (audit O2 / AGENTS.md)");
    return false;
  }

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
  if (!flash_encryption_enabled()) {
    /* Match canary-wap mesh_network.cpp:1080 — if FE is off we
     * MUST NOT load any stored secret either. A device with FE
     * unblown but a previously-persisted secret (e.g. the eFuse was
     * disabled after pairing) is in a misconfigured state; report
     * "no secret" so the firmware boots as unpaired. */
    Serial.println("[ALERT][mesh_state] refused load_opera_secret — "
                   "flash encryption disabled (audit O2 / AGENTS.md)");
    return false;
  }

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
  /* Preferences::remove returns false when the key is absent OR on a
   * real NVS error (flash corruption, hardware fault). To make this
   * call idempotent across factory-reset semantics WITHOUT swallowing
   * real failures, we check isKey() on the false path:
   *   • remove → true                 ⇒ erased successfully
   *   • remove → false, isKey → false ⇒ already absent, idempotent OK
   *   • remove → false, isKey → true  ⇒ real NVS write failure */
  bool ok = prefs.remove(NVS_KEY_OPERA);
  if (!ok) {
    ok = !prefs.isKey(NVS_KEY_OPERA);
  }
  prefs.end();
  return ok;
#endif
}

}  /* namespace mesh_state */
