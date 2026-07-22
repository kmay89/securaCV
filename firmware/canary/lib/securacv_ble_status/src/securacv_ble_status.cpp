/*
 * SecuraCV Canary — BLE GATT Status Service Implementation
 *
 * Uses NimBLE to expose device status over BLE GATT. A companion phone
 * can connect and read battery level, health score, chain sequence,
 * firmware version, and degradation state without needing WiFi.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_ble_status.h"

#if FEATURE_BLE_STATUS

#include <NimBLEDevice.h>
#include <fleet_beacon.h>
#include "securacv_witness.h"
#include "securacv_diagnostics.h"
#include "securacv_power.h"
#include "log_level.h"

#if FEATURE_SETUP_WIZARD
#include "securacv_setup.h"
#endif

/* ════════════════════════════════════════════════════════════════════════════ */
/*  STATIC STATE                                                              */
/* ════════════════════════════════════════════════════════════════════════════ */

static NimBLEServer*          s_server          = nullptr;
static NimBLECharacteristic*  s_batt_level_char = nullptr;
static NimBLECharacteristic*  s_dev_name_char   = nullptr;
static NimBLECharacteristic*  s_fw_ver_char     = nullptr;
static NimBLECharacteristic*  s_chain_seq_char  = nullptr;
static NimBLECharacteristic*  s_health_char     = nullptr;
static NimBLECharacteristic*  s_degrade_char    = nullptr;
static NimBLECharacteristic*  s_uptime_char     = nullptr;
static NimBLECharacteristic*  s_sd_usage_char   = nullptr;

static bool     s_initialized      = false;
static bool     s_connected        = false;
static uint32_t s_last_update_ms   = 0;

/* Fleet-link presence beacon fingerprint (fp2): the last 2 bytes of this
 * device's Ed25519 pubkey fingerprint. Fixed once at init from
 * witness_get_device().pubkey_fp[6..7] — the same 2 bytes the "SCV-XXXX"
 * device id derives from — so a canary-display can resolve the device from
 * the broker-free beacon alone. See <fleet_beacon.h> for the wire contract. */
static uint8_t  s_beacon_fp[2]     = {0, 0};

/* ════════════════════════════════════════════════════════════════════════════ */
/*  CONNECTION CALLBACKS                                                      */
/* ════════════════════════════════════════════════════════════════════════════ */

class BleStatusCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    s_connected = true;
    log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH,
               "BLE client connected", nullptr);
    /* Allow multiple connections by restarting advertising. Most phones
     * only need one, but this avoids locking out a second observer. */
    NimBLEDevice::startAdvertising();
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    s_connected = (pServer->getConnectedCount() > 0);
    log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH,
               "BLE client disconnected", nullptr);
    NimBLEDevice::startAdvertising();
  }
};

static BleStatusCallbacks s_callbacks;

/* ════════════════════════════════════════════════════════════════════════════ */
/*  INIT                                                                      */
/* ════════════════════════════════════════════════════════════════════════════ */

/* Resolve the BLE device name: prefer the user-configured setup-wizard name,
 * fall back to the device_id. Returns a pointer to a static buffer. */
static const char* resolve_ble_name(void) {
#if FEATURE_SETUP_WIZARD
  static char setup_name[SETUP_DEVICE_NAME_MAX + 1];
  if (setup_get_device_name(setup_name, sizeof(setup_name)) && setup_name[0]) {
    return setup_name;
  }
#endif
  return witness_get_device().device_id;
}

/* ════════════════════════════════════════════════════════════════════════════ */
/*  FLEET-LINK PRESENCE BEACON ADVERT (single (re)build chokepoint)           */
/* ════════════════════════════════════════════════════════════════════════════ */

