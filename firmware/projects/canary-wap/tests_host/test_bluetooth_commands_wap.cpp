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
// Every test but the threaded one is one thread, so a task is a role the
// test plays (host_sim::task): "httpd" while a handler waits in submit(),
// "loop" for update()'s passes (given on the handler's sleeps, in the stub
// vTaskDelay), "nimble" for the NimBLE host's callbacks, "bringup" for the
// boot worker's init(). The handlers' own source is held by
// firmware/scripts/check_wap_loop_commands.py (ArduinoJson, which they build
// their answers with, is not on the host).
//
// And POST /api/bluetooth/settings's "enabled" (sweep F144): set_settings()
// decided the change after assigning the new settings, so both of its
// branches were dead; it now turns Bluetooth off as a disable does and on as
// an enable does (a command still never brings the stack up).
//
// And the NimBLE host task's callbacks (sweep F143): a connect, a passkey
// shown or to confirm, a bond, a GATT read or write, a scan result, a
// scan's end and a disconnect wrote the channel's state (and saved the
// paired list, and restarted advertising) on the NimBLE host task. Each now
// posts an event (loop_event_queue.h) that update() applies on the loop
// task; the pending pairing's heap copy travels in its event, so it has one
// owner. Played deterministically ("nimble" between the loop task's
// passes), and for real on three threads (the NimBLE host task, the loop
// task, the HTTP server's), which `make tsan-bt-commands` runs under
// ThreadSanitizer.
//
// Host-tested only: the stand-in is not NimBLE, and nothing here runs a
// radio; the Arduino compile is CI's (firmware.yml's canary-wap legs).
//
// Run: ./test_bluetooth_commands_wap [name]

