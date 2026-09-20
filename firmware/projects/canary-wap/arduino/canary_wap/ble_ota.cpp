/*
 * SecuraCV Canary — BLE OTA over GATT (Ed25519-signed) — implementation
 */

#include "build_config.h"

#if FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)

#include "ble_ota.h"

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <Ed25519.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <Arduino.h>
#include <string.h>

#include "health_log.h"
#include "securacv_ota.h"  // pull-OTA engine state — the two channels exclude each other

namespace ble_ota {

// ════════════════════════════════════════════════════════════════════════════
// STATE
// ════════════════════════════════════════════════════════════════════════════

static OtaState                    g_state           = OTA_IDLE;
static const uint8_t*              g_pubkey          = nullptr;
static uint32_t                    g_image_size      = 0;
static uint32_t                    g_received        = 0;
static uint8_t                     g_expected_sha[32] = {};
static char                        g_version[33]     = {};
static const esp_partition_t*      g_ota_partition   = nullptr;
static esp_ota_handle_t            g_ota_handle      = 0;
static mbedtls_sha256_context      g_sha_ctx;
static bool                        g_sha_active      = false;
static char                        g_last_error[64]  = {0};
static uint32_t                    g_last_notify_pct = 0;

// Policy inputs bound by configure(). NULL until the sketch binds them,
// and every BEGIN is refused until then (fail closed).
static const char*                 g_product         = nullptr;
static const char*                 g_running_version = nullptr;
static BreakGlassHook              g_break_glass     = nullptr;
static bool                        g_last_break_glass = false;

static NimBLEService*             g_service = nullptr;
static NimBLECharacteristic*      g_control = nullptr;
static NimBLECharacteristic*      g_data    = nullptr;
static NimBLECharacteristic*      g_status  = nullptr;

// ════════════════════════════════════════════════════════════════════════════
// HELPERS
// ════════════════════════════════════════════════════════════════════════════

static void notify_status() {
  if (!g_status) return;
  uint8_t pkt[8] = {};
  pkt[0] = (uint8_t)g_state;
  uint32_t pct = g_image_size > 0
                   ? (uint32_t)((uint64_t)g_received * 100 / g_image_size)
                   : 0;
  pkt[1] = (uint8_t)pct;
  uint32_t bytes_left = g_image_size > g_received ? g_image_size - g_received : 0;
  memcpy(&pkt[2], &bytes_left, 4);
  g_status->setValue(pkt, sizeof(pkt));
  g_status->notify();
}

static void cleanup_sha() {
  if (g_sha_active) {
    mbedtls_sha256_free(&g_sha_ctx);
    g_sha_active = false;
  }
}

static void abort_ota(const char* reason) {
  cleanup_sha();
  // A refused or failed session is not a break-glass session, whatever the
  // previous accepted BEGIN was; /api/bluetooth/ota reports the flag "for
  // that session" (docs/firmware_ota.md).
  g_last_break_glass = false;
  if (g_ota_handle) {
    esp_ota_abort(g_ota_handle);
    g_ota_handle = 0;
  }
  strncpy(g_last_error, reason ? reason : "unknown", sizeof(g_last_error) - 1);
  g_last_error[sizeof(g_last_error) - 1] = '\0';
  log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH, "OTA aborted", g_last_error);
  g_state = OTA_FAILED;
  notify_status();
}

static bool pubkey_provisioned() {
  return policy_detail::pubkey_provisioned(g_pubkey);
}

// Crypto's Ed25519::verify behind the policy's injected-primitive shape.
static bool ed25519_verify_adapter(const uint8_t sig[64], const uint8_t pub[32],
                                   const uint8_t* msg, size_t msg_len) {
  return Ed25519::verify(sig, pub, msg, msg_len);
}

// ════════════════════════════════════════════════════════════════════════════
// CONTROL CHARACTERISTIC
// ════════════════════════════════════════════════════════════════════════════

class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*info*/) override {
    std::string val = c->getValue();
    if (val.empty()) return;

    uint8_t cmd = (uint8_t)val[0];

    // ── ABORT ────────────────────────────────────────────────────────────
    if (cmd == CMD_ABORT) {
      if (g_state != OTA_IDLE) abort_ota("aborted by client");
      return;
    }

    // ── BEGIN / BEGIN_V2 ─────────────────────────────────────────────────
    if (cmd != CMD_BEGIN_V1 && cmd != CMD_BEGIN_V2) {
      abort_ota("unknown command");
      return;
    }

    if (g_state == OTA_RECEIVING || g_state == OTA_VERIFYING) {
      // Don't allow a second BEGIN to corrupt an in-flight session.
      abort_ota("BEGIN while session active");
      return;
    }

    if (securacv_ota_get_state() != SECURACV_OTA_IDLE) {
      // The pull-OTA engine is mid-check/download/flash. Two writers on
      // the inactive partition would corrupt both updates; the WiFi
      // session was first, so it wins.
      abort_ota("busy with a network update");
      return;
    }

    if (!pubkey_provisioned()) {
      abort_ota(REASON_UNPROVISIONED);
      return;
    }

