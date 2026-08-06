// src/net/fleet_roster_scan.cpp — fleet-link BLE roster scanner.
//
// The RX side of the direct BLE fleet link: a low-duty passive NimBLE scan that
// hears the OTHER Canaries' presence beacons (fleet_beacon.h, type 0x10) and
// 17-byte Chirps, and drops each into the shared, pure fleet roster
// (firmware/common/fleet_link/fleet_roster.h) — so this witness keeps its own
// "who's in my fleet / when did I last hear them" view, broker-free and
// WiFi-free, just like the display does. UNSIGNED presence only.
//
// Threading mirrors the display's chirp_scan.cpp: the NimBLE scan callback runs
// on the BLE host task and only parses + enqueues into a lock-guarded SPSC ring;
// the (non-thread-safe) FleetRoster is touched exclusively from the main loop
// in fleet_roster_scan_tick(). NimBLE init is shared with fleet_beacon_adv (both
// call NimBLEDevice::init once; it is idempotent) — advertise + passive scan
// coexist on the single 2.4 GHz radio.
//
// Cross-core: NimBLE-Arduino 1.4.x (arduino-esp32 core 2.x, this project) and
// 2.x (core 3.x, canary-sense) differ on the scan-callback shape,
// isInitialized() and start(); the ESP_ARDUINO_VERSION_MAJOR split mirrors
// firmware/projects/canary-display/src/net/chirp_scan.cpp.

#include "canary/net/fleet_roster_scan.h"

#if defined(FEATURE_FLEET_ROSTER) && FEATURE_FLEET_ROSTER

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <fleet_beacon.h>   // presence-beacon wire format (common/fleet_link)
#include <fleet_roster.h>   // shared pure roster           (common/fleet_link)
#include <string>

#include "canary/config.h"
#include "canary/log.h"

namespace canary::net {

namespace {

constexpr uint32_t BURST_MS  = 3000;   // passive scan window
constexpr uint32_t PERIOD_MS = 60000;  // burst cadence while on WiFi (low-duty)

// Keep the BT controller off unless a comfortable internal DMA-capable margin
// exists — a thin pool mid-WiFi-reconnect is normal, so a miss is skip-and-
// retry, not a permanent failure. Same rationale as the display's ble_gate.
constexpr size_t BLE_MIN_INTERNAL_HEAP = 24 * 1024;

FleetRoster s_roster;
bool     s_roster_inited = false;
bool     s_ble_up     = false;
bool     s_ble_failed = false;   // failed bring-up disables the scanner this boot
volatile bool s_scanning = false;
uint32_t s_next_burst_ms = 0;
uint32_t s_last_expire_ms = 0;
uint32_t s_seen = 0;

// One queued advert, NimBLE task -> main loop. Beacon (11-byte mfg) and chirp
// (17-byte mfg) share the ring, discriminated by have_status: a beacon carries
// decoded battery/health/chain/flags, a chirp only presence + type.
struct RosterMsg {
  char     fp4[5];
  uint8_t  type;
  int      battery_pct;   // -1 unknown
  int      health_pct;    // -1 unknown
  uint16_t chain_lo;
  uint8_t  flags;
  int8_t   rssi;
  bool     have_status;   // beacon vs chirp
};

constexpr int QCAP = 12;
RosterMsg s_q[QCAP];
volatile int s_q_head = 0;
volatile int s_q_tail = 0;
portMUX_TYPE s_q_mux = portMUX_INITIALIZER_UNLOCKED;

void enqueue(const RosterMsg& msg) {
  portENTER_CRITICAL(&s_q_mux);
  const int next = (s_q_head + 1) % QCAP;
  if (next != s_q_tail) {  // full ring drops newest — adverts repeat anyway
    s_q[s_q_head] = msg;
    s_q_head = next;
  }
  portEXIT_CRITICAL(&s_q_mux);
}

void fp4_from_bytes(uint8_t b0, uint8_t b1, char out[5]) {
  static const char H[] = "0123456789abcdef";
  out[0] = H[(b0 >> 4) & 0xF];
  out[1] = H[b0 & 0xF];
  out[2] = H[(b1 >> 4) & 0xF];
  out[3] = H[b1 & 0xF];
  out[4] = '\0';
}

void handle_advert(const NimBLEAdvertisedDevice* d, int8_t rssi) {
  if (!d) return;
  // NimBLE 1.4.x's accessors aren't const-qualified (2.x fixed that); shed
  // constness once — neither major's accessors mutate.
  NimBLEAdvertisedDevice* dev = const_cast<NimBLEAdvertisedDevice*>(d);
  if (!dev->haveManufacturerData()) return;
  const std::string m = dev->getManufacturerData();
  const uint8_t* p = reinterpret_cast<const uint8_t*>(m.data());

  // Fleet-link presence beacon (11-byte v1 / 13-byte v2 mfg, type 0x10) —
  // full status. v2 adds detection class + confidence (canary-vision);
  // fleet_beacon_parse accepts both, and the flags byte (incl. ALERT) rides
  // into the roster either way.
  if (m.size() == FLEET_BEACON_MFG_LEN || m.size() == FLEET_BEACON_MFG_V2_LEN) {
    FleetBeaconFields f;
    if (!fleet_beacon_parse(p, m.size(), &f)) return;
    RosterMsg msg;
    fp4_from_bytes(f.fp_b0, f.fp_b1, msg.fp4);
    msg.type        = FLEET_BEACON_TYPE;
    msg.battery_pct = f.battery_pct;
    msg.health_pct  = f.health_pct;
    msg.chain_lo    = f.chain_lo16;
    msg.flags       = f.flags;
    msg.rssi        = rssi;
    msg.have_status = true;
    enqueue(msg);
    return;
  }

  // Chirp (17-byte mfg, company 0xFFFF LE, type 0x01..0x05) — presence + type
  // only. fp2 rides at [15-16], same 2 bytes the beacon carries.
  if (m.size() == 17) {
    if (p[0] != 0xFF || p[1] != 0xFF) return;
    const uint8_t type = p[2];
    if (type < 0x01 || type > 0x05) return;
    RosterMsg msg;
    fp4_from_bytes(p[15], p[16], msg.fp4);
    msg.type        = type;
    msg.battery_pct = -1;   // status-less: the roster keeps last known
    msg.health_pct  = -1;
    msg.chain_lo    = 0;
    msg.flags       = 0;
    msg.rssi        = rssi;
    msg.have_status = false;
    enqueue(msg);
    return;
  }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3

class RosterCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    handle_advert(d, d ? (int8_t)d->getRSSI() : 0);
  }
  void onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) override {
    s_scanning = false;
  }
};

