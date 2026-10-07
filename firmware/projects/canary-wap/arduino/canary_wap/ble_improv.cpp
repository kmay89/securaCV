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
extern uint32_t ble_improv_window_used_ms();                          // the window earlier boots spent (RTC record)
extern void   ble_improv_note_window_used(uint32_t used_ms);          // the running tally, for the next software reset
extern void   ble_improv_note_activity();                             // a sign of life for the first-boot wizard's timer
extern int    ble_improv_scan_request();                              // 1 asked / sharing / fresh, 0 lock busy (ask again), -1 refused
extern int    ble_improv_scan_poll();                                 // 1 list ready, 0 sweeping, -1 none
extern int    ble_improv_scan_row(int i, char* ssid, size_t ssid_cap, int* rssi,
                                  char* auth, size_t auth_cap);       // 1 row, 0 end, -1 busy

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
// How often the spent first-boot window is written to the sketch's RTC
// record while the no-credentials door is open: a software reset loses at
// most this much of the tally (the window gains it back — never the other
// way).
constexpr uint32_t WINDOW_NOTE_MS = 2000;
// GET_WIFI_NETWORKS: a sweep the sketch's scan never reports on reads as
// "no networks" after this long (the common glue's bound); at most this
// many rows, one per pass (the sketch's cache holds no more).
constexpr uint32_t SCAN_TIMEOUT_MS = 30000;
constexpr int      SCAN_MAX = 20;

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

// The window tally's last note, and whether a spent window was noted.
uint32_t s_window_noted_ms = 0;
bool     s_window_spent_noted = false;

// GET_WIFI_NETWORKS in flight: asking the sketch (its cache lock may be
// busy for a pass), then waiting on its sweep, then streaming the cache one
// row per pass (s_scan_emit = the next row; -1 = not streaming).
bool     s_scan_asking = false;
bool     s_scan_waiting = false;
uint32_t s_scan_started_ms = 0;
int      s_scan_emit = -1;

uint32_t s_accepted = 0;
uint32_t s_refused = 0;
uint32_t s_claims = 0;

uint8_t capabilities() {
  return improv::CAP_IDENTIFY | improv::CAP_DEVICE_INFO | improv::CAP_SCAN_WIFI;
}

void start_scan(uint32_t now_ms);   // the network list, below the claim

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
  __atomic_store_n(&s_claim_armed, false, __ATOMIC_RELEASE);
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
    // The mailbox crosses cores (the NimBLE host task fills it, the loop
    // task drains it): the flag is published with release after every
    // field is written and read with acquire, so the reader sees the
    // packet the flag announces, never a stale one.
    if (__atomic_load_n(&s_rx_pending, __ATOMIC_ACQUIRE)) return;   // one at a time; the loop task is on it
    s_rx_conn = info.getConnHandle();
    s_rx_addr = *info.getAddress().getBase();
    if (!info.isEncrypted()) {
      // The second check (the first is the characteristic's WRITE_ENC):
      // nothing from a link that is not encrypted is parsed, let alone
      // believed. The loop task answers NotAuthorized and counts it.
      s_rx_unencrypted = true;
      s_rx_len = 0;
      __atomic_store_n(&s_rx_pending, true, __ATOMIC_RELEASE);
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
    __atomic_store_n(&s_rx_pending, true, __ATOMIC_RELEASE);
  }
};

