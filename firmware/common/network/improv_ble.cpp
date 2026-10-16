// improv_ble.cpp — Improv Wi-Fi over BLE, the NimBLE glue. See improv_ble.h.
//
// Division of labor: improv_core.h decides (host-tested), this file moves
// bytes on and off the radio and hands credentials to the shared setup
// portal's join path. Every decision that could be wrong lives in the
// header, where a g++ test can reach it.
//
// Threading: NimBLE's callbacks run on its host task. The write callbacks
// (the RPC command and the companion service's HUB) only copy the packet
// into a mailbox; everything else — parsing, the session, WiFi calls,
// notifications, the hub hand-off to the board — happens in tick() on the
// loop task, the same task the portal's own join state machine runs on.

// The feature flag is the project's: include/canary/config.h carries the
// FEATURE_IMPROV default (and IMPROV_FIRST_BOOT_WINDOW_MS) on every family
// that compiles this file, and CI's per-board -DFEATURE_IMPROV=0 veto
// reaches it the same way the beacon's does.
#if __has_include("canary/config.h")
#include "canary/config.h"
#endif

#include "network/improv_ble.h"

#if defined(FEATURE_IMPROV) && FEATURE_IMPROV

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_attr.h>     // RTC_NOINIT_ATTR: the window across a software reset
#include <esp_system.h>   // esp_reset_reason(): a power-on clears it
#include <esp_idf_version.h>   // the USB / JTAG / glitch reset reasons exist from IDF 5.1
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
// The first-boot window: the project's config.h may override; the core's
// default (half an hour) otherwise. 0 = tap-only.
#ifndef IMPROV_FIRST_BOOT_WINDOW_MS
#define IMPROV_FIRST_BOOT_WINDOW_MS (improv::Timing{}.first_boot_window_ms)
#endif
constexpr uint32_t SCAN_TIMEOUT_MS = 30000;
constexpr int SCAN_MAX = 20;
constexpr size_t ADV_NAME_MAX = 12;

Identity s_id{};
char s_adv_name[ADV_NAME_MAX + 1] = {0};
bool s_active = false;
Session s_session;
improv::Timing s_timing;

// The first-boot window across a SOFTWARE reset. "Re-armed by a power cycle"
// must mean a power cycle: a board whose firmware restarts itself (an
// outage reboot, a watchdog, a route that restarts) must not open a fresh
// window every time. The spent time lives in RTC slow memory, which every
// reset but a power-on keeps; a power-on (or a brownout, which is a power
// event, or a reset a host asserted from the USB / JTAG port) finds the
// magic gone and starts from zero. Written every few seconds while the
// window runs, read once at begin().
struct WindowRecord {
  uint32_t magic;
  uint32_t used_ms;
};
constexpr uint32_t WINDOW_MAGIC = 0x1D00u * 0x10000u + 0x5E7Fu;   // "door" + "self"
RTC_NOINIT_ATTR WindowRecord s_window_rtc;
constexpr uint32_t WINDOW_SAVE_PERIOD_MS = 2000;
uint32_t s_window_saved_ms = 0;

// A reset that re-arms the window: the chip powered up (or its power
// glitched), or a host at the USB / JTAG port asserted a reset — the
// operator's own act (the Flasher ends every write with one, and a unit
// just erased and re-flashed must not inherit the previous image's spent
// window: RTC memory survives a flash) — or a reset the chip cannot name
// (the record is garbage as far as anyone can tell; clearing it is the safe
// side). A software reset, a watchdog, a panic and a deep-sleep wake keep
// the record. The USB / JTAG / glitch reasons exist from IDF 5.1 (core 3.x);
// the 1.4.x stack's core 2.x (IDF 4.4) has no such resets to name.
bool reset_is_power_cycle(esp_reset_reason_t why) {
  switch (why) {
    case ESP_RST_POWERON:
    case ESP_RST_BROWNOUT:
    case ESP_RST_UNKNOWN:
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
    case ESP_RST_USB:
    case ESP_RST_JTAG:
    case ESP_RST_PWR_GLITCH:
#endif
      return true;
    default:
      return false;
  }
}

