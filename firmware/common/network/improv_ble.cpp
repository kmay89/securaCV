// improv_ble.cpp — Improv Wi-Fi over BLE, the NimBLE glue. See improv_ble.h.
//
// Division of labor: improv_core.h decides (host-tested), this file moves
// bytes on and off the radio and hands credentials to the shared setup
// portal's join path. Every decision that could be wrong lives in the
// header, where a g++ test can reach it.
//
// Threading: NimBLE's callbacks run on its host task. The write callback
// only copies the packet into a mailbox; everything else — parsing, the
// session, WiFi calls, notifications — happens in tick() on the loop task,
// the same task the portal's own join state machine runs on.

// The feature flag is the project's: include/canary/config.h carries the
// FEATURE_IMPROV default on every family that compiles this file, and CI's
// per-board -DFEATURE_IMPROV=0 veto reaches it the same way the beacon's does.
#if __has_include("canary/config.h")
#include "canary/config.h"
#endif

#include "network/improv_ble.h"

#if defined(FEATURE_IMPROV) && FEATURE_IMPROV

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <string.h>
#include <string>

#include "network/setup_portal.h"

namespace canary {
namespace net {
namespace improv_ble {

namespace {

using improv::Command;
using improv::Error;
using improv::Parse;
using improv::Session;
using improv::State;

// A join the portal never reports on fails at its own 30 s; give it a beat
// more so the portal's verdict (with its reason) wins over our timeout.
constexpr uint32_t PROVISIONING_TIMEOUT_MS = 40000;
constexpr uint32_t SCAN_TIMEOUT_MS = 30000;
constexpr int SCAN_MAX = 20;
constexpr size_t ADV_NAME_MAX = 12;

Identity s_id{};
char s_adv_name[ADV_NAME_MAX + 1] = {0};
bool s_active = false;
Session s_session;
improv::Timing s_timing;

NimBLEServer*         s_server = nullptr;
NimBLEService*        s_service = nullptr;
NimBLECharacteristic* s_state = nullptr;
NimBLECharacteristic* s_error = nullptr;
NimBLECharacteristic* s_command = nullptr;
NimBLECharacteristic* s_result = nullptr;
NimBLECharacteristic* s_caps = nullptr;

// The mailbox: one command at a time, host task → loop task.
uint8_t s_rx[improv::MAX_COMMAND_LEN];
volatile size_t s_rx_len = 0;
volatile bool s_rx_pending = false;

volatile bool s_connected = false;
volatile bool s_readvertise = false;   // a disconnect handed the radio back

// The last beacon the carrier handed over, so a state change can be re-put
// on air with the same bytes.
uint8_t s_last_beacon[32];
size_t  s_last_beacon_len = 0;
bool    s_aired_open = false;          // what the last advert set said
bool    s_aired_once = false;

// Network scan (GET_WIFI_NETWORKS): the results stream one per loop pass so
// twenty notifications never queue up on the host at once.
bool     s_scan_in_flight = false;
uint32_t s_scan_started_ms = 0;
int      s_scan_count = -1;      // results harvested, -1 = none
int      s_scan_emit = 0;        // next row to send

uint8_t capabilities() {
  uint8_t caps = improv::CAP_DEVICE_INFO | improv::CAP_SCAN_WIFI;
  if (s_id.identify) caps |= improv::CAP_IDENTIFY;
  return caps;
}

bool door_open_now() { return s_active && improv::advert_for(s_session) == improv::Advert::Improv; }

void publish_state() {
  if (!s_state) return;
  const uint8_t b = (uint8_t)s_session.state;
  s_state->setValue(&b, 1);
  s_state->notify();
}

void publish_error() {
  if (!s_error) return;
  const uint8_t b = (uint8_t)s_session.error;
  s_error->setValue(&b, 1);
  s_error->notify();
}

void send_result(Command cmd, const char* const* strings, size_t count) {
  if (!s_result) return;
  uint8_t frame[improv::MAX_RESULT_LEN];
  const size_t n = improv::build_result(frame, sizeof(frame), cmd, strings, count);
  if (n == 0) return;
  s_result->setValue(frame, n);
  s_result->notify();
}

const char* auth_name(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN:            return "NO";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2 EAP";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA3";
    default:                        return "WPA2";
  }
}

// ── NimBLE callbacks (host task) ───────────────────────────────────────────

class CommandCb : public NimBLECharacteristicCallbacks {
  void take(NimBLECharacteristic* c) {
    const auto v = c->getValue();
    const size_t n = v.length();
    if (s_rx_pending) return;   // one at a time; the loop task is on it
    if (n == 0 || n > sizeof(s_rx)) {
      // Not a frame we could ever accept: queue a one-byte marker so the
      // loop task answers InvalidRpc rather than silence.
      s_rx[0] = 0; s_rx_len = 1; s_rx_pending = true;
      return;
    }
    memcpy(s_rx, v.data(), n);
    s_rx_len = n;
    s_rx_pending = true;
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*info*/) override { take(c); }
#else
  void onWrite(NimBLECharacteristic* c) override { take(c); }
#endif
};

class ServerCb : public NimBLEServerCallbacks {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  void onConnect(NimBLEServer* /*s*/, NimBLEConnInfo& /*info*/) override { s_connected = true; }
  void onDisconnect(NimBLEServer* /*s*/, NimBLEConnInfo& /*info*/, int /*reason*/) override {
    s_connected = false;
    s_readvertise = true;
  }
#else
  void onConnect(NimBLEServer* /*s*/) override { s_connected = true; }
  void onDisconnect(NimBLEServer* /*s*/) override {
    s_connected = false;
    s_readvertise = true;
  }
#endif
};

CommandCb s_command_cb;
ServerCb  s_server_cb;

// ── Advertising ────────────────────────────────────────────────────────────

void put_on_air() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) return;