/* Build the 11-byte fleet-link presence beacon from this canary's live state
 * and install it as the PRIMARY advertisement, moving the service UUIDs +
 * device name into the SCAN RESPONSE. A canary-display active-scans, so it
 * still resolves both — backward compatible with the old UUID-only advert,
 * while adding a broker-free / WiFi-free presence beacon a display finds
 * directly. Mirrors the canary-wap sketch's ble_opera::applyBeaconAdvertising().
 *
 * Values are sourced from canary's real modules; fields without a clean source
 * carry the documented 0xFF/unknown sentinel or a cleared flag — never a
 * fabricated value:
 *   flags        tamper (witness tamper_active), degraded (diag degrade level),
 *                on_wifi_sta (system health wifi_active). mic_muted / alert are
 *                left clear (canary exposes no clean signal at this layer).
 *   battery_pct  power_get_state().soc_pct, else 0xFF unknown.
 *   health_pct   diag self-test health_score (once it has run), else 0xFF.
 *   chain_height witness device seq (low 16 bits ride the wire).
 *   fp2          s_beacon_fp — last 2 bytes of the pubkey fingerprint.
 *
 * Fail-safe: if the explicit adv-data path fails for any reason, fall back to
 * today's addServiceUUID + startAdvertising behavior so the device is NEVER
 * left un-advertised. Returns true when the beacon path took, false on
 * fallback. Cheap — safe to call from the periodic ble_status_update(). */
static bool apply_beacon_advertising(void) {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) return false;

  uint8_t  flags        = 0;
  int      battery_pct  = -1;   /* -1 -> 0xFF unknown on the wire */
  int      health_pct   = -1;   /* -1 -> 0xFF unknown on the wire */
  uint32_t chain_height = 0;

  /* Chain height + tamper flag from the witness identity. */
  {
    DeviceIdentity& dev = witness_get_device();
    chain_height = dev.seq;
    if (dev.tamper_active) flags |= FLEET_BEACON_FLAG_TAMPER;
  }

  /* On-WiFi-STA flag from system health. */
  {
    SystemHealth& health = witness_get_health();
    if (health.wifi_active) flags |= FLEET_BEACON_FLAG_ON_WIFI_STA;
  }

#if FEATURE_POWER_MONITOR
  {
    power_state_t pwr;
    if (power_get_state(&pwr)) {
      battery_pct = (int)pwr.soc_pct;  /* fleet_beacon_pct() clamps >100 -> 0xFF */
    }
  }
#endif

#if FEATURE_DIAGNOSTICS
  {
    selftest_report_t st;
    if (diag_get_selftest(&st) && st.has_run) {
      health_pct = (int)st.health_score;
    }
    if (diag_get_degrade_level() != DEGRADE_NONE) {
      flags |= FLEET_BEACON_FLAG_DEGRADED;
    }
  }
#endif

  /* Payload bytes [2..10]; NimBLE prepends the 2 company-id bytes. */
  uint8_t payload[FLEET_BEACON_PAYLOAD_LEN];
  fleet_beacon_build(payload, flags, battery_pct, health_pct, chain_height,
                     s_beacon_fp[0], s_beacon_fp[1]);

  /* Full manufacturer blob = company id (LE) + payload. */
  uint8_t mfg[FLEET_BEACON_PAYLOAD_LEN + 2];
  mfg[0] = FLEET_BEACON_COMPANY_ID & 0xFF;
  mfg[1] = (FLEET_BEACON_COMPANY_ID >> 8) & 0xFF;
  memcpy(&mfg[2], payload, FLEET_BEACON_PAYLOAD_LEN);

  NimBLEAdvertisementData advData;
  advData.setManufacturerData(std::string((char*)mfg, sizeof(mfg)));

  /* Service UUIDs + name move to the scan response (still discoverable by an
   * active scanner) to make room for the beacon in the primary advert. */
  NimBLEAdvertisementData scanData;
  scanData.setName(resolve_ble_name());
  scanData.addServiceUUID(BLE_BATTERY_SERVICE_UUID);
  scanData.addServiceUUID(BLE_SCV_SERVICE_UUID);

  bool ok = adv->setAdvertisementData(advData);
  ok = adv->setScanResponseData(scanData) && ok;
  if (!ok) {
    /* Fallback: legacy UUID + name advert (pre-beacon behavior). Never leave
     * the device un-advertised. */
    adv->addServiceUUID(BLE_BATTERY_SERVICE_UUID);
    adv->addServiceUUID(BLE_SCV_SERVICE_UUID);
    return false;
  }
  return true;
}