uint32_t window_used_from_rtc() {
  const esp_reset_reason_t why = esp_reset_reason();
  if (reset_is_power_cycle(why)) {
    s_window_rtc.magic = 0;
    s_window_rtc.used_ms = 0;
    return 0;
  }
  if (s_window_rtc.magic != WINDOW_MAGIC) return 0;
  return s_window_rtc.used_ms;
}

void window_save(uint32_t used_ms) {
  s_window_rtc.used_ms = used_ms;
  s_window_rtc.magic = WINDOW_MAGIC;
}

NimBLEServer*         s_server = nullptr;
NimBLEService*        s_service = nullptr;
NimBLECharacteristic* s_state = nullptr;
NimBLECharacteristic* s_error = nullptr;
NimBLECharacteristic* s_command = nullptr;
NimBLECharacteristic* s_result = nullptr;
NimBLECharacteristic* s_caps = nullptr;

// The companion service and its one characteristic (hub_handoff.h). Null
// when the service could not be registered: the door still works, the
// hand-off is off this boot.
NimBLEService*        s_hub_service = nullptr;
NimBLECharacteristic* s_hub = nullptr;

// The mailbox: one command at a time, host task → loop task.
uint8_t s_rx[improv::MAX_COMMAND_LEN];
volatile size_t s_rx_len = 0;
volatile bool s_rx_pending = false;

// The hub hand-off's own mailbox, the same release/acquire discipline: one
// document at a time, host task → loop task. A length past the buffer marks
// an oversize write (nothing copied; the loop task answers TooLong).
uint8_t  s_hub_rx[hub_handoff::MAX_PAYLOAD];
volatile uint16_t s_hub_rx_len = 0;
volatile bool s_hub_rx_pending = false;
volatile bool s_hub_rx_unencrypted = false;
hub_handoff::Status s_hub_status = hub_handoff::Status::None;

volatile bool s_connected = false;
volatile bool s_readvertise = false;   // a disconnect handed the radio back
// The one link, as the host task reports it: connect/disconnect edges the
// loop task folds into the session (link_up/link_down), and the handle an
// idle disconnect needs.
volatile uint8_t  s_conn_gen = 0;        // bumped by every connect
volatile uint16_t s_conn_handle = 0;
// A write that arrived on an unencrypted link: refused NotAuthorized on the
// loop task (the characteristic's WRITE_ENC should already have stopped it;
// this is the second check).
volatile bool s_rx_unencrypted = false;

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

// What a READ of HUB answers: the status byte and nothing else. Set on the
// loop task; the host task sets it too, right after it takes a write, so
// the document a phone wrote is never what the next read returns.
void publish_hub_status() {
  if (!s_hub) return;
  const uint8_t b = (uint8_t)s_hub_status;
  s_hub->setValue(&b, 1);
}

void set_hub_status(hub_handoff::Status st) {
  s_hub_status = st;
  publish_hub_status();
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
  // The mailbox crosses tasks (and cores): the frame is written first and
  // the pending flag published with release; the loop task takes the flag
  // with acquire before it reads the frame.
  void take(NimBLECharacteristic* c, bool encrypted) {
    if (__atomic_load_n(&s_rx_pending, __ATOMIC_ACQUIRE)) return;   // one at a time
    if (!encrypted) {
      // The second check (the first is the characteristic's WRITE_ENC):
      // nothing from a link that is not encrypted is parsed, let alone
      // believed. The loop task answers NotAuthorized.
      s_rx_unencrypted = true;
      s_rx_len = 0;
      __atomic_store_n(&s_rx_pending, true, __ATOMIC_RELEASE);
      return;
    }
    const auto v = c->getValue();
    const size_t n = v.length();
    if (n == 0 || n > sizeof(s_rx)) {
      // Not a frame we could ever accept: queue a one-byte marker so the
      // loop task answers InvalidRpc rather than silence.
      s_rx[0] = 0; s_rx_len = 1;
      __atomic_store_n(&s_rx_pending, true, __ATOMIC_RELEASE);
      return;
    }
    memcpy(s_rx, v.data(), n);
    s_rx_len = n;
    __atomic_store_n(&s_rx_pending, true, __ATOMIC_RELEASE);
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    take(c, !s_id.require_encryption || info.isEncrypted());
  }
#else
  // 1.4.x calls both onWrite overloads; only the one with the descriptor is
  // overridden here, so a write is taken once.
  void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc* desc) override {
    take(c, !s_id.require_encryption || (desc && desc->sec_state.encrypted));
  }