  const bool open = door_open_now();
  NimBLEAdvertisementData primary;
  NimBLEAdvertisementData scan;

  if (s_last_beacon_len) {
    primary.setManufacturerData(std::string((const char*)s_last_beacon, s_last_beacon_len));
  }
  if (open) {
    // Primary: flags (3) + beacon (15 or 17) + name (2 + up to 12) ≤ 31.
    // Scan response: the 128-bit Improv UUID (18) + the 0x4677 service data
    // (10) = 28 — what a service-filtered scan on a phone matches.
    primary.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    const size_t name_len = strlen(s_adv_name);
    if (name_len && 3 + 2 + s_last_beacon_len + 2 + name_len <= 31) {
      primary.setName(std::string(s_adv_name, name_len));
    }
    scan.addServiceUUID(NimBLEUUID(improv::SERVICE_UUID));
    uint8_t sd[improv::SERVICE_DATA_LEN];
    improv::build_service_data(sd, s_session.state, capabilities());
    scan.setServiceData(NimBLEUUID((uint16_t)improv::SERVICE_DATA_UUID16),
                        std::string((const char*)sd, sizeof(sd)));
  }
  // Door shut: the beacon alone, exactly the bytes a provisioned device put
  // on air before this module existed; an empty scan response clears the
  // one the open door left.

  // stop -> set -> start refreshes the on-air payload deterministically on
  // BOTH NimBLE majors (1.4.x only latches new data for the next start()).
  // The setters are called as statements: void on 1.4.x, bool on 2.x.
  adv->stop();
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  adv->setConnectableMode(open ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON);
#else
  adv->setAdvertisementType(open ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON);
#endif
  adv->setAdvertisementData(primary);
  adv->setScanResponseData(scan);
  adv->start();
  s_aired_open = open;
  s_aired_once = true;
  s_readvertise = false;
}

// ── Commands (loop task) ───────────────────────────────────────────────────

void answer_device_info() {
  const char* strings[4] = {
    s_id.firmware_name ? s_id.firmware_name : "",
    s_id.firmware_version ? s_id.firmware_version : "",
    s_id.hardware ? s_id.hardware : "",
    s_id.device_name ? s_id.device_name : "",
  };
  send_result(Command::GetDeviceInfo, strings, 4);
}

void start_scan(uint32_t now_ms) {
  if (s_scan_in_flight) return;
  if (setup_portal_join_in_flight()) {
    // Never sweep under a join (a live scan handle can fail WiFi.begin):
    // answer "no networks" and the phone shows its typed-name field.
    send_result(Command::GetWifiNetworks, nullptr, 0);
    return;
  }
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) WiFi.scanDelete();
  const int rc = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false, /*passive=*/false, 300);
  if (rc != WIFI_SCAN_RUNNING) {
    send_result(Command::GetWifiNetworks, nullptr, 0);
    return;
  }
  s_scan_in_flight = true;
  s_scan_started_ms = now_ms;
  s_scan_count = -1;
  s_scan_emit = 0;
}

void follow_scan(uint32_t now_ms) {
  if (s_scan_in_flight) {
    const int rc = WiFi.scanComplete();
    if (rc == WIFI_SCAN_RUNNING) {
      if ((int32_t)(now_ms - s_scan_started_ms) > (int32_t)SCAN_TIMEOUT_MS) {
        WiFi.scanDelete();
        s_scan_in_flight = false;
        send_result(Command::GetWifiNetworks, nullptr, 0);
      }
      return;
    }
    s_scan_in_flight = false;
    s_scan_count = rc < 0 ? 0 : (rc > SCAN_MAX ? SCAN_MAX : rc);
    s_scan_emit = 0;
    // Fall through to stream the first row this pass.
  }
  if (s_scan_count < 0) return;
  if (s_scan_emit < s_scan_count) {
    const int i = s_scan_emit++;
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0 || ssid.length() > improv::SSID_MAX) return;  // next pass
    char rssi[8];
    snprintf(rssi, sizeof(rssi), "%d", (int)WiFi.RSSI(i));
    const char* row[3] = { ssid.c_str(), rssi, auth_name(WiFi.encryptionType(i)) };
    send_result(Command::GetWifiNetworks, row, 3);
    return;
  }
  // Every row sent: the empty result closes the list.
  WiFi.scanDelete();
  s_scan_count = -1;
  send_result(Command::GetWifiNetworks, nullptr, 0);
}