class ClaimCb : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    // Served to the link that provisioned (its handle AND its address: a
    // handle the stack reuses for a later peer is not that link), once;
    // everyone else reads "{}". The value is set here, right before NimBLE
    // answers the read, and wiped by the loop task right after. The arm is
    // read with acquire: the loop task filled the JSON, the connection and
    // the address on the other core and published the arm with release
    // after them, so an arm seen here is a buffer seen whole.
    if (!__atomic_load_n(&s_claim_armed, __ATOMIC_ACQUIRE) ||
        info.getConnHandle() != s_claim_conn ||
        !same_addr(*info.getAddress().getBase(), s_claim_addr)) {
      c->setValue((const uint8_t*)"{}", 2);
      return;
    }
    const size_t n = s_claim_json_len;
    if (n == 0 || n >= sizeof(s_claim_json)) {
      c->setValue((const uint8_t*)"{}", 2);
      __atomic_store_n(&s_claim_armed, false, __ATOMIC_RELEASE);
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
  // Every command the door receives is a phone at work: the first-boot
  // wizard's abandonment restart must not land under it.
  ble_improv_note_activity();

  if (s_rx_unencrypted) {
    s_rx_unencrypted = false;
    __atomic_store_n(&s_rx_pending, false, __ATOMIC_RELEASE);
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
  __atomic_store_n(&s_rx_pending, false, __ATOMIC_RELEASE);

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
          ble_improv_note_activity();   // a join in flight: the wizard waits for it
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
  const wl_status_t ws = WiFi.status();
  if (ws == WL_CONNECTED) {
    improv::session_on_join_result(s_session, true, now_ms);
    // Proven: the sketch persists the credentials now (the door shuts on
    // its own once they are stored), before the claim names the .local URL.
    ble_improv_join_verdict(true);
    // The claim for the link that asked: minted and armed FIRST, on this
    // task, readable by that link alone for the ticket's TTL — then the
    // verdict goes out. A phone that reads CLAIM the moment it hears
    // Provisioned finds the claim, never "{}". The token stays home.
    disarm_claim(false);
    const size_t n = ble_improv_mint_claim(s_claim_json, sizeof(s_claim_json), now_ms);
    bool armed = false;
    if (n == 0 || n >= sizeof(s_claim_json) || s_provisioning_conn == NO_CONN) {
      improv::wipe(s_claim_json, sizeof(s_claim_json));
      ble_improv_wipe_claim();
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
                 "Setup door: joined, but no claim could be armed (the BOOT tap still serves the receipt)",
                 nullptr);
    } else {
      s_claim_json_len = n;
      s_claim_addr = s_provisioning_addr;
      s_claim_conn = s_provisioning_conn;
      s_claim_armed_ms = now_ms;
      // Published with release after every field above: the NimBLE host
      // task's read callback acquires the arm and then reads the buffer.
      __atomic_store_n(&s_claim_armed, true, __ATOMIC_RELEASE);
      armed = true;
      ble_improv_note_activity();   // the phone is about to spend it
    }
    publish_error();
    publish_state();
    char url[96] = {0};
    ble_improv_reach_url(url, sizeof(url));
    const char* strings[1] = { url };
    send_result(Command::WifiSettings, strings, 1);
    if (armed) {
      log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
                 "Setup door: joined; claim armed for the provisioning link", nullptr);
    }
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
  if (__atomic_load_n(&s_claim_armed, __ATOMIC_ACQUIRE) && s_claim_read_seen &&
      ((uint32_t)(now_ms - s_claim_read_ms) >= CLAIM_READ_GRACE_MS || !s_session.link_up)) {
    disarm_claim(false);
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
               "Setup door: claim read by the provisioning link — withdrawn from the air", nullptr);
  }
  if (__atomic_load_n(&s_claim_armed, __ATOMIC_ACQUIRE) &&
      (uint32_t)(now_ms - s_claim_armed_ms) >= CLAIM_TTL_MS) {
    disarm_claim(true);
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
               "Setup door: claim unread for three minutes — withdrawn (the BOOT tap still serves the receipt)",
               nullptr);
  }
  // After a read the host task cleared the arm; wipe the loop task's copy
  // and the characteristic's own here, on the loop task, where setValue is
  // calm. The ticket itself stays: the phone is spending it on the LAN.
  const bool armed = __atomic_load_n(&s_claim_armed, __ATOMIC_ACQUIRE);
  if (!armed && s_claim_json_len != 0) {
    improv::wipe(s_claim_json, sizeof(s_claim_json));
    s_claim_json_len = 0;
  }
  if (!armed && s_claim && s_claim->getValue().length() > 2) {
    s_claim->setValue((const uint8_t*)"{}", 2);
  }
}

// ── The network list (loop task) ────────────────────────────────────────
// GET_WIFI_NETWORKS, the common glue's shape (firmware/common/network/
// improv_ble.cpp): one result per network — ssid, rssi as decimal text, the
// auth name — streamed one row per pass so twenty notifications never queue
// on the host at once, closed by one empty result; at most SCAN_MAX rows;
// an SSID the standard cannot carry (empty, or over SSID_MAX) is skipped.
// The rows are the sketch's scan cache, the same list the wizard's
// GET /api/wifi/scan serves, through the sketch's own async scan
// (ble_improv_scan_request / _poll / _row, every one on this task): a
// sweep already running is shared, a fresh cache costs the radio nothing.

void end_scan_list() {
  s_scan_asking = false;
  s_scan_waiting = false;
  s_scan_emit = -1;
  send_result(Command::GetWifiNetworks, nullptr, 0);
}

// Ask the sketch for the list; a busy cache lock is "ask again next pass",
// never an early end to the list (the phone would show an empty picker for
// a lock the wizard's handler held for a millisecond).
void ask_scan(uint32_t now_ms) {
  const int asked = ble_improv_scan_request();
  if (asked == 0) return;                      // busy: next pass
  s_scan_asking = false;
  if (asked < 0) {
    // Refused: a join is in flight (a sweep under WiFi.begin() can fail
    // it), or the radio refused. "No networks" at once — the phone shows
    // its typed-name field.
    end_scan_list();
    return;
  }
  s_scan_waiting = true;
  s_scan_started_ms = now_ms;
}

void start_scan(uint32_t now_ms) {
  if (s_scan_asking || s_scan_waiting || s_scan_emit >= 0) return;   // one list at a time; the first asks for it
  s_scan_asking = true;
  s_scan_started_ms = now_ms;
  ask_scan(now_ms);
}

