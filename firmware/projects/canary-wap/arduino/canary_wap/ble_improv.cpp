/*
 * SecuraCV Canary WAP — the Bluetooth setup door — implementation.
 * See ble_improv.h. The rules are improv_core.h's (host-tested); this file
 * moves bytes on and off the radio and bridges into the sketch.
 *
 * Threading, as ble_provision: NimBLE's callbacks run on its host task. The
 * write callback only copies the packet into a mailbox; parsing, the
 * session, the join bridge and the notifications run in tick() on the loop
 * task, which also owns the channel's security profile.
 */

#include "build_config.h"
#include "ble_improv.h"

#if FEATURE_IMPROV && __has_include(<NimBLEDevice.h>)

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <string>

#include "bluetooth_channel.h"
#include "health_log.h"
#include "improv_core.h"

// External-linkage bridges defined in canary_wap.ino.
extern bool   ble_request_wifi_provisioning(const char* ssid, const char* password);
extern bool   ble_improv_door_should_open();                          // no credentials stored
extern size_t ble_improv_reach_url(char* out, size_t cap);            // http(s)://<mdns>.local/
extern size_t ble_improv_receipt_json(char* out, size_t cap);         // the pairing receipt
extern void   ble_improv_identify();                                  // blink / chirp
extern size_t ble_improv_name_suffix(char* out, size_t cap);          // "AB12"
extern const char* ble_improv_firmware_version();                    // FIRMWARE_VERSION

namespace ble_improv {

namespace {

namespace improv = canary::net::improv;
using improv::Command;
using improv::Error;
using improv::Parse;
using improv::State;

// WiFi.status() reports a failed join quickly; a join that never reports
// reads as failed after this long.
constexpr uint32_t PROVISIONING_TIMEOUT_MS = 40000;
// The receipt stays readable this long after the join succeeded; a phone
// that never reads it needs the BOOT-tap route later.
constexpr uint32_t RECEIPT_TTL_MS = 60000;
constexpr size_t   RECEIPT_MAX = 1024;

bool s_active = false;
improv::Session s_session;
improv::Timing s_timing;

NimBLEService*        s_service = nullptr;
NimBLECharacteristic* s_state = nullptr;
NimBLECharacteristic* s_error = nullptr;
NimBLECharacteristic* s_command = nullptr;
NimBLECharacteristic* s_result = nullptr;
NimBLECharacteristic* s_caps = nullptr;
NimBLECharacteristic* s_receipt = nullptr;

char s_adv_name[16] = {0};

// The mailbox: one command at a time, host task → loop task, with the
// connection it came on (the receipt is that connection's alone).
uint8_t  s_rx[improv::MAX_COMMAND_LEN];
volatile size_t   s_rx_len = 0;
volatile uint16_t s_rx_conn = 0;
volatile bool     s_rx_pending = false;

// The receipt gate: armed for one connection handle when the join it asked
// for succeeded; disarmed by the first read, the TTL, or the door shutting.
volatile uint16_t s_receipt_conn = 0;
volatile bool     s_receipt_armed = false;
uint32_t          s_receipt_armed_ms = 0;
uint16_t          s_provisioning_conn = 0;

bool s_door_applied = false;   // the security profile the channel holds now

uint32_t s_accepted = 0;
uint32_t s_refused = 0;
uint32_t s_receipts = 0;

uint8_t capabilities() { return improv::CAP_IDENTIFY | improv::CAP_DEVICE_INFO; }

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

void disarm_receipt() {
  s_receipt_armed = false;
  s_receipt_conn = 0;
  if (s_receipt) s_receipt->setValue((const uint8_t*)"{}", 2);
}

// ── NimBLE callbacks (host task) ───────────────────────────────────────────

class CommandCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    const auto v = c->getValue();
    const size_t n = v.length();
    if (s_rx_pending) return;   // one at a time; the loop task is on it
    if (n == 0 || n > sizeof(s_rx)) {
      s_rx[0] = 0; s_rx_len = 1;
    } else {
      memcpy(s_rx, v.data(), n);
      s_rx_len = n;
    }
    s_rx_conn = info.getConnHandle();
    s_rx_pending = true;
  }
};

