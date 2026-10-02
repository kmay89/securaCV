// Host test: the owner's Bluetooth commands run on the loop task (sweep
// F111), against the REAL bluetooth_channel.cpp, compiled on the host over
// stubs/bt: a NimBLE-Arduino 2.x stand-in that records every radio, bond and
// passkey call with the task that made it, and an in-memory main NVS that
// does the same. bluetooth_channel.cpp is #included below.
//
// bluetooth_api.h's handlers called bluetooth_channel::enable, disable,
// cancel_pairing, confirm_pairing (and every other mutator: advertising,
// scans, reject, disconnect, the paired devices, the settings, the name, the
// TX power) straight from esp_http_server's task, while update() on the loop
// task cancels a pairing that timed out, stops a scan that ran its course
// and disconnects an idle link, over the same state and with no lock: a
// confirm on the HTTP task and the timeout's cancel on the loop task could
// both find the pending Numeric-Comparison pairing and both answer and
// delete it. Those functions are now internal to bluetooth_channel.cpp; a
// handler hands a Command to bluetooth_channel::submit(), which posts it to
// a ring (loop_command_ring.h) that update() drains on the loop task, and
// waits for its Result. The one call a handler still makes itself is
// init(), which brings the NimBLE stack up and can block past the loop
// task's watchdog, so no command makes it.
//
// The harness is one thread, so a task is a role the test plays
// (host_sim::task): "httpd" while a handler waits in submit(), "loop" for
// update()'s passes (given on the handler's sleeps, in the stub vTaskDelay),
// "nimble" for the NimBLE host's callbacks, "bringup" for the boot worker's
// init(). The handlers' own source is held by
// firmware/scripts/check_wap_loop_commands.py (ArduinoJson, which they build
// their answers with, is not on the host).
//
// Host-tested only: the stand-in is not NimBLE, and nothing here runs a
// radio; the Arduino compile is CI's (firmware.yml's canary-wap legs). The
// NimBLE host task's own writes to this state (a connect, a passkey to
// confirm, a bond) are not moved by F111, and nothing here races them.
//
// Run: ./test_bluetooth_commands_wap [name]

#ifndef BLUETOOTH_CHANNEL_CPP
#error "BLUETOOTH_CHANNEL_CPP (absolute path to bluetooth_channel.cpp) must be defined"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"
#include "NimBLEDevice.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// nvs_store.h's NvsMainSession, over an in-memory store; every write records
// the task that made it.
#define SECURACV_NVS_STORE_H
namespace host_sim {
inline std::map<std::string, std::vector<uint8_t>> main_nvs;
}  // namespace host_sim
class FakeMainNvs {
 public:
  size_t putBytes(const char* k, const void* v, size_t n) {
    host_sim::note("nvs_write");
    const uint8_t* b = static_cast<const uint8_t*>(v);
    host_sim::main_nvs[k].assign(b, b + n);
    return n;
  }
  size_t getBytesLength(const char* k) {
    auto it = host_sim::main_nvs.find(k);
    return it == host_sim::main_nvs.end() ? 0 : it->second.size();
  }
  size_t getBytes(const char* k, void* buf, size_t n) {
    auto it = host_sim::main_nvs.find(k);
    if (it == host_sim::main_nvs.end() || it->second.size() > n) return 0;
    if (!it->second.empty()) memcpy(buf, it->second.data(), it->second.size());
    return it->second.size();
  }
  template <typename T> T get(const char* k, T d) {
    T v = d;
    return getBytes(k, &v, sizeof v) == sizeof v ? v : d;
  }
  bool getBool(const char* k, bool d) { return get<bool>(k, d); }
  int8_t getChar(const char* k, int8_t d) { return get<int8_t>(k, d); }
  uint32_t getULong(const char* k, uint32_t d) { return get<uint32_t>(k, d); }
  size_t putBool(const char* k, bool v) { return putBytes(k, &v, sizeof v); }
  size_t putChar(const char* k, int8_t v) { return putBytes(k, &v, sizeof v); }
  size_t putULong(const char* k, uint32_t v) { return putBytes(k, &v, sizeof v); }
};
class NvsMainSession {
 public:
  explicit NvsMainSession(bool) {}
  bool isOpen() const { return true; }
  FakeMainNvs* operator->() { return &nvs_; }
 private:
  FakeMainNvs nvs_;
};

// ble_ota.h pulls the OTA engine in; the channel calls only its init().
#define SECURACV_BLE_OTA_H
namespace ble_ota {
bool init(NimBLEServer* server, const uint8_t release_pubkey[32]);
}  // namespace ble_ota

