/*
 * SecuraCV Canary — BLE Scout NimBLE passive scan loop
 * Version 0.1.0
 *
 * Compiled ONLY in builds that set FEATURE_BLE_SCAN=1 AND have
 * NimBLEDevice.h available (i.e. NimBLE-Arduino is in lib_deps).
 * The host build (CSI_TEST_HOST_BUILD) excludes this TU entirely so
 * the unit tests link without a Bluetooth stack.
 *
 * Privacy posture (matches design doc §"BLE Scout"):
 *   • setActiveScan(false) — we never emit scan-request frames.
 *   • Raw MAC enters ble_scout_on_advert() and is hashed immediately;
 *     no other consumer in this TU touches the raw bytes.
 *
 * Lifecycle:
 *   nimble_scan_init()  — called from ble_scout_init() (device build only).
 *   nimble_scan_start() — start continuous passive scan.
 *   nimble_scan_stop()  — release the radio (e.g. for OTA).
 *
 * VENDORED COPY — intentional divergences from the canonical library
 * (firmware/canary/lib/securacv_ble_scan/src/ble_scout_nimble.cpp), normalized
 * away by firmware/scripts/check_ble_scan_sync.sh:
 *   1. No fleet_roster_feed consumer. The canonical Scout offers each advert's
 *      manufacturer data to fleet_roster_feed (tracking OTHER Canaries). The WAP
 *      tracks its fleet through the mesh layer (mesh_network / ble_nearby),
 *      never the Scout scan, so fleet_roster_feed — and its fleet_roster.h
 *      dependency — is deliberately NOT staged into this sketch.
 *   2. NimBLE init ownership. This build's single init owner is
 *      bluetooth_channel.cpp (not the FEATURE_BLE_STATUS securacv_ble_status
 *      service the canary PIO build uses), so the Scout brings the stack up
 *      itself (idempotent) rather than deferring to a named owner. Both init
 *      sites consult the SAME ble_heap_guard::can_init() crash guard.
 */

/* FEATURE_BLE_SCAN comes from platformio.ini build_flags in the canary
 * PIO build and from build_config.h in canary-wap. Include build_config.h
 * when present so the gate sees the same flag in both builds — without
 * this, canary-wap's FULL profile would silently compile an empty TU. */
#if defined(__has_include)
  #if __has_include("build_config.h")
    #include "build_config.h"
  #endif
#endif

#if defined(FEATURE_BLE_SCAN) && FEATURE_BLE_SCAN \
    && !defined(CSI_TEST_HOST_BUILD) \
    && __has_include(<NimBLEDevice.h>)

#include "ble_scout.h"
#include "ble_scan.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include "ble_heap_guard.h"

namespace ble_scout {

namespace {

NimBLEScan* s_scanner   = nullptr;
bool        s_running   = false;

/* Scout-tuned duty cycle: 200 ms interval, 100 ms window. Listens
 * 50 % of the time — gives 1–2 adverts per second per beacon at the
 * typical 1-Hz advertising rate, plenty for the Kalman filter to
 * stay primed. Lower than ble_presence's 60 % duty so we leave more
 * radio time for the WiFi STA-on-Hub link. NimBLE units are 0.625 ms. */
constexpr uint16_t SCAN_INTERVAL_UNITS = 320;   /* 200 ms */
constexpr uint16_t SCAN_WINDOW_UNITS   = 160;   /* 100 ms */

class ScoutScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    if (!device) return;
    /* NimBLE addresses are little-endian in .getBase()->val (6 bytes).
     * ble_scout::ble_scout_on_advert hashes them in the order they
     * arrive; ble_scout_pair() must use the SAME byte order on the
     * paired side. The setup UI (PR 5c) is the only producer of
     * pair-time MACs and reads them via NimBLEAddress too, so this
     * stays consistent. */
    const uint8_t* mac  = device->getAddress().getBase()->val;
    const int8_t   rssi = (int8_t)device->getRSSI();
    ble_scout_on_advert(mac, rssi, millis());
  }
  void onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) override {
    /* Continuous mode: NimBLE will auto-restart. */
  }
};

ScoutScanCallbacks s_callbacks;

}  /* namespace */

bool nimble_scan_init() {
  if (s_scanner) return true;

  /* Fail closed on low memory: if the stack isn't already up and there's no
   * room for the ~30 KB controller allocation, don't call init() — it would
   * assert and boot-loop the device (the "BLE_INIT: Malloc failed" panic seen
   * on no-PSRAM builds). Skip the Scout instead; enabling PSRAM is the fix. */
  if (!NimBLEDevice::isInitialized() && !ble_heap_guard::can_init(nullptr)) {
    Serial.println("[SCOUT] BLE stack not started: insufficient heap (enable PSRAM)");
    return false;
  }

  /* Bring up the NimBLE stack. NimBLEDevice::init() is documented as
   * idempotent in NimBLE-Arduino 2.x — safe to call even if another
   * module already initialized the stack. The name is intentionally
   * generic ("securacv-scout") because the Scout role never
   * advertises; the name is only visible if a future build enables
   * advertising, which this TU does not. */
  NimBLEDevice::init("securacv-scout");

  s_scanner = NimBLEDevice::getScan();
  if (!s_scanner) return false;
  s_scanner->setActiveScan(false);
  s_scanner->setInterval(SCAN_INTERVAL_UNITS);
  s_scanner->setWindow(SCAN_WINDOW_UNITS);
  s_scanner->setScanCallbacks(&s_callbacks);
  return true;
}

bool nimble_scan_start() {
  if (!s_scanner) return false;
  if (s_running)  return true;
  /* duration=0 → continuous scan; second arg unused in continuous mode. */
  if (!s_scanner->start(0, false)) return false;
  s_running = true;
  return true;
}

void nimble_scan_stop() {
  if (!s_scanner || !s_running) return;
  s_scanner->stop();
  s_running = false;
}

bool nimble_scan_running() {
  return s_running;
}

}  /* namespace ble_scout */

#endif  /* FEATURE_BLE_SCAN && !CSI_TEST_HOST_BUILD && __has_include(<NimBLEDevice.h>) */
