/*
 * SecuraCV Canary WAP — the Bluetooth setup door — implementation.
 * See ble_improv.h. The rules are improv_core.h's (host-tested); this file
 * moves bytes on and off the radio and bridges into the sketch.
 *
 * Threading, as ble_provision: NimBLE's callbacks run on its host task. The
 * write callback only copies the packet into a mailbox; parsing, the
 * session, the join bridge, the claim's minting and the notifications run
 * in tick() on the loop task, which also owns the channel's security
 * profile. The claim read callback copies a buffer the loop task filled
 * and clears the arm; the loop task wipes every copy on its next pass.
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
#include "claim_ticket.h"
#include "health_log.h"
#include "improv_core.h"

// External-linkage bridges defined in canary_wap.ino.
extern bool   ble_improv_submit_join(const char* ssid, const char* password);   // RAM-only until the verdict
extern void   ble_improv_join_verdict(bool joined);                              // persist on success, forget on failure
extern bool   ble_improv_door_should_open();                          // no credentials stored
extern size_t ble_improv_reach_url(char* out, size_t cap);            // http(s)://<mdns>.local/
extern size_t ble_improv_mint_claim(char* out, size_t cap, uint32_t now_ms);  // the claim JSON
extern void   ble_improv_wipe_claim();                                // forget the outstanding claim
extern void   ble_improv_identify();                                  // blink / chirp
extern size_t ble_improv_name_suffix(char* out, size_t cap);          // "AB12"
extern const char* ble_improv_firmware_version();                    // FIRMWARE_VERSION

namespace ble_improv {

namespace {

namespace improv = canary::net::improv;
namespace claim_ticket = canary::net::claim_ticket;
using improv::Command;
using improv::Error;
using improv::Parse;
using improv::State;

// WiFi.status() reports a failed join quickly; a join that never reports
// reads as failed after this long.
constexpr uint32_t PROVISIONING_TIMEOUT_MS = 40000;
// The first-boot window: build_config.h's (beside FEATURE_IMPROV); the
// core's default (half an hour) if a build left it out. 0 = tap-only.
#ifndef IMPROV_FIRST_BOOT_WINDOW_MS
#define IMPROV_FIRST_BOOT_WINDOW_MS (improv::Timing{}.first_boot_window_ms)
#endif
// The claim stays readable over the link that provisioned for the ticket's
// own TTL; a phone that never reads it needs the BOOT-tap route later.
constexpr uint32_t CLAIM_TTL_MS = claim_ticket::TTL_MS;
// After the provisioning link's first read of the claim, the value stays
// readable this long — enough for the Read Blob continuations of a value
// longer than the MTU, at any connection interval a phone negotiates.
constexpr uint32_t CLAIM_READ_GRACE_MS = 3000;
// The claim JSON (ble_improv_mint_claim): seven short keys, a 31-char
// device id, 32 hex, a URL under 120 bytes, a 64-hex fingerprint, a dotted
// address and a 39-char host — under 420 bytes at their longest.
constexpr size_t CLAIM_JSON_MAX = 512;
// How often the loop task asks the stack whether the door's link is still
// held (the channel's server callbacks are the channel's; the door learns
// of a link when it writes, and of its end here).
constexpr uint32_t LINK_POLL_MS = 250;
constexpr uint16_t NO_CONN = 0xFFFF;

bool s_active = false;
improv::Session s_session;
improv::Timing s_timing;

NimBLEService*        s_service = nullptr;
NimBLECharacteristic* s_state = nullptr;
NimBLECharacteristic* s_error = nullptr;
NimBLECharacteristic* s_command = nullptr;
NimBLECharacteristic* s_result = nullptr;
NimBLECharacteristic* s_caps = nullptr;
NimBLEService*        s_claim_service = nullptr;
NimBLECharacteristic* s_claim = nullptr;

char s_adv_name[16] = {0};

// The mailbox: one command at a time, host task → loop task, with the
// connection it came on (the claim is that connection's alone). The
// address rides along so a handle the stack reuses for another peer is
// not mistaken for the link that wrote.
uint8_t  s_rx[improv::MAX_COMMAND_LEN];
volatile size_t   s_rx_len = 0;
volatile uint16_t s_rx_conn = NO_CONN;
ble_addr_t        s_rx_addr = {};
volatile bool     s_rx_pending = false;
// A write that arrived on an unencrypted link: refused NotAuthorized on the
// loop task (the characteristic's WRITE_ENC should already have stopped
// it; this is the second check, and the two are deliberate).
volatile bool     s_rx_unencrypted = false;

// The one link that uses the door: recorded at its first write, released
// by the idle disconnect or when the stack no longer holds it.
uint16_t   s_door_conn = NO_CONN;
ble_addr_t s_door_addr = {};
uint32_t   s_link_polled_ms = 0;

// The link whose WIFI_SETTINGS the join in flight came from.
uint16_t   s_provisioning_conn = NO_CONN;
ble_addr_t s_provisioning_addr = {};

// The claim gate: armed for one connection (handle AND address) when the
// join it asked for succeeded; disarmed by the first read, the TTL, the
// door reopening, or the door shutting for any reason but that join.
volatile bool     s_claim_armed = false;
volatile uint16_t s_claim_conn = NO_CONN;
// The first read's clock: a ~300-byte value is read as one Read Response
// plus Read Blob continuations, each its own onRead, so the arm must
// outlive the whole read. The loop task withdraws the claim a grace after
// the first read, or when the link that read it is gone.
volatile bool     s_claim_read_seen = false;
volatile uint32_t s_claim_read_ms = 0;
ble_addr_t        s_claim_addr = {};
uint32_t          s_claim_armed_ms = 0;
char              s_claim_json[CLAIM_JSON_MAX];
volatile size_t   s_claim_json_len = 0;

bool s_door_applied = false;   // the security profile the channel holds now

uint32_t s_accepted = 0;
uint32_t s_refused = 0;
uint32_t s_claims = 0;

uint8_t capabilities() { return improv::CAP_IDENTIFY | improv::CAP_DEVICE_INFO; }

bool door_open_now() { return s_active && improv::advert_for(s_session) == improv::Advert::Improv; }

bool same_addr(const ble_addr_t& a, const ble_addr_t& b) {
  return a.type == b.type && memcmp(a.val, b.val, sizeof(a.val)) == 0;
}

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

// Loop task. Withdraws the claim from the air (the characteristic answers
// "{}" again) and wipes the loop task's copy; `wipe_ticket` also forgets
// the ticket itself (claim_ticket::wipe in the sketch), so no HTTP take can
// spend it either.
void disarm_claim(bool wipe_ticket) {
  s_claim_armed = false;
  s_claim_read_seen = false;
  s_claim_read_ms = 0;
  s_claim_conn = NO_CONN;
  memset(&s_claim_addr, 0, sizeof(s_claim_addr));
  improv::wipe(s_claim_json, sizeof(s_claim_json));
  s_claim_json_len = 0;
  if (s_claim) s_claim->setValue((const uint8_t*)"{}", 2);
  if (wipe_ticket) ble_improv_wipe_claim();
}

// ── NimBLE callbacks (host task) ───────────────────────────────────────────

class CommandCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    if (s_rx_pending) return;   // one at a time; the loop task is on it
    s_rx_conn = info.getConnHandle();
    s_rx_addr = *info.getAddress().getBase();
    if (!info.isEncrypted()) {
      // The second check (the first is the characteristic's WRITE_ENC):
      // nothing from a link that is not encrypted is parsed, let alone
      // believed. The loop task answers NotAuthorized and counts it.
      s_rx_unencrypted = true;
      s_rx_len = 0;
      s_rx_pending = true;
      return;
    }
    const auto v = c->getValue();
    const size_t n = v.length();
    if (n == 0 || n > sizeof(s_rx)) {
      s_rx[0] = 0; s_rx_len = 1;
    } else {
      memcpy(s_rx, v.data(), n);
      s_rx_len = n;
    }
    s_rx_pending = true;
  }
};

class ClaimCb : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    // Served to the link that provisioned (its handle AND its address: a
    // handle the stack reuses for a later peer is not that link), once;
    // everyone else reads "{}". The value is set here, right before NimBLE
    // answers the read, and wiped by the loop task right after.
    if (!s_claim_armed || info.getConnHandle() != s_claim_conn ||
        !same_addr(*info.getAddress().getBase(), s_claim_addr)) {
      c->setValue((const uint8_t*)"{}", 2);
      return;
    }
    const size_t n = s_claim_json_len;
    if (n == 0 || n >= sizeof(s_claim_json)) {
      c->setValue((const uint8_t*)"{}", 2);
      s_claim_armed = false;
      return;
    }
    c->setValue((const uint8_t*)s_claim_json, n);
    // One read — but a long one: the phone's Read Blob continuations land
    // here again for the same value, so the arm stays until the loop task
    // withdraws it (CLAIM_READ_GRACE_MS after this first read, or when
    // this link is gone). Only that link ever gets past the check above.
    if (!s_claim_read_seen) {
      s_claim_read_ms = millis();
      s_claim_read_seen = true;
      s_claims++;
    }
  }
};

CommandCb s_command_cb;
ClaimCb   s_claim_cb;

// ── Commands (loop task) ───────────────────────────────────────────────────

void answer_device_info() {
  const char* strings[4] = { "canary-wap", ble_improv_firmware_version(), "XIAO ESP32-S3", s_adv_name };
  send_result(Command::GetDeviceInfo, strings, 4);
}

// The link that wrote is the door's link from its first write on; every
// write, accepted or not, is activity on it.
void note_link_wrote(uint16_t conn, const ble_addr_t& addr, uint32_t now_ms) {
  if (!s_session.link_up || conn != s_door_conn || !same_addr(addr, s_door_addr)) {
    s_door_conn = conn;
    s_door_addr = addr;
    improv::session_link_up(s_session, now_ms);
  }
  improv::session_touch(s_session, now_ms);
}

void handle_command(uint32_t now_ms) {
  const uint16_t conn = s_rx_conn;
  const ble_addr_t addr = s_rx_addr;
  note_link_wrote(conn, addr, now_ms);

  if (s_rx_unencrypted) {
    s_rx_unencrypted = false;
    s_rx_pending = false;
    s_session.error = Error::NotAuthorized;
    s_refused++;
    publish_error();
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "Setup door: write on an unencrypted link refused", nullptr);
    return;
  }

  uint8_t frame[improv::MAX_COMMAND_LEN];
  const size_t n = s_rx_len;
  memcpy(frame, s_rx, n);
  improv::wipe(s_rx, sizeof(s_rx));
  s_rx_pending = false;

  improv::ParsedCommand cmd = improv::parse_command(frame, n);
  improv::wipe(frame, sizeof(frame));
  if (cmd.verdict != Parse::Ok) {
    if (cmd.command == Command::WifiSettings) {
      // A credentials frame that did not parse (Malformed / TooLong) still
      // counts against the open door's cap: garbage is an attempt too.
      improv::session_on_malformed_wifi_settings(s_session, now_ms, s_timing);
      s_refused++;
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
        if (ble_improv_submit_join(cmd.wifi.ssid, cmd.wifi.password)) {
          s_provisioning_conn = conn;
          s_provisioning_addr = addr;
          s_accepted++;
        } else {
          improv::session_on_join_result(s_session, false, now_ms);
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
    improv::session_on_join_result(s_session, true, now_ms);
    // Proven: the sketch persists the credentials now (the door shuts on
    // its own once they are stored), before the claim names the .local URL.
    ble_improv_join_verdict(true);
    publish_error();
    publish_state();
    char url[96] = {0};
    ble_improv_reach_url(url, sizeof(url));
    const char* strings[1] = { url };
    send_result(Command::WifiSettings, strings, 1);
    // The claim for the link that asked: minted now, on this task, readable
    // by that link alone for the ticket's TTL. The token stays home.
    disarm_claim(false);
    const size_t n = ble_improv_mint_claim(s_claim_json, sizeof(s_claim_json), now_ms);
    if (n == 0 || n >= sizeof(s_claim_json) || s_provisioning_conn == NO_CONN) {
      improv::wipe(s_claim_json, sizeof(s_claim_json));
      ble_improv_wipe_claim();
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
                 "Setup door: joined, but no claim could be armed (the BOOT tap still serves the receipt)",
                 nullptr);
      return;
    }
    s_claim_json_len = n;
    s_claim_addr = s_provisioning_addr;
    s_claim_conn = s_provisioning_conn;
    s_claim_armed_ms = now_ms;
    s_claim_armed = true;
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
               "Setup door: joined; claim armed for the provisioning link", nullptr);
  } else if (ws == WL_CONNECT_FAILED || ws == WL_NO_SSID_AVAIL) {
    improv::session_on_join_result(s_session, false, now_ms);
    // Not proven: the sketch forgets the attempt; the door stays open.
    ble_improv_join_verdict(false);
    publish_error();
    publish_state();
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH, "Setup door: join failed", nullptr);
  }
}

void follow_claim(uint32_t now_ms) {
  // Read once, and the whole read is over: withdraw it from the air (the
  // ticket itself lives on — the phone is spending it on the LAN).
  if (s_claim_armed && s_claim_read_seen &&
      ((uint32_t)(now_ms - s_claim_read_ms) >= CLAIM_READ_GRACE_MS || !s_session.link_up)) {
    disarm_claim(false);
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
               "Setup door: claim read by the provisioning link — withdrawn from the air", nullptr);
  }
  if (s_claim_armed && (uint32_t)(now_ms - s_claim_armed_ms) >= CLAIM_TTL_MS) {
    disarm_claim(true);
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
               "Setup door: claim unread for three minutes — withdrawn (the BOOT tap still serves the receipt)",
               nullptr);
  }
  // After a read the host task cleared the arm; wipe the loop task's copy
  // and the characteristic's own here, on the loop task, where setValue is
  // calm. The ticket itself stays: the phone is spending it on the LAN.
  if (!s_claim_armed && s_claim_json_len != 0) {
    improv::wipe(s_claim_json, sizeof(s_claim_json));
    s_claim_json_len = 0;
  }
  if (!s_claim_armed && s_claim && s_claim->getValue().length() > 2) {
    s_claim->setValue((const uint8_t*)"{}", 2);
  }
}

// The one link: still held by the stack? Silent past the idle bound, or
// done with its join and past the linger? The channel's server callbacks
// are the channel's own (ble_server_dispatch.h has no third observer), so
// the door asks the stack for its handle, the way the channel's own
// reconcile_link() does, a few times a second.
void follow_link(uint32_t now_ms) {
  if (!s_session.link_up) return;
  if ((uint32_t)(now_ms - s_link_polled_ms) < LINK_POLL_MS) return;
  s_link_polled_ms = now_ms;
  NimBLEServer* server = NimBLEDevice::getServer();
  bool held = false;
  if (server && s_door_conn != NO_CONN && server->getConnectedCount() != 0) {
    const NimBLEConnInfo live = server->getPeerInfoByHandle(s_door_conn);
    held = live.getConnHandle() == s_door_conn &&
           same_addr(*live.getAddress().getBase(), s_door_addr);
  }
  if (!held) {
    improv::session_link_down(s_session);
    s_door_conn = NO_CONN;
    return;
  }
  if (improv::idle_disconnect_due(s_session, now_ms, s_timing)) {
    // A client that asked for nothing, or one whose join is done and
    // answered (and whose claim it has had the linger to read): hand the
    // radio back so the beacon returns and nobody parks on the single link.
    server->disconnect(s_door_conn);
    improv::session_link_down(s_session);
    s_door_conn = NO_CONN;
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
               s_session.state == State::Provisioned
                   ? "Setup door: link dropped (joined, verdict delivered)"
                   : "Setup door: link dropped (idle)", nullptr);
  }
}

void follow_door() {
  const bool open = door_open_now();
  if (open == s_door_applied) return;
  // The channel decides through provisioning_logic::security_profile_for:
  // refused (false) while a Numeric Comparison is pending or an
  // authenticated / bonded link is up; try again next pass.
  if (bluetooth_channel::set_setup_door(open)) {
    s_door_applied = open;
    if (open) {
      // A door that reopens (a factory reset re-armed the window) is a new
      // provisioning: a claim from the last one is void.
      disarm_claim(true);
    } else if (s_session.state != State::Provisioned) {
      // Shut for any reason but the join that minted the claim (the window
      // ran out, the cap was reached): nothing is owed to anyone. After a
      // successful join the door shuts too, and the claim lives on for its
      // TTL — that shut is the claim's whole point.
      disarm_claim(true);
    }
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
  // Encrypted, not authenticated: the Just Works key the door admits. The
  // write handler checks the link's encryption again.
  s_command = s_service->createCharacteristic(improv::RPC_COMMAND_UUID,
                                              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
                                                | NIMBLE_PROPERTY::WRITE_ENC);
  s_result  = s_service->createCharacteristic(improv::RPC_RESULT_UUID,
                                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
                                                | NIMBLE_PROPERTY::READ_ENC);
  s_caps    = s_service->createCharacteristic(improv::CAPABILITIES_UUID, NIMBLE_PROPERTY::READ);
  if (!s_state || !s_error || !s_command || !s_result || !s_caps) return false;
  s_command->setCallbacks(&s_command_cb);

  // The companion service: the claim, beside the Improv service, on the
  // same server, under the same encrypted-link profile.
  s_claim_service = server->createService(CLAIM_SERVICE_UUID);
  if (!s_claim_service) return false;
  s_claim = s_claim_service->createCharacteristic(CLAIM_UUID,
                                                  NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
  if (!s_claim) return false;
  s_claim->setCallbacks(&s_claim_cb);
  s_claim->setValue((const uint8_t*)"{}", 2);

  const uint8_t caps = capabilities();
  s_caps->setValue(&caps, 1);

  // "WAP-XXXX": the same four characters as the "SCV-XXXX" name Opera
  // advertises, so the phone can tell the beacon and the door are one device.
  char suffix[5] = {0};
  if (!ble_improv_name_suffix(suffix, sizeof(suffix))) snprintf(suffix, sizeof(suffix), "0000");
  snprintf(s_adv_name, sizeof(s_adv_name), "WAP-%s", suffix);

  s_service->start();
  s_claim_service->start();

  // The first-boot window is measured from here — the channel comes up a
  // few seconds after boot on a fresh unit (BLE_DISCOVERY_FRESH_SETTLE_MS),
  // so "after boot" is "after boot, give or take the settle".
  s_timing = improv::Timing{};
  s_timing.provisioning_timeout_ms = PROVISIONING_TIMEOUT_MS;
  s_timing.first_boot_window_ms = (uint32_t)(IMPROV_FIRST_BOOT_WINDOW_MS);
  const bool no_credentials = ble_improv_door_should_open();
  improv::session_begin(s_session, no_credentials, millis(), s_timing);
  s_active = true;
  publish_state();
  publish_error();
  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
             improv::door_open(s_session)
                 ? "Setup door service ready (door open: no credentials stored, first-boot window running)"
                 : (no_credentials ? "Setup door service ready (door shut: tap-only build, no tap on this board)"
                                   : "Setup door service ready (door shut: credentials stored)"),
             nullptr);
  return true;
}

void tick() {
  if (!s_active) return;
  const uint32_t now_ms = millis();
  improv::session_set_no_credentials(s_session, ble_improv_door_should_open(), now_ms, s_timing);
  follow_link(now_ms);
  if (s_rx_pending) handle_command(now_ms);
  follow_join(now_ms);
  follow_claim(now_ms);
  const bool was_provisioning = s_session.state == State::Provisioning;
  if (improv::session_tick(s_session, now_ms, s_timing)) {
    publish_error();
    publish_state();
    if (was_provisioning && s_session.state != State::Provisioning) {
      // The core timed the join out (WiFi never reported either way): the
      // same verdict as a failed join — forget the attempt, keep the door.
      ble_improv_join_verdict(false);
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH, "Setup door: join timed out", nullptr);
    }
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
  out->claims_served = s_claims;
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
  out->claims_served = 0;
  return true;
}
}  // namespace ble_improv

#endif  // FEATURE_IMPROV && __has_include(<NimBLEDevice.h>)