void handle_command(uint32_t now_ms) {
  uint8_t frame[improv::MAX_COMMAND_LEN];
  const size_t n = s_rx_len;
  memcpy(frame, s_rx, n);
  improv::wipe(s_rx, sizeof(s_rx));
  s_rx_pending = false;

  improv::ParsedCommand cmd = improv::parse_command(frame, n);
  improv::wipe(frame, sizeof(frame));
  if (cmd.verdict != Parse::Ok) {
    improv::session_on_bad_packet(s_session, cmd.verdict);
    publish_error();
    return;
  }

  switch (cmd.command) {
    case Command::WifiSettings: {
      const Error e = improv::session_on_wifi_settings(s_session, now_ms);
      if (e == Error::None) {
        if (!setup_portal_submit_join(cmd.wifi.ssid, cmd.wifi.password, now_ms)) {
          // The portal refused (a wizard join is testing, or it is not up
          // after all): the join never started.
          improv::session_on_join_result(s_session, false);
        }
      }
      improv::wipe(&cmd.wifi, sizeof(cmd.wifi));
      publish_error();
      publish_state();
      Serial.printf("[IMPROV] Wi-Fi settings %s\n",
                    s_session.state == State::Provisioning ? "accepted, joining" : "refused");
      break;
    }
    case Command::Identify:
      if (s_id.identify) s_id.identify();
      s_session.error = Error::None;
      publish_error();
      break;
    case Command::GetDeviceInfo:
      s_session.error = Error::None;
      publish_error();
      answer_device_info();
      break;
    case Command::GetWifiNetworks:
      s_session.error = Error::None;
      publish_error();
      start_scan(now_ms);
      break;
    default:
      improv::session_on_unknown_command(s_session);
      publish_error();
      break;
  }
}

void follow_join(uint32_t now_ms) {
  if (s_session.state != State::Provisioning) return;
  switch (setup_portal_join_state()) {
    case SetupPortalJoin::Success: {
      improv::session_on_join_result(s_session, true);
      publish_error();
      publish_state();
      char url[96] = {0};
      if (s_id.reach_url) s_id.reach_url(url, sizeof(url));
      const char* strings[1] = { url };
      send_result(Command::WifiSettings, strings, 1);
      // The phone has its verdict over this door: the portal may start its
      // short linger beat rather than wait out the cap.
      setup_portal_note_acked(now_ms);
      Serial.println("[IMPROV] Joined — the phone has the verdict.");
      break;
    }
    case SetupPortalJoin::Fail:
      improv::session_on_join_result(s_session, false);
      publish_error();
      publish_state();
      Serial.printf("[IMPROV] Join failed: %s\n", setup_portal_fail_reason());
      break;
    default:
      break;
  }
}

}  // namespace

// ── Public API ─────────────────────────────────────────────────────────────

bool begin(const Identity& identity, bool no_wifi, uint32_t now_ms) {
  (void)now_ms;
  if (s_active) return true;
  s_id = identity;
  snprintf(s_adv_name, sizeof(s_adv_name), "%s", identity.adv_name ? identity.adv_name : "");

  // The beacon normally brought NimBLE up under the device id; if it did
  // not (FEATURE_FLEET_BEACON=0), do it here under the same name.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!NimBLEDevice::isInitialized()) {
    NimBLEDevice::init(identity.device_name ? identity.device_name : "SecuraCV");
    if (!NimBLEDevice::isInitialized()) return false;
  }
#else
  if (!NimBLEDevice::getAdvertising()) {
    NimBLEDevice::init(identity.device_name ? identity.device_name : "SecuraCV");
    if (!NimBLEDevice::getAdvertising()) return false;
  }