    // Parse the header for the command that announced it. Everything the
    // policy acts on comes from the parsed copy, never the raw bytes.
    ParsedHeader hdr;
    const uint8_t* payload = (const uint8_t*)val.data() + 1;
    const size_t   payload_len = val.size() - 1;
    const bool parsed = (cmd == CMD_BEGIN_V2)
                          ? parse_v2(payload, payload_len, &hdr)
                          : parse_v1(payload, payload_len, &hdr);
    if (!parsed) {
      abort_ota(cmd == CMD_BEGIN_V2 ? REASON_MALFORMED : "BEGIN payload truncated");
      return;
    }

    // Verify the release signature and apply product binding + the
    // anti-rollback floor BEFORE touching the OTA partition, so a forged
    // or stale BEGIN can't even start an erase cycle on the inactive
    // partition. The floor is the pull engine's: max(running, NVS floor).
    char nvs_floor[SECURACV_OTA_VERSION_MAX] = {0};
    securacv_ota_get_min_version(nvs_floor, sizeof(nvs_floor));

    PolicyDeps deps;
    deps.ed25519_verify  = ed25519_verify_adapter;
    deps.release_pubkey  = g_pubkey;
    deps.product         = g_product;
    deps.running_version = g_running_version;
    deps.nvs_floor       = nvs_floor;

    Verdict verdict = decide(hdr, deps);
    if (verdict.decision == DECISION_REFUSE) {
      abort_ota(verdict.reason);
      return;
    }

    // Sanity BEFORE the owner's arming is spent: an image that cannot fit
    // (or a missing OTA slot) is refused without consuming the one BOOT tap
    // the owner gave, so a malformed rescue does not burn the 30 s window.
    g_ota_partition = esp_ota_get_next_update_partition(nullptr);
    if (!g_ota_partition) {
      abort_ota("no OTA partition available");
      return;
    }
    if (hdr.image_size == 0 || hdr.image_size > g_ota_partition->size) {
      abort_ota("image size out of range");
      return;
    }

    if (verdict.decision == DECISION_NEEDS_BREAK_GLASS) {
      // Only now is the owner's arming consulted — and consumed, if set.
      // A v2 header at or above the floor never reaches this line.
      const bool armed = (g_break_glass != nullptr) && g_break_glass();
      verdict = apply_break_glass(verdict, armed);
      if (verdict.decision == DECISION_REFUSE) {
        abort_ota(verdict.reason);
        return;
      }
    }

    // v2: the version is inside the signed canonical message, so it is
    // the release's own claim and labels the update-outcome record on the
    // next boot. v1: parse_v1 sanitized it to printable ASCII and it is
    // display data only — the "applied" determination never trusts it
    // (that compares the marker against the firmware's compiled version).
    snprintf(g_version, sizeof(g_version), "%s", hdr.version);

    g_last_break_glass = (verdict.decision == DECISION_ACCEPT_BREAK_GLASS);
    if (g_last_break_glass) {
      // The one line the audit trail needs, written the moment the gate is
      // spent — before esp_ota_begin can fail — so the health log always
      // says WHAT was bypassed, for WHICH version, against WHICH floor.
      // No key material, no signature bytes.
      char detail[96];
      if (verdict.need == BG_V1) {
        snprintf(detail, sizeof(detail),
                 "v1 header, product/floor unchecked, version=%.31s", g_version);
      } else {
        snprintf(detail, sizeof(detail),
                 "v2 %.31s below floor (running %.15s, nvs %.15s)",
                 g_version, g_running_version, nvs_floor[0] ? nvs_floor : "-");
      }
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
                 "OTA break-glass: anti-rollback floor bypassed", detail);
    }

    esp_err_t err = esp_ota_begin(g_ota_partition, hdr.image_size, &g_ota_handle);
    if (err != ESP_OK) {
      abort_ota("esp_ota_begin failed");
      return;
    }

    g_image_size = hdr.image_size;
    g_received   = 0;
    memcpy(g_expected_sha, hdr.sha256, 32);

    cleanup_sha();
    mbedtls_sha256_init(&g_sha_ctx);
    if (mbedtls_sha256_starts(&g_sha_ctx, 0) != 0) {  // 0 = SHA-256
      abort_ota("sha256_starts failed");
      return;
    }
    g_sha_active = true;

    g_state = OTA_RECEIVING;
    g_last_error[0] = '\0';
    g_last_notify_pct = 0;
    char detail[56];
    snprintf(detail, sizeof(detail), "%s %u bytes, version=%.31s",
             hdr.kind == HDR_V2 ? "v2" : "v1", (unsigned)g_image_size, g_version);
    log_health(SCV_LOG_NOTICE, SCV_CAT_BLUETOOTH, "OTA session begun", detail);
    notify_status();
  }
};

// ════════════════════════════════════════════════════════════════════════════
// DATA CHARACTERISTIC
// ════════════════════════════════════════════════════════════════════════════

class DataCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*info*/) override {
    if (g_state != OTA_RECEIVING) return;