class ReceiptCb : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    // Served to the link that provisioned, once; everyone else reads "{}".
    // The value is set here, right before NimBLE answers the read, and
    // wiped right after it has been handed over — it carries the token.
    if (!s_receipt_armed || info.getConnHandle() != s_receipt_conn) {
      c->setValue((const uint8_t*)"{}", 2);
      return;
    }
    char* json = (char*)malloc(RECEIPT_MAX);
    if (!json) { c->setValue((const uint8_t*)"{}", 2); return; }
    const size_t n = ble_improv_receipt_json(json, RECEIPT_MAX);
    if (n == 0 || n >= RECEIPT_MAX) {
      c->setValue((const uint8_t*)"{}", 2);
    } else {
      c->setValue((const uint8_t*)json, n);
      s_receipts++;
    }
    improv::wipe(json, RECEIPT_MAX);
    free(json);
    // One read: the next one, from anyone, gets "{}" (the loop task also
    // wipes the characteristic's own copy on its next pass).
    s_receipt_armed = false;
  }
};

CommandCb s_command_cb;
ReceiptCb s_receipt_cb;

// ── Commands (loop task) ───────────────────────────────────────────────────

void answer_device_info() {
  const char* strings[4] = { "canary-wap", ble_improv_firmware_version(), "XIAO ESP32-S3", s_adv_name };
  send_result(Command::GetDeviceInfo, strings, 4);
}

void handle_command(uint32_t now_ms) {
  uint8_t frame[improv::MAX_COMMAND_LEN];
  const size_t n = s_rx_len;
  const uint16_t conn = s_rx_conn;
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
        if (ble_request_wifi_provisioning(cmd.wifi.ssid, cmd.wifi.password)) {
          s_provisioning_conn = conn;
          s_accepted++;
        } else {
          improv::session_on_join_result(s_session, false);
          s_refused++;
        }
      } else {
        s_refused++;
      }
      improv::wipe(&cmd.wifi, sizeof(cmd.wifi));
      publish_error();
      publish_state();
      log_health(s_session.state == State::Provisioning ? SCV_LOG_INFO : SCV_LOG_WARNING,
                 SCV_CAT_BLUETOOTH,
                 s_session.state == State::Provisioning
                     ? "Setup door: Wi-Fi settings accepted, joining"
                     : "Setup door: Wi-Fi settings refused", nullptr);
      break;
    }
    case Command::Identify:
      ble_improv_identify();
      s_session.error = Error::None;
      publish_error();
      break;
    case Command::GetDeviceInfo:
      s_session.error = Error::None;
      publish_error();
      answer_device_info();
      break;
    default:
      improv::session_on_unknown_command(s_session);
      publish_error();
      break;
  }
}

void follow_join(uint32_t now_ms) {
  if (s_session.state != State::Provisioning) return;
  const wl_status_t ws = WiFi.status();
  if (ws == WL_CONNECTED) {
    improv::session_on_join_result(s_session, true);
    publish_error();
    publish_state();
    char url[96] = {0};
    ble_improv_reach_url(url, sizeof(url));
    const char* strings[1] = { url };
    send_result(Command::WifiSettings, strings, 1);
    // The receipt for the link that asked, for a minute.
    s_receipt_conn = s_provisioning_conn;
    s_receipt_armed = true;
    s_receipt_armed_ms = now_ms;
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
               "Setup door: joined; receipt armed for the provisioning link", nullptr);
  } else if (ws == WL_CONNECT_FAILED || ws == WL_NO_SSID_AVAIL) {
    improv::session_on_join_result(s_session, false);
    publish_error();
    publish_state();
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH, "Setup door: join failed", nullptr);
  }
}

void follow_receipt(uint32_t now_ms) {
  if (s_receipt_armed &&
      (int32_t)(now_ms - s_receipt_armed_ms) >= (int32_t)RECEIPT_TTL_MS) {
    disarm_receipt();
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
               "Setup door: receipt unread for a minute — withdrawn (the BOOT tap still serves it)", nullptr);
  }
  // After a read the host task cleared the arm; wipe the characteristic's
  // own copy of the token here, on the loop task, where setValue is calm.
  if (!s_receipt_armed && s_receipt && s_receipt->getValue().length() > 2) {
    s_receipt->setValue((const uint8_t*)"{}", 2);
  }
}

void follow_door() {
  const bool open = door_open_now();
  if (open == s_door_applied) return;
  // The channel refuses the swap while a Numeric Comparison is pending;
  // try again next pass.
  if (bluetooth_channel::set_setup_door(open)) {
    s_door_applied = open;
    if (!open) disarm_receipt();
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
               open ? "Setup door open: Just Works pairing accepted for the Improv service only"
                    : "Setup door shut: Numeric Comparison pairing restored", nullptr);
  }
}

}  // namespace

