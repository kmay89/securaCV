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
#include "fleet_roster_feed.h"   // second consumer of this scan: the fleet roster

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include "ble_heap_guard.h"
#include <string>

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
    const uint32_t now  = millis();
    ble_scout_on_advert(mac, rssi, now);

    /* Second consumer, same advert: offer the manufacturer data to the fleet
     * roster. A fleet-link presence beacon / Chirp is parsed + tabled there;
     * anything else (the paired phones/watches above) is ignored. The roster
     * never sees a raw MAC — only the self-reported fingerprint suffix on the
     * wire. */
    if (device->haveManufacturerData()) {
      const std::string m = device->getManufacturerData();
      fleet_roster_feed::on_advert(
          reinterpret_cast<const uint8_t*>(m.data()), m.size(), rssi, now);
    }
  }
  void onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) override {
    /* Continuous mode: NimBLE will auto-restart. */
  }
};

ScoutScanCallbacks s_callbacks;

}  /* namespace */

bool nimble_scan_init() {
  if (s_scanner) return true;

  /* Attach to — not own — the NimBLE stack. The Scout is RX-only and never
   * advertises, so it must NOT own the GAP device name. When the BLE GATT
   * status service (securacv_ble_status, FEATURE_BLE_STATUS) is compiled in it
   * is the single NimBLE init owner: it brings the stack up under the
   * configured device name + TX power EARLY in setup() (ble_status_stack_begin),
   * before this Scout init runs. So here we only ATTACH — we must not call
   * NimBLEDevice::init() ourselves, or a failure to set the owner's name would
   * leave the device advertising as the generic "securacv-scout".
   *
   * If the stack still isn't up here with the status service present, the
   * owner's init() failed (radio/host error) — there's nothing for the Scout
   * to attach to, so bail. In a Scout-only build (no status service) there is
   * no other owner, so the Scout brings the stack up itself under the generic
   * name (not user-visible — the Scout never advertises). NimBLE 2.x init()
   * returns false when the controller/host stack can't come up. */
  if (!NimBLEDevice::isInitialized()) {
#if defined(FEATURE_BLE_STATUS) && FEATURE_BLE_STATUS
    return false;
#else
    /* Fail closed on low memory: if there's no room for the ~30 KB contiguous
     * controller allocation, don't call init() — it would assert and boot-loop
     * the device (the "BLE_INIT: Malloc failed" panic seen on no-PSRAM builds).
     * Skip the Scout instead; enabling PSRAM is the fix. This is the same guard
     * the canary-wap NimBLE init owner (bluetooth_channel.cpp / ble_heap_guard.h)
     * consults — the guard header's rule is that EVERY NimBLEDevice::init() call
     * site checks first, and this is the Scout-only build's own init site. */
    if (!ble_heap_guard::can_init(nullptr)) {
      Serial.println("[SCOUT] BLE stack not started: insufficient heap (enable PSRAM)");
      return false;
    }
    if (!NimBLEDevice::init("securacv-scout")) {
      return false;
    }
#endif
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
