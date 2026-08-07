// ESP-NOW peer presence runtime — see canary/net/espnow_peer.h.
//
// The whole translation unit is behind FEATURE_ESPNOW so the default and
// emulator builds compile it to nothing (byte-neutral wasm). No stubs are
// needed: every caller in main.cpp is under the same gate. The wire format +
// validation are the host-tested beacon parser (canary/net/beacon_parse.h) —
// one contract, shared with the BLE chirp path; this file is only the ESP-NOW
// transport and the receive-drain plumbing (the same shape chirp_scan.cpp uses:
// a minimal ISR-context callback that parses + enqueues, drained by the loop).

#include "flavor_config.h"

#if defined(FEATURE_ESPNOW) && FEATURE_ESPNOW

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#include "espnow_peer.h"
#include "espnow_peer_logic.h"  // pure decode (shared with host test)
#include "fleet_instance.h"
#include "pair_demo_ui.h"  // First Light demo tap (inert unless FEATURE_PAIR_DEMO)
#include "log.h"

namespace canary {
namespace net {

namespace {

// One decoded peer beacon waiting to be drained into the fleet model.
struct PeerMsg {
  char fp4[5];
  canary::fleet::BeaconStatus payload{};
  bool have_status = false;
};

constexpr int QCAP = 16;
portMUX_TYPE s_q_mux = portMUX_INITIALIZER_UNLOCKED;
PeerMsg s_q[QCAP];
volatile int s_q_head = 0, s_q_tail = 0;

bool s_ready = false;
volatile uint32_t s_seen = 0;

// Enqueue a decoded beacon (called from the ESP-NOW receive callback, which runs
// on the WiFi task — keep it to parse + copy, no fleet mutation here).
void enqueue(const PeerMsg& m) {
  portENTER_CRITICAL(&s_q_mux);
  const int next = (s_q_head + 1) % QCAP;
  if (next != s_q_tail) {  // drop on overflow rather than block the WiFi task
    s_q[s_q_head] = m;
    s_q_head = next;
  }
  portEXIT_CRITICAL(&s_q_mux);
}

// Parse a raw ESP-NOW payload as a fleet-link presence beacon and enqueue it.
// The pure espnow_decode() rejects noise/foreign traffic (size/company/type/
// version) before anything touches the fleet.
void handle_payload(const uint8_t* data, int len) {
  PeerMsg m;
  if (!espnow_decode(data, len, m.fp4, m.payload, m.have_status)) return;
  s_seen++;
  enqueue(m);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void on_recv(const esp_now_recv_info_t* /*info*/, const uint8_t* data, int len) {
  handle_payload(data, len);
}
#else
void on_recv(const uint8_t* /*mac*/, const uint8_t* data, int len) {
  handle_payload(data, len);
}
#endif

}  // namespace

bool espnow_ready() { return s_ready; }
uint32_t espnow_seen() { return s_seen; }

bool espnow_begin() {
  if (s_ready) return true;
  // ESP-NOW rides the WiFi driver; it must already be started (STA). We listen
  // on whatever channel STA is on — receive-only, so no peer is added and we
  // never transmit (the dash observes, it doesn't advertise).
  if (esp_now_init() != ESP_OK) {
    log_line("ESPNOW", "init failed - MQTT/BLE remain the fleet transports");
    return false;
  }
  esp_now_register_recv_cb(on_recv);
  s_ready = true;
  log_line("ESPNOW", "peer listener up (receive-only presence)");
  return true;
}

void espnow_loop(uint32_t now) {
  if (!s_ready) return;
  for (;;) {
    PeerMsg m;
    bool have = false;
    portENTER_CRITICAL(&s_q_mux);
    if (s_q_tail != s_q_head) {
      m = s_q[s_q_tail];
      s_q_tail = (s_q_tail + 1) % QCAP;
      have = true;
    }
    portEXIT_CRITICAL(&s_q_mux);
    if (!have) break;
    // Unsigned observation, same footing as the BLE beacon — the fleet model
    // does the semantic 60 s dedupe, so a chatty broadcast medium is fine.
    // Tagged Mesh so the event line names the band that actually carried it;
    // these frames used to be reported as "(ble)", which they never were.
    canary::fleet::the_fleet().on_beacon(m.fp4, m.payload, m.have_status, now,
                                         canary::fleet::Via::Mesh);
    // The First Light demo hears every frame UN-deduped: trigger timing is
    // the demo's whole point, and the fleet model's 60 s semantic dedupe
    // would hide exactly the edges it exists to show.
    canary::ui::pair_demo_note_beacon(m.fp4, m.payload, m.have_status, now,
                                      canary::fleet::Via::Mesh);
  }
}

}  // namespace net
}  // namespace canary

#endif  // FEATURE_ESPNOW
