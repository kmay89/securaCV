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

  /* Bring up the NimBLE stack. NimBLEDevice::init() is documented as
   * idempotent in NimBLE-Arduino 2.x — safe to call even if another
   * module already initialized the stack. It returns bool in 2.x; false
   * means the controller/host stack failed to come up (BT compiled out, no
   * radio, or a coexistence/heap failure), so propagate that instead of
   * marching on to getScan() and dereferencing a null scanner. The name is
   * intentionally generic ("securacv-scout") because the Scout role never
   * advertises; the name is only visible if a future build enables
   * advertising, which this TU does not. */
  if (!NimBLEDevice::init("securacv-scout")) {
    return false;
  }

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
