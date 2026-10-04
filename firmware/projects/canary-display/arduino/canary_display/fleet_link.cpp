// src/net/fleet_link.cpp — Layer 3 of the direct, broker-free BLE fleet link:
// an on-demand NimBLE CENTRAL that connects to a nearby WAP's GATT status
// service and reads its live self-report (chain seq, health, degrade, SD%,
// mic-muted, battery) with NO MQTT broker and NO home WiFi.
//
// SAFETY POSTURE (this is delicate, un-flashed firmware):
//   - Gated by FEATURE_FLEET_LINK — a display without it pays nothing.
//   - Reuses the shared BLE heap gate (ble_gate.h) + an s_ble_failed latch, so
//     a thin/failing radio is skipped, never thrashed.
//   - Only runs while the broker is down AND a request is pending — a healthy
//     display never opens a client.
//   - Read-once-then-disconnect (NO notify/subscribe): the minimal correct
//     shape the task calls for when the richer path can't be validated on HW.
//   - UNSIGNED + coarse, exactly like a chirp/beacon: results feed liveness +
//     diagnostics via on_beacon(), and NEVER set the trust badge.
//   - TOFU identity-pin: the first address seen for an fp4 is bound; a later
//     mismatch refuses the status and logs "ble identity mismatch".
//
// The connect + read is bounded but synchronous (NimBLE-Arduino's client
// connect blocks up to its timeout). That is acceptable here because the whole
// sequence only fires on an explicit request while off-grid, and returns
// immediately on every other loop pass.
#include "flavor_config.h"
#if defined(FEATURE_FLEET_LINK) && FEATURE_FLEET_LINK

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "fleet_link.h"
#include "ble_gate.h"
#include "beacon_parse.h"
#include "fleet_instance.h"
#include "fleet_model.h"
#include "log.h"