#ifndef BLUETOOTH_CHANNEL_CPP
#error "BLUETOOTH_CHANNEL_CPP (absolute path to bluetooth_channel.cpp) must be defined"
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Arduino.h"
#include "NimBLEDevice.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// nvs_store.h's NvsMainSession, over an in-memory store; every write records
// the task that made it.
#define SECURACV_NVS_STORE_H
// (A lock of its own, as the device's NVS has: the threaded test finds only
// the channel's races.)
namespace host_sim {
inline std::map<std::string, std::vector<uint8_t>> main_nvs;
inline std::recursive_mutex main_nvs_mu;
}  // namespace host_sim
class FakeMainNvs {
 public:
  size_t putBytes(const char* k, const void* v, size_t n) {
    host_sim::note("nvs_write");
    std::lock_guard<std::recursive_mutex> g(host_sim::main_nvs_mu);
    const uint8_t* b = static_cast<const uint8_t*>(v);
    host_sim::main_nvs[k].assign(b, b + n);
    return n;
  }
  size_t getBytesLength(const char* k) {
    std::lock_guard<std::recursive_mutex> g(host_sim::main_nvs_mu);
    auto it = host_sim::main_nvs.find(k);
    return it == host_sim::main_nvs.end() ? 0 : it->second.size();
  }
  size_t getBytes(const char* k, void* buf, size_t n) {
    std::lock_guard<std::recursive_mutex> g(host_sim::main_nvs_mu);
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
std::mutex g_health_mu;   // the device's health log takes its own lock
void log_health(LogLevel, LogCategory, const char* message, const char*) {
  std::lock_guard<std::mutex> g(g_health_mu);
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
  bc::g_events.consume([](const bc::Event& e) {      // a copy a test left waiting
    if (e.type == bc::BT_EV_CONFIRM_PASSKEY) delete e.u.passkey.conn;
  });
  bc::g_events = decltype(bc::g_events)();
  bc::g_events_dropped_seen = 0;
  bc::g_events_dropped_logged_ms = 0;
  bc::g_status_view = decltype(bc::g_status_view)();       // nothing published yet
  bc::g_scan_view = decltype(bc::g_scan_view)();
  bc::g_paired_view = decltype(bc::g_paired_view)();
  bc::g_local_address[0] = '\0';
  memcpy(bc::g_settings.device_name, bc::kDefaultSettings.device_name, sizeof bc::g_settings.device_name);
  bc::g_settings.inactivity_timeout_ms = bc::kDefaultSettings.inactivity_timeout_ms;
  bc::g_settings.notify_on_connect = bc::kDefaultSettings.notify_on_connect;
  CHECK(host_sim::conn_heap == 0);                   // every pending copy was deleted once
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

// One pass of the loop task: what the NimBLE host task reported is applied
// there (sweep F143), with the owner's commands.
void loop_pass() {
  const std::string was = host_sim::task;
  host_sim::task = "loop";
  bc::update();
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
  loop_pass();
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
  loop_pass();
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
  loop_pass();
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
  loop_pass();
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

// ── The NimBLE host task's callbacks (sweep F143) ───────────────────────

// How many health-log lines say `message`.
size_t health_says(const char* message) {
  size_t n = 0;
  for (const std::string& m : g_health) n += m == message;
  return n;
}

// A link up, a bond, a scan's results and its end, a link down: each
// callback, on the NimBLE host task, changes none of the channel's state
// and makes no radio, bond or NVS call; the loop task's next pass applies
// it, in order, and the advertising restart and the paired list's NVS save
// are the loop task's. Before F143 each callback wrote the state, saved the
// paired list and restarted advertising on the NimBLE host task itself.
void test_a_callback_changes_nothing_until_the_loop_task_applies_it() {
  boot();
  CHECK(host_sim::advertising.isAdvertising() && bc::g_state == bc::BT_ADVERTISING);
  NimBLEConnInfo phone = link(21, 0xE1);
  phone.encrypted = phone.authenticated = phone.bonded = true;
  host_sim::server->peers = {21};
  host_sim::now_ms = 50000;
  const uint32_t connections_before = bc::g_total_connections;
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  host_sim::now_ms = 50400;                                 // the loop task's turn comes later
  CHECK(!bc::g_connection.connected && bc::g_state == bc::BT_ADVERTISING);
  CHECK(bc::g_total_connections == connections_before);
  CHECK(host_sim::count("", "nimble") == 0);
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 21);
  CHECK(bc::g_total_connections == connections_before + 1);
  CHECK(bc::g_connection.connected_since_ms == 50000);       // when the stack said so
  CHECK(bc::g_state == bc::BT_CONNECTED && !host_sim::advertising.isAdvertising());
  CHECK(host_sim::count("adv_stop", "loop") == 1);

  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(phone); });
  CHECK(bc::g_paired_count == 0 && bc::g_connection.security == bc::SEC_AUTHENTICATED);
  CHECK(host_sim::count("nvs_write", "nimble") == 0);
  loop_pass();
  CHECK(bc::g_paired_count == 1 && bc::g_connection.security == bc::SEC_BONDED);
  CHECK(memcmp(bc::g_paired_devices[0].address, phone.address.getBase()->val, 6) == 0);
  CHECK(bc::g_paired_devices[0].last_connected_ms == 50400);
  CHECK(host_sim::count("nvs_write", "loop") > 0);

  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  CHECK(bc::g_connection.connected && !host_sim::advertising.isAdvertising());
  loop_pass();
  CHECK(!bc::g_connection.connected && bc::g_connection_handle == 0xFFFF);
  CHECK(host_sim::advertising.isAdvertising() && bc::g_state == bc::BT_ADVERTISING);
  CHECK(host_sim::count("adv_start", "loop") == 1);

  // A scan: its results and its end wait for the loop task too.
  bc::Command scan = cmd_of(bc::BT_CMD_SCAN_START);
  scan.duration_ms = 5000;
  CHECK(rest(scan).r.ok && bc::g_scanning);
  NimBLEAdvertisedDevice watch;
  const uint8_t a1[6] = {1, 2, 3, 4, 5, 6};
  watch.address = NimBLEAddress(a1, 0);
  watch.name = "Garmin watch";
  watch.rssi = -70;
  on_nimble([&] { host_sim::scan.callbacks()->onResult(&watch); });
  watch.rssi = -55;                                          // heard again, closer
  on_nimble([&] { host_sim::scan.callbacks()->onResult(&watch); });
  on_nimble([&] { host_sim::scan.callbacks()->onScanEnd(NimBLEScanResults(), 0); });
  CHECK(bc::g_scanned_count == 0 && bc::g_scanning && bc::g_state == bc::BT_SCANNING);
  loop_pass();
  CHECK(bc::g_scanned_count == 1 && bc::g_scanned_devices[0].rssi == -55);
  CHECK(strcmp(bc::g_scanned_devices[0].name, "Garmin watch") == 0);
  CHECK(bc::g_scanned_devices[0].type == bc::DEV_WEARABLE);
  CHECK(!bc::g_scanning && bc::g_state == bc::BT_IDLE);
  CHECK(host_sim::count("", "nimble") == 0);
  none_on_httpd();
  std::printf("PASS a_callback_changes_nothing_until_the_loop_task_applies_it\n");
}

// The pending Numeric-Comparison pairing has one owner. Its heap copy
// travels in the callback's event, so the NimBLE host task never touches
// the pending pointer: a second passkey before the loop task's pass, a PIN
// confirm that waits while the stack asks again, and the pairing timeout
// each find one pending pairing, answer it once and delete it once (the
// stand-in counts the heap copies alive). Before F143 onConfirmPassKey()
// deleted and replaced the pending copy on the NimBLE host task while the
// loop task's confirm or timeout could be answering and deleting it.
void test_the_pending_pairing_has_one_owner() {
  boot();
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok);
  NimBLEConnInfo phone = link(31, 0xF1);
  on_nimble([&] { host_sim::server->callbacks()->onConfirmPassKey(phone, 111111); });
  CHECK(bc::g_pending_pair_info == nullptr && !bc::g_pending_pair_active);
  CHECK(bc::g_pairing.state == bc::PAIR_INITIATED);
  CHECK(host_sim::conn_heap == 1);                           // in the event
  // The stack asks again before the loop task's pass (the phone retried).
  on_nimble([&] { host_sim::server->callbacks()->onConfirmPassKey(phone, 222222); });
  CHECK(host_sim::conn_heap == 2);
  loop_pass();
  CHECK(bc::g_pairing.state == bc::PAIR_CONFIRMING && bc::g_pairing.pin_code == 222222);
  CHECK(bc::g_pending_pair_active && bc::g_pending_pair_info != nullptr);
  CHECK(host_sim::conn_heap == 1);                           // the first, replaced, deleted once
  CHECK(host_sim::passkey_answers.empty());

