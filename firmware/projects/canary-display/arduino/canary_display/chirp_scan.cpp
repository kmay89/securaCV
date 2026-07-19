// src/net/chirp_scan.cpp — passive Chirp listener (spec §6).
//
// NimBLE (same stack the Canaries chirp with; ~60% less RAM than
// bluedroid). The scan callback runs on the NimBLE host task, so it only
// parses + enqueues into a lock-guarded ring; the main loop drains into
// the fleet model. WiFi/BLE share the 2.4 GHz radio — bursts are short
// and only run while the broker is already unreachable.
#include "flavor_config.h"
#if defined(FEATURE_CHIRP_SCAN) && FEATURE_CHIRP_SCAN

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>

#include "chirp_scan.h"
#include "fleet_instance.h"
#include "log.h"

namespace canary::net {

namespace {

constexpr uint32_t BURST_MS  = 4000;   // scan window
constexpr uint32_t PERIOD_MS = 20000;  // burst cadence while broker down

// Minimum *internal* SRAM headroom required before standing the BT controller
// up. The controller (plus the NimBLE host) draw tens of KB of internal,
// DMA-capable RAM that PSRAM cannot back; with WiFi already associated the two
// contend for it, and starting the controller when that pool is thin is what
// logged "BLE_INIT: Malloc failed" and then tripped the interrupt watchdog (an
// emi.c assert on the controller task) into a reboot loop. Gate the one-time
// bring-up on a generous margin — the off-grid chirp is explicitly the
// expendable radio decision (chirp_scan.h); a live glass beats a boot-looping
// one, so under memory pressure we simply skip the burst.
constexpr size_t BLE_MIN_FREE_INTERNAL  = 56 * 1024;
constexpr size_t BLE_MIN_BLOCK_INTERNAL = 20 * 1024;

struct ChirpMsg {
  char fp4[5];
  uint8_t type;
};

// Tiny SPSC ring, NimBLE task -> main loop.
constexpr int QCAP = 8;
ChirpMsg s_q[QCAP];
volatile int s_q_head = 0;
volatile int s_q_tail = 0;
portMUX_TYPE s_q_mux = portMUX_INITIALIZER_UNLOCKED;

bool s_ble_up = false;
bool s_ble_failed = false;   // a failed bring-up disables bursts for this boot
volatile bool s_scanning = false;
uint32_t s_next_burst_ms = 0;
uint32_t s_seen = 0;

// Shared advert parser — the callback API differs between NimBLE majors,
// the payload handling must not.
void handle_advert(const NimBLEAdvertisedDevice* d) {
  if (!d) return;
  // NimBLE 1.4.x's accessors aren't const-qualified (2.x fixed that), so the
  // shared parser sheds constness once; neither major's accessors mutate.
  NimBLEAdvertisedDevice* dev = const_cast<NimBLEAdvertisedDevice*>(d);
  if (!dev->haveManufacturerData()) return;
  const std::string m = dev->getManufacturerData();
  if (m.size() != 17) return;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(m.data());
  if (p[0] != 0xFF || p[1] != 0xFF) return;   // company id 0xFFFF (LE)
  const uint8_t type = p[2];
  if (type < 0x01 || type > 0x05) return;

  ChirpMsg msg;
  static const char H[] = "0123456789abcdef";
  msg.fp4[0] = H[(p[15] >> 4) & 0xF];
  msg.fp4[1] = H[p[15] & 0xF];
  msg.fp4[2] = H[(p[16] >> 4) & 0xF];
  msg.fp4[3] = H[p[16] & 0xF];
  msg.fp4[4] = '\0';
  msg.type = type;

  portENTER_CRITICAL(&s_q_mux);
  const int next = (s_q_head + 1) % QCAP;
  if (next != s_q_tail) {  // full ring drops newest — bursts repeat anyway
    s_q[s_q_head] = msg;
    s_q_head = next;
  }
  portEXIT_CRITICAL(&s_q_mux);
}

// NimBLE's scan-callback API split with its 2.x major, which tracks the
// arduino-esp32 core major (1.4.x is IDF4/core-2-only, 2.x is IDF5/core-3-
// only) — so the core version macro is the reliable selector. The 2.x shape
// mirrors the WAP's ble_scout_nimble.cpp.
#if ESP_ARDUINO_VERSION_MAJOR >= 3

class ChirpCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override { handle_advert(d); }
  void onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) override {
    s_scanning = false;
  }
};

