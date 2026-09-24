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
bool        s_restart_pending = false;  /* unexpected scan end; recover() re-arms */

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
     * paired side. Its only production caller is the proximity pairing
     * window inside ble_scout_on_advert() itself (repo sweep F27), which
     * passes these very bytes — so pair-time and scan-time hashes always
     * agree, and no MAC is ever typed, stored or sent anywhere. */
    const uint8_t* mac  = device->getAddress().getBase()->val;
    const int8_t   rssi = (int8_t)device->getRSSI();
    ble_scout_on_advert(mac, rssi, millis());
  }
  void onScanEnd(const NimBLEScanResults& /*results*/, int reason) override {
    /* An "infinite" (duration=0) scan still ends — host reset, controller
     * preemption, a connection procedure. NimBLE does NOT auto-restart it
     * (start()'s restart parameter only applies to a scan that is still in
     * progress), so a silent end here used to leave the Scout dark forever
     * while s_running kept reading true. Restart only an UNEXPECTED end:
     * nimble_scan_stop() clears s_running before canceling, so a deliberate
     * stop lands here with s_running already false. If the radio refuses
     * the restart, record the stop so the ~1 Hz module tick can re-arm. */
    if (!s_running) return;
    if (s_scanner && s_scanner->start(0, false)) return;
    s_running = false;
    s_restart_pending = true;
    Serial.printf("[SCOUT] scan ended (reason %d); restart deferred to tick\n", reason);
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
  /* Report EVERY advert, not just the first per device. The controller's
   * duplicate filter defaults ON (NimBLEScan's ble_gap_disc_params ends
   * {..., passive=1, filter_duplicates=1}), and on an indefinite
   * (duration=0) scan it suppresses repeat adverts from an address for the
   * LIFETIME of the scan — a fixed-MAC beacon would be reported exactly
   * once, the presence tracker would time it out, and it could never come
   * back. Presence hold, departed detection and RSSI tracking all need the
   * repeats. */
  s_scanner->setDuplicateFilter(false);
  s_scanner->setInterval(SCAN_INTERVAL_UNITS);
  s_scanner->setWindow(SCAN_WINDOW_UNITS);
  s_scanner->setScanCallbacks(&s_callbacks);
  return true;
}

bool nimble_scan_start() {
  if (!s_scanner) return false;
  if (s_running)  return true;
  /* duration=0 → continuous scan; isContinue=false clears prior results. */
  if (!s_scanner->start(0, false)) return false;
  s_running = true;
  s_restart_pending = false;
  return true;
}

/* Tick-cadence recovery for a scan whose inline restart in onScanEnd was
 * refused by the radio. A deliberate nimble_scan_stop() clears the pending
 * flag, so a released radio (e.g. for OTA) stays released. */
void nimble_scan_recover() {
  if (s_restart_pending) nimble_scan_start();
}

void nimble_scan_stop() {
  s_restart_pending = false;
  if (!s_scanner || !s_running) return;
  /* Clear the intent flag BEFORE canceling: if the host delivers a
   * DISC_COMPLETE for the cancel, onScanEnd must see this stop as
   * deliberate and not restart the scan out from under us. */
  s_running = false;
  s_scanner->stop();
}

bool nimble_scan_running() {
  return s_running;
}

}  /* namespace ble_scout */

#endif  /* FEATURE_BLE_SCAN && !CSI_TEST_HOST_BUILD && __has_include(<NimBLEDevice.h>) */
