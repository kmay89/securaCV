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

#include "chirp_scan.h"
#include "fleet_instance.h"
#include "log.h"

namespace canary::net {

namespace {

constexpr uint32_t BURST_MS  = 4000;   // scan window
constexpr uint32_t PERIOD_MS = 20000;  // burst cadence while broker down

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
volatile bool s_scanning = false;
uint32_t s_next_burst_ms = 0;
uint32_t s_seen = 0;

// Shared advert parser — the callback API differs between NimBLE majors,
// the payload handling must not.
void handle_advert(const NimBLEAdvertisedDevice* d) {
  if (!d || !d->haveManufacturerData()) return;
  const std::string m = d->getManufacturerData();
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
  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan) return false;
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
  if (!ble_up()) return;

  s_next_burst_ms = now_ms + PERIOD_MS;
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