  // A confirm waits for the loop task while the stack asks a third time:
  // the loop task's pass applies the new passkey, then the confirm, which
  // names it: one answer, yes, and the copy deleted.
  bc::Command c = cmd_of(bc::BT_CMD_PAIR_CONFIRM);
  c.pin = 333333;
  Rest r;
  {
    unsigned sleeps = 0;
    host_sim::on_task_delay = [&](uint32_t ms) {
      host_sim::now_ms += ms;
      ++sleeps;
      if (sleeps == 1) on_nimble([&] { host_sim::server->callbacks()->onConfirmPassKey(phone, 333333); });
      if (sleeps == 2) loop_pass();
    };
    host_sim::task = "httpd";
    r.wait = bc::submit(c, &r.r);
    host_sim::task = "loop";
    host_sim::on_task_delay = nullptr;
  }
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(host_sim::passkey_answers.size() == 1 && host_sim::passkey_answers[0].accept);
  CHECK(host_sim::passkey_answers[0].task == "loop");
  CHECK(bc::g_pending_pair_info == nullptr && !bc::g_pending_pair_active);
  CHECK(host_sim::conn_heap == 0);

  // A passkey the stack asks in the pass the pairing times out: applied,
  // then answered no by the timeout, once.
  boot();
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok);
  host_sim::now_ms += bc::PAIRING_TIMEOUT_MS;
  on_nimble([&] { host_sim::server->callbacks()->onConfirmPassKey(phone, 444444); });
  loop_pass();
  CHECK(host_sim::passkey_answers.size() == 1 && !host_sim::passkey_answers[0].accept);
  CHECK(host_sim::passkey_answers[0].task == "loop");
  CHECK(bc::g_pairing.state == bc::PAIR_NONE && host_sim::conn_heap == 0);
  CHECK(host_sim::count("", "nimble") == 0);
  none_on_httpd();
  std::printf("PASS the_pending_pairing_has_one_owner\n");
}

// The queue full (the loop task stalled): a scan result or a link's
// activity is posted only while fewer than EVENT_LOSSY_LIMIT wait, so the
// rest of the room is a link's; an event that finds no room is dropped and
// counted, the count goes to the health log once a minute at most, and a
// passkey to confirm that finds no room is answered no on the NimBLE host
// task (fails closed) with its copy deleted there.
void test_a_full_queue_keeps_the_links_room_and_fails_a_pairing_closed() {
  boot();
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok);
  bc::Command scan = cmd_of(bc::BT_CMD_SCAN_START);
  scan.duration_ms = 5000;
  CHECK(rest(scan).r.ok);
  NimBLEConnInfo phone = link(41, 0xA4);
  host_sim::server->peers = {41};
  // A burst of advertisements: twenty heard before the loop task's pass.
  for (uint8_t i = 0; i < 20; ++i) {
    NimBLEAdvertisedDevice d;
    const uint8_t a[6] = {i, 9, 9, 9, 9, 9};
    d.address = NimBLEAddress(a, 0);
    on_nimble([&] { host_sim::scan.callbacks()->onResult(&d); });
  }
  CHECK(bc::g_events.waiting() == bc::EVENT_LOSSY_LIMIT && bc::g_events.dropped() == 4);
  // A phone connects and the stack asks for a passkey: the reserve holds them.
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  on_nimble([&] { host_sim::server->callbacks()->onConfirmPassKey(phone, 555555); });
  CHECK(bc::g_events.dropped() == 4 && host_sim::passkey_answers.empty());
  // Fill the rest; then a second passkey finds no room: answered no here.
  while (bc::g_events.waiting() < bc::EVENT_SLOTS) {
    on_nimble([&] { host_sim::scan.callbacks()->onScanEnd(NimBLEScanResults(), 0); });
  }
  NimBLEConnInfo other = link(42, 0xA5);
  on_nimble([&] { host_sim::server->callbacks()->onConfirmPassKey(other, 666666); });
  CHECK(bc::g_events.dropped() == 5);
  CHECK(host_sim::passkey_answers.size() == 1 && !host_sim::passkey_answers[0].accept);
  CHECK(host_sim::passkey_answers[0].task == "nimble" && host_sim::passkey_answers[0].handle == 42);
  CHECK(host_sim::conn_heap == 1);                           // only the queued one's copy
  g_health.clear();
  loop_pass();
  CHECK(bc::g_events.waiting() == 0);
  CHECK(bc::g_scanned_count == bc::EVENT_LOSSY_LIMIT);       // the sixteen that fit
  CHECK(bc::g_connection.connected);                         // the link's events kept
  CHECK(bc::g_pairing.state == bc::PAIR_CONFIRMING && bc::g_pairing.pin_code == 555555);
  CHECK(health_says("BLE events dropped (queue full)") == 1);
  // More drops within the minute: counted, not logged again; after it, logged.
  for (int i = 0; i < 30; ++i) {
    on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0); });
  }
  loop_pass();
  CHECK(health_says("BLE events dropped (queue full)") == 1);
  host_sim::now_ms += 60000;
  loop_pass();
  CHECK(health_says("BLE events dropped (queue full)") == 2);
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_CANCEL)).r.ok);
  CHECK(host_sim::conn_heap == 0);
  std::printf("PASS a_full_queue_keeps_the_links_room_and_fails_a_pairing_closed\n");
}

// A link that ends after Bluetooth was turned off (the settings' "enabled":
// false, or POST /disable, drops it) leaves it off: the state stays
// disabled and nothing advertises. Before, the disconnect set it idle.
void test_a_link_that_ends_after_bluetooth_is_off_leaves_it_off() {
  boot();
  NimBLEConnInfo phone = link(51, 0xB5);
  host_sim::server->peers = {51};
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  loop_pass();
  bc::Command off = cmd_of(bc::BT_CMD_SETTINGS);
  off.set_mask = bc::BT_SET_ENABLED;
  CHECK(rest(off).r.ok && bc::g_state == bc::BT_DISABLED);
  host_sim::calls.clear();
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x16); });
  loop_pass();
  CHECK(!bc::g_connection.connected && bc::g_state == bc::BT_DISABLED);
  CHECK(host_sim::count("adv_start") == 0 && !host_sim::advertising.isAdvertising());
  std::printf("PASS a_link_that_ends_after_bluetooth_is_off_leaves_it_off\n");
}