#else  // core 2.x / NimBLE 1.4.x

class ChirpCb : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* d) override { handle_advert(d); }
};

void scan_ended(NimBLEScanResults) { s_scanning = false; }

#endif

ChirpCb s_cb;

bool ble_up() {
  if (s_ble_up) return true;
  if (s_ble_failed) return false;

  // Preventive heap gate: never call into the BT controller without a
  // comfortable internal-RAM margin (see BLE_MIN_* above). A thin pool here is
  // the normal state mid-WiFi-reconnect, so this is a skip-and-retry, not a
  // failure — re-check next window, when memory may have recovered.
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < BLE_MIN_FREE_INTERNAL ||
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) <
          BLE_MIN_BLOCK_INTERNAL) {
    return false;
  }

  NimBLEDevice::init("");
  // If the stack did not actually come up, stand down for the rest of this boot
  // rather than hammer a failing radio every window.
  if (!NimBLEDevice::isInitialized()) {
    s_ble_failed = true;
    log_line("CHIRP", "BLE stack init failed - off-grid listener off this boot.");
    return false;
  }
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan) {
    s_ble_failed = true;
    return false;
  }
  // wantDuplicates stays true deliberately: NimBLE's duplicate filter is
  // per-address, so it would hide a chirp *type escalation* (heartbeat ->
  // tamper from the same canary) for the rest of a burst. Flooding isn't a
  // risk: the ring is drained every main-loop pass (ms cadence vs ~100 ms
  // advert cadence) and the fleet model dedupes semantically (60 s).
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  scan->setScanCallbacks(&s_cb, /*wantDuplicates=*/true);
#else
  scan->setAdvertisedDeviceCallbacks(&s_cb, /*wantDuplicates=*/true);
#endif
  scan->setActiveScan(false);          // passive: we never transmit
  scan->setInterval(100);
  scan->setWindow(99);
  s_ble_up = true;
  log_line("CHIRP", "BLE listener up (passive, broker-down bursts only).");
  return true;
}

}  // namespace

void chirp_scan_loop(uint32_t now_ms, bool broker_down) {
  // Drain whatever a burst captured (cheap, every pass).
  for (;;) {
    ChirpMsg msg;
    bool have = false;
    portENTER_CRITICAL(&s_q_mux);
    if (s_q_tail != s_q_head) {
      msg = s_q[s_q_tail];
      s_q_tail = (s_q_tail + 1) % QCAP;
      have = true;
    }
    portEXIT_CRITICAL(&s_q_mux);
    if (!have) break;
    s_seen++;
    canary::fleet::the_fleet().on_chirp(msg.fp4, msg.type, now_ms);
  }

  if (!broker_down) {
    if (s_ble_up && s_scanning) {
      NimBLEDevice::getScan()->stop();
      s_scanning = false;
    }
    return;
  }

  if (s_scanning) return;
  if ((int32_t)(now_ms - s_next_burst_ms) < 0) return;

  // Schedule the next window up front so a skipped burst — heap gate not met,
  // or the stack disabled for this boot — waits a full period instead of
  // re-probing the heap (or re-initializing a failing radio) every loop pass.
  s_next_burst_ms = now_ms + PERIOD_MS;
  if (!ble_up()) return;

  s_scanning = true;
  // Async burst. NimBLE 1.x start() takes seconds + an end callback; 2.x
  // takes milliseconds + is_continue, with onScanEnd clearing the flag.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!NimBLEDevice::getScan()->start(BURST_MS, /*is_continue=*/false)) {
    s_scanning = false;
  }
#else
  if (!NimBLEDevice::getScan()->start(BURST_MS / 1000, scan_ended, false)) {
    s_scanning = false;
  }
#endif
}

uint32_t chirp_scan_count() { return s_seen; }

}  // namespace canary::net

#endif  // FEATURE_CHIRP_SCAN