// The firmware file is held to the device build's warnings, not to this
// Makefile's -Wextra -Wpedantic -Werror; this test's own code keeps them.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wc++20-extensions"   // g_settings' designated initializer
#include BLUETOOTH_CHANNEL_CPP
#pragma GCC diagnostic pop

#include "http_status_line.h"   // the status line a Bluetooth not-run answer sends

// What the sketch links in on a device (canary_wap.ino and the BLE modules).
std::vector<std::string> g_health;
void log_health(LogLevel, LogCategory, const char* message, const char*) {
  g_health.push_back(message);
}
namespace ble_ota {
bool init(NimBLEServer*, const uint8_t[32]) { return true; }
}  // namespace ble_ota
namespace ble_presence {
bool init() { return true; }
void deinit() {}
bool start() { return true; }
void pause_for_user_scan() {}
void resume_continuous_scan() {}
void notify_console_connected(bool) {}
}  // namespace ble_presence
namespace ble_console {
bool init(NimBLEServer*) { return true; }
void tick() {}
}  // namespace ble_console
namespace ble_provision {
bool init(NimBLEServer*) { return true; }
void tick() {}
}  // namespace ble_provision
namespace ble_log_export {
bool init(NimBLEServer*) { return true; }
void tick() {}
}  // namespace ble_log_export
namespace ble_witness_export {
bool init(NimBLEServer*) { return true; }
void tick() {}
}  // namespace ble_witness_export

namespace bt_commands {

int g_checks = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)

namespace bc = bluetooth_channel;
namespace lcr = loop_command_ring;

// ── Helpers ─────────────────────────────────────────────────────────────

// A device powering up: RAM gone (the channel's statics, the stack), NVS
// kept unless `wipe`. When `bring_up`, the boot worker runs the real init()
// ("bringup"), which turns Bluetooth on and advertises (the defaults).
void boot(bool bring_up = true, bool wipe = true) {
  if (wipe) host_sim::main_nvs.clear();
  NimBLEDevice::deinit(true);
  host_sim::advertising = NimBLEAdvertising();
  host_sim::scan = NimBLEScan();
  host_sim::bonds.clear();
  host_sim::passkey_answers.clear();
  host_sim::calls.clear();
  host_sim::task = "loop";
  host_sim::on_task_delay = nullptr;
  bc::g_state = bc::BT_DISABLED;
  bc::g_initialized = false;
  bc::g_init_fail_reason[0] = '\0';
  bc::g_settings.enabled = bt_defaults::ENABLED;
  bc::g_settings.auto_advertise = bt_defaults::AUTO_ADVERTISE;
  bc::g_settings.allow_pairing = bt_defaults::ALLOW_PAIRING;
  bc::g_settings.require_pin = bt_defaults::REQUIRE_PIN;
  bc::g_settings.tx_power = 9;
  bc::g_settings.long_range_mode = false;
  memset(&bc::g_connection, 0, sizeof bc::g_connection);
  memset(&bc::g_pairing, 0, sizeof bc::g_pairing);
  delete bc::g_pending_pair_info;
  bc::g_pending_pair_info = nullptr;
  bc::g_pending_pair_active = false;
  bc::g_connection_handle = 0xFFFF;
  memset(bc::g_paired_devices, 0, sizeof bc::g_paired_devices);
  bc::g_paired_count = 0;
  memset(bc::g_scanned_devices, 0, sizeof bc::g_scanned_devices);
  bc::g_scanned_count = 0;
  bc::g_scanning = false;
  bc::g_commands = decltype(bc::g_commands)();
  if (bring_up) {
    host_sim::task = "bringup";
    CHECK(bc::init());
    host_sim::task = "loop";
    CHECK(bc::is_initialized() && bc::is_enabled());
  }
  host_sim::calls.clear();
}

// What one REST call saw while its handler waited.
struct Rest {
  lcr::Wait wait = lcr::Wait::kBusy;
  bc::Result r = {};
  unsigned sleeps = 0;          // vTaskDelay calls the handler made
  unsigned loop_turns = 0;      // update() passes the loop task ran meanwhile
  uint32_t waited_ms = 0;
  // Before the loop task's first turn: the state, the pairing state, and the
  // radio/bond/NVS calls made so far.
  bc::BluetoothState state_before_turn = bc::BT_ERROR;
  bc::PairingState pairing_before_turn = bc::PAIR_FAILED;
  size_t calls_before_turn = 0;
};