// A scan's end is applied once: when the owner (or update()'s scan
// timeout) stopped the scan first, the stack's end of it changes nothing,
// so a pairing started since keeps its state.
void test_a_scan_end_is_applied_once() {
  boot();
  bc::Command scan = cmd_of(bc::BT_CMD_SCAN_START);
  scan.duration_ms = 5000;
  CHECK(rest(scan).r.ok);
  CHECK(rest(cmd_of(bc::BT_CMD_SCAN_STOP)).r.ok && !bc::g_scanning);
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok && bc::g_state == bc::BT_PAIRING);
  g_health.clear();
  on_nimble([&] { host_sim::scan.callbacks()->onScanEnd(NimBLEScanResults(), 0); });
  loop_pass();
  CHECK(bc::g_state == bc::BT_PAIRING && health_says("BLE scan complete") == 0);
  std::printf("PASS a_scan_end_is_applied_once\n");
}

// ── Two tasks at once (sweep F143), for ThreadSanitizer ─────────────────

// The NimBLE host task's callbacks on one thread, the loop task's passes on
// another (this one) and the HTTP server's commands and GET reads (F138) on
// a third, as on the device, where the NimBLE host task runs beside the
// loop task. A link, a
// passkey shown and one to confirm, a bond, a GATT write, a scan result and
// a scan's end, a link down, over and over, while update() runs and the
// owner's commands arrive. What must hold: no crash; every pending
// pairing's copy deleted once (none left, none twice: the stand-in counts
// them); no more passkey answers than passkeys asked; the NimBLE host
// task's own calls only the answers a full queue makes; the paired list in
// its bounds. `make tsan-bt-commands` runs the suite under
// -fsanitize=thread, where a callback that writes the channel's state (the
// pre-F143 code) is a reported data race.
void test_threads_callbacks_loop_and_commands() {
  boot();
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok);
  host_sim::server->peers = {61};
  NimBLEServerCallbacks* server_cb = host_sim::server->callbacks();
  NimBLEScanCallbacks* scan_cb = &bc::g_scan_callbacks;
  NimBLECharacteristicCallbacks* char_cb = &bc::g_char_callbacks;
  NimBLECharacteristic* command_char = bc::g_command_char;
  CHECK(server_cb != nullptr && command_char != nullptr);
  std::atomic<bool> nimble_done{false};
  std::atomic<uint32_t> passkeys_asked{0};
  constexpr int kRounds = 2000;

  std::thread nimble([&] {
    host_sim::task = "nimble";
    NimBLEConnInfo phone = link(61, 0xC6);
    phone.encrypted = phone.authenticated = phone.bonded = true;
    for (int i = 0; i < kRounds; ++i) {
      server_cb->onConnect(host_sim::server.get(), phone);
      (void)server_cb->onPassKeyDisplay();
      passkeys_asked.fetch_add(1);
      server_cb->onConfirmPassKey(phone, 100000u + (uint32_t)i);
      server_cb->onAuthenticationComplete(phone);
      char_cb->onWrite(command_char, phone);
      char_cb->onRead(command_char, phone);
      NimBLEAdvertisedDevice d;
      const uint8_t a[6] = {(uint8_t)(i % 40), 7, 7, 7, 7, 7};
      d.address = NimBLEAddress(a, 0);
      scan_cb->onResult(&d);
      scan_cb->onScanEnd(NimBLEScanResults(), 0);
      server_cb->onDisconnect(host_sim::server.get(), phone, 0x13);
      // A radio's pace: wait while the loop task is behind, so its passes
      // and these callbacks interleave rather than the queue filling.
      while (bc::g_events.waiting() > bc::EVENT_SLOTS / 2) std::this_thread::yield();
    }
    nimble_done.store(true);
  });

  const bc::CommandType kinds[] = {bc::BT_CMD_PAIR_CONFIRM, bc::BT_CMD_PAIR_START,
                                   bc::BT_CMD_SCAN_START,   bc::BT_CMD_PAIR_REJECT,
                                   bc::BT_CMD_DISCONNECT,   bc::BT_CMD_SCAN_STOP,
                                   bc::BT_CMD_PAIR_CANCEL,  bc::BT_CMD_ADVERTISE_START,
                                   bc::BT_CMD_PAIRED_CLEAR, bc::BT_CMD_SCAN_CLEAR};
  std::atomic<uint32_t> commands_done{0};
  std::atomic<uint32_t> reads_done{0};
  std::atomic<bool> reads_whole{true};
  std::thread httpd([&] {
    host_sim::task = "httpd";
    for (uint32_t i = 0; !nimble_done.load(); ++i) {
      bc::Command c = cmd_of(kinds[i % (sizeof kinds / sizeof kinds[0])]);
      c.pin = 100000u + i;
      c.duration_ms = 1000;
      bc::Result r;
      if (bc::submit(c, &r, 40) == lcr::Wait::kDone) commands_done.fetch_add(1);
      // The GET routes' reads (sweep F138), while the loop task publishes.
      bc::BluetoothStatus st;
      bc::read_status(&st);
      bc::ScanView scan;
      bc::read_scan(&scan);
      bc::PairedView paired;
      bc::read_paired(&paired);
      const bc::BluetoothSettings set = bc::read_settings();
      const bool whole = st.scanned_count <= bc::MAX_SCANNED_DEVICES &&
                         st.paired_count <= bc::MAX_PAIRED_DEVICES &&
                         scan.count <= bc::MAX_SCANNED_DEVICES &&
                         paired.count <= bc::MAX_PAIRED_DEVICES &&
                         memchr(set.device_name, '\0', sizeof set.device_name) != nullptr &&
                         (st.connected == st.connection.connected);
      if (!whole) reads_whole.store(false);
      reads_done.fetch_add(1);
    }
  });

  host_sim::task = "loop";
  while (!nimble_done.load()) {
    bc::update();
    host_sim::now_ms += 1;
    std::this_thread::yield();
  }
  nimble.join();
  httpd.join();
  bc::update();                                              // what is left
  CHECK(bc::g_events.waiting() == 0);
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_CANCEL)).r.ok);
  CHECK(host_sim::conn_heap == 0);
  size_t answers = 0;
  {
    std::lock_guard<std::mutex> g(host_sim::passkey_mu);
    answers = host_sim::passkey_answers.size();
  }
  CHECK(answers <= passkeys_asked.load());
  CHECK(host_sim::count("", "nimble") == host_sim::count("passkey_answer", "nimble"));
  CHECK(bc::g_paired_count <= bc::MAX_PAIRED_DEVICES && bc::g_scanned_count <= bc::MAX_SCANNED_DEVICES);
  CHECK(host_sim::mux_depth == 0);
  CHECK(reads_whole.load() && reads_done.load() > 0);
  std::printf("PASS threads_callbacks_loop_and_commands (%u commands ran, %u view reads, "
              "%zu passkey answers, %u events dropped)\n", (unsigned)commands_done.load(),
              (unsigned)reads_done.load(), answers, (unsigned)bc::g_events.dropped());
}