bool ble_status_stack_begin(void) {
  /* Already up (an earlier caller, or ble_status_init re-entry) — the first
   * caller owns the GAP name + TX power; don't disturb it. */
  if (NimBLEDevice::isInitialized()) return true;

  const char* ble_name = resolve_ble_name();

  /* NimBLE 2.x init() returns false when the controller/host stack can't come
   * up (BT compiled out, radio unavailable, coexistence/heap failure). Honor
   * it instead of marching on to createServer() (which would then return null);
   * this is the documented graceful-degradation contract. */
  if (!NimBLEDevice::init(ble_name)) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_BLUETOOTH,
               "NimBLE init failed", ble_name);
    return false;
  }
  /* Single TX-power owner (mirrors canary-wap's F5 fix): set once at stack
   * bring-up. BLE_TX_POWER_DBM defaults to +9 dBm — the S3 max, aligned with
   * canary-wap's NVS default and sized for the XIAO's external-antenna-only
   * RF path. */
  NimBLEDevice::setPower(BLE_TX_POWER_DBM);

  /* Bump default ATT MTU to 247 (244-byte payload). The 23-byte default
   * fragments every status read (battery/health/chain JSON) into 3+ ATT
   * packets, ~tripling connection-event radio time; 247 is the largest a
   * single LE Data Length Extension packet carries without fragmentation and
   * both iOS and modern Android accept it. (Matches the canary-wap sketch's
   * bluetooth_channel.) */
  NimBLEDevice::setMTU(247);
  return true;
}

bool ble_status_init(void) {
  if (s_initialized) return true;

  /* This service is the single NimBLE init owner — bring the stack up under
   * the configured device name + TX power. Normally a no-op here because
   * ble_status_stack_begin() was already called early in setup() (so the BLE
   * Scout attaches to a stack that already carries our name); calling it again
   * keeps this entry point self-sufficient if used standalone. */
  if (!ble_status_stack_begin()) {
    return false;
  }

  const char* ble_name = resolve_ble_name();

  s_server = NimBLEDevice::createServer();
  if (!s_server) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_BLUETOOTH,
               "BLE server creation failed", nullptr);
    return false;
  }
  s_server->setCallbacks(&s_callbacks);

  /* ── Standard Battery Service (0x180F) ────────────────────────────── */
  NimBLEService* batt_svc = s_server->createService(BLE_BATTERY_SERVICE_UUID);
  if (!batt_svc) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_BLUETOOTH,
               "BLE battery service creation failed", nullptr);
    return false;
  }

  s_batt_level_char = batt_svc->createCharacteristic(
      BLE_BATTERY_LEVEL_CHAR_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  {
    uint8_t initial_batt = 0;
    s_batt_level_char->setValue(&initial_batt, 1);
  }
  batt_svc->start();

  /* ── Custom SecuraCV Status Service ───────────────────────────────── */
  NimBLEService* scv_svc = s_server->createService(BLE_SCV_SERVICE_UUID);
  if (!scv_svc) {
    log_health(LOG_LEVEL_ERROR, LOG_CAT_BLUETOOTH,
               "BLE SecuraCV service creation failed", nullptr);
    return false;
  }

  s_dev_name_char = scv_svc->createCharacteristic(
      BLE_SCV_DEVICE_NAME_UUID, NIMBLE_PROPERTY::READ);
  s_dev_name_char->setValue(ble_name);

  s_fw_ver_char = scv_svc->createCharacteristic(
      BLE_SCV_FW_VERSION_UUID, NIMBLE_PROPERTY::READ);
  s_fw_ver_char->setValue(FIRMWARE_VERSION);

  s_chain_seq_char = scv_svc->createCharacteristic(
      BLE_SCV_CHAIN_SEQ_UUID, NIMBLE_PROPERTY::READ);
  {
    uint32_t seq = 0;
    s_chain_seq_char->setValue(seq);
  }

  s_health_char = scv_svc->createCharacteristic(
      BLE_SCV_HEALTH_SCORE_UUID, NIMBLE_PROPERTY::READ);
  {
    uint8_t score = 0;
    s_health_char->setValue(&score, 1);
  }

  s_degrade_char = scv_svc->createCharacteristic(
      BLE_SCV_DEGRADE_LEVEL_UUID, NIMBLE_PROPERTY::READ);
  {
    uint8_t dl = 0;
    s_degrade_char->setValue(&dl, 1);
  }

  s_uptime_char = scv_svc->createCharacteristic(
      BLE_SCV_UPTIME_UUID, NIMBLE_PROPERTY::READ);
  {
    uint32_t up = 0;
    s_uptime_char->setValue(up);
  }

  s_sd_usage_char = scv_svc->createCharacteristic(
      BLE_SCV_SD_USAGE_UUID, NIMBLE_PROPERTY::READ);
  {
    uint8_t sd = 0;
    s_sd_usage_char->setValue(&sd, 1);
  }

  scv_svc->start();

  /* ── Advertising ──────────────────────────────────────────────────── */
  /* Fix the fleet-link beacon fingerprint: the last 2 bytes of the Ed25519
   * pubkey fingerprint — the same 2 bytes the "SCV-XXXX" device id derives
   * from — so a canary-display resolves this device from the beacon alone. */
  {
    DeviceIdentity& dev = witness_get_device();
    s_beacon_fp[0] = dev.pubkey_fp[6];
    s_beacon_fp[1] = dev.pubkey_fp[7];
  }

  // NimBLE-Arduino 2.x removed NimBLEAdvertising::setScanResponse(bool) and
  // set{Min,Max}Preferred(); scan response is enabled via enableScanResponse()
  // and its contents set via NimBLEAdvertisementData. The service UUIDs + name
  // ride the scan response so the primary advert carries the fleet-link
  // presence beacon (mirrors the canary-wap sketch's ble_opera advert).
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->enableScanResponse(true);
  if (!apply_beacon_advertising()) {
    /* apply_beacon_advertising() already installed the legacy UUID+name advert
     * as a fallback — the device is advertising, just without the presence
     * beacon. Log so the failure is visible in the field. */
    log_health(LOG_LEVEL_WARNING, LOG_CAT_BLUETOOTH,
               "BLE beacon adv setup failed — legacy UUID advert active", nullptr);
  }
  NimBLEDevice::startAdvertising();

  s_initialized = true;
  s_last_update_ms = millis();

  log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH,
             "BLE GATT status service started", ble_name);
  return true;
}