// The REST handler submits `cmd` ("httpd"), and the loop task gets one
// update() pass on the handler's `turn_at`-th sleep (never, when 0).
Rest rest(const bc::Command& cmd, unsigned turn_at = 1) {
  Rest out;
  const uint32_t start = host_sim::now_ms;
  host_sim::on_task_delay = [&](uint32_t ms) {
    host_sim::now_ms += ms;
    ++out.sleeps;
    if (out.sleeps == 1) {
      out.state_before_turn = bc::g_state;
      out.pairing_before_turn = bc::g_pairing.state;
      out.calls_before_turn = host_sim::calls.size();
    }
    if (turn_at != 0 && out.sleeps == turn_at) {
      host_sim::task = "loop";   // the loop task's turn
      bc::update();
      ++out.loop_turns;
      host_sim::task = "httpd";
    }
  };
  host_sim::task = "httpd";
  out.wait = bc::submit(cmd, &out.r);
  host_sim::task = "loop";
  host_sim::on_task_delay = nullptr;
  out.waited_ms = host_sim::now_ms - start;
  return out;
}

bc::Command cmd_of(bc::CommandType t) { return bc::make_command(t); }

// The NimBLE host task's callback `fn`.
void on_nimble(const std::function<void()>& fn) {
  const std::string was = host_sim::task;
  host_sim::task = "nimble";
  fn();
  host_sim::task = was;
}

NimBLEConnInfo link(uint16_t handle, uint8_t last_byte) {
  NimBLEConnInfo c;
  c.handle = handle;
  const uint8_t addr[6] = {last_byte, 0x22, 0x33, 0x44, 0x55, 0x66};
  c.address = NimBLEAddress(addr, 0);
  return c;
}

// A phone pairing: the owner started pairing mode (over REST), the phone
// connected, and the stack asks to confirm `pin` (Numeric Comparison).
void pairing_awaiting_confirm(uint32_t pin, const NimBLEConnInfo& phone) {
  const Rest start = rest(cmd_of(bc::BT_CMD_PAIR_START));
  CHECK(start.wait == lcr::Wait::kDone && start.r.ok);
  NimBLEConnInfo c = phone;
  on_nimble([&] { host_sim::server->callbacks()->onConfirmPassKey(c, pin); });
  CHECK(bc::g_pairing.state == bc::PAIR_CONFIRMING && bc::g_pending_pair_active);
}

// No radio, bond or NVS call came from the handler's task.
void none_on_httpd() { CHECK(host_sim::count("", "httpd") == 0); }

// ── The pairing ─────────────────────────────────────────────────────────

// POST /api/bluetooth/pair/confirm with the PIN both screens show: the
// passkey answer is the loop task's, made once, on its turn.
void test_a_pin_confirm_runs_on_the_loop_task() {
  boot();
  const NimBLEConnInfo phone = link(7, 0xA1);
  pairing_awaiting_confirm(482913, phone);
  bc::Command c = cmd_of(bc::BT_CMD_PAIR_CONFIRM);
  c.pin = 482913;
  const Rest r = rest(c);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.pairing_before_turn == bc::PAIR_CONFIRMING);
  CHECK(host_sim::passkey_answers.size() == 1);
  CHECK(host_sim::passkey_answers[0].accept && host_sim::passkey_answers[0].handle == 7);
  CHECK(host_sim::passkey_answers[0].task == "loop");
  CHECK(!bc::g_pending_pair_active && bc::g_pending_pair_info == nullptr);
  CHECK(bc::g_pairing.user_confirmed);
  none_on_httpd();
  std::printf("PASS a_pin_confirm_runs_on_the_loop_task\n");
}

// The owner's confirm lands the pass the pairing times out: the drain runs
// first, so the passkey is answered once (accepted), and the timeout that
// follows finds nothing pending to answer or delete. Before F111 the confirm
// ran on the HTTP task, concurrently with the timeout's cancel_pairing() on
// the loop task, and both could answer and delete the one pending pairing.
void test_a_confirm_and_the_timeout_answer_once() {
  boot();
  pairing_awaiting_confirm(111222, link(9, 0xB2));
  host_sim::now_ms += bc::PAIRING_TIMEOUT_MS;          // the timeout is due this pass
  bc::Command c = cmd_of(bc::BT_CMD_PAIR_CONFIRM);
  c.pin = 111222;
  const Rest r = rest(c);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(host_sim::passkey_answers.size() == 1 && host_sim::passkey_answers[0].accept);
  CHECK(bc::g_pending_pair_info == nullptr && !bc::g_pending_pair_active);
  CHECK(bc::g_pairing.state == bc::PAIR_NONE);          // the timeout's cancel, on the same pass
  // A cancel after that answers nothing either.
  const Rest cancel = rest(cmd_of(bc::BT_CMD_PAIR_CANCEL));
  CHECK(cancel.wait == lcr::Wait::kDone && cancel.r.ok);
  CHECK(host_sim::passkey_answers.size() == 1);
  none_on_httpd();
  std::printf("PASS a_confirm_and_the_timeout_answer_once\n");
}