// ── What the status routes read (sweep F138) ────────────────────────────

// GET /api/bluetooth, /scan/results, /paired and /settings read
// read_status(), read_scan(), read_paired() and read_settings(): what the
// last pass published, not the state the loop task is changing. Here the
// test plays the loop task mid-pass (it changes the live state, as update()
// and the events it applies do): a read shows the pass before, whole, until
// the pass ends and publishes. Before F138 the routes read get_status(), the
// scan and paired tables and the settings in place.
void test_a_route_reads_the_last_published_pass() {
  boot();
  loop_pass();
  bc::BluetoothStatus st;
  bc::read_status(&st);
  CHECK(st.state == bc::BT_ADVERTISING && st.enabled && st.advertising && !st.scanning);
  // Mid-pass: a scan list being filled, a paired device appended, a new name.
  bc::g_scanned_devices[0].rssi = -40;
  strcpy(bc::g_scanned_devices[0].name, "Pixel");
  bc::g_scanned_count = 1;
  bc::g_scanning = true;
  bc::g_state = bc::BT_SCANNING;
  bc::g_paired_devices[0].connection_count = 4;
  bc::g_paired_count = 1;
  strcpy(bc::g_settings.device_name, "Mid-pass");
  bc::ScanView scan;
  bc::read_scan(&scan);
  CHECK(scan.count == 0 && !scan.scanning);
  bc::PairedView paired;
  bc::read_paired(&paired);
  CHECK(paired.count == 0);
  bc::read_status(&st);
  CHECK(st.state == bc::BT_ADVERTISING && !st.scanning && st.scanned_count == 0);
  CHECK(strcmp(st.device_name, "SecuraCV-Canary") == 0);
  CHECK(strcmp(bc::read_settings().device_name, "SecuraCV-Canary") == 0);
  // The pass ends: all of it at once.
  bc::g_scanning = false;                                   // update()'s early work
  loop_pass();
  bc::read_scan(&scan);
  CHECK(scan.count == 1 && scan.devices[0].rssi == -40 && strcmp(scan.devices[0].name, "Pixel") == 0);
  bc::read_paired(&paired);
  CHECK(paired.count == 1 && paired.devices[0].connection_count == 4);
  bc::read_status(&st);
  CHECK(st.scanned_count == 1 && st.paired_count == 1 && strcmp(st.device_name, "Mid-pass") == 0);
  CHECK(strcmp(bc::read_settings().device_name, "Mid-pass") == 0);
  // A disabled channel's pass publishes too (update()'s early return).
  CHECK(rest(cmd_of(bc::BT_CMD_DISABLE)).r.ok);
  bc::g_paired_count = 0;
  loop_pass();
  bc::read_paired(&paired);
  bc::read_status(&st);
  CHECK(paired.count == 0 && st.state == bc::BT_DISABLED && !st.enabled);
  std::printf("PASS a_route_reads_the_last_published_pass\n");
}