#else  // core 2.x / NimBLE 1.4.x

class RosterCb : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* d) override {
    handle_advert(d, d ? (int8_t)d->getRSSI() : 0);
  }
};

void scan_ended(NimBLEScanResults) { s_scanning = false; }

#endif

RosterCb s_cb;

bool ble_heap_ok() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= BLE_MIN_INTERNAL_HEAP;
}

bool ble_up() {
  if (s_ble_up) return true;
  if (s_ble_failed) return false;
  if (!ble_heap_ok()) return false;   // skip-and-retry, not a failure

  // Idempotent: fleet_beacon_adv may have already brought NimBLE up. init()
  // returns void on 1.4.x and bool on 2.x — call it as a statement and confirm
  // via isInitialized() on 2.x (1.4.x lacks it, so the scan handle is the
  // signal). Same guard style as chirp_scan.cpp / fleet_beacon_adv.cpp.
  NimBLEDevice::init("");
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!NimBLEDevice::isInitialized()) {
    s_ble_failed = true;
    canary::log_line("ROSTER", "NimBLE init failed — fleet roster scan off this boot.");
    return false;
  }
#endif
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan) {
    s_ble_failed = true;
    return false;
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  scan->setScanCallbacks(&s_cb, /*wantDuplicates=*/true);
#else
  scan->setAdvertisedDeviceCallbacks(&s_cb, /*wantDuplicates=*/true);
#endif
  scan->setActiveScan(false);   // passive: we never transmit a scan request
  scan->setInterval(100);
  scan->setWindow(99);
  s_ble_up = true;
  canary::log_line("ROSTER", "Fleet roster scan up (passive, low-duty).");
  return true;
}

}  // namespace

void fleet_roster_scan_tick(uint32_t now, bool wifi_up, bool wifi_provisioned) {
  if (!s_roster_inited) {
    fleet_roster_init(&s_roster);
    s_roster_inited = true;
  }

  // Drain whatever the scan captured (cheap, every pass) into the roster.
  for (;;) {
    RosterMsg msg;
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
    fleet_roster_note(&s_roster, msg.fp4, msg.type, msg.battery_pct,
                      msg.health_pct, msg.chain_lo, msg.flags, msg.rssi, now);
  }

  // Age stale peers out ~once a second (the roster's own 120 s window does the
  // real work; this just keeps the live count honest between sightings).
  if ((uint32_t)(now - s_last_expire_ms) >= 1000UL) {
    fleet_roster_expire(&s_roster, now);
    s_last_expire_ms = now;
  }

  // Scan continuously ONLY when there is no join to protect: an unprovisioned
  // unit has no network to associate with, so the beacon genuinely is the last
  // channel left and nothing competes for the radio.
  //
  // A PROVISIONED unit that is merely offline stays bursty. BLE and WiFi share
  // one 2.4 GHz radio here, and wifi_loop() is retrying the whole time it is
  // down — a never-ending passive scan across that window starves the very
  // association it is waiting for, so a unit that drops off WiFi (or misses
  // the boot-join timeout once) can never get back on. Low-duty bursts keep
  // the roster fresh without holding the radio hostage. (field report: an
  // ESP32-C3 Vision that would not rejoin the WiFi it was flashed with)
  const bool continuous = !wifi_up && !wifi_provisioned;
  if (s_scanning) return;
  if (!continuous && (int32_t)(now - s_next_burst_ms) < 0) return;

  // Schedule the next window up front so a skipped attempt (heap gate not met,
  // stack disabled for this boot) waits a full period instead of re-probing.
  s_next_burst_ms = now + PERIOD_MS;
  if (!ble_up()) return;

  s_scanning = true;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  const uint32_t dur_ms = continuous ? 0 : BURST_MS;
  if (!NimBLEDevice::getScan()->start(dur_ms, /*is_continue=*/false)) {
    s_scanning = false;
  }
#else
  const uint32_t dur_s = continuous ? 0 : (BURST_MS / 1000);
  if (!NimBLEDevice::getScan()->start(dur_s, scan_ended, false)) {
    s_scanning = false;
  }
#endif
}

int fleet_roster_scan_peer_count() {
  return s_roster_inited ? fleet_roster_count(&s_roster) : 0;
}

uint32_t fleet_roster_scan_seen() { return s_seen; }

}  // namespace canary::net

#else  // FEATURE_FLEET_ROSTER off — no-op stubs (per-board size-guard off switch)

#include <stdint.h>
namespace canary::net {
void fleet_roster_scan_tick(uint32_t, bool, bool) {}
int fleet_roster_scan_peer_count() { return 0; }
uint32_t fleet_roster_scan_seen() { return 0; }
}  // namespace canary::net

#endif  // FEATURE_FLEET_ROSTER