namespace canary::net {

namespace {

// ── WAP GATT status service (mirrors canary-wap/ble_status_api.h) ──────────
constexpr const char* SVC_UUID      = "5e63a1b0-7c3d-4f2e-8a91-0d1b2c3e4f5a";
constexpr const char* CH_FW_UUID    = "5e63a1b2-7c3d-4f2e-8a91-0d1b2c3e4f5a";
constexpr const char* CH_CHAIN_UUID = "5e63a1b3-7c3d-4f2e-8a91-0d1b2c3e4f5a";
constexpr const char* CH_HEALTH_UUID= "5e63a1b4-7c3d-4f2e-8a91-0d1b2c3e4f5a";
constexpr const char* CH_DEGRADE_UUID="5e63a1b5-7c3d-4f2e-8a91-0d1b2c3e4f5a";
constexpr const char* CH_SD_UUID    = "5e63a1b7-7c3d-4f2e-8a91-0d1b2c3e4f5a";
constexpr const char* CH_MIC_UUID   = "5e63a1b8-7c3d-4f2e-8a91-0d1b2c3e4f5a";
constexpr const char* BATT_SVC_UUID = "180F";
constexpr const char* BATT_LVL_UUID = "2A19";

constexpr uint32_t SCAN_FIND_MS   = 1500;   // bounded scan to locate target
constexpr uint32_t CONNECT_TMO_MS = 3000;   // bounded connect

// TOFU address<->fp4 bindings (first-seen wins; mismatch is refused).
struct Pin { char fp4[5]; char addr[18]; bool used; };
constexpr int PIN_CAP = 12;
Pin s_pins[PIN_CAP] = {};

char     s_pending[5] = {0};
bool     s_have_pending = false;
bool     s_mtu_set = false;
bool     s_ble_failed = false;
uint32_t s_count = 0;

// Bind (or verify) an fp4->addr pin. Returns true when the address is allowed
// (new binding, or matches the existing pin); false on a TOFU mismatch.
bool tofu_ok(const char* fp4, const char* addr) {
  int free_slot = -1;
  for (int i = 0; i < PIN_CAP; i++) {
    if (!s_pins[i].used) { if (free_slot < 0) free_slot = i; continue; }
    if (strncmp(s_pins[i].fp4, fp4, 4) == 0) {
      return strncmp(s_pins[i].addr, addr, sizeof(s_pins[i].addr)) == 0;
    }
  }
  if (free_slot < 0) return true;  // table full — don't block, just don't pin
  strncpy(s_pins[free_slot].fp4, fp4, 4);
  s_pins[free_slot].fp4[4] = '\0';
  strncpy(s_pins[free_slot].addr, addr, sizeof(s_pins[free_slot].addr) - 1);
  s_pins[free_slot].addr[sizeof(s_pins[free_slot].addr) - 1] = '\0';
  s_pins[free_slot].used = true;
  return true;
}

// Bring the BLE stack up if needed. Latches s_ble_failed on hard failure.
bool ble_ready() {
  if (s_ble_failed) return false;
  if (!ble_heap_ok()) return false;   // skip-and-retry, not a failure
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!NimBLEDevice::isInitialized()) {
    NimBLEDevice::init("");
    if (!NimBLEDevice::isInitialized()) {
      s_ble_failed = true;
      log_line("FLEETLINK", "BLE stack init failed - fleet link off this boot.");
      return false;
    }
  }
#else
  NimBLEDevice::init("");   // 1.4.x: idempotent, no isInitialized()
#endif
  if (!s_mtu_set) {
    NimBLEDevice::setMTU(247);
    s_mtu_set = true;
  }
  return true;
}

// Read one unsigned byte from a characteristic. Returns false if absent/empty.
bool read_u8(NimBLERemoteService* svc, const char* uuid, uint8_t& out) {
  if (!svc) return false;
  NimBLERemoteCharacteristic* c = svc->getCharacteristic(uuid);
  if (!c || !c->canRead()) return false;
  auto v = c->readValue();               // NimBLEAttValue (2.x) / std::string (1.x)
  if (v.length() < 1) return false;
  out = ((const uint8_t*)v.data())[0];
  return true;
}

// Read a little-endian uint32 (chain seq). Returns false if absent/short.
bool read_u32le(NimBLERemoteService* svc, const char* uuid, uint32_t& out) {
  if (!svc) return false;
  NimBLERemoteCharacteristic* c = svc->getCharacteristic(uuid);
  if (!c || !c->canRead()) return false;
  auto v = c->readValue();
  if (v.length() < 4) return false;
  const uint8_t* b = (const uint8_t*)v.data();
  out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
        ((uint32_t)b[3] << 24);
  return true;
}

// Scan briefly for the WAP advertising fp4; on a hit, hand back its
// NimBLEAddress. Returns true when found. Uses the same manufacturer-data
// beacon parse the passive listener uses so the address always ties back to
// the fingerprint. We keep the NimBLEAddress object (NOT a string) so the
// BLE address TYPE (public vs. random) survives to connect() — ESP32
// peripherals advertise random static addresses, and reconstructing them as
// BLE_ADDR_PUBLIC makes the connection fail.
bool find_target_addr(const char* fp4, NimBLEAddress& addr_out) {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan) return false;
  scan->setActiveScan(true);   // we need the scan response (name/uuid) too
  scan->setInterval(100);
  scan->setWindow(99);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  NimBLEScanResults res = scan->getResults(SCAN_FIND_MS, false);
#else
  NimBLEScanResults res = scan->start(SCAN_FIND_MS / 1000, false);
#endif
  const int n = (int)res.getCount();
  for (int i = 0; i < n; i++) {
    // NimBLE 2.x getResults returns a const pointer; 1.4.x returns the device
    // by value. Normalize to a usable object either way.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    const NimBLEAdvertisedDevice* d = res.getDevice(i);
    if (!d) continue;
#else
    NimBLEAdvertisedDevice dev = res.getDevice(i);
    NimBLEAdvertisedDevice* d = &dev;
#endif
    if (!d->haveManufacturerData()) continue;
    const std::string m = d->getManufacturerData();
    char seen[5];
    if (!beacon_fp4_from_mfg(reinterpret_cast<const uint8_t*>(m.data()),
                             m.size(), seen)) {
      continue;
    }
    if (strncmp(seen, fp4, 4) != 0) continue;
    addr_out = d->getAddress();   // preserves the address type
    scan->clearResults();
    return true;
  }
  scan->clearResults();
  return false;
}