// The loop task drains the commands at the start of its pass; a command's
// handler answers as soon as its result is posted, and the dashboard reads
// the status right after (it reloads the paired list after a removal, the
// status after Start/Stop Advertising). The view already shows what the
// command did, before the rest of that pass.
void test_a_read_right_after_a_post_shows_what_it_did() {
  boot();
  NimBLEConnInfo phone = link(71, 0xD7);
  phone.encrypted = phone.authenticated = phone.bonded = true;
  host_sim::server->peers = {71};
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(phone); });
  loop_pass();
  bc::PairedView paired;
  bc::read_paired(&paired);
  CHECK(paired.count == 1);
  // The loop task's turn runs only the drain (the start of its pass).
  auto drain_only = [](const bc::Command& c) {
    bc::Result r;
    host_sim::on_task_delay = [](uint32_t ms) {
      host_sim::now_ms += ms;
      const std::string was = host_sim::task;
      host_sim::task = "loop";
      bc::g_commands.drain(bc::run_command);
      host_sim::task = was;
    };
    host_sim::task = "httpd";
    const lcr::Wait w = bc::submit(c, &r);
    host_sim::task = "loop";
    host_sim::on_task_delay = nullptr;
    return w == lcr::Wait::kDone && r.ok;
  };
  bc::Command remove = cmd_of(bc::BT_CMD_PAIRED_REMOVE);
  memcpy(remove.address, phone.address.getBase()->val, 6);
  CHECK(drain_only(remove));
  bc::read_paired(&paired);
  CHECK(paired.count == 0);
  bc::Command name = cmd_of(bc::BT_CMD_NAME);
  strcpy(name.name, "Garage");
  CHECK(drain_only(name));
  CHECK(strcmp(bc::read_settings().device_name, "Garage") == 0);
  bc::BluetoothStatus st;
  bc::read_status(&st);
  CHECK(strcmp(st.device_name, "Garage") == 0 && st.paired_count == 0);
  std::printf("PASS a_read_right_after_a_post_shows_what_it_did\n");
}

// Before the loop task's first pass publishes (the HTTP server starts in
// setup(), before loop() runs): what the channel holds at boot, as
// get_status() and get_settings() answered then, whatever the live state
// says in the meantime.
void test_reads_before_the_first_pass_show_the_boot_state() {
  boot(/*bring_up=*/false);
  strcpy(bc::g_settings.device_name, "Loaded");             // as if init() were loading NVS
  bc::g_settings.tx_power = -6;
  bc::g_paired_count = 2;
  bc::BluetoothStatus st;
  bc::read_status(&st);
  CHECK(st.state == bc::BT_DISABLED && st.enabled == bt_defaults::ENABLED);
  CHECK(strcmp(st.device_name, "SecuraCV-Canary") == 0 && st.tx_power == 9);
  CHECK(st.mtu == 23 && st.battery_pct == 100 && st.paired_count == 0 && !st.connected);
  CHECK(st.pairing.state == bc::PAIR_NONE && st.local_address[0] == '\0');
  const bc::BluetoothSettings s = bc::read_settings();
  CHECK(strcmp(s.device_name, "SecuraCV-Canary") == 0 && s.tx_power == 9);
  CHECK(s.enabled == bt_defaults::ENABLED && s.auto_advertise == bt_defaults::AUTO_ADVERTISE);
  CHECK(s.allow_pairing == bt_defaults::ALLOW_PAIRING && s.require_pin == bt_defaults::REQUIRE_PIN);
  CHECK(s.inactivity_timeout_ms == bc::INACTIVITY_TIMEOUT_MS && s.notify_on_connect && !s.long_range_mode);
  bc::ScanView scan;
  bc::read_scan(&scan);
  bc::PairedView paired;
  bc::read_paired(&paired);
  CHECK(scan.count == 0 && !scan.scanning && paired.count == 0);
  bc::g_paired_count = 0;
  std::printf("PASS reads_before_the_first_pass_show_the_boot_state\n");
}

// The advertising and connected times are counted to the read, as
// get_status() counted them, with no pass in between.
void test_the_status_read_counts_the_times_to_now() {
  boot();
  host_sim::now_ms = 200000;
  CHECK(rest(cmd_of(bc::BT_CMD_ADVERTISE_STOP)).r.ok);
  CHECK(rest(cmd_of(bc::BT_CMD_ADVERTISE_START)).r.ok);      // advertising since 200000-ish
  const uint32_t since = bc::g_advertising_start_ms;
  const uint32_t total = bc::g_advertising_total_ms;
  host_sim::now_ms = since + 5000;
  bc::BluetoothStatus st;
  bc::read_status(&st);
  CHECK(st.advertising && st.advertising_time_ms == total + 5000);
  NimBLEConnInfo phone = link(81, 0xE8);
  host_sim::server->peers = {81};
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  loop_pass();
  const uint32_t connected_at = bc::g_connection.connected_since_ms;
  const uint32_t connected_total = bc::g_connected_total_ms;
  host_sim::now_ms = connected_at + 7000;
  bc::read_status(&st);
  CHECK(st.connected && st.connected_time_ms == connected_total + 7000);
  CHECK(st.connection.connected_since_ms == connected_at);  // the route counts connected_sec from it
  std::printf("PASS the_status_read_counts_the_times_to_now\n");
}