#endif
};

class ServerCb : public NimBLEServerCallbacks {
  void up(uint16_t handle) {
    s_conn_handle = handle;
    s_connected = true;
    s_conn_gen = (uint8_t)(s_conn_gen + 1);
  }
  void down() {
    s_connected = false;
    s_readvertise = true;
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  void onConnect(NimBLEServer* /*s*/, NimBLEConnInfo& info) override { up(info.getConnHandle()); }
  void onDisconnect(NimBLEServer* /*s*/, NimBLEConnInfo& /*info*/, int /*reason*/) override { down(); }
#else
  // 1.4.x calls both onConnect overloads; only the one with the descriptor
  // is overridden here, so an edge is taken once.
  void onConnect(NimBLEServer* /*s*/, ble_gap_conn_desc* desc) override {
    up(desc ? desc->conn_handle : 0);
  }
  void onDisconnect(NimBLEServer* /*s*/) override { down(); }
#endif
};

// The HUB write: the same mailbox discipline as the RPC command, a second
// buffer so a hand-off and a command in the same instant lose neither.
class HubCb : public NimBLECharacteristicCallbacks {
  void take(NimBLECharacteristic* c, bool encrypted) {
    if (__atomic_load_n(&s_hub_rx_pending, __ATOMIC_ACQUIRE)) { scrub(c); return; }   // one at a time
    if (!encrypted) {
      // The second check (the first is the characteristic's WRITE_ENC):
      // nothing from a link that is not encrypted is parsed. The loop task
      // answers Rejected.
      s_hub_rx_unencrypted = true;
      s_hub_rx_len = 0;
      scrub(c);
      __atomic_store_n(&s_hub_rx_pending, true, __ATOMIC_RELEASE);
      return;
    }
    const auto v = c->getValue();
    const size_t n = v.length();
    if (n == 0 || n > sizeof(s_hub_rx)) {
      // Nothing copied: the loop task answers TooLong / Malformed from the
      // length alone.
      s_hub_rx_len = (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n);
      scrub(c);
      __atomic_store_n(&s_hub_rx_pending, true, __ATOMIC_RELEASE);
      return;
    }
    memcpy(s_hub_rx, v.data(), n);
    s_hub_rx_len = (uint16_t)n;
    scrub(c);
    __atomic_store_n(&s_hub_rx_pending, true, __ATOMIC_RELEASE);
  }
  // The stack stored the written document as the characteristic's value;
  // put the status byte back before this callback returns, over a zeroed
  // buffer, so neither a read nor the attribute's own memory keeps it.
  static void scrub(NimBLECharacteristic* c) {
    uint8_t zero[hub_handoff::MAX_PAYLOAD];
    memset(zero, 0, sizeof(zero));
    c->setValue(zero, sizeof(zero));
    const uint8_t b = (uint8_t)s_hub_status;
    c->setValue(&b, 1);
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    take(c, !s_id.require_encryption || info.isEncrypted());
  }
#else
  // 1.4.x calls both onWrite overloads; only the one with the descriptor is
  // overridden here, so a write is taken once.
  void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc* desc) override {
    take(c, !s_id.require_encryption || (desc && desc->sec_state.encrypted));
  }
#endif
};

CommandCb s_command_cb;
HubCb     s_hub_cb;
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
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    scan.addServiceUUID(NimBLEUUID(improv::SERVICE_UUID));
#else
    // 1.4.x spells the complete 128-bit service list differently.
    scan.setCompleteServices(NimBLEUUID(improv::SERVICE_UUID));