// A wrong PIN is rejected (answered no) on the loop task; reject and cancel
// answer a pending pairing no, once.
void test_a_wrong_pin_reject_and_cancel_answer_no_once() {
  boot();
  pairing_awaiting_confirm(123456, link(3, 0xC3));
  bc::Command c = cmd_of(bc::BT_CMD_PAIR_CONFIRM);
  c.pin = 654321;
  Rest r = rest(c);
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);
  CHECK(host_sim::passkey_answers.size() == 1 && !host_sim::passkey_answers[0].accept);
  CHECK(bc::g_pairing.state == bc::PAIR_FAILED);

  boot();
  pairing_awaiting_confirm(123456, link(4, 0xC4));
  r = rest(cmd_of(bc::BT_CMD_PAIR_REJECT));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(host_sim::passkey_answers.size() == 1 && !host_sim::passkey_answers[0].accept);
  CHECK(bc::g_pairing.state == bc::PAIR_NONE);
  r = rest(cmd_of(bc::BT_CMD_PAIR_REJECT));               // nothing left to reject
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);

  boot();
  pairing_awaiting_confirm(123456, link(5, 0xC5));
  r = rest(cmd_of(bc::BT_CMD_PAIR_CANCEL));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.pairing_before_turn == bc::PAIR_CONFIRMING);
  CHECK(host_sim::passkey_answers.size() == 1 && !host_sim::passkey_answers[0].accept);
  CHECK(host_sim::passkey_answers[0].task == "loop");
  CHECK(bc::g_pairing.state == bc::PAIR_NONE && bc::g_state != bc::BT_PAIRING);
  none_on_httpd();
  std::printf("PASS a_wrong_pin_reject_and_cancel_answer_no_once\n");
}

// ── Every command ───────────────────────────────────────────────────────

// Each command, one REST call: nothing moves before the loop task's turn,
// and every radio, bond and NVS call is the loop task's.
void test_every_command_runs_on_the_loop_task() {
  boot();
  CHECK(host_sim::advertising.isAdvertising());          // the bring-up's own

  Rest r = rest(cmd_of(bc::BT_CMD_ADVERTISE_STOP));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.calls_before_turn == 0 && !host_sim::advertising.isAdvertising());
  CHECK(host_sim::count("adv_stop", "loop") == 1);
  r = rest(cmd_of(bc::BT_CMD_ADVERTISE_START));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && r.r.refusal == bc::BT_REFUSED_NONE);
  CHECK(host_sim::advertising.isAdvertising() && bc::g_state == bc::BT_ADVERTISING);

  bc::Command scan = cmd_of(bc::BT_CMD_SCAN_START);
  scan.duration_ms = 5000;
  r = rest(scan);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(bc::g_scanning && bc::g_scan_duration_ms == 5000 && bc::g_state == bc::BT_SCANNING);
  r = rest(scan);                                          // one scan at a time
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);
  bc::g_scanned_count = 3;
  r = rest(cmd_of(bc::BT_CMD_SCAN_CLEAR));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && bc::g_scanned_count == 0);
  r = rest(cmd_of(bc::BT_CMD_SCAN_STOP));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && !bc::g_scanning);
  CHECK(host_sim::count("scan_start", "loop") == 1 && host_sim::count("scan_stop", "loop") == 1);

  // A phone connects and bonds (the NimBLE host task's callbacks).
  NimBLEConnInfo phone = link(11, 0xD1);
  phone.encrypted = phone.authenticated = phone.bonded = true;
  host_sim::server->peers = {11};
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(phone); });
  CHECK(bc::g_connection.connected && bc::g_paired_count == 1);
  host_sim::calls.clear();

  r = rest(cmd_of(bc::BT_CMD_ADVERTISE_START));            // refused while connected
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && r.r.refusal == bc::BT_REFUSED_CONNECTED);
  CHECK(host_sim::count("adv_start") == 0);

  bc::Command trust = cmd_of(bc::BT_CMD_PAIRED_TRUST);
  memcpy(trust.address, phone.address.getBase()->val, 6);
  trust.flag = true;
  r = rest(trust);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && bc::g_paired_devices[0].trusted);
  bc::Command block = trust;
  block.type = bc::BT_CMD_PAIRED_BLOCK;
  r = rest(block);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && bc::g_paired_devices[0].blocked);
  bc::Command stranger = trust;
  stranger.address[0] ^= 0xFF;
  r = rest(stranger);
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);

  r = rest(cmd_of(bc::BT_CMD_DISCONNECT));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(host_sim::count("disconnect", "loop") == 1);

  bc::Command remove = trust;
  remove.type = bc::BT_CMD_PAIRED_REMOVE;
  r = rest(remove);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && bc::g_paired_count == 0);
  CHECK(host_sim::count("bond_delete", "loop") == 1);
  r = rest(remove);
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);
  r = rest(cmd_of(bc::BT_CMD_PAIRED_CLEAR));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);

  bc::Command name = cmd_of(bc::BT_CMD_NAME);
  strcpy(name.name, "Porch Canary");
  r = rest(name);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(strcmp(bc::g_settings.device_name, "Porch Canary") == 0);
  // A name one byte too long stays too long in the command, and is refused.
  memset(name.name, 'x', bc::MAX_DEVICE_NAME_LEN + 1);
  name.name[bc::MAX_DEVICE_NAME_LEN + 1] = '\0';
  r = rest(name);
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);
  CHECK(strcmp(bc::g_settings.device_name, "Porch Canary") == 0);
  r = rest(cmd_of(bc::BT_CMD_NAME));                       // empty
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);

  bc::Command power = cmd_of(bc::BT_CMD_POWER);
  power.power = -6;
  r = rest(power);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && bc::g_settings.tx_power == -6);
  CHECK(host_sim::count("set_power", "loop") == 1);
  power.power = 12;
  r = rest(power);
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && bc::g_settings.tx_power == -6);

  r = rest(cmd_of(bc::BT_CMD_DISABLE));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.state_before_turn != bc::BT_DISABLED);
  CHECK(!bc::g_settings.enabled && bc::g_state == bc::BT_DISABLED);
  // Disabled: update()'s early return comes after the drain, so ENABLE runs.
  r = rest(cmd_of(bc::BT_CMD_ENABLE));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.state_before_turn == bc::BT_DISABLED);
  CHECK(bc::g_settings.enabled && bc::g_state == bc::BT_IDLE);
  CHECK(host_sim::count("nvs_write", "loop") > 0);
  none_on_httpd();
  CHECK(host_sim::mux_depth == 0);
  std::printf("PASS every_command_runs_on_the_loop_task\n");
}