/* ════════════════════════════════════════════════════════════════════════════ */
/*  UPDATE                                                                    */
/* ════════════════════════════════════════════════════════════════════════════ */

void ble_status_update(void) {
  if (!s_initialized) return;

  uint32_t now = millis();
  if ((now - s_last_update_ms) < BLE_STATUS_UPDATE_INTERVAL_MS) return;
  s_last_update_ms = now;

  /* ── Battery level ────────────────────────────────────────────────── */
#if FEATURE_POWER_MONITOR
  {
    power_state_t pwr;
    if (power_get_state(&pwr)) {
      uint8_t batt = pwr.soc_pct;
      if (batt > 100) batt = 100;
      s_batt_level_char->setValue(&batt, 1);
      if (s_connected) {
        s_batt_level_char->notify();
      }
    }
  }
#endif

  /* ── Chain sequence ───────────────────────────────────────────────── */
  {
    DeviceIdentity& dev = witness_get_device();
    uint32_t seq = dev.seq;
    s_chain_seq_char->setValue(seq);
  }

  /* ── Health score + degradation ───────────────────────────────────── */
#if FEATURE_DIAGNOSTICS
  {
    selftest_report_t st;
    if (diag_get_selftest(&st) && st.has_run) {
      uint8_t score = st.health_score;
      s_health_char->setValue(&score, 1);
    }
    uint8_t dl = (uint8_t)diag_get_degrade_level();
    s_degrade_char->setValue(&dl, 1);
  }

  /* ── SD usage ─────────────────────────────────────────────────────── */
  {
    diag_snapshot_t snap;
    if (diag_get_snapshot(&snap)) {
      uint8_t sd_pct = snap.sd.usage_pct;
      s_sd_usage_char->setValue(&sd_pct, 1);
    }
  }
#endif

  /* ── Uptime ───────────────────────────────────────────────────────── */
  {
    SystemHealth& health = witness_get_health();
    uint32_t up = health.uptime_sec;
    s_uptime_char->setValue(up);
  }

  /* ── Fleet-link presence beacon ───────────────────────────────────── */
  /* Re-apply the manufacturer-data beacon with the live battery/health/
   * chain/flags just refreshed above. The chokepoint re-reads the same
   * modules, so the on-air beacon tracks device state at the 5 s cadence. */
  apply_beacon_advertising();
}

/* ════════════════════════════════════════════════════════════════════════════ */
/*  QUERY                                                                     */
/* ════════════════════════════════════════════════════════════════════════════ */

bool ble_status_is_connected(void) {
  return s_connected;
}

#endif /* FEATURE_BLE_STATUS */