#endif
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
  improv::session_touch(s_session, now_ms);   // it asked for something
  if (s_rx_unencrypted) {
    s_rx_unencrypted = false;
    __atomic_store_n(&s_rx_pending, false, __ATOMIC_RELEASE);
    s_session.error = Error::NotAuthorized;
    publish_error();
    Serial.println("[IMPROV] write on an unencrypted link refused");
    return;
  }
  uint8_t frame[improv::MAX_COMMAND_LEN];
  const size_t n = s_rx_len;
  memcpy(frame, s_rx, n);
  improv::wipe(s_rx, sizeof(s_rx));
  __atomic_store_n(&s_rx_pending, false, __ATOMIC_RELEASE);

  improv::ParsedCommand cmd = improv::parse_command(frame, n);
  improv::wipe(frame, sizeof(frame));
  if (cmd.verdict != Parse::Ok) {
    if (cmd.command == Command::WifiSettings) {
      // A credentials frame that did not parse still counts against the
      // open door's cap: garbage is an attempt too.
      improv::session_on_malformed_wifi_settings(s_session, now_ms, s_timing);
    } else {
      improv::session_on_bad_packet(s_session, cmd.verdict);
    }
    publish_error();
    return;
  }

  switch (cmd.command) {
    case Command::WifiSettings: {
      const Error e = improv::session_on_wifi_settings(s_session, now_ms, s_timing);
      if (e == Error::None) {
        // A new join is a new hand-off slot: the status reads None until the
        // phone writes the hub for this one.
        set_hub_status(hub_handoff::Status::None);
        if (!setup_portal_submit_join(cmd.wifi.ssid, cmd.wifi.password, now_ms)) {
          // The portal refused (a wizard join is testing, or it is not up
          // after all): the join never started.
          improv::session_on_join_result(s_session, false, now_ms);
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

// The hub hand-off, on the loop task: the mailbox's document is parsed
// (hub_handoff.h) and, when the session has a join to attach it to, handed
// to the board through Identity::store_hub. Every path ends in the status
// byte the phone reads; a refusal is one health line, never the bytes.
void handle_hub(uint32_t now_ms) {
  using hub_handoff::Status;
  using hub_handoff::Verdict;
  improv::session_touch(s_session, now_ms);   // the link did something
  if (s_hub_rx_unencrypted) {
    s_hub_rx_unencrypted = false;
    s_hub_rx_len = 0;
    __atomic_store_n(&s_hub_rx_pending, false, __ATOMIC_RELEASE);
    set_hub_status(Status::Rejected);
    Serial.println("[IMPROV] hub hand-off on an unencrypted link refused");
    return;
  }
  uint8_t doc[hub_handoff::MAX_PAYLOAD];
  const size_t n = s_hub_rx_len;
  const bool oversize = n > sizeof(doc);
  if (!oversize) memcpy(doc, s_hub_rx, n);
  improv::wipe(s_hub_rx, sizeof(s_hub_rx));
  s_hub_rx_len = 0;
  __atomic_store_n(&s_hub_rx_pending, false, __ATOMIC_RELEASE);

  // Only the phone that has a join in this session may hand over a hub:
  // Provisioned (it gave the device its Wi-Fi), or Provisioning for a phone
  // that writes a beat before the verdict lands.
  if (s_session.state != State::Provisioned && s_session.state != State::Provisioning) {
    improv::wipe(doc, sizeof(doc));
    set_hub_status(Status::Rejected);
    Serial.println("[IMPROV] hub hand-off refused: no join this session");
    return;
  }

  hub_handoff::HubHandoff hub;
  const Verdict v = hub_handoff::parse_hub_handoff(oversize ? nullptr : doc, n, hub);
  improv::wipe(doc, sizeof(doc));
  if (v != Verdict::Ok) {
    set_hub_status(Status::Rejected);
    Serial.printf("[IMPROV] hub hand-off refused: %s\n", hub_handoff::verdict_name(v));
    return;
  }
  const bool stored = s_id.store_hub ? s_id.store_hub(hub) : false;
  if (stored) {
    Serial.printf("[IMPROV] hub hand-off stored: %s:%u%s\n", hub.host, (unsigned)hub.port,
                  hub.has_credentials ? " with a login" : "");
  } else {
    Serial.println(s_id.store_hub ? "[IMPROV] hub hand-off not stored (the store refused)"
                                  : "[IMPROV] hub hand-off refused: this board takes none");
  }
  hub_handoff::wipe(hub);
  set_hub_status(stored ? Status::Stored : Status::Rejected);
}

void follow_join(uint32_t now_ms) {
  if (s_session.state != State::Provisioning) return;
  switch (setup_portal_join_state()) {
    case SetupPortalJoin::Success: {
      improv::session_on_join_result(s_session, true, now_ms);
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
      improv::session_on_join_result(s_session, false, now_ms);
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

bool begin(const Identity& identity, bool no_credentials, uint32_t now_ms) {
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
  // 1.4.x: getAdvertising() allocates lazily and is never null, so the
  // initialization query is the one honest answer.
  if (!NimBLEDevice::getInitialized()) {
    NimBLEDevice::init(identity.device_name ? identity.device_name : "SecuraCV");
    if (!NimBLEDevice::getInitialized()) return false;
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

  // The beacon is already on air by now, and NimBLE's host refuses every
  // GATT-table change while advertising (ble_gatts_mutable: EBUSY): on 2.x
  // the server's start() would fail and the door would never register; on
  // 1.4.x createServer() asserts and the board reboots. Take the beacon off
  // air for the registration; s_readvertise puts it back on the first tick.
  if (NimBLEAdvertising* adv = NimBLEDevice::getAdvertising()) adv->stop();

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

  // The SecuraCV companion service beside the Improv one — the same UUID
  // the WAP registers for its claim; here it carries HUB alone. Outside the
  // Improv service so a standard client never meets it, and not in the
  // advert (the scan response is full; the phone that needs it is already
  // connected). Encrypted by property AND by the handler, like the RPC
  // write. Fail-soft: a board that cannot register it keeps its Wi-Fi door.
  s_hub_service = s_server->createService(hub_handoff::SERVICE_UUID);
  if (s_hub_service) {
    const uint32_t hub_props = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ
        | (identity.require_encryption ? (NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::READ_ENC) : 0);
    s_hub = s_hub_service->createCharacteristic(hub_handoff::HUB_UUID, hub_props);
    if (s_hub) {
      s_hub->setCallbacks(&s_hub_cb);
      publish_hub_status();
    } else {
      s_server->removeService(s_hub_service, /*deleteSvc=*/true);
      s_hub_service = nullptr;
    }
  }
  if (!s_hub) Serial.println("[IMPROV] companion service unavailable — no hub hand-off this boot");

#if ESP_ARDUINO_VERSION_MAJOR < 3
  if (!s_service->start()) {   // 1.4.x starts services one by one; 2.x starts them with the server
    Serial.println("[IMPROV] service registration refused — door stays shut this boot");
    s_readvertise = true;      // the beacon goes back on air regardless
    return false;
  }
  if (s_hub_service && !s_hub_service->start()) {
    Serial.println("[IMPROV] companion service refused — no hub hand-off this boot");
    s_server->removeService(s_hub_service, /*deleteSvc=*/true);
    s_hub_service = nullptr;
    s_hub = nullptr;
  }
  s_server->start();
#else
  if (!s_server->start()) {
    Serial.println("[IMPROV] GATT start refused — door stays shut this boot");
    s_readvertise = true;
    return false;
  }
#endif

  s_timing = improv::Timing{};
  s_timing.provisioning_timeout_ms = PROVISIONING_TIMEOUT_MS;
  s_timing.first_boot_window_ms = (uint32_t)(IMPROV_FIRST_BOOT_WINDOW_MS);
  const uint32_t window_used = no_credentials ? window_used_from_rtc() : 0;
  improv::session_begin(s_session, no_credentials, now_ms, s_timing, window_used);
  if (window_used) {
    Serial.printf("[IMPROV] first-boot window: %lu s already spent before this restart\n",
                  (unsigned long)(window_used / 1000));
  }
  s_active = true;
  publish_state();
  publish_error();
  s_readvertise = true;
  Serial.printf("[IMPROV] Improv Wi-Fi service up (%s).\n",
                improv::door_open(s_session)
                    ? "door open: no credentials stored, first-boot window running"
                    : (no_credentials ? "door shut: tap-only build" : "door shut: credentials stored"));
  return true;
}

void follow_link(uint32_t now_ms) {
  // The link as a LEVEL plus a connect generation, not two edge flags: a
  // drop and a reconnect inside one loop pass (a long inference, a TLS
  // connect right after the join) would otherwise apply up-then-down and
  // leave the session believing a live link is gone.
  static uint8_t seen_gen = 0;
  const bool connected = s_connected;
  const uint8_t gen = s_conn_gen;
  if (connected != s_session.link_up || gen != seen_gen) {
    seen_gen = gen;
    if (connected) improv::session_link_up(s_session, now_ms);
    else improv::session_link_down(s_session);
  }
  if (s_connected && improv::idle_disconnect_due(s_session, now_ms, s_timing) && s_server) {
    // A client that asked for nothing, or one whose join is done and
    // answered: hand the radio back so the beacon returns and nobody can
    // park on the single link.
    s_server->disconnect(s_conn_handle);
    improv::session_link_down(s_session);
    Serial.printf("[IMPROV] link dropped (%s)\n",
                  s_session.state == State::Provisioned ? "joined, verdict delivered" : "idle");
  }
}

void tick(uint32_t now_ms, bool no_credentials) {
  if (!s_active) return;
  const State before = s_session.state;

  improv::session_set_no_credentials(s_session, no_credentials, now_ms, s_timing);
  // Keep the window's spent time where a software reset cannot lose it.
  if (s_session.door == improv::Door::NoCredentials || s_session.window_spent) {
    if ((uint32_t)(now_ms - s_window_saved_ms) >= WINDOW_SAVE_PERIOD_MS) {
      s_window_saved_ms = now_ms;
      window_save(improv::session_window_used_ms(s_session, now_ms, s_timing));
    }
  } else if (!no_credentials && s_window_rtc.magic == WINDOW_MAGIC && s_session.window_at_ms == 0) {
    // Credentials landed: the window is moot; a later factory reset starts fresh.
    s_window_rtc.magic = 0;
    s_window_rtc.used_ms = 0;
  }
  follow_link(now_ms);
  if (__atomic_load_n(&s_rx_pending, __ATOMIC_ACQUIRE)) handle_command(now_ms);
  if (__atomic_load_n(&s_hub_rx_pending, __ATOMIC_ACQUIRE)) handle_hub(now_ms);
  follow_join(now_ms);
  follow_scan(now_ms);
  if (improv::session_tick(s_session, now_ms, s_timing)) {
    publish_error();
    publish_state();
  }

  if (s_session.state != before) {
    publish_state();
    if (s_session.state == State::Authorized || s_session.state == State::AwaitingAuthorization) {
      Serial.printf("[IMPROV] door %s%s\n", improv::door_open(s_session) ? "open" : "shut",
                    s_session.door == improv::Door::Tap ? " (tap)"
                    : s_session.door == improv::Door::NoCredentials ? " (no credentials)"
                    : s_session.window_spent ? " (window over)" : "");
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

hub_handoff::Status hub_status() { return s_hub_status; }

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

// This file sits in common/network beside setup_portal.cpp, so PlatformIO's
// dependency finder compiles it into every env that touches that folder —
// the display family included, some of whose boards carry no NimBLE at all
// (the 4 MB C3 nightlight). The stub must therefore compile with or without
// the stack: with it, advertise() is the carrier's own beacon-only put-on-air;
// without it, nothing calls advertise() and it is empty.
#if __has_include(<NimBLEDevice.h>)
#include <NimBLEDevice.h>
#include <string>
#define IMPROV_BLE_HAVE_NIMBLE 1
#else
#define IMPROV_BLE_HAVE_NIMBLE 0
#endif

namespace canary {
namespace net {
namespace improv_ble {
bool begin(const Identity&, bool, uint32_t) { return false; }
void tick(uint32_t, bool) {}
void tap(uint32_t) {}
bool active() { return false; }
improv::State state() { return improv::State::Stopped; }
bool setup_open() { return false; }
hub_handoff::Status hub_status() { return hub_handoff::Status::None; }
void advertise(const uint8_t* beacon_mfg, size_t beacon_len) {
#if IMPROV_BLE_HAVE_NIMBLE
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
#else
  (void)beacon_mfg;
  (void)beacon_len;
#endif
}
}  // namespace improv_ble
}  // namespace net
}  // namespace canary

#endif  // FEATURE_IMPROV