// POST /api/bluetooth/settings names some fields: the loop task applies
// those to the settings it holds and leaves the rest, whatever else the
// command carries.
void test_settings_apply_the_fields_named() {
  boot();
  const int8_t power_before = bc::g_settings.tx_power;   // as load_settings() left it
  bc::Command s = cmd_of(bc::BT_CMD_SETTINGS);
  s.settings.allow_pairing = false;
  s.settings.tx_power = -12;                // not named: ignored
  s.settings.enabled = false;               // not named: ignored
  s.set_mask = bc::BT_SET_ALLOW_PAIRING;
  Rest r = rest(s);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(!bc::g_settings.allow_pairing && bc::g_settings.enabled);
  CHECK(bc::g_settings.tx_power == power_before && power_before != -12);
  bc::Command t = cmd_of(bc::BT_CMD_SETTINGS);
  t.settings.tx_power = 20;                 // clamped to +9 as before
  t.settings.inactivity_timeout_ms = 60000;
  strcpy(t.settings.device_name, "Shed");
  t.set_mask = bc::BT_SET_TX_POWER | bc::BT_SET_INACTIVITY | bc::BT_SET_DEVICE_NAME;
  r = rest(t);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(!bc::g_settings.allow_pairing);     // the first post's field stands
  CHECK(bc::g_settings.tx_power == 9 && bc::g_settings.inactivity_timeout_ms == 60000);
  CHECK(strcmp(bc::g_settings.device_name, "Shed") == 0);

  // Pairing with pairing disallowed: refused, and the answer says why.
  r = rest(cmd_of(bc::BT_CMD_PAIR_START));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && !r.r.allow_pairing);
  CHECK(r.r.refusal == bc::BT_REFUSED_NONE);
  none_on_httpd();
  std::printf("PASS settings_apply_the_fields_named\n");
}