// ── Public API ─────────────────────────────────────────────────────────────

bool init(NimBLEServer* server) {
  if (!server) return false;
  if (s_service) return true;

  s_service = server->createService(improv::SERVICE_UUID);
  if (!s_service) return false;

  s_state   = s_service->createCharacteristic(improv::CURRENT_STATE_UUID,
                                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_error   = s_service->createCharacteristic(improv::ERROR_STATE_UUID,
                                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  // Encrypted, not authenticated: the Just Works key the door admits.
  s_command = s_service->createCharacteristic(improv::RPC_COMMAND_UUID,
                                              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
                                                | NIMBLE_PROPERTY::WRITE_ENC);
  s_result  = s_service->createCharacteristic(improv::RPC_RESULT_UUID,
                                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
                                                | NIMBLE_PROPERTY::READ_ENC);
  s_caps    = s_service->createCharacteristic(improv::CAPABILITIES_UUID, NIMBLE_PROPERTY::READ);
  s_receipt = s_service->createCharacteristic(RECEIPT_UUID,
                                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
  if (!s_state || !s_error || !s_command || !s_result || !s_caps || !s_receipt) return false;
  s_command->setCallbacks(&s_command_cb);
  s_receipt->setCallbacks(&s_receipt_cb);
  s_receipt->setValue((const uint8_t*)"{}", 2);

  const uint8_t caps = capabilities();
  s_caps->setValue(&caps, 1);

  // "WAP-XXXX": the same four characters as the "SCV-XXXX" name Opera
  // advertises, so the phone can tell the beacon and the door are one device.
  char suffix[5] = {0};
  if (!ble_improv_name_suffix(suffix, sizeof(suffix))) snprintf(suffix, sizeof(suffix), "0000");
  snprintf(s_adv_name, sizeof(s_adv_name), "WAP-%s", suffix);

  s_service->start();

  const bool no_wifi = ble_improv_door_should_open();
  improv::session_begin(s_session, no_wifi);
  s_timing = improv::Timing{};
  s_timing.provisioning_timeout_ms = PROVISIONING_TIMEOUT_MS;
  s_active = true;
  publish_state();
  publish_error();
  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
             no_wifi ? "Setup door service ready (door open: no Wi-Fi of its own yet)"
                     : "Setup door service ready (door shut: credentials stored)", nullptr);
  return true;
}

void tick() {
  if (!s_active) return;
  const uint32_t now_ms = millis();
  improv::session_set_no_wifi(s_session, ble_improv_door_should_open(), now_ms);
  if (s_rx_pending) handle_command(now_ms);
  follow_join(now_ms);
  follow_receipt(now_ms);
  if (improv::session_tick(s_session, now_ms, s_timing)) {
    publish_error();
    publish_state();
  }
  follow_door();
}

bool setup_open() { return door_open_now() && s_door_applied; }

const char* adv_name() { return s_adv_name; }

void compose_scan_response(NimBLEAdvertisementData& scan) {
  scan.addServiceUUID(NimBLEUUID(improv::SERVICE_UUID));
  uint8_t sd[improv::SERVICE_DATA_LEN];
  improv::build_service_data(sd, s_session.state, capabilities());
  scan.setServiceData(NimBLEUUID((uint16_t)improv::SERVICE_DATA_UUID16),
                      std::string((const char*)sd, sizeof(sd)));
}

bool get_stats(Stats* out) {
  if (!out) return false;
  out->credentials_accepted = s_accepted;
  out->credentials_refused = s_refused;
  out->receipts_served = s_receipts;
  return true;
}

}  // namespace ble_improv

#else  // !(FEATURE_IMPROV && __has_include(<NimBLEDevice.h>))

#include "ble_improv.h"
namespace ble_improv {
bool init(NimBLEServer* /*server*/) { return false; }
void tick() {}
bool setup_open() { return false; }
const char* adv_name() { return ""; }
void compose_scan_response(NimBLEAdvertisementData& /*scan*/) {}
bool get_stats(Stats* out) {
  if (!out) return false;
  out->credentials_accepted = 0;
  out->credentials_refused = 0;
  out->receipts_served = 0;
  return true;
}
}  // namespace ble_improv

#endif  // FEATURE_IMPROV && __has_include(<NimBLEDevice.h>)