// Every field each route shows reaches it: the live state set field by
// field to values of its own, one pass, and each reader's copy compared.
void test_each_view_field_reaches_the_route() {
  boot();
  bc::g_state = bc::BT_PAIRING;
  bc::g_scanning = true;
  bc::g_connection.connected = true;
  const uint8_t ca[6] = {9, 8, 7, 6, 5, 4};
  memcpy(bc::g_connection.address, ca, 6);
  strcpy(bc::g_connection.name, "Kitchen tablet");
  bc::g_connection.rssi = -61;
  bc::g_connection.security = bc::SEC_BONDED;
  bc::g_connection.connected_since_ms = 777;
  bc::g_connection.last_activity_ms = 888;
  bc::g_connection.bytes_sent = 1234;
  bc::g_connection.bytes_received = 4321;
  bc::g_pairing.state = bc::PAIR_PIN_DISPLAYED;
  memcpy(bc::g_pairing.peer_address, ca, 6);
  strcpy(bc::g_pairing.peer_name, "Peer");
  bc::g_pairing.pin_code = 13579;
  bc::g_pairing.started_ms = host_sim::now_ms;
  bc::g_pairing.pin_displayed = true;
  bc::g_connection_mtu = 185;
  bc::g_total_connections = 11;
  bc::g_total_bytes_sent = 22;
  bc::g_total_bytes_received = 33;
  bc::g_connected_total_ms = 44000;
  strcpy(bc::g_settings.device_name, "Porch");
  bc::g_settings.tx_power = -3;
  bc::g_settings.auto_advertise = false;
  bc::g_settings.allow_pairing = false;
  bc::g_settings.require_pin = false;
  bc::g_settings.inactivity_timeout_ms = 123000;
  bc::g_settings.notify_on_connect = false;
  bc::g_settings.long_range_mode = true;
  for (size_t i = 0; i < 3; ++i) {
    bc::ScannedDevice& d = bc::g_scanned_devices[i];
    memset(&d, 0, sizeof d);
    d.address[0] = (uint8_t)(0x10 + i);
    snprintf(d.name, sizeof d.name, "dev-%u", (unsigned)i);
    d.rssi = (int8_t)(-50 - (int)i);
    d.type = bc::DEV_WEARABLE;
    d.connectable = i != 1;
    d.has_securacv_service = i == 2;
    d.last_seen_ms = 1000u + (uint32_t)i;
  }
  bc::g_scanned_count = 3;
  for (size_t i = 0; i < 2; ++i) {
    bc::PairedDevice& p = bc::g_paired_devices[i];
    memset(&p, 0, sizeof p);
    p.address[5] = (uint8_t)(0x20 + i);
    p.address_type = 1;
    snprintf(p.name, sizeof p.name, "phone-%u", (unsigned)i);
    p.paired_timestamp = 5000u + (uint32_t)i;
    p.last_connected_ms = 6000u + (uint32_t)i;
    p.connection_count = 7u + (uint32_t)i;
    p.security = bc::SEC_AUTHENTICATED;
    p.trusted = i == 0;
    p.blocked = i == 1;
  }
  bc::g_paired_count = 2;
  // update() itself must not move them: run the end of a pass only.
  bc::publish_views();
  bc::BluetoothStatus st;
  bc::read_status(&st);
  CHECK(st.state == bc::BT_PAIRING && st.enabled && st.scanning && st.connected);
  CHECK(st.advertising == host_sim::advertising.isAdvertising());
  CHECK(strcmp(st.device_name, "Porch") == 0 && strcmp(st.local_address, "00:00:00:00:00:00") == 0);
  CHECK(st.tx_power == -3 && st.mtu == 185 && st.battery_pct == 100);
  CHECK(st.paired_count == 2 && st.scanned_count == 3);
  CHECK(memcmp(&st.connection, &bc::g_connection, sizeof st.connection) == 0);
  CHECK(memcmp(&st.pairing, &bc::g_pairing, sizeof st.pairing) == 0);
  CHECK(st.total_connections == 11 && st.total_bytes_sent == 22 && st.total_bytes_received == 33);
  CHECK(st.connected_time_ms == 44000 + (host_sim::now_ms - 777));
  const bc::BluetoothSettings s = bc::read_settings();
  CHECK(memcmp(&s, &bc::g_settings, sizeof s) == 0);
  bc::ScanView scan;
  bc::read_scan(&scan);
  CHECK(scan.scanning && scan.count == 3);
  CHECK(memcmp(scan.devices, bc::g_scanned_devices, 3 * sizeof(bc::ScannedDevice)) == 0);
  bc::PairedView paired;
  bc::read_paired(&paired);
  CHECK(paired.count == 2);
  CHECK(memcmp(paired.devices, bc::g_paired_devices, 2 * sizeof(bc::PairedDevice)) == 0);
  memset(&bc::g_connection, 0, sizeof bc::g_connection);
  memset(&bc::g_pairing, 0, sizeof bc::g_pairing);
  std::printf("PASS each_view_field_reaches_the_route\n");
}

// ── The settings' "enabled" (sweep F144) ────────────────────────────────

// The saved value of a bool NVS key ("bt_enabled").
bool nvs_bool(const char* key) {
  const auto it = host_sim::main_nvs.find(key);
  CHECK(it != host_sim::main_nvs.end() && it->second.size() == sizeof(bool));
  bool v = false;
  memcpy(&v, it->second.data(), sizeof v);
  return v;
}

std::vector<std::string> call_names() {
  std::vector<std::string> out;
  for (const host_sim::Call& c : host_sim::calls) out.push_back(c.what + "@" + c.task);
  return out;
}

