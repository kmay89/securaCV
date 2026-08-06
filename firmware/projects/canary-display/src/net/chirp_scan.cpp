// src/net/chirp_scan.cpp — passive Chirp listener (spec §6).
//
// NimBLE (same stack the Canaries chirp with; ~60% less RAM than
// bluedroid). The scan callback runs on the NimBLE host task, so it only
// parses + enqueues into a lock-guarded ring; the main loop drains into
// the fleet model. WiFi/BLE share the 2.4 GHz radio — bursts are short
// and only run while the broker is already unreachable.
#include <config.h>
#if defined(FEATURE_CHIRP_SCAN) && FEATURE_CHIRP_SCAN

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>

#include "canary/net/chirp_scan.h"
#include "canary/net/ble_gate.h"       // shared BLE heap gate (chirp + fleet-link)
#include "canary/net/beacon_parse.h"   // fleet-link presence beacon wire format
#include "canary/fleet/fleet_instance.h"
#include "canary/log.h"

namespace canary::net {

namespace {

constexpr uint32_t BURST_MS  = 4000;   // scan window
constexpr uint32_t PERIOD_MS = 20000;  // burst cadence while broker down
// Burst cadence while the broker is UP. The scan used to stop dead here, which
// meant a Canary that speaks only BLE — never provisioned onto the broker, or
// out of WiFi range — disappeared from the display the moment a hub arrived.
// A hub is supposed to ADD information, never subtract it. So keep listening,
// just rarely: one 4 s window a minute is a ~7% duty cycle on the shared
// 2.4 GHz radio, low enough to stay out of WiFi's way and often enough that a
// beacon (broadcast every few seconds) is still caught well inside the
// roster's expiry.
constexpr uint32_t PERIOD_BROKER_UP_MS = 60000;

// BLE controller heap gate lives in ble_gate.h now (ble_heap_ok()), shared
// with the fleet-link GATT path; the rationale comment moved there too.

// One queued advert from the NimBLE task to the main loop. A chirp (17-byte
// mfg) and a fleet-link presence beacon (11-byte mfg) share this ring,
// discriminated by `kind`; a Beacon carries a decoded BeaconStatus payload.
struct ChirpMsg {
  enum class Kind : uint8_t { Chirp, Beacon };
  Kind    kind = Kind::Chirp;
  char    fp4[5];
  uint8_t type;                                  // chirp type (Chirp only)
  canary::fleet::BeaconStatus payload{};         // decoded status (Beacon only)
  bool    have_status = false;                    // Beacon: payload valid
};

// Tiny SPSC ring, NimBLE task -> main loop. Sized a touch larger now that
// beacons share it with chirps (a fully off-grid display sees both continuously).
constexpr int QCAP = 12;
ChirpMsg s_q[QCAP];
volatile int s_q_head = 0;
volatile int s_q_tail = 0;
portMUX_TYPE s_q_mux = portMUX_INITIALIZER_UNLOCKED;

bool s_ble_up = false;
bool s_ble_failed = false;   // a failed bring-up disables bursts for this boot
volatile bool s_scanning = false;
// True when the RUNNING scan was started with duration 0 (fully off-grid).
// Such a scan never ends on its own, so nothing would ever clear s_scanning
// and the duty cycle could not resume once connectivity came back — it has to
// be stopped explicitly when the regime changes.
bool s_scan_continuous = false;
uint32_t s_next_burst_ms = 0;
uint32_t s_seen = 0;

// Shared advert parser — the callback API differs between NimBLE majors,
// the payload handling must not.
void enqueue(const ChirpMsg& msg) {
  portENTER_CRITICAL(&s_q_mux);
  const int next = (s_q_head + 1) % QCAP;
  if (next != s_q_tail) {  // full ring drops newest — adverts repeat anyway
    s_q[s_q_head] = msg;
    s_q_head = next;
  }
  portEXIT_CRITICAL(&s_q_mux);
}

void handle_advert(const NimBLEAdvertisedDevice* d) {
  if (!d) return;
  // NimBLE 1.4.x's accessors aren't const-qualified (2.x fixed that), so the
  // shared parser sheds constness once; neither major's accessors mutate.
  NimBLEAdvertisedDevice* dev = const_cast<NimBLEAdvertisedDevice*>(d);
  if (!dev->haveManufacturerData()) return;
  const std::string m = dev->getManufacturerData();
  const uint8_t* p = reinterpret_cast<const uint8_t*>(m.data());

  // Chirp (17-byte mfg, type 0x01..0x05) — spec §6, unchanged.
  if (m.size() == 17) {
    if (p[0] != 0xFF || p[1] != 0xFF) return;   // company id 0xFFFF (LE)
    const uint8_t type = p[2];
    if (type < 0x01 || type > 0x05) return;

    ChirpMsg msg;
    msg.kind = ChirpMsg::Kind::Chirp;
    static const char H[] = "0123456789abcdef";
    msg.fp4[0] = H[(p[15] >> 4) & 0xF];
    msg.fp4[1] = H[p[15] & 0xF];
    msg.fp4[2] = H[(p[16] >> 4) & 0xF];
    msg.fp4[3] = H[p[16] & 0xF];
    msg.fp4[4] = '\0';
    msg.type = type;
    enqueue(msg);
    return;
  }

  // Fleet-link presence beacon (11-byte v1 / 13-byte v2 mfg, type 0x10) —
  // the direct, broker-free channel. beacon_parse.h validates size/company/
  // type/version; a mismatch (or a chirp-sized blob above) is silently
  // ignored. v2 adds the detection class + confidence bytes (canary-vision).
  if (m.size() == BEACON_MFG_LEN || m.size() == BEACON_MFG_V2_LEN) {
    ChirpMsg msg;
    msg.kind = ChirpMsg::Kind::Beacon;
    if (!beacon_fp4_from_mfg(p, m.size(), msg.fp4)) return;
    msg.have_status = beacon_parse_status(p, m.size(), msg.payload);
    enqueue(msg);
    return;
  }
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
  // comfortable internal DMA-capable margin (ble_gate.h). A thin pool here is
  // the normal state mid-WiFi-reconnect, so this is a skip-and-retry, not a
  // failure — re-check next window, when memory may have recovered.
  if (!ble_heap_ok()) {
    return false;
  }

  NimBLEDevice::init("");
  // Verify the stack came up before using it; if it didn't, stand down for the
  // rest of this boot rather than hammer a failing radio every window. NimBLE
  // 2.x (core 3) exposes isInitialized(); 1.4.x (core 2) does not, so there we
  // rely on the scan handle alone (the original signal).
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!NimBLEDevice::isInitialized()) {
    s_ble_failed = true;
    log_line("CHIRP", "BLE stack init failed - off-grid listener off this boot.");
    return false;
  }
#endif
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
#if defined(FEATURE_BLE5_SCAN) && FEATURE_BLE5_SCAN
  // BLE 5 long-range (spec §6, bench-gated like the chime). When the NimBLE
  // library is built with extended advertising (CONFIG_BT_NIMBLE_EXT_ADV=1),
  // the controller's extended scanner reports adverts on the Coded PHY too —
  // ~4x the range of legacy 1M, the difference between hearing the garage
  // Canary's tamper chirp and not, once the router's cut. We keep the same
  // passive callback (a coded-PHY chirp carries the identical 17-byte mfg
  // payload — BLE 5 buys range, not a new wire format), but widen the window
  // to a continuous dwell so the sparser, slower coded adverts aren't missed
  // between hops. No per-PHY Arduino call is needed: enabling ext-adv in the
  // library IS the switch; this build flag raises the heap gate (ble_gate.h)
  // and says so on the log.
  scan->setInterval(160);
  scan->setWindow(160);
  log_line("CHIRP", "BLE 5 extended scan armed (Coded PHY / long range).");
#else
  scan->setInterval(100);
  scan->setWindow(99);
#endif
  s_ble_up = true;
  log_line("CHIRP", "BLE listener up (passive presence bursts, hub or no hub).");
  return true;
}

}  // namespace