    std::string val = c->getValue();
    if (val.empty()) return;

    if (g_received + val.size() > g_image_size) {
      abort_ota("overrun");
      return;
    }

    esp_err_t err = esp_ota_write(g_ota_handle, val.data(), val.size());
    if (err != ESP_OK) {
      abort_ota("esp_ota_write failed");
      return;
    }

    if (mbedtls_sha256_update(&g_sha_ctx, (const uint8_t*)val.data(), val.size()) != 0) {
      abort_ota("sha256_update failed");
      return;
    }

    g_received += val.size();

    // Notify on every percent change to avoid flooding the link.
    uint32_t pct = (uint32_t)((uint64_t)g_received * 100 / g_image_size);
    if (pct != g_last_notify_pct) {
      g_last_notify_pct = pct;
      notify_status();
    }

    if (g_received < g_image_size) return;

    // ── Image complete — verify and finalize ───────────────────────────
    g_state = OTA_VERIFYING;
    notify_status();

    uint8_t actual_sha[32];
    if (mbedtls_sha256_finish(&g_sha_ctx, actual_sha) != 0) {
      abort_ota("sha256_finish failed");
      return;
    }
    cleanup_sha();

    if (memcmp(actual_sha, g_expected_sha, 32) != 0) {
      // The header was signed but the image bytes don't hash to the
      // signed digest — either corruption or substitution mid-stream.
      abort_ota("SHA-256 mismatch");
      return;
    }

    err = esp_ota_end(g_ota_handle);
    g_ota_handle = 0;
    if (err != ESP_OK) {
      // esp_ota_end runs the bootloader's own image validation. If it
      // fails the image isn't a valid ESP32 firmware (wrong magic,
      // truncated, etc.) even though our hash matched.
      abort_ota("esp_ota_end failed");
      return;
    }

    err = esp_ota_set_boot_partition(g_ota_partition);
    if (err != ESP_OK) {
      abort_ota("set_boot_partition failed");
      return;
    }

    g_state = OTA_REBOOTING;
    notify_status();

    // Same outcome bookkeeping as the pull-OTA path: the next boot reads
    // this marker to witness applied vs rolled-back, and the image boots
    // PENDING_VERIFY (the engine owns rollback confirmation), so a broken
    // BLE-pushed image also auto-reverts on its first bad boot.
    securacv_ota_mark_pending_install(g_version);

    log_health(SCV_LOG_NOTICE, SCV_CAT_BLUETOOTH, "OTA complete — rebooting", nullptr);
    // Give NimBLE half a second to flush the final notification before we
    // pull the rug out from under it.
    delay(500);
    esp_restart();
  }
};

static ControlCallbacks g_control_cb;
static DataCallbacks    g_data_cb;

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

void configure(const char* product, const char* running_version, BreakGlassHook hook) {
  g_product         = product;
  g_running_version = running_version;
  g_break_glass     = hook;
}

bool init(NimBLEServer* server, const uint8_t release_pubkey[32]) {
  if (!server || !release_pubkey) return false;
  if (g_service) return true;  // already registered

  g_pubkey = release_pubkey;

  g_service = server->createService(OTA_SERVICE_UUID);
  if (!g_service) return false;

  // Control: writable + notifies state transitions back.
  g_control = g_service->createCharacteristic(
    OTA_CONTROL_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
  );
  g_control->setCallbacks(&g_control_cb);

  // Data: writable both ack'd and unack'd; the unack'd path is critical for
  // throughput, since the alternative round-trips every chunk.
  g_data = g_service->createCharacteristic(
    OTA_DATA_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  g_data->setCallbacks(&g_data_cb);

  // Status: read-or-subscribe for progress polling.
  g_status = g_service->createCharacteristic(
    OTA_STATUS_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  uint8_t initial[8] = { (uint8_t)OTA_IDLE, 0, 0, 0, 0, 0, 0, 0 };
  g_status->setValue(initial, sizeof(initial));

  g_service->start();

  if (!pubkey_provisioned()) {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "BLE OTA registered but disabled — release pubkey is all zeros",
               nullptr);
  } else if (!g_product || !g_running_version) {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "BLE OTA registered but disabled — policy not configured",
               nullptr);
  } else {
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE OTA service ready", nullptr);
  }
  return true;
}

OtaState    get_state()              { return g_state; }
uint32_t    get_progress_percent()   {
  return g_image_size > 0
           ? (uint32_t)((uint64_t)g_received * 100 / g_image_size)
           : 0;
}
uint32_t    get_image_size()         { return g_image_size; }
uint32_t    get_bytes_received()     { return g_received; }
const char* last_error()             { return g_last_error; }
bool        last_break_glass()       { return g_last_break_glass; }

const char* state_name(OtaState s) {
  switch (s) {
    case OTA_IDLE:      return "idle";
    case OTA_RECEIVING: return "receiving";
    case OTA_VERIFYING: return "verifying";
    case OTA_REBOOTING: return "rebooting";
    case OTA_FAILED:    return "failed";
    default:            return "unknown";
  }
}

} // namespace ble_ota

#endif // FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)