// Connect to addr, read the status service, feed the model. Returns true on a
// successful, TOFU-approved read. Takes the NimBLEAddress by value/ref so the
// address type is preserved through to connect().
bool pull_status(const char* fp4, const NimBLEAddress& addr, uint32_t now) {
  NimBLEClient* client = NimBLEDevice::createClient();
  if (!client) return false;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  client->setConnectTimeout(CONNECT_TMO_MS);          // 2.x: milliseconds
#else
  client->setConnectTimeout((uint8_t)(CONNECT_TMO_MS / 1000));  // 1.x: seconds
#endif

  bool ok = client->connect(addr);
  if (!ok) {
    NimBLEDevice::deleteClient(client);
    return false;
  }

  bool got = false;
  NimBLERemoteService* svc = client->getService(SVC_UUID);
  if (svc) {
    canary::fleet::BeaconStatus st;

    uint32_t chain = 0;
    if (read_u32le(svc, CH_CHAIN_UUID, chain)) {
      st.chain_seq = chain;
      st.chain_present = true;
    }
    uint8_t health = 0;
    if (read_u8(svc, CH_HEALTH_UUID, health)) {
      st.health = (int16_t)(health > 100 ? 100 : health);
      st.health_present = true;
    }
    uint8_t degrade = 0;
    if (read_u8(svc, CH_DEGRADE_UUID, degrade)) st.degraded = (degrade != 0);
    uint8_t mic = 0;
    if (read_u8(svc, CH_MIC_UUID, mic)) st.mic_muted = (mic != 0);
    // SD% and fw are read to prove the link but aren't part of BeaconStatus;
    // they can be surfaced later without changing this contract.
    uint8_t sd = 0; (void)read_u8(svc, CH_SD_UUID, sd);
    (void)CH_FW_UUID;

    // Standard Battery Service (separate service on the WAP).
    NimBLERemoteService* bsvc = client->getService(BATT_SVC_UUID);
    uint8_t batt = 0;
    if (read_u8(bsvc, BATT_LVL_UUID, batt)) {
      st.battery_pct = (int16_t)(batt > 100 ? 100 : batt);
      st.battery_present = true;
    }

    // Unsigned, non-trust ingest — same path as the passive beacon.
    canary::fleet::the_fleet().on_beacon(fp4, st, /*have_status=*/true, now);
    got = true;
  }

  client->disconnect();
  NimBLEDevice::deleteClient(client);
  return got;
}

}  // namespace

void fleet_link_request(const char* fp) {
  if (!fp) return;
  // Accept either a bare 4-hex fp4 or a full fingerprint: the beacon carries
  // the LAST 2 fingerprint bytes (last 4 hex — the same suffix on_beacon and
  // beacon_fp4_from_mfg use), so match on the suffix, not the prefix.
  size_t n = 0;
  while (fp[n]) n++;
  if (n < 4) { s_have_pending = false; return; }
  const char* suffix = fp + n - 4;
  for (int i = 0; i < 4; i++) s_pending[i] = suffix[i];
  s_pending[4] = '\0';
  s_have_pending = true;
}

void fleet_link_loop(uint32_t now, bool broker_down, bool /*wifi_up*/) {
  if (!s_have_pending) return;      // nothing to do — cheap early-out
  // Deliberately NOT gated on the broker. This path only ever runs because
  // someone tapped a specific device on the glass, and if the broker already
  // had that device's detail the display would be showing it — so the tap is
  // evidence the broker does NOT have it (a Canary that talks BLE but was
  // never pointed at this hub). Dropping the request when a hub is present
  // made the hub actively worse than no hub for those devices. Bounded: one
  // attempt per request, and the request only exists because a human asked.
  (void)broker_down;
  if (!ble_ready()) return;         // heap not ready / stack down — retry later

  char fp4[5];
  memcpy(fp4, s_pending, 5);
  s_have_pending = false;           // one attempt per request (fail-safe)

  // Stop the passive listener's scan before we drive the shared radio.
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan) scan->stop();

  NimBLEAddress addr;
  if (!find_target_addr(fp4, addr)) return;   // not in range this window

  if (!tofu_ok(fp4, addr.toString().c_str())) {
    log_line("FLEETLINK", "ble identity mismatch - status refused");
    return;
  }

  if (pull_status(fp4, addr, now)) s_count++;
}

uint32_t fleet_link_count() { return s_count; }

}  // namespace canary::net

#else  // !FEATURE_FLEET_LINK

#include <stdint.h>  // the stubs still speak the header's uint32_t types

namespace canary::net {
void fleet_link_loop(uint32_t, bool, bool) {}
void fleet_link_request(const char*) {}
uint32_t fleet_link_count() { return 0; }
}  // namespace canary::net

#endif  // FEATURE_FLEET_LINK