void chirp_scan_loop(uint32_t now_ms, bool broker_down, bool wifi_up) {
  // Drain whatever a scan captured (cheap, every pass). Branch on kind: a
  // chirp feeds on_chirp (unchanged); a beacon feeds on_beacon with its
  // decoded status. Both are unsigned/coarse, so neither touches the badge.
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
    if (msg.kind == ChirpMsg::Kind::Beacon) {
      canary::fleet::the_fleet().on_beacon(msg.fp4, msg.payload, msg.have_status,
                                           now_ms, canary::fleet::Via::Ble);
    } else {
      canary::fleet::the_fleet().on_chirp(msg.fp4, msg.type, now_ms);
    }
  }

  // Three regimes, not two. The broker being up no longer silences BLE — see
  // PERIOD_BROKER_UP_MS: a hub must never make a device you own invisible.
  //  - WiFi also down (fully off-grid): the beacon/chirp is the ONLY channel
  //    left and there's no WiFi to coexist with, so run a CONTINUOUS passive
  //    scan (duration 0) rather than duty-cycling — nothing is lost between
  //    bursts.
  //  - WiFi up (broker unreachable but associated): keep today's 4 s / 20 s
  //    bursts so BLE and WiFi don't fight over the shared 2.4 GHz radio.
  //  - Broker up: the same 4 s bursts, but only once a minute — presence only,
  //    staying out of the way of a healthy display's traffic.
  const bool continuous = broker_down && !wifi_up;
  const uint32_t period = broker_down ? PERIOD_MS : PERIOD_BROKER_UP_MS;

  // A continuous scan has no end callback to clear s_scanning, so if the
  // regime has since narrowed — WiFi or the broker came back — it would run
  // forever and the bounded duty cycle would never be restored, leaving BLE
  // competing with WiFi on the shared radio indefinitely. Stop it here and let
  // the normal cadence take over. (The old code got this for free from the
  // `if (!broker_down) stop()` branch that this rework removed.)
  if (s_scanning && s_scan_continuous && !continuous) {
    NimBLEDevice::getScan()->stop();
    s_scanning = false;
    s_scan_continuous = false;
    s_next_burst_ms = now_ms;  // first bounded burst may start immediately
  }

  if (s_scanning) return;
  if (!continuous && (int32_t)(now_ms - s_next_burst_ms) < 0) return;

  // Schedule the next window up front so a skipped attempt — heap gate not met,
  // or the stack disabled for this boot — waits a full period instead of
  // re-probing the heap (or re-initializing a failing radio) every loop pass.
  s_next_burst_ms = now_ms + period;
  if (!ble_up()) return;

  s_scanning = true;
  s_scan_continuous = continuous;
  // Async scan. NimBLE 1.x start() takes seconds + an end callback; 2.x takes
  // milliseconds + is_continue, with onScanEnd clearing the flag. Duration 0
  // means "scan until stopped" on both majors (continuous, fully-off-grid).
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  const uint32_t dur_ms = continuous ? 0 : BURST_MS;
  if (!NimBLEDevice::getScan()->start(dur_ms, /*is_continue=*/false)) {
    s_scanning = false;
    s_scan_continuous = false;
  }
#else
  const uint32_t dur_s = continuous ? 0 : (BURST_MS / 1000);
  if (!NimBLEDevice::getScan()->start(dur_s, scan_ended, false)) {
    s_scanning = false;
    s_scan_continuous = false;
  }
#endif
}

uint32_t chirp_scan_count() { return s_seen; }

}  // namespace canary::net

#endif  // FEATURE_CHIRP_SCAN