#endif

  // One LE Data Length packet carries the longest legal credential frame.
  NimBLEDevice::setMTU(247);
  if (identity.require_encryption) {
    // LE Secure Connections, Just Works, no bond kept: the link is
    // encrypted (ECDH), the phone shows one pairing sheet, and nothing is
    // stored on either side afterwards. The key is unauthenticated, so a
    // characteristic that asks for *_AUTHEN (none here) would still refuse it.
    NimBLEDevice::setSecurityAuth(/*bonding=*/false, /*mitm=*/false, /*sc=*/true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  }

  s_server = NimBLEDevice::createServer();
  if (!s_server) return false;
  s_server->setCallbacks(&s_server_cb);

  s_service = s_server->createService(improv::SERVICE_UUID);
  if (!s_service) return false;

  const uint32_t write_props = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
      | (identity.require_encryption ? NIMBLE_PROPERTY::WRITE_ENC : 0);
  const uint32_t result_props = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
      | (identity.require_encryption ? NIMBLE_PROPERTY::READ_ENC : 0);

  s_state   = s_service->createCharacteristic(improv::CURRENT_STATE_UUID,
                                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_error   = s_service->createCharacteristic(improv::ERROR_STATE_UUID,
                                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_command = s_service->createCharacteristic(improv::RPC_COMMAND_UUID, write_props);
  s_result  = s_service->createCharacteristic(improv::RPC_RESULT_UUID, result_props);
  s_caps    = s_service->createCharacteristic(improv::CAPABILITIES_UUID, NIMBLE_PROPERTY::READ);
  if (!s_state || !s_error || !s_command || !s_result || !s_caps) return false;
  s_command->setCallbacks(&s_command_cb);

  const uint8_t caps = capabilities();
  s_caps->setValue(&caps, 1);

#if ESP_ARDUINO_VERSION_MAJOR < 3
  s_service->start();   // 1.4.x starts services one by one; 2.x starts them with the server
#endif
  s_server->start();

  improv::session_begin(s_session, no_wifi);
  s_timing = improv::Timing{};
  s_timing.provisioning_timeout_ms = PROVISIONING_TIMEOUT_MS;
  s_active = true;
  publish_state();
  publish_error();
  s_readvertise = true;
  Serial.printf("[IMPROV] Improv Wi-Fi service up (%s).\n",
                no_wifi ? "door open: no Wi-Fi of its own yet" : "door shut: on its own Wi-Fi");
  return true;
}

void tick(uint32_t now_ms, bool no_wifi) {
  if (!s_active) return;
  const State before = s_session.state;

  improv::session_set_no_wifi(s_session, no_wifi, now_ms);
  if (s_rx_pending) handle_command(now_ms);
  follow_join(now_ms);
  follow_scan(now_ms);
  if (improv::session_tick(s_session, now_ms, s_timing)) {
    publish_error();
    publish_state();
  }

  if (s_session.state != before) {
    publish_state();
    if (s_session.state == State::Authorized || s_session.state == State::AwaitingAuthorization) {
      Serial.printf("[IMPROV] door %s\n", improv::door_open(s_session) ? "open" : "shut");
    }
  }
  // Re-put on air when a disconnect handed the radio back or the door
  // changed which advert set belongs on it; never while a client is
  // connected (a connected peripheral does not advertise, and the carrier
  // refreshes the beacon bytes every few seconds anyway).
  if (!s_connected && (s_readvertise || (s_aired_once && s_aired_open != door_open_now()))) put_on_air();
}

void tap(uint32_t now_ms) {
  if (!s_active) return;
  improv::session_tap(s_session, now_ms);
  publish_error();
  publish_state();
  s_readvertise = true;
}

bool active() { return s_active; }

improv::State state() { return s_session.state; }

bool setup_open() { return door_open_now(); }

void advertise(const uint8_t* beacon_mfg, size_t beacon_len) {
  if (beacon_mfg && beacon_len && beacon_len <= sizeof(s_last_beacon)) {
    memcpy(s_last_beacon, beacon_mfg, beacon_len);
    s_last_beacon_len = beacon_len;
  } else if (!beacon_mfg) {
    s_last_beacon_len = 0;
  }
  if (s_connected) return;   // the carrier's refresh waits for the link to drop
  put_on_air();
}

}  // namespace improv_ble
}  // namespace net
}  // namespace canary

#else  // FEATURE_IMPROV off — no-op stubs so call sites compile unchanged.

#include <NimBLEDevice.h>
#include <string>

namespace canary {
namespace net {
namespace improv_ble {
bool begin(const Identity&, bool, uint32_t) { return false; }
void tick(uint32_t, bool) {}
void tap(uint32_t) {}
bool active() { return false; }
improv::State state() { return improv::State::Stopped; }
bool setup_open() { return false; }
void advertise(const uint8_t* beacon_mfg, size_t beacon_len) {
  // The carrier's own stop -> set -> start, beacon primary, nothing else.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) return;
  NimBLEAdvertisementData advData;
  if (beacon_mfg && beacon_len) {
    advData.setManufacturerData(std::string((const char*)beacon_mfg, beacon_len));
  }
  adv->stop();
  adv->setAdvertisementData(advData);
  adv->start();
}
}  // namespace improv_ble
}  // namespace net
}  // namespace canary

#endif  // FEATURE_IMPROV