// The settings fields, one by one: a POST naming one field changes that
// field and no other, whatever the rest of the command's settings say. The
// dashboard's Bluetooth panel sends auto_advertise and long_range_mode on
// their own; settings_apply_the_fields_named covers allow_pairing, the name,
// the TX power and the inactivity timeout together.
void test_each_settings_field_applies_alone() {
  struct Field {
    uint16_t bit;
    const char* name;
  };
  const Field fields[] = {
      {bc::BT_SET_ENABLED, "enabled"},
      {bc::BT_SET_AUTO_ADVERTISE, "auto_advertise"},
      {bc::BT_SET_ALLOW_PAIRING, "allow_pairing"},
      {bc::BT_SET_REQUIRE_PIN, "require_pin"},
      {bc::BT_SET_DEVICE_NAME, "device_name"},
      {bc::BT_SET_TX_POWER, "tx_power"},
      {bc::BT_SET_INACTIVITY, "inactivity_timeout"},
      {bc::BT_SET_NOTIFY_ON_CONNECT, "notify_on_connect"},
      {bc::BT_SET_LONG_RANGE, "long_range_mode"},
  };
  // Which fields differ between two settings.
  auto differ = [](const bc::BluetoothSettings& a, const bc::BluetoothSettings& b) {
    uint16_t m = 0;
    if (a.enabled != b.enabled) m |= bc::BT_SET_ENABLED;
    if (a.auto_advertise != b.auto_advertise) m |= bc::BT_SET_AUTO_ADVERTISE;
    if (a.allow_pairing != b.allow_pairing) m |= bc::BT_SET_ALLOW_PAIRING;
    if (a.require_pin != b.require_pin) m |= bc::BT_SET_REQUIRE_PIN;
    if (strcmp(a.device_name, b.device_name) != 0) m |= bc::BT_SET_DEVICE_NAME;
    if (a.tx_power != b.tx_power) m |= bc::BT_SET_TX_POWER;
    if (a.inactivity_timeout_ms != b.inactivity_timeout_ms) m |= bc::BT_SET_INACTIVITY;
    if (a.notify_on_connect != b.notify_on_connect) m |= bc::BT_SET_NOTIFY_ON_CONNECT;
    if (a.long_range_mode != b.long_range_mode) m |= bc::BT_SET_LONG_RANGE;
    return m;
  };
  for (const Field& f : fields) {
    boot();
    const bc::BluetoothSettings before = bc::g_settings;
    // Every field different from the device's, in range (tx_power is
    // clamped to -12..+9 and the name is at most MAX_DEVICE_NAME_LEN).
    bc::Command s = cmd_of(bc::BT_CMD_SETTINGS);
    s.settings.enabled = !before.enabled;
    s.settings.auto_advertise = !before.auto_advertise;
    s.settings.allow_pairing = !before.allow_pairing;
    s.settings.require_pin = !before.require_pin;
    strcpy(s.settings.device_name, strcmp(before.device_name, "Shed") == 0 ? "Barn" : "Shed");
    s.settings.tx_power = before.tx_power == -6 ? -3 : -6;
    s.settings.inactivity_timeout_ms = before.inactivity_timeout_ms + 60000;
    s.settings.notify_on_connect = !before.notify_on_connect;
    s.settings.long_range_mode = !before.long_range_mode;
    CHECK(differ(before, s.settings) == 0x1FF);
    s.set_mask = f.bit;
    const Rest r = rest(s);
    CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
    if (differ(before, bc::g_settings) != f.bit) {
      std::fprintf(stderr, "field %s: changed mask 0x%x\n", f.name,
                   (unsigned)differ(before, bc::g_settings));
    }
    CHECK(differ(before, bc::g_settings) == f.bit);
    CHECK(differ(s.settings, bc::g_settings) == (0x1FF & ~f.bit));
    none_on_httpd();
  }
  std::printf("PASS each_settings_field_applies_alone\n");
}

// Trust and block take the owner's flag both ways: untrust and unblock are
// the same POSTs with false.
void test_trust_and_block_take_the_owners_flag() {
  boot();
  NimBLEConnInfo phone = link(11, 0xD1);
  phone.encrypted = phone.authenticated = phone.bonded = true;
  host_sim::server->peers = {11};
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(phone); });
  CHECK(bc::g_paired_count == 1);
  bc::Command t = cmd_of(bc::BT_CMD_PAIRED_TRUST);
  memcpy(t.address, phone.address.getBase()->val, 6);
  t.flag = true;
  CHECK(rest(t).r.ok && bc::g_paired_devices[0].trusted);
  t.flag = false;
  Rest r = rest(t);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && !bc::g_paired_devices[0].trusted);
  bc::Command b = t;
  b.type = bc::BT_CMD_PAIRED_BLOCK;
  b.flag = true;
  CHECK(rest(b).r.ok && bc::g_paired_devices[0].blocked);
  b.flag = false;
  r = rest(b);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && !bc::g_paired_devices[0].blocked);
  CHECK(!bc::g_paired_devices[0].trusted);
  none_on_httpd();
  std::printf("PASS trust_and_block_take_the_owners_flag\n");
}