// POST /api/bluetooth/settings {"enabled": false} turns Bluetooth off as
// POST /api/bluetooth/disable does: advertising and a scan stop, a link is
// dropped, the state reads disabled and the setting is saved, all on the
// loop task. Before F144 set_settings() compared the new value with
// is_enabled() after assigning it, so it stored false and left the radio
// running until the next boot.
void test_settings_enabled_false_turns_bluetooth_off() {
  boot();
  bc::Command scan = cmd_of(bc::BT_CMD_SCAN_START);
  scan.duration_ms = 5000;
  CHECK(rest(scan).r.ok && bc::g_scanning);
  CHECK(host_sim::advertising.isAdvertising());
  host_sim::calls.clear();
  bc::Command off = cmd_of(bc::BT_CMD_SETTINGS);
  off.settings.enabled = false;
  off.set_mask = bc::BT_SET_ENABLED;
  Rest r = rest(off);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && r.r.refusal == bc::BT_REFUSED_NONE);
  CHECK(r.calls_before_turn == 0);
  CHECK(host_sim::count("adv_stop", "loop") == 1 && !host_sim::advertising.isAdvertising());
  CHECK(host_sim::count("scan_stop", "loop") == 1 && !bc::g_scanning);
  CHECK(!bc::g_settings.enabled && bc::g_state == bc::BT_DISABLED);
  CHECK(!nvs_bool("bt_enabled"));
  bc::update();                                             // a later pass starts nothing
  CHECK(!host_sim::advertising.isAdvertising() && bc::g_state == bc::BT_DISABLED);
  none_on_httpd();

  // A phone connected: the same POST drops the link.
  boot();
  NimBLEConnInfo phone = link(11, 0xD1);
  host_sim::server->peers = {11};
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  bc::update();                                             // the loop task's pass
  CHECK(bc::g_connection.connected);
  host_sim::calls.clear();
  r = rest(off);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(host_sim::count("disconnect", "loop") == 1);
  CHECK(!bc::g_settings.enabled && bc::g_state == bc::BT_DISABLED);
  none_on_httpd();
  std::printf("PASS settings_enabled_false_turns_bluetooth_off\n");
}

// {"enabled": true} turns Bluetooth on the way POST /api/bluetooth/enable
// does: the same state, the same calls, the setting saved; with the stack
// down it is refused like an enable and applies nothing, since a command
// never brings the stack up (the handler's bring_up() does, first, as the
// enable handler's does: check_wap_loop_commands.py rule C3).
void test_settings_enabled_true_turns_bluetooth_on_as_enable_does() {
  // POST /api/bluetooth/enable, for comparison.
  boot();
  CHECK(rest(cmd_of(bc::BT_CMD_DISABLE)).r.ok && bc::g_state == bc::BT_DISABLED);
  host_sim::calls.clear();
  Rest r = rest(cmd_of(bc::BT_CMD_ENABLE));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  const bc::BluetoothState state_by_enable = bc::g_state;
  const std::vector<std::string> calls_by_enable = call_names();
  CHECK(state_by_enable == bc::BT_IDLE);

  boot();
  CHECK(rest(cmd_of(bc::BT_CMD_DISABLE)).r.ok && bc::g_state == bc::BT_DISABLED);
  host_sim::calls.clear();
  bc::Command on = cmd_of(bc::BT_CMD_SETTINGS);
  on.settings.enabled = true;
  on.set_mask = bc::BT_SET_ENABLED;
  r = rest(on);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && r.r.refusal == bc::BT_REFUSED_NONE);
  CHECK(r.state_before_turn == bc::BT_DISABLED && r.calls_before_turn == 0);
  CHECK(bc::g_settings.enabled && bc::g_state == state_by_enable);
  CHECK(call_names() == calls_by_enable);
  CHECK(nvs_bool("bt_enabled"));
  CHECK(host_sim::count("nimble_init") == 0);
  r = rest(cmd_of(bc::BT_CMD_ADVERTISE_START));             // on, as after an enable
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && host_sim::advertising.isAdvertising());
  none_on_httpd();

  // The stack never came up: refused, and none of the POST's fields applied.
  boot(/*bring_up=*/false);
  bc::g_settings.enabled = false;
  on.settings.allow_pairing = !bc::g_settings.allow_pairing;
  on.set_mask = bc::BT_SET_ENABLED | bc::BT_SET_ALLOW_PAIRING;
  const bc::BluetoothSettings before = bc::g_settings;
  r = rest(on);
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && r.r.refusal == bc::BT_REFUSED_NOT_ENABLED);
  CHECK(bc::g_settings.enabled == before.enabled && !bc::g_settings.enabled);
  CHECK(bc::g_settings.allow_pairing == before.allow_pairing);
  CHECK(bc::g_state == bc::BT_DISABLED);
  CHECK(host_sim::count("nimble_init") == 0 && host_sim::count("nvs_write") == 0);
  std::printf("PASS settings_enabled_true_turns_bluetooth_on_as_enable_does\n");
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
    {"a_callback_changes_nothing_until_the_loop_task_applies_it",
     test_a_callback_changes_nothing_until_the_loop_task_applies_it},
    {"the_pending_pairing_has_one_owner", test_the_pending_pairing_has_one_owner},
    {"a_full_queue_keeps_the_links_room_and_fails_a_pairing_closed",
     test_a_full_queue_keeps_the_links_room_and_fails_a_pairing_closed},
    {"a_link_that_ends_after_bluetooth_is_off_leaves_it_off",
     test_a_link_that_ends_after_bluetooth_is_off_leaves_it_off},
    {"a_scan_end_is_applied_once", test_a_scan_end_is_applied_once},
    {"threads_callbacks_loop_and_commands", test_threads_callbacks_loop_and_commands},
    {"a_route_reads_the_last_published_pass", test_a_route_reads_the_last_published_pass},
    {"a_read_right_after_a_post_shows_what_it_did", test_a_read_right_after_a_post_shows_what_it_did},
    {"reads_before_the_first_pass_show_the_boot_state", test_reads_before_the_first_pass_show_the_boot_state},
    {"the_status_read_counts_the_times_to_now", test_the_status_read_counts_the_times_to_now},
    {"each_view_field_reaches_the_route", test_each_view_field_reaches_the_route},
    {"settings_enabled_false_turns_bluetooth_off", test_settings_enabled_false_turns_bluetooth_off},
    {"settings_enabled_true_turns_bluetooth_on_as_enable_does",
     test_settings_enabled_true_turns_bluetooth_on_as_enable_does},
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