void follow_scan(uint32_t now_ms) {
  if (s_scan_asking) {
    if ((uint32_t)(now_ms - s_scan_started_ms) > SCAN_TIMEOUT_MS) { end_scan_list(); return; }
    ask_scan(now_ms);
    return;
  }
  if (s_scan_waiting) {
    const int verdict = ble_improv_scan_poll();
    if (verdict == 0) {
      if ((uint32_t)(now_ms - s_scan_started_ms) > SCAN_TIMEOUT_MS) end_scan_list();
      return;
    }
    s_scan_waiting = false;
    if (verdict < 0) { end_scan_list(); return; }
    s_scan_emit = 0;
    // Fall through to stream the first row this pass.
  }
  if (s_scan_emit < 0) return;
  if (s_scan_emit >= SCAN_MAX) { end_scan_list(); return; }
  char ssid[improv::SSID_MAX + 1] = {0};
  char auth[12] = {0};
  int rssi = 0;
  const int got = ble_improv_scan_row(s_scan_emit, ssid, sizeof(ssid), &rssi, auth, sizeof(auth));
  if (got < 0) return;                       // the cache is busy this pass: next pass
  if (got == 0) { end_scan_list(); return; }   // every row sent: the empty result closes the list
  ++s_scan_emit;
  const size_t len = strlen(ssid);
  if (len == 0 || len > improv::SSID_MAX) return;   // not a row the standard carries: next pass
  char rssi_text[8];
  snprintf(rssi_text, sizeof(rssi_text), "%d", rssi);
  const char* row[3] = { ssid, rssi_text, auth };
  send_result(Command::GetWifiNetworks, row, 3);
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
    // A network list in flight was that link's: nothing to stream to, and
    // the next link asks for its own.
    s_scan_asking = false;
    s_scan_waiting = false;
    s_scan_emit = -1;
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
  // "After boot" is after the last POWER CYCLE: the window a never-
  // provisioned unit spent before a software reset (the first-boot
  // wizard's 15-minute abandonment restart, a watchdog) is carried in by
  // the sketch's RTC record and back-dated into the stamp, so the restarts
  // never re-arm it; a window those boots spent whole begins shut.
  s_timing = improv::Timing{};
  s_timing.provisioning_timeout_ms = PROVISIONING_TIMEOUT_MS;
  s_timing.first_boot_window_ms = (uint32_t)(IMPROV_FIRST_BOOT_WINDOW_MS);
  const bool no_credentials = ble_improv_door_should_open();
  const uint32_t now_ms = millis();
  const uint32_t carried_ms = ble_improv_window_used_ms();
  improv::session_begin(s_session, no_credentials, now_ms, s_timing, carried_ms);
  s_window_noted_ms = now_ms;
  s_window_spent_noted = s_session.window_spent;
  if (s_session.window_spent) ble_improv_note_window_used(s_timing.first_boot_window_ms);
  s_active = true;
  publish_state();
  publish_error();
  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
             improv::door_open(s_session)
                 ? (carried_ms != 0
                        ? "Setup door service ready (door open: no credentials stored, first-boot window carried across the reset)"
                        : "Setup door service ready (door open: no credentials stored, first-boot window running)")
                 : (no_credentials
                        ? (s_session.window_spent
                               ? "Setup door service ready (door shut: first-boot window spent before the reset; a power cycle re-arms it)"
                               : "Setup door service ready (door shut: tap-only build, no tap on this board)")
                        : "Setup door service ready (door shut: credentials stored)"),
             nullptr);
  return true;
}

// The window tally for the sketch's RTC record: every WINDOW_NOTE_MS while
// the no-credentials window runs, and the moment it is spent (by time or by
// the attempt cap) — session_window_used_ms answers the whole window then.
// A window re-armed at runtime (a credential wipe: the sketch zeroed its
// record too) starts a fresh tally.
void follow_window(uint32_t now_ms) {
  if (!s_session.window_spent) s_window_spent_noted = false;
  if (s_session.window_spent && !s_window_spent_noted) {
    s_window_spent_noted = true;
    s_window_noted_ms = now_ms;
    ble_improv_note_window_used(improv::session_window_used_ms(s_session, now_ms, s_timing));
    return;
  }
  if (s_session.door != improv::Door::NoCredentials) return;
  if ((uint32_t)(now_ms - s_window_noted_ms) < WINDOW_NOTE_MS) return;
  s_window_noted_ms = now_ms;
  ble_improv_note_window_used(improv::session_window_used_ms(s_session, now_ms, s_timing));
}

void tick() {
  if (!s_active) return;
  const uint32_t now_ms = millis();
  improv::session_set_no_credentials(s_session, ble_improv_door_should_open(), now_ms, s_timing);
  follow_link(now_ms);
  if (__atomic_load_n(&s_rx_pending, __ATOMIC_ACQUIRE)) handle_command(now_ms);
  follow_join(now_ms);
  follow_claim(now_ms);
  follow_scan(now_ms);
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
  follow_window(now_ms);
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