// DELETE /api/bluetooth/paired/all forgets every paired device: the table,
// each NimBLE bond and the saved list, on the loop task.
void test_paired_clear_forgets_every_device() {
  boot();
  NimBLEConnInfo phone = link(11, 0xD1);
  phone.encrypted = phone.authenticated = phone.bonded = true;
  host_sim::server->peers = {11};
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(phone); });
  CHECK(bc::g_paired_count == 1);
  host_sim::bonds = {phone.address, link(12, 0xD2).address};
  host_sim::calls.clear();
  const Rest r = rest(cmd_of(bc::BT_CMD_PAIRED_CLEAR));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.calls_before_turn == 0);
  CHECK(bc::g_paired_count == 0);
  CHECK(host_sim::count("bond_delete", "loop") == 2);
  CHECK(host_sim::count("nvs_write", "loop") > 0);
  none_on_httpd();
  std::printf("PASS paired_clear_forgets_every_device\n");
}

// "Start Advertising" and "Pair" are the owner turning Bluetooth on: with
// the stack up and Bluetooth off (POST /api/bluetooth/disable, or
// enabled=false in NVS), each command enables it on the loop task and then
// does its own work. no_command_brings_the_stack_up covers the stack down.
void test_advertise_and_pair_turn_bluetooth_on() {
  boot();
  Rest r = rest(cmd_of(bc::BT_CMD_DISABLE));
  CHECK(r.wait == lcr::Wait::kDone && !bc::is_enabled());
  CHECK(!host_sim::advertising.isAdvertising());
  r = rest(cmd_of(bc::BT_CMD_ADVERTISE_START));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && r.r.refusal == bc::BT_REFUSED_NONE);
  CHECK(r.state_before_turn == bc::BT_DISABLED);
  CHECK(bc::is_enabled() && host_sim::advertising.isAdvertising());
  CHECK(bc::g_state == bc::BT_ADVERTISING);

  r = rest(cmd_of(bc::BT_CMD_DISABLE));
  CHECK(r.wait == lcr::Wait::kDone && !bc::is_enabled());
  r = rest(cmd_of(bc::BT_CMD_PAIR_START));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && r.r.refusal == bc::BT_REFUSED_NONE);
  CHECK(r.r.allow_pairing);                 // the handler reads it on a refusal
  CHECK(bc::is_enabled() && bc::g_pairing.state == bc::PAIR_INITIATED);
  CHECK(host_sim::advertising.isAdvertising());   // pairing mode advertises
  CHECK(host_sim::count("nimble_init") == 0);
  none_on_httpd();
  std::printf("PASS advertise_and_pair_turn_bluetooth_on\n");
}

// ── The stack is init()'s, never a command's ────────────────────────────

// The owner turns Bluetooth on before the bring-up worker has run: the
// handler brings the stack up on its own task (bluetooth_api.h's
// bring_up(), the init() enable() used to run there), and the command turns
// it on. A command alone never brings it up: ENABLE, and ADVERTISE_START's
// and PAIR_START's auto-enable, are refused on the loop task instead, so
// NimBLE's init never blocks it.
void test_no_command_brings_the_stack_up() {
  boot(/*bring_up=*/false);
  bc::g_settings.enabled = false;
  Rest r = rest(cmd_of(bc::BT_CMD_ENABLE));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && r.r.refusal == bc::BT_REFUSED_NOT_ENABLED);
  r = rest(cmd_of(bc::BT_CMD_ADVERTISE_START));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && r.r.refusal == bc::BT_REFUSED_NOT_ENABLED);
  r = rest(cmd_of(bc::BT_CMD_PAIR_START));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && r.r.refusal == bc::BT_REFUSED_NOT_ENABLED);
  CHECK(host_sim::count("nimble_init") == 0 && !bc::is_initialized());

  // The handler's way: init() on its own task, then the command.
  host_sim::task = "httpd";
  CHECK(bc::is_initialized() || bc::init());
  host_sim::task = "loop";
  CHECK(host_sim::count("nimble_init", "httpd") == 1);
  r = rest(cmd_of(bc::BT_CMD_ENABLE));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(bc::is_enabled() && bc::is_initialized());
  CHECK(host_sim::count("nimble_init", "loop") == 0);
  std::printf("PASS no_command_brings_the_stack_up\n");
}

// ── The ring's edges, through the real submit() and update() ────────────

// The loop task never gets to it: after COMMAND_WAIT_MS the handler
// withdraws the command, answers 503 bluetooth_timeout, and no later pass
// runs it.
void test_a_command_the_loop_never_reaches_is_withdrawn() {
  boot();
  const Rest r = rest(cmd_of(bc::BT_CMD_DISABLE), /*turn_at=*/0);
  CHECK(r.wait == lcr::Wait::kWithdrawn);
  CHECK(r.waited_ms >= bc::COMMAND_WAIT_MS && r.waited_ms < bc::COMMAND_WAIT_MS + 10);
  CHECK(!r.r.ok);
  bc::update();
  CHECK(bc::is_enabled() && bc::g_state == bc::BT_ADVERTISING);
  CHECK(bc::not_run_status(r.wait) == 503);
  CHECK(strcmp(bc::not_run_error(r.wait), "bluetooth_timeout") == 0);
  CHECK(strcmp(http_status_line(503), "503 Service Unavailable") == 0);
  std::printf("PASS a_command_the_loop_never_reaches_is_withdrawn\n");
}

// Four commands wait already: the fifth is refused at once (409
// bluetooth_busy) and never runs; the four run on the next pass, in post
// order.
void test_a_full_ring_answers_busy() {
  boot();
  uint32_t t[bc::COMMAND_SLOTS];
  t[0] = bc::g_commands.post(cmd_of(bc::BT_CMD_ADVERTISE_STOP));
  t[1] = bc::g_commands.post(cmd_of(bc::BT_CMD_ADVERTISE_START));
  t[2] = bc::g_commands.post(cmd_of(bc::BT_CMD_ADVERTISE_STOP));
  bc::Command p = cmd_of(bc::BT_CMD_POWER);
  p.power = 3;
  t[3] = bc::g_commands.post(p);
  for (uint32_t ticket : t) CHECK(ticket != 0);
  const Rest r = rest(cmd_of(bc::BT_CMD_DISABLE));
  CHECK(r.wait == lcr::Wait::kBusy && r.sleeps == 0);
  CHECK(bc::not_run_status(r.wait) == 409);
  CHECK(strcmp(bc::not_run_error(r.wait), "bluetooth_busy") == 0);
  CHECK(strcmp(http_status_line(409), "409 Conflict") == 0);
  bc::update();
  std::vector<std::string> order;
  for (const host_sim::Call& c : host_sim::calls) order.push_back(c.what);
  CHECK((order == std::vector<std::string>{"adv_stop", "adv_start", "adv_stop", "nvs_write",
                                           "nvs_write", "nvs_write", "nvs_write", "nvs_write",
                                           "nvs_write", "nvs_write", "nvs_write", "set_power"}));
  CHECK(!host_sim::advertising.isAdvertising() && bc::g_settings.tx_power == 3);
  CHECK(bc::is_enabled());                                 // the refused DISABLE never ran
  for (uint32_t ticket : t) {
    bc::Result res;
    CHECK(bc::g_commands.poll(ticket, &res) == lcr::Poll::kDone && res.ok);
  }
  std::printf("PASS a_full_ring_answers_busy\n");
}

struct Test {
  const char* name;
  void (*fn)();
};
const Test kTests[] = {
    {"a_pin_confirm_runs_on_the_loop_task", test_a_pin_confirm_runs_on_the_loop_task},
    {"a_confirm_and_the_timeout_answer_once", test_a_confirm_and_the_timeout_answer_once},
    {"a_wrong_pin_reject_and_cancel_answer_no_once", test_a_wrong_pin_reject_and_cancel_answer_no_once},
    {"every_command_runs_on_the_loop_task", test_every_command_runs_on_the_loop_task},
    {"settings_apply_the_fields_named", test_settings_apply_the_fields_named},
    {"each_settings_field_applies_alone", test_each_settings_field_applies_alone},
    {"trust_and_block_take_the_owners_flag", test_trust_and_block_take_the_owners_flag},
    {"paired_clear_forgets_every_device", test_paired_clear_forgets_every_device},
    {"advertise_and_pair_turn_bluetooth_on", test_advertise_and_pair_turn_bluetooth_on},
    {"no_command_brings_the_stack_up", test_no_command_brings_the_stack_up},
    {"a_command_the_loop_never_reaches_is_withdrawn", test_a_command_the_loop_never_reaches_is_withdrawn},
    {"a_full_ring_answers_busy", test_a_full_ring_answers_busy},
};

}  // namespace bt_commands

int main(int argc, char** argv) {
  using namespace bt_commands;
  const char* only = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (const Test& t : kTests) {
    if (only != nullptr && std::strstr(t.name, only) == nullptr) continue;
    t.fn();
    ++ran;
  }
  CHECK(ran > 0);
  if (only != nullptr) {
    std::printf("%d of %zu tests run (%d checks), filter \"%s\"\n", ran,
                sizeof kTests / sizeof kTests[0], g_checks, only);
    return 0;
  }
  std::printf("ALL %d Bluetooth command checks PASSED (%d tests)\n", g_checks, ran);
  return 0;
}
