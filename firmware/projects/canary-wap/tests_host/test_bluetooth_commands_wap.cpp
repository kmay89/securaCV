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
// And the review of F143: a pairing awaiting the owner's answer ends with
// its link, and an answer goes only to the link the stack still holds on
// that handle (a reused handle never takes another phone's yes); turning
// Bluetooth off ends a pairing first; a link's event carries the stack's
// ble_addr_t whole, so its name prints as NimBLE prints the phone's address
// (the stand-in's byte-array constructor reverses, as NimBLE-Arduino 2.5.0's
// does); GATT activity and scan results are posted under the lossy limit
// and their drops are logged apart from a link's; and each field an event
// carries reaches the state it did.
//
// And the FULL profile (sweep F171): NimBLE keeps one callbacks pointer
// per server, and ble_opera::init() replaced the channel's with its own, so
// the library's default answered every Numeric Comparison yes. Both now
// install through one dispatcher (ble_server_dispatch.h); the FULL tests
// build both inits (Opera's real header) over the stand-in, whose default
// server callbacks answer as NimBLE-Arduino 2.5.0's do, in either order.
//
// And the review of F172: NimBLE refuses to forget a bond that carries the
// phone's IRK while it advertises or scans (ble_gap_unpair(): BLE_HS_EBUSY),
// and the WAP nearly always does one or the other; Remove threw the answer
// away. The stand-in's deleteBond() now answers as ble_gap_unpair() does
// (the presence loop's scan counts as a discovery), and Remove and Clear
// delete with the radio quiet and keep an entry whose bond the stack kept.
// No health-log line names a peer's address; only the recorded link's
// events label the connection card; the inactivity timeout drops only the
// recorded link.
//
// Host-tested only: the stand-in is not NimBLE, and nothing here runs a
// radio; the Arduino compile is CI's (firmware.yml's canary-wap legs).
//
// Run: ./test_bluetooth_commands_wap [name]

#ifndef BLUETOOTH_CHANNEL_CPP
#error "BLUETOOTH_CHANNEL_CPP (absolute path to bluetooth_channel.cpp) must be defined"
#endif

#include <algorithm>
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
// The FULL profile's other owner of the server's callbacks (sweep F171):
// Opera, which ble_manager::init() brings up after the channel on the boot
// worker. Its real header, over the same stand-in.
#include "ble_opera.h"
#pragma GCC diagnostic pop

#include "http_status_line.h"   // the status line a Bluetooth not-run answer sends

// What the sketch links in on a device (canary_wap.ino and the BLE modules).
// The health log keeps each line's message, detail, level and the task that
// wrote it; the presence sensor's calls are recorded with their task too, so
// a test can see that none is the NimBLE host task's (sweep F143).
std::vector<std::string> g_health;
struct HealthLine {
  LogLevel level;
  std::string message;
  std::string detail;
  std::string task;
};
std::vector<HealthLine> g_health_lines;
std::mutex g_health_mu;   // the device's health log takes its own lock
void log_health(LogLevel level, LogCategory, const char* message, const char* detail) {
  std::lock_guard<std::mutex> g(g_health_mu);
  g_health.push_back(message);
  g_health_lines.push_back({level, message, detail ? detail : "", host_sim::task});
}
namespace ble_ota {
bool init(NimBLEServer*, const uint8_t[32]) { return true; }
}  // namespace ble_ota
namespace host_sim {
inline std::mutex presence_mu;
inline std::vector<Call> presence_calls;   // ble_presence's, with their task
inline void presence(const char* what) {
  std::lock_guard<std::mutex> g(presence_mu);
  presence_calls.push_back({what, task});
}
}  // namespace host_sim
// The presence loop's scan runs on the NimBLE scanner from the channel's
// init() on, without end, except while paused for the owner's scan (or, since
// the F172 review, a bond's delete): host_sim::presence_disc, which the
// stand-in's deleteBond() reads as ble_gap_disc_active() would.
namespace host_sim {
inline std::atomic<bool> presence_paused{false};
}  // namespace host_sim
namespace ble_presence {
bool init() { return true; }
void deinit() { host_sim::presence_disc = false; }
bool start() {
  if (host_sim::presence_paused) return false;
  host_sim::presence_disc = true;
  return true;
}
void pause_for_user_scan() {
  host_sim::presence("pause");
  host_sim::presence_paused = true;
  host_sim::presence_disc = false;
}
void resume_continuous_scan() {
  host_sim::presence("resume");
  host_sim::presence_paused = false;
  start();
}
void notify_console_connected(bool on) { host_sim::presence(on ? "console_on" : "console_off"); }
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
// (the channel's keys and NimBLE's bonds) kept unless `wipe`. When `bring_up`, the boot worker runs the real init()
// ("bringup"), which turns Bluetooth on and advertises (the defaults).
void boot(bool bring_up = true, bool wipe = true) {
  if (wipe) host_sim::main_nvs.clear();
  NimBLEDevice::deinit(true);
  host_sim::advertising = NimBLEAdvertising();
  host_sim::scan = NimBLEScan();
  if (wipe) host_sim::bonds.clear();       // NimBLE's bond store is in NVS too
  if (wipe) host_sim::bond_irks.clear();
  host_sim::bonds_deleted.clear();
  host_sim::bonds_busy.clear();
  host_sim::before_unpair = nullptr;
  host_sim::presence_disc = false;         // the presence loop starts in init()
  host_sim::presence_paused = false;
  host_sim::passkey_answers.clear();
  host_sim::presence_calls.clear();
  g_health.clear();
  g_health_lines.clear();
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
  bc::g_paired_by_identity = false;
  memset(bc::g_scanned_devices, 0, sizeof bc::g_scanned_devices);
  bc::g_scanned_count = 0;
  bc::g_scanning = false;
  bc::g_commands = decltype(bc::g_commands)();
  bc::g_events.consume([](const bc::Event& e) {      // a copy a test left waiting
    if (e.type == bc::BT_EV_CONFIRM_PASSKEY) delete e.u.passkey.conn;
  });
  bc::g_events = decltype(bc::g_events)();
  bc::g_link_drops = {0, 0};
  bc::g_lossy_drops = {0, 0};
  bc::g_link_drops_reconciled = 0;
  bc::g_status_view = decltype(bc::g_status_view)();       // nothing published yet
  bc::g_scan_view = decltype(bc::g_scan_view)();
  bc::g_paired_view = decltype(bc::g_paired_view)();
  bc::g_local_address[0] = '\0';
  memcpy(bc::g_settings.device_name, bc::kDefaultSettings.device_name, sizeof bc::g_settings.device_name);
  bc::g_settings.inactivity_timeout_ms = bc::kDefaultSettings.inactivity_timeout_ms;
  bc::g_settings.notify_on_connect = bc::kDefaultSettings.notify_on_connect;
  // The server's callbacks' owners (sweep F171) and Opera's own flag, as at
  // power-up: no module has installed anything yet.
  ble_server_dispatch::g_dispatcher.set(ble_server_dispatch::kPairing, nullptr);
  ble_server_dispatch::g_dispatcher.set(ble_server_dispatch::kLink, nullptr);
  ble_opera::g_advertising = false;
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

// A phone's link on `handle`: its address prints "<last_byte>:22:33:44:55:66"
// (NimBLE's byte-array constructor takes the printed order), of `type`
// (0 public, 1 random).
NimBLEConnInfo link(uint16_t handle, uint8_t last_byte, uint8_t type = 0) {
  NimBLEConnInfo c;
  c.handle = handle;
  const uint8_t addr[6] = {last_byte, 0x22, 0x33, 0x44, 0x55, 0x66};
  c.address = NimBLEAddress(addr, type);
  return c;
}

// A phone pairing: the owner started pairing mode (over REST), the phone
// connected (the stack holds its link), and the stack asks to confirm `pin`
// (Numeric Comparison).
void pairing_awaiting_confirm(uint32_t pin, const NimBLEConnInfo& phone) {
  const Rest start = rest(cmd_of(bc::BT_CMD_PAIR_START));
  CHECK(start.wait == lcr::Wait::kDone && start.r.ok);
  host_sim::server->link_up(phone);
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

// The level and the detail of the last health-log line saying `message`.
LogLevel health_level(const char* message) {
  LogLevel level = SCV_LOG_TAMPER;
  for (const HealthLine& l : g_health_lines) {
    if (l.message == message) level = l.level;
  }
  return level;
}
std::string health_detail(const char* message) {
  std::string detail = "(none)";
  for (const HealthLine& l : g_health_lines) {
    if (l.message == message) detail = l.detail;
  }
  return detail;
}

// Whether any health-log line names `addr` (a peer's address, in either
// case). The log is durable (the SD card's /HEALTH, the BLE log export), and
// the no-identity invariant keeps a peer's address out of it (the F172
// review: "New device paired" logged the phone's identity address).
bool health_names(const std::string& addr) {
  std::string lo = addr, up = addr;
  for (char& ch : lo) ch = (char)tolower((unsigned char)ch);
  for (char& ch : up) ch = (char)toupper((unsigned char)ch);
  for (const HealthLine& l : g_health_lines) {
    for (const std::string* text : {&l.message, &l.detail}) {
      if (text->find(lo) != std::string::npos || text->find(up) != std::string::npos) return true;
    }
  }
  return false;
}

// Health-log lines and presence-sensor calls made on `task`.
size_t health_on(const char* task) {
  size_t n = 0;
  for (const HealthLine& l : g_health_lines) n += l.task == task;
  return n;
}
size_t presence_on(const char* task, const char* what = "") {
  size_t n = 0;
  for (const host_sim::Call& c : host_sim::presence_calls) {
    n += c.task == task && (what[0] == '\0' || c.what == what);
  }
  return n;
}

const char* const kLinkDrops = "BLE link events dropped (queue full)";
const char* const kLossyDrops = "BLE scan/activity events dropped (queue full)";

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
  // The advertising the disconnect restarted goes on (sweep F170: this read
  // idle, beside "advertising": true).
  CHECK(!bc::g_scanning && bc::g_state == bc::BT_ADVERTISING && host_sim::advertising.isAdvertising());
  CHECK(host_sim::count("", "nimble") == 0);
  // The health log's lines and the presence sensor's calls are the loop
  // task's too (a link up and down, the scan handed back).
  CHECK(health_on("nimble") == 0 && health_on("loop") > 0);
  CHECK(presence_on("nimble") == 0);
  CHECK(presence_on("loop", "console_on") == 1 && presence_on("loop", "console_off") == 1);
  CHECK(presence_on("loop", "resume") == 1);
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
  host_sim::server->link_up(phone);
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
  host_sim::server->link_up(phone);
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
// counted by kind, each count goes to the health log once a minute at most
// (a link's as a warning, the lower limit's at debug level), and a passkey
// to confirm that finds no room is answered no on the NimBLE host task
// (fails closed) with its copy deleted there.
void test_a_full_queue_keeps_the_links_room_and_fails_a_pairing_closed() {
  boot();
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok);
  bc::Command scan = cmd_of(bc::BT_CMD_SCAN_START);
  scan.duration_ms = 5000;
  CHECK(rest(scan).r.ok);
  NimBLEConnInfo phone = link(41, 0xA4);
  host_sim::server->peers = {41};
  host_sim::server->link_up(phone);                         // the stack holds the link
  // A burst of advertisements: twenty heard before the loop task's pass.
  for (uint8_t i = 0; i < 20; ++i) {
    NimBLEAdvertisedDevice d;
    const uint8_t a[6] = {i, 9, 9, 9, 9, 9};
    d.address = NimBLEAddress(a, 0);
    on_nimble([&] { host_sim::scan.callbacks()->onResult(&d); });
  }
  CHECK(bc::g_events.waiting() == bc::EVENT_LOSSY_LIMIT && bc::g_events.dropped() == 4);
  CHECK(bc::g_events.dropped_limited() == 4 && bc::g_events.dropped_reserved() == 0);
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
  CHECK(bc::g_events.dropped() == 5 && bc::g_events.dropped_reserved() == 1);
  CHECK(host_sim::passkey_answers.size() == 1 && !host_sim::passkey_answers[0].accept);
  CHECK(host_sim::passkey_answers[0].task == "nimble" && host_sim::passkey_answers[0].handle == 42);
  CHECK(host_sim::conn_heap == 1);                           // only the queued one's copy
  g_health.clear();
  loop_pass();
  CHECK(bc::g_events.waiting() == 0);
  CHECK(bc::g_scanned_count == bc::EVENT_LOSSY_LIMIT);       // the sixteen that fit
  CHECK(bc::g_connection.connected);                         // the link's events kept
  CHECK(bc::g_pairing.state == bc::PAIR_CONFIRMING && bc::g_pairing.pin_code == 555555);
  CHECK(health_says(kLinkDrops) == 1 && health_says(kLossyDrops) == 1);
  CHECK(health_level(kLinkDrops) == SCV_LOG_WARNING && health_level(kLossyDrops) == SCV_LOG_DEBUG);
  // More drops within the minute: counted, not logged again; after it, logged.
  for (int i = 0; i < 30; ++i) {
    on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0); });
  }
  loop_pass();
  CHECK(health_says(kLinkDrops) == 1);
  host_sim::now_ms += 60000;
  loop_pass();
  CHECK(health_says(kLinkDrops) == 2 && health_says(kLossyDrops) == 1);
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

// ── Review of F143: a link's address, its pairing, the queue's kinds ────

// The GATT characteristics' callbacks, as the stack calls them.
NimBLECharacteristicCallbacks* char_cb() { return &bc::g_char_callbacks; }

// A link is named by its address as NimBLE prints it: the connection's
// name, and a newly paired phone's (saved, and shown by GET /paired), are
// the phone's getAddress().toString(). The link's event carries the
// stack's own ble_addr_t; bytes copied out of getBase() and put back
// through NimBLE-Arduino 2.x's byte-array constructor (which takes the
// printed order and reverses it) printed the name backwards. And the
// address type (a random address, as phones use) reaches the paired list
// and the bond delete.
void test_a_link_is_named_by_its_address() {
  boot();
  NimBLEConnInfo phone = link(91, 0xA1, /*type=*/1);
  phone.encrypted = phone.authenticated = phone.bonded = true;
  const std::string printed = phone.getAddress().toString();
  CHECK(printed == "a1:22:33:44:55:66");                    // as NimBLE prints it
  host_sim::server->peers = {91};
  host_sim::server->link_up(phone);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(phone); });
  loop_pass();
  CHECK(strcmp(bc::g_connection.name, printed.c_str()) == 0);
  CHECK(memcmp(bc::g_connection.address, phone.getAddress().getBase()->val, 6) == 0);
  CHECK(bc::g_paired_count == 1);
  CHECK(strcmp(bc::g_paired_devices[0].name, printed.c_str()) == 0);
  CHECK(memcmp(bc::g_paired_devices[0].address, phone.getAddress().getBase()->val, 6) == 0);
  CHECK(bc::g_paired_devices[0].address_type == 1);
  CHECK(health_says("New device paired") == 1 && health_detail("New device paired").empty());
  CHECK(health_says("BLE device connected") == 1 && !health_names(printed));   // no address logged
  bc::BluetoothStatus st;
  bc::read_status(&st);
  CHECK(strcmp(st.connection.name, printed.c_str()) == 0);
  bc::PairedView paired;
  bc::read_paired(&paired);
  CHECK(paired.count == 1 && strcmp(paired.devices[0].name, printed.c_str()) == 0);
  CHECK(paired.devices[0].address_type == 1);
  bc::Command remove = cmd_of(bc::BT_CMD_PAIRED_REMOVE);
  memcpy(remove.address, phone.getAddress().getBase()->val, 6);
  CHECK(rest(remove).r.ok);
  CHECK(host_sim::bonds_deleted.size() == 1 && host_sim::bonds_deleted[0].getType() == 1);
  std::printf("PASS a_link_is_named_by_its_address\n");
}

// A pairing awaiting the owner's answer ends with its link: when the phone
// walks away mid-confirm, the loop task applies the link's end, the
// pending copy goes unanswered (there is no link to answer), the pairing
// reads failed and stops showing the dead link's digits, and a confirm of
// them afterwards answers nothing. Before, the copy and the confirming
// state outlived the link, and the confirm answered yes into its handle.
void test_a_pairing_ends_with_its_link() {
  boot();
  NimBLEConnInfo a = link(7, 0xA1);
  host_sim::server->peers = {7};
  host_sim::server->link_up(a);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), a); });
  loop_pass();
  pairing_awaiting_confirm(482913, a);
  host_sim::server->link_down(7);                           // the stack ends it, then says so
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), a, 0x13); });
  loop_pass();
  CHECK(bc::g_pairing.state == bc::PAIR_FAILED);
  CHECK(!bc::g_pending_pair_active && bc::g_pending_pair_info == nullptr);
  CHECK(host_sim::conn_heap == 0 && host_sim::passkey_answers.empty());
  CHECK(health_says("Pairing link lost before confirmation") == 1);
  bc::BluetoothStatus st;
  bc::read_status(&st);
  CHECK(st.pairing.state == bc::PAIR_FAILED && !st.pairing.pin_displayed && !st.connected);
  bc::Command c = cmd_of(bc::BT_CMD_PAIR_CONFIRM);
  c.pin = 482913;
  const Rest r = rest(c);
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);
  CHECK(host_sim::passkey_answers.empty());
  none_on_httpd();

  // The phone is back on the same handle before the loop task applies the
  // end of its old link (it reconnected at once and pairs afresh): the old
  // copy still goes unanswered, so no stale no lands on the new pairing.
  boot();
  host_sim::server->peers = {7};
  pairing_awaiting_confirm(482913, a);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), a, 0x13); });
  host_sim::server->link_up(a);                             // the same phone, the same handle, again
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), a); });
  loop_pass();
  CHECK(host_sim::passkey_answers.empty() && host_sim::conn_heap == 0);
  CHECK(bc::g_pairing.state == bc::PAIR_FAILED && bc::g_connection.connected);
  std::printf("PASS a_pairing_ends_with_its_link\n");
}

// Phone A walks away mid-confirm and phone B connects on the handle A had;
// B's stack asks its own Numeric Comparison while the loop task is
// applying B's link (so B's passkey waits for the next pass). The owner's
// confirm of A's six digits, drained in that same pass, must not answer
// yes on the handle B now holds: B's digits were never shown. They are, on
// the next pass, and the owner's confirm of them goes to B.
NimBLEConnInfo g_phone_b;
bool g_b_asks_on_connect = false;
void b_asks_when_it_connects(const bc::ConnectionInfo*, bool connected) {
  if (!connected || !g_b_asks_on_connect) return;
  g_b_asks_on_connect = false;
  on_nimble([] { host_sim::server->callbacks()->onConfirmPassKey(g_phone_b, 999999); });
}
void test_a_reused_handle_never_takes_the_old_yes() {
  boot();
  NimBLEConnInfo a = link(7, 0xA1);
  host_sim::server->peers = {7};
  pairing_awaiting_confirm(482913, a);
  g_phone_b = link(7, 0xBB);
  bc::set_connection_callback(b_asks_when_it_connects);
  host_sim::server->link_down(7);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), a, 0x13); });
  host_sim::server->link_up(g_phone_b);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), g_phone_b); });
  g_b_asks_on_connect = true;
  bc::Command c = cmd_of(bc::BT_CMD_PAIR_CONFIRM);
  c.pin = 482913;
  Rest r = rest(c);                                         // A's end, B's link, then the confirm
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);
  CHECK(host_sim::passkey_answers.empty());
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 7);
  loop_pass();                                              // B's own digits
  CHECK(bc::g_pairing.state == bc::PAIR_CONFIRMING && bc::g_pairing.pin_code == 999999);
  CHECK(bc::g_pending_pair_info != nullptr && bc::g_pending_pair_info->getAddress() == g_phone_b.getAddress());
  bc::set_connection_callback(nullptr);
  c.pin = 999999;
  r = rest(c);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(host_sim::passkey_answers.size() == 1 && host_sim::passkey_answers[0].accept);
  CHECK(host_sim::passkey_answers[0].handle == 7 && host_sim::passkey_answers[0].task == "loop");
  CHECK(host_sim::conn_heap == 0);
  std::printf("PASS a_reused_handle_never_takes_the_old_yes\n");
}

// The answer goes only to the link the digits came from, as the stack
// holds it now: when A's end never reached the loop task (its event lost
// to a full queue) and B took the handle, or the handle has no link at
// all, a confirm (or a no) answers nothing, the copy is deleted and the
// pairing reads failed.
void test_an_answer_goes_only_to_its_own_link() {
  boot();
  pairing_awaiting_confirm(482913, link(7, 0xA1));
  host_sim::server->link_up(link(7, 0xBB));                 // B on A's handle; no event for A's end
  bc::Command c = cmd_of(bc::BT_CMD_PAIR_CONFIRM);
  c.pin = 482913;
  Rest r = rest(c);
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);
  CHECK(host_sim::passkey_answers.empty());
  CHECK(bc::g_pending_pair_info == nullptr && !bc::g_pending_pair_active && host_sim::conn_heap == 0);
  CHECK(bc::g_pairing.state == bc::PAIR_FAILED && !bc::g_pairing.user_confirmed);
  CHECK(health_says("Pairing link gone — confirmation not sent") == 1);

  boot();
  pairing_awaiting_confirm(111222, link(9, 0xB2));
  host_sim::server->link_down(9);                           // no link on the handle at all
  c.pin = 111222;
  r = rest(c);
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && host_sim::passkey_answers.empty());

  boot();
  pairing_awaiting_confirm(333444, link(5, 0xC5));
  host_sim::server->link_down(5);
  r = rest(cmd_of(bc::BT_CMD_PAIR_REJECT));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && host_sim::passkey_answers.empty());
  CHECK(host_sim::conn_heap == 0);
  std::printf("PASS an_answer_goes_only_to_its_own_link\n");
}

// Turning Bluetooth off ends a pairing awaiting the owner: the pending
// Numeric Comparison is answered no while its link is still up (before the
// link is dropped), the pairing reads none, and a disabled channel's passes
// leave nothing pending. Before, the copy and the confirming state stayed
// (update() returns early while disabled) until Bluetooth came back on.
void test_turning_bluetooth_off_ends_a_pairing() {
  for (const bool by_settings : {true, false}) {
    boot();
    NimBLEConnInfo a = link(13, 0xE1);
    host_sim::server->peers = {13};
    host_sim::server->link_up(a);
    on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), a); });
    loop_pass();
    pairing_awaiting_confirm(222333, a);
    host_sim::calls.clear();
    bc::Command off = cmd_of(bc::BT_CMD_SETTINGS);
    off.set_mask = bc::BT_SET_ENABLED;                      // enabled: false
    const Rest r = rest(by_settings ? off : cmd_of(bc::BT_CMD_DISABLE));
    CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
    CHECK(host_sim::passkey_answers.size() == 1 && !host_sim::passkey_answers[0].accept);
    CHECK(host_sim::passkey_answers[0].handle == 13 && host_sim::passkey_answers[0].task == "loop");
    CHECK(bc::g_pairing.state == bc::PAIR_NONE && !bc::g_pending_pair_active);
    CHECK(bc::g_pending_pair_info == nullptr && host_sim::conn_heap == 0);
    std::vector<std::string> order;
    for (const host_sim::Call& call : host_sim::calls) order.push_back(call.what);
    const auto answered = std::find(order.begin(), order.end(), "passkey_answer");
    const auto dropped = std::find(order.begin(), order.end(), "disconnect");
    CHECK(answered != order.end() && dropped != order.end() && answered < dropped);
    CHECK(bc::g_state == bc::BT_DISABLED);
    host_sim::server->link_down(13);
    on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), a, 0x16); });
    host_sim::now_ms += 10 * bc::PAIRING_TIMEOUT_MS;
    loop_pass();
    loop_pass();
    CHECK(host_sim::passkey_answers.size() == 1 && bc::g_state == bc::BT_DISABLED);
    CHECK(bc::g_pairing.state == bc::PAIR_NONE);
  }
  std::printf("PASS turning_bluetooth_off_ends_a_pairing\n");
}

// GATT activity is posted under EVENT_LOSSY_LIMIT, as scan results are: a
// burst of writes, then of reads, with no pass between fills the queue to
// the limit and no further, and a link's own events after each burst (a
// link down, a link up, a passkey) are all kept, none dropped.
void test_gatt_activity_never_takes_the_links_room() {
  boot();
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok);
  NimBLEConnInfo phone = link(44, 0xA4);
  host_sim::server->peers = {44};
  host_sim::server->link_up(phone);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  loop_pass();
  const uint32_t total_before = bc::g_total_bytes_received;
  bc::g_command_char->setValue((const uint8_t*)"hello", 5);
  for (int i = 0; i < 20; ++i) {
    on_nimble([&] { char_cb()->onWrite(bc::g_command_char, phone); });
  }
  CHECK(bc::g_events.waiting() == bc::EVENT_LOSSY_LIMIT);
  CHECK(bc::g_events.dropped_limited() == 4 && bc::g_events.dropped_reserved() == 0);
  NimBLEConnInfo next = link(45, 0xA5);
  host_sim::server->link_down(44);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  host_sim::server->link_up(next);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), next); });
  on_nimble([&] { host_sim::server->callbacks()->onConfirmPassKey(next, 777777); });
  CHECK(bc::g_events.waiting() == bc::EVENT_LOSSY_LIMIT + 3);
  CHECK(bc::g_events.dropped_reserved() == 0 && host_sim::passkey_answers.empty());
  loop_pass();
  CHECK(bc::g_total_bytes_received == total_before + 16 * 5);    // the sixteen that fit
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 45);
  CHECK(bc::g_pairing.state == bc::PAIR_CONFIRMING && bc::g_pairing.pin_code == 777777);

  for (int i = 0; i < 20; ++i) {
    on_nimble([&] { char_cb()->onRead(bc::g_status_char, next); });
  }
  CHECK(bc::g_events.waiting() == bc::EVENT_LOSSY_LIMIT);
  CHECK(bc::g_events.dropped_limited() == 8 && bc::g_events.dropped_reserved() == 0);
  host_sim::server->link_down(45);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), next, 0x13); });
  CHECK(bc::g_events.waiting() == bc::EVENT_LOSSY_LIMIT + 1 && bc::g_events.dropped_reserved() == 0);
  loop_pass();
  CHECK(!bc::g_connection.connected && bc::g_pairing.state == bc::PAIR_FAILED);
  CHECK(host_sim::conn_heap == 0);
  std::printf("PASS gatt_activity_never_takes_the_links_room\n");
}

// The drops are logged by kind: a busy room advertising through a stall
// (scan results the lower limit refused) is a debug line, never the
// warning; a link's event that found no room is the warning (the
// connection state may now be stale), with the count.
void test_a_busy_room_is_no_warning() {
  boot();
  bc::Command scan = cmd_of(bc::BT_CMD_SCAN_START);
  scan.duration_ms = 5000;
  CHECK(rest(scan).r.ok);
  for (int i = 0; i < 40; ++i) {
    NimBLEAdvertisedDevice d;
    const uint8_t a[6] = {(uint8_t)(i % 3), 9, 9, 9, 9, 9};  // three devices, heard over and over
    d.address = NimBLEAddress(a, 0);
    on_nimble([&] { host_sim::scan.callbacks()->onResult(&d); });
  }
  loop_pass();
  CHECK(bc::g_scanned_count == 3);
  CHECK(health_says(kLinkDrops) == 0);
  CHECK(health_says(kLossyDrops) == 1 && health_level(kLossyDrops) == SCV_LOG_DEBUG);
  CHECK(health_detail(kLossyDrops) == "24 since boot");
  NimBLEConnInfo phone = link(46, 0xA6);
  while (bc::g_events.waiting() < bc::EVENT_SLOTS) {
    on_nimble([&] { host_sim::scan.callbacks()->onScanEnd(NimBLEScanResults(), 0); });
  }
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  CHECK(bc::g_events.dropped_reserved() == 1);
  host_sim::now_ms += 60000;
  loop_pass();
  CHECK(health_says(kLinkDrops) == 1 && health_level(kLinkDrops) == SCV_LOG_WARNING);
  CHECK(health_detail(kLinkDrops) == "1 since boot");
  CHECK(health_says(kLossyDrops) == 1);                     // nothing new of that kind
  std::printf("PASS a_busy_room_is_no_warning\n");
}

// What the stack said of a link's security reaches the connection and the
// paired list as it said it: encrypted only, authenticated, none; a bond
// that completes authenticated but unbonded is no paired device (nothing
// saved); one that fails authentication is a failed pairing.
void test_a_links_security_is_what_the_stack_said() {
  boot();
  host_sim::server->peers = {51};
  NimBLEConnInfo enc = link(51, 0xB1);
  enc.encrypted = true;
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), enc); });
  loop_pass();
  CHECK(bc::g_connection.security == bc::SEC_ENCRYPTED);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), enc, 0x13); });
  NimBLEConnInfo plain = link(52, 0xB2);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), plain); });
  loop_pass();
  CHECK(bc::g_connection.security == bc::SEC_NONE);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), plain, 0x13); });
  NimBLEConnInfo auth = link(53, 0xB3);
  auth.encrypted = auth.authenticated = true;
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), auth); });
  loop_pass();
  CHECK(bc::g_connection.security == bc::SEC_AUTHENTICATED);
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok);          // the owner's pairing mode: no timeout due
  host_sim::calls.clear();
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(auth); });   // not bonded
  loop_pass();
  CHECK(bc::g_connection.security == bc::SEC_AUTHENTICATED && bc::g_paired_count == 0);
  CHECK(host_sim::main_nvs.count("bt_paired") == 0);
  CHECK(bc::g_pairing.state == bc::PAIR_COMPLETE);
  NimBLEConnInfo failed = auth;
  failed.authenticated = false;
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(failed); });
  loop_pass();
  CHECK(bc::g_pairing.state == bc::PAIR_FAILED && health_says("Pairing failed") == 1);
  CHECK(bc::g_paired_count == 0);
  std::printf("PASS a_links_security_is_what_the_stack_said\n");
}

// A GATT write counts its bytes (the link's and the totals) and, like a
// read, marks the link active when the stack saw it, so the inactivity
// timeout counts from there, not from the link's start or the pass.
void test_gatt_activity_counts_and_keeps_a_link() {
  boot();
  bc::g_settings.inactivity_timeout_ms = 60000;
  NimBLEConnInfo phone = link(54, 0xB4);
  host_sim::server->peers = {54};
  const uint32_t t0 = host_sim::now_ms;
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  loop_pass();
  CHECK(bc::g_connection.last_activity_ms == t0);
  const uint32_t total_before = bc::g_total_bytes_received;
  host_sim::now_ms = t0 + 59000;
  bc::g_command_char->setValue((const uint8_t*)"1234567", 7);
  on_nimble([&] { char_cb()->onWrite(bc::g_command_char, phone); });
  host_sim::now_ms = t0 + 59500;                            // the pass comes later
  loop_pass();
  CHECK(bc::g_connection.bytes_received == 7 && bc::g_total_bytes_received == total_before + 7);
  CHECK(bc::g_connection.last_activity_ms == t0 + 59000);
  host_sim::now_ms = t0 + 59000 + 30000;
  on_nimble([&] { char_cb()->onRead(bc::g_status_char, phone); });
  host_sim::now_ms = t0 + 59000 + 30500;
  loop_pass();
  CHECK(bc::g_connection.last_activity_ms == t0 + 89000 && bc::g_connection.bytes_received == 7);
  host_sim::calls.clear();
  host_sim::now_ms = t0 + 89000 + 59000;
  loop_pass();
  CHECK(host_sim::count("disconnect") == 0);                // a minute since the read: not yet
  host_sim::now_ms = t0 + 89000 + 60000;
  loop_pass();
  CHECK(host_sim::count("disconnect", "loop") == 1);
  std::printf("PASS gatt_activity_counts_and_keeps_a_link\n");
}

// A passkey the stack asks this device to show (onPassKeyDisplay, the
// passkey-entry flow) reaches the owner on the loop task's next pass: the
// pairing reads pin_displayed with the digits the callback returned.
void test_a_passkey_to_show_reaches_the_owner() {
  boot();
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok);
  uint32_t shown = 0;
  on_nimble([&] { shown = host_sim::server->callbacks()->onPassKeyDisplay(); });
  CHECK(shown < 1000000);
  CHECK(bc::g_pairing.state == bc::PAIR_INITIATED);         // nothing yet
  loop_pass();
  CHECK(bc::g_pairing.state == bc::PAIR_PIN_DISPLAYED && bc::g_pairing.pin_code == shown);
  CHECK(bc::g_pairing.pin_displayed);
  bc::BluetoothStatus st;
  bc::read_status(&st);
  CHECK(st.pairing.state == bc::PAIR_PIN_DISPLAYED && st.pairing.pin_code == shown);
  char six[16];
  snprintf(six, sizeof six, "%06lu", (unsigned long)shown);
  CHECK(health_detail("Pairing PIN displayed") == six);
  CHECK(health_on("nimble") == 0);
  std::printf("PASS a_passkey_to_show_reaches_the_owner\n");
}

// A scan result keeps what its advertisement said: connectable or not,
// the SecuraCV service or not (and so its type), its name and RSSI.
void test_a_scan_result_keeps_what_the_advertisement_said() {
  boot();
  bc::Command scan = cmd_of(bc::BT_CMD_SCAN_START);
  scan.duration_ms = 5000;
  CHECK(rest(scan).r.ok);
  NimBLEAdvertisedDevice canary;
  const uint8_t ca[6] = {0xC1, 1, 1, 1, 1, 1};
  canary.address = NimBLEAddress(ca, 0);
  canary.connectable = false;
  canary.services = {NimBLEUUID(bc::SERVICE_UUID)};
  canary.rssi = -71;
  NimBLEAdvertisedDevice pixel;
  const uint8_t pa[6] = {0xC2, 2, 2, 2, 2, 2};
  pixel.address = NimBLEAddress(pa, 0);
  pixel.name = "Pixel 8";
  pixel.rssi = -48;
  on_nimble([&] { host_sim::scan.callbacks()->onResult(&canary); });
  on_nimble([&] { host_sim::scan.callbacks()->onResult(&pixel); });
  loop_pass();
  CHECK(bc::g_scanned_count == 2);
  const bc::ScannedDevice& c = bc::g_scanned_devices[0];
  const bc::ScannedDevice& p = bc::g_scanned_devices[1];
  CHECK(memcmp(c.address, canary.address.getBase()->val, 6) == 0);
  CHECK(!c.connectable && c.has_securacv_service && c.type == bc::DEV_SECURACV);
  CHECK(c.rssi == -71 && c.name[0] == '\0');
  CHECK(p.connectable && !p.has_securacv_service && p.type == bc::DEV_PHONE);
  CHECK(p.rssi == -48 && strcmp(p.name, "Pixel 8") == 0);
  bc::ScanView view;
  bc::read_scan(&view);
  CHECK(view.count == 2 && memcmp(view.devices, bc::g_scanned_devices, 2 * sizeof(bc::ScannedDevice)) == 0);
  std::printf("PASS a_scan_result_keeps_what_the_advertisement_said\n");
}

// The times a link, a bond, a re-bond and a re-heard device carry are the
// callback's (the stack's moment), not the pass's that applies them; a
// link's end counts its time from them, and the health log carries the
// stack's disconnect reason.
void test_times_are_the_callbacks() {
  boot();
  NimBLEConnInfo phone = link(55, 0xB5);
  phone.encrypted = phone.authenticated = phone.bonded = true;
  host_sim::server->peers = {55};
  host_sim::now_ms = 400000;
  const uint32_t t0 = host_sim::now_ms;
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  host_sim::now_ms += 300;
  loop_pass();
  const uint32_t t1 = host_sim::now_ms;
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(phone); });
  host_sim::now_ms += 700;
  loop_pass();
  CHECK(bc::g_paired_count == 1 && bc::g_paired_devices[0].last_connected_ms == t1);
  CHECK(bc::g_paired_devices[0].paired_timestamp == t1 / 1000);
  const uint32_t t2 = host_sim::now_ms;
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(phone); });   // a re-bond
  host_sim::now_ms += 900;
  loop_pass();
  CHECK(bc::g_paired_devices[0].last_connected_ms == t2 && bc::g_paired_devices[0].connection_count == 2);
  const uint32_t connected_before = bc::g_connected_total_ms;
  host_sim::now_ms = t0 + 5000;
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  host_sim::now_ms = t0 + 9000;
  loop_pass();
  CHECK(bc::g_connected_total_ms == connected_before + 5000);
  CHECK(health_detail("BLE device disconnected") == "Duration: 5s, Reason: 19");

  bc::Command scan = cmd_of(bc::BT_CMD_SCAN_START);
  scan.duration_ms = 60000;
  CHECK(rest(scan).r.ok);
  NimBLEAdvertisedDevice d;
  const uint8_t a[6] = {0xD1, 3, 3, 3, 3, 3};
  d.address = NimBLEAddress(a, 0);
  const uint32_t t3 = host_sim::now_ms;
  on_nimble([&] { host_sim::scan.callbacks()->onResult(&d); });
  host_sim::now_ms += 100;
  loop_pass();
  CHECK(bc::g_scanned_count == 1 && bc::g_scanned_devices[0].last_seen_ms == t3);
  const uint32_t t4 = host_sim::now_ms;
  d.rssi = -40;
  on_nimble([&] { host_sim::scan.callbacks()->onResult(&d); });   // heard again
  host_sim::now_ms += 400;
  loop_pass();
  CHECK(bc::g_scanned_devices[0].last_seen_ms == t4 && bc::g_scanned_devices[0].rssi == -40);
  std::printf("PASS times_are_the_callbacks\n");
}

// A link's connection-parameter request, and in long-range mode its LE
// Coded PHY request, go out from the loop task when it applies the link's
// connect (they read the settings there), and the PHY request only in
// long-range mode.
void test_a_links_requests_go_out_from_the_loop_task() {
  boot();
  host_sim::server->peers = {56};
  NimBLEConnInfo phone = link(56, 0xB6);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  CHECK(host_sim::count("conn_params") == 0);
  loop_pass();
  CHECK(host_sim::count("conn_params", "loop") == 1 && host_sim::count("conn_params") == 1);
  CHECK(host_sim::count("le_phy") == 0);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  loop_pass();
  bc::Command lr = cmd_of(bc::BT_CMD_SETTINGS);
  lr.settings.long_range_mode = true;
  lr.set_mask = bc::BT_SET_LONG_RANGE;
  CHECK(rest(lr).r.ok && bc::g_settings.long_range_mode);
  host_sim::calls.clear();
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  loop_pass();
  CHECK(host_sim::count("conn_params", "loop") == 1 && host_sim::count("le_phy", "loop") == 1);
  CHECK(host_sim::count("le_phy") == 1);
  std::printf("PASS a_links_requests_go_out_from_the_loop_task\n");
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
      host_sim::server->link_up(phone);                      // the stack's own record
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
      host_sim::server->link_down(61);
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

// ── The FULL profile: one server, two owners of its callbacks (F171) ────

// What ble_manager::init() hands Opera (the device id hash, the firmware
// version, the chain height and head it reads live).
uint32_t g_opera_chain_height = 7;
uint8_t g_opera_chain_head[32] = {0xAB};

// ble_manager::init()'s Opera step, on the boot worker, as the FULL profile
// runs it (after the channel's init(), unless a REST handler's bring_up()
// ran the channel's later); then the loop task's finalize starts Opera's
// advertising (ble_manager::operaStart()).
void opera_init() {
  host_sim::task = "bringup";
  CHECK(ble_opera::init("A3F7B2C1D4E5F6A7", "9.9.9", &g_opera_chain_height, g_opera_chain_head));
  host_sim::task = "loop";
  ble_opera::startAdvertising();
}

// The server's callbacks are the one dispatcher, installed so NimBLE never
// deletes it with the server.
void the_dispatcher_is_installed() {
  CHECK(host_sim::server->callbacks() == &ble_server_dispatch::g_dispatcher);
  CHECK(!host_sim::server->callbacks_deleted_with_server());
}

// A phone pairs on a FULL build, the channel's init() first and Opera's
// second (the boot worker's order). NimBLE keeps one callbacks pointer per
// server, and before F171 ble_opera::init()'s setCallbacks() replaced the
// channel's: the stack's Numeric Comparison reached Opera's object, which
// does not override it, so NimBLE-Arduino's default answered yes on the
// NimBLE host task, the owner never saw the six digits, and the channel
// never saw the link or the bond. Now both go through the dispatcher: the
// passkey reaches the channel and waits for the owner, only the owner's
// confirm answers it (once, yes, on the loop task), the bond reaches the
// paired list, and Opera still counts the link up and down.
void test_full_profile_the_owner_answers_every_pairing() {
  boot();
  opera_init();
  const uint32_t opera_total = ble_opera::getConnectionsTotal();
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok);
  NimBLEConnInfo phone = link(7, 0xA1);
  host_sim::server->peers = {7};
  host_sim::server->link_up(phone);
  host_sim::calls.clear();
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  CHECK(ble_opera::getConnectionsTotal() == opera_total + 1 && ble_opera::getConnectedNow() == 1);
  CHECK(host_sim::count("adv_start", "nimble") == 1);       // Opera re-advertises, as it always did
  // The stack asks for the Numeric Comparison before the loop task's pass.
  on_nimble([&] { host_sim::server->callbacks()->onConfirmPassKey(phone, 482913); });
  CHECK(host_sim::passkey_answers.empty());                  // nobody said yes for the owner
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 7);
  CHECK(bc::g_pairing.state == bc::PAIR_CONFIRMING && bc::g_pairing.pin_code == 482913);
  // Opera's advertising (the fleet-link beacon) stays on the air through
  // the link, as on every FULL build before: the channel stops the shared
  // advertiser for a link only when no other owner advertises on it.
  CHECK(host_sim::advertising.isAdvertising() && host_sim::count("adv_stop") == 0);
  CHECK(bc::g_pending_pair_active && host_sim::passkey_answers.empty());
  bc::BluetoothStatus st;
  bc::read_status(&st);
  CHECK(st.pairing.state == bc::PAIR_CONFIRMING && st.pairing.pin_displayed);
  CHECK(st.pairing.pin_code == 482913);                      // the PIN box the owner compares

  bc::Command c = cmd_of(bc::BT_CMD_PAIR_CONFIRM);
  c.pin = 482913;
  const Rest r = rest(c);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(host_sim::passkey_answers.size() == 1 && host_sim::passkey_answers[0].accept);
  CHECK(host_sim::passkey_answers[0].task == "loop" && host_sim::passkey_answers[0].handle == 7);

  phone.encrypted = phone.authenticated = phone.bonded = true;
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(phone); });
  loop_pass();
  CHECK(bc::g_paired_count == 1 && bc::g_pairing.state == bc::PAIR_COMPLETE);

  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  CHECK(ble_opera::getConnectedNow() == 0);
  loop_pass();
  CHECK(!bc::g_connection.connected);
  CHECK(host_sim::passkey_answers.size() == 1 && host_sim::conn_heap == 0);
  none_on_httpd();
  the_dispatcher_is_installed();
  std::printf("PASS full_profile_the_owner_answers_every_pairing\n");
}

// The other order: Opera's init() first, the channel's after it (a REST
// handler's bring_up() after a boot whose channel init failed or lost the
// race). Before F171 the channel's setCallbacks() then replaced Opera's,
// and Opera never counted a link. Either order ends with the dispatcher
// installed, both owners reached, and the passkey shown being the channel's
// (not the library's 123456), which reaches the PIN box.
void test_full_profile_either_init_order() {
  boot(/*bring_up=*/false);
  opera_init();
  host_sim::task = "httpd";                                  // bring_up() on a handler's task
  CHECK(bc::init());
  host_sim::task = "loop";
  const uint32_t opera_total = ble_opera::getConnectionsTotal();
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok);
  NimBLEConnInfo phone = link(9, 0xB2);
  host_sim::server->peers = {9};
  host_sim::server->link_up(phone);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  CHECK(ble_opera::getConnectionsTotal() == opera_total + 1);
  uint32_t shown = 0;
  on_nimble([&] { shown = host_sim::server->callbacks()->onPassKeyDisplay(); });
  CHECK(shown != 123456 && shown < 1000000);
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 9);
  CHECK(bc::g_pairing.state == bc::PAIR_PIN_DISPLAYED && bc::g_pairing.pin_code == shown);
  on_nimble([&] { host_sim::server->callbacks()->onConfirmPassKey(phone, 135790); });
  CHECK(host_sim::passkey_answers.empty());
  loop_pass();
  CHECK(bc::g_pairing.state == bc::PAIR_CONFIRMING && bc::g_pending_pair_active);
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_REJECT)).r.ok);          // the owner says no
  CHECK(host_sim::passkey_answers.size() == 1 && !host_sim::passkey_answers[0].accept);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  CHECK(ble_opera::getConnectedNow() == 0);
  loop_pass();
  CHECK(!bc::g_connection.connected && host_sim::conn_heap == 0);
  the_dispatcher_is_installed();
  std::printf("PASS full_profile_either_init_order\n");
}

// With no pairing owner (Opera alone: a build without the pairing channel,
// or before it is up) the dispatcher answers a Numeric Comparison no and
// shows a passkey nobody sees, where the library's defaults said yes and
// 123456; Opera still sees the link, and nothing reaches the channel.
void test_no_pairing_owner_fails_closed() {
  boot(/*bring_up=*/false);
  opera_init();
  NimBLEConnInfo phone = link(5, 0xC5);
  const uint32_t opera_total = ble_opera::getConnectionsTotal();
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  CHECK(ble_opera::getConnectionsTotal() == opera_total + 1);
  uint32_t shown = 0;
  on_nimble([&] { shown = host_sim::server->callbacks()->onPassKeyDisplay(); });
  CHECK(shown != 123456 && shown < 1000000);
  on_nimble([&] { host_sim::server->callbacks()->onConfirmPassKey(phone, 246810); });
  CHECK(host_sim::passkey_answers.size() == 1 && !host_sim::passkey_answers[0].accept);
  CHECK(host_sim::passkey_answers[0].task == "nimble" && host_sim::passkey_answers[0].handle == 5);
  CHECK(bc::g_events.waiting() == 0 && host_sim::conn_heap == 0);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  CHECK(ble_opera::getConnectedNow() == 0);
  the_dispatcher_is_installed();
  std::printf("PASS no_pairing_owner_fails_closed\n");
}

// ── A paired phone's address: its identity, printed as it is (F172) ─────

// A phone that uses resolvable private addresses, as most do: the link
// shows the one it is using now (random, its two top bits 01), and NimBLE
// keys the bond by its identity address (here a public one). `ota_tag`
// tells two of its over-the-air addresses apart.
const uint8_t kPhoneIdentity[6] = {0xC8, 0x2B, 0x96, 0x0A, 0x1B, 0x2C};   // printed order
NimBLEConnInfo rpa_phone(uint16_t handle, uint8_t ota_tag) {
  NimBLEConnInfo c;
  c.handle = handle;
  const uint8_t ota[6] = {0x5A, ota_tag, 0x22, 0x33, 0x44, 0x55};        // printed 5a:<tag>:22:33:44:55
  c.address = NimBLEAddress(ota, 1);
  c.id_address = NimBLEAddress(kPhoneIdentity, 0);
  c.id_known = true;
  return c;
}

// The phone pairs and bonds on `handle` (the stack stores its bond by its
// identity, as NimBLE does, with the IRK the phone hands over), and the loop
// task applies it.
void rpa_phone_bonds(NimBLEConnInfo& phone) {
  phone.encrypted = phone.authenticated = phone.bonded = true;
  host_sim::server->peers = {phone.getConnHandle()};
  host_sim::server->link_up(phone);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(phone); });
  host_sim::store_bond(phone.getIdAddress(), /*irk=*/true);
  loop_pass();
}

std::string upper(std::string s) {
  for (char& ch : s) ch = (char)toupper((unsigned char)ch);
  return s;
}

// The paired list keeps the identity address NimBLE keys the bond by, and
// Remove forgets that bond. Before F172 the list kept the over-the-air
// address (here a resolvable private one the phone drops every few
// minutes), Remove handed its bytes to NimBLE-Arduino's byte-array
// constructor (which reverses them), and ble_gap_unpair() was asked for an
// address no bond had: the phone kept its bond. The same phone back on a
// new private address is the same paired device, not a second one. And the
// addresses print most significant first, as the phone shows its own: GET
// /paired's `address` (and its `name`) is the identity, the connection's
// `address` is the link's own and reads as `connection.name` does; an
// address a route printed finds its entry again (DELETE /paired's round
// trip, through parse_address()).
void test_remove_forgets_the_bond_by_its_identity() {
  boot();
  NimBLEConnInfo phone = rpa_phone(17, 0x11);
  CHECK(phone.getAddress().isRpa() && phone.getAddress() != phone.getIdAddress());
  rpa_phone_bonds(phone);
  CHECK(bc::g_paired_count == 1);
  const NimBLEAddress identity = phone.getIdAddress();
  CHECK(memcmp(bc::g_paired_devices[0].address, identity.getBase()->val, 6) == 0);
  CHECK(bc::g_paired_devices[0].address_type == 0);
  CHECK(strcmp(bc::g_paired_devices[0].name, "c8:2b:96:0a:1b:2c") == 0);
  // The identity is the phone's stable address: in the list, never the log.
  CHECK(health_says("New device paired") == 1 && health_detail("New device paired").empty());
  CHECK(!health_names("c8:2b:96:0a:1b:2c") && !health_names(phone.getAddress().toString()));

  // As the routes print them.
  bc::PairedView paired;
  bc::read_paired(&paired);
  char printed[bc::BLE_ADDRESS_STR_LEN];
  bc::format_address(paired.devices[0].address, printed);
  CHECK(strcmp(printed, "C8:2B:96:0A:1B:2C") == 0);
  bc::BluetoothStatus st;
  bc::read_status(&st);
  char conn[bc::BLE_ADDRESS_STR_LEN];
  bc::format_address(st.connection.address, conn);
  CHECK(strcmp(conn, "5A:11:22:33:44:55") == 0);
  CHECK(upper(st.connection.name) == conn);

  // The phone leaves and comes back on another private address: the bond's
  // identity finds its entry.
  host_sim::server->link_down(17);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  loop_pass();
  NimBLEConnInfo again = rpa_phone(18, 0x77);
  rpa_phone_bonds(again);
  CHECK(bc::g_paired_count == 1 && bc::g_paired_devices[0].connection_count == 2);

  // DELETE /api/bluetooth/paired with the address GET /paired printed,
  // with the radio as a device's always is: the presence loop scanning
  // (advertising is off for the phone's link on this, the DEV, wiring).
  // NimBLE refuses to forget a bond that carries the phone's IRK meanwhile
  // (ble_gap_unpair(): BLE_HS_EBUSY, the bond kept; the F172 review), so the
  // loop task quiets the radio for the call and brings it back after.
  CHECK(host_sim::presence_disc && host_sim::radio_busy());
  CHECK(!host_sim::advertising.isAdvertising());
  bc::Command remove = cmd_of(bc::BT_CMD_PAIRED_REMOVE);
  CHECK(bc::parse_address(printed, remove.address));
  CHECK(memcmp(remove.address, paired.devices[0].address, 6) == 0);
  host_sim::calls.clear();
  host_sim::presence_calls.clear();
  const Rest r = rest(remove);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && bc::g_paired_count == 0);
  CHECK(r.r.refusal == bc::BT_REFUSED_NONE);
  CHECK(host_sim::bonds_deleted.size() == 1 && host_sim::bonds_deleted[0] == identity);
  CHECK(host_sim::bonds_busy.empty());
  CHECK(!NimBLEDevice::isBonded(identity) && host_sim::bonds.empty());
  CHECK(host_sim::count("adv_stop") == 0 && host_sim::count("adv_start") == 0);
  CHECK(host_sim::presence_disc);                            // the presence loop back
  CHECK(host_sim::presence_calls.size() == 2);
  CHECK(host_sim::presence_calls[0].what == "pause" && host_sim::presence_calls[1].what == "resume");
  CHECK(host_sim::presence_calls[0].task == "loop" && host_sim::presence_calls[1].task == "loop");
  // ble_gap_unpair() ended the phone's live link (its end arrives as usual).
  CHECK(host_sim::server->ended_by_unpair == std::vector<uint16_t>{18});
  CHECK(health_says("Paired device removed") == 1);
  none_on_httpd();
  std::printf("PASS remove_forgets_the_bond_by_its_identity\n");
}

// format_address() prints the stored bytes most significant first and
// parse_address() takes that string back to the same bytes, for every
// position; a string that is not an address is refused.
void test_an_address_prints_most_significant_first_and_round_trips() {
  const uint8_t stored[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};   // as getBase()->val holds them
  char out[bc::BLE_ADDRESS_STR_LEN];
  bc::format_address(stored, out);
  CHECK(strcmp(out, "AB:89:67:45:23:01") == 0);
  ble_addr_t base;
  base.type = 0;
  memcpy(base.val, stored, 6);
  CHECK(upper(NimBLEAddress(base).toString()) == out);      // as NimBLE prints it
  uint8_t back[6] = {0};
  CHECK(bc::parse_address(out, back) && memcmp(back, stored, 6) == 0);
  CHECK(bc::parse_address("ab:89:67:45:23:01", back) && memcmp(back, stored, 6) == 0);
  CHECK(!bc::parse_address("AB:89:67:45:23", back));
  CHECK(!bc::parse_address("AB:89:67:45:23:0G", back));
  std::printf("PASS an_address_prints_most_significant_first_and_round_trips\n");
}

// A paired list saved before F172 is rebuilt from the bond store the first
// time the stack is up: the entry that names a bond (a phone with one
// address) keeps what it said; the bond kept under a phone's identity
// whose entry named an old private address gets an entry under its
// identity (so Remove can forget it), as does a bond with no entry; an
// entry that names no bond is dropped. Once: the rebuilt list is saved
// marked, and the next boot loads it as it is.
void test_an_old_paired_list_is_rebuilt_from_the_bond_store() {
  boot(/*bring_up=*/false);
  NimBLEConnInfo a = rpa_phone(21, 0x11);                   // saved by its private address
  const uint8_t b_addr[6] = {0x00, 0x1A, 0x7D, 0xDA, 0x71, 0x13};
  const NimBLEAddress b(b_addr, 0);                         // a public address: one address
  const uint8_t c_addr[6] = {0x00, 0x1A, 0x7D, 0xDA, 0x71, 0x99};
  const NimBLEAddress c(c_addr, 0);                         // its bond is gone
  const uint8_t d_addr[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
  const NimBLEAddress d(d_addr, 1);                         // bonded, never listed (random static)
  bc::PairedDevice old[3];
  memset(old, 0, sizeof old);
  memcpy(old[0].address, a.getAddress().getBase()->val, 6);
  old[0].address_type = 1;
  old[0].trusted = true;
  memcpy(old[1].address, b.getBase()->val, 6);
  old[1].address_type = 1;                                  // the bond says public
  strcpy(old[1].name, "kitchen tablet");
  old[1].connection_count = 7;
  old[1].trusted = true;
  old[1].security = bc::SEC_BONDED;
  memcpy(old[2].address, c.getBase()->val, 6);
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(old);
  host_sim::main_nvs["bt_paired"].assign(raw, raw + sizeof old);
  host_sim::bonds = {a.getIdAddress(), b, d};
  host_sim::bond_irks = {a.getIdAddress()};                 // the phone handed over its IRK
  host_sim::task = "bringup";
  CHECK(bc::init());
  host_sim::task = "loop";
  CHECK(bc::g_paired_count == 3);
  CHECK(memcmp(bc::g_paired_devices[0].address, a.getIdAddress().getBase()->val, 6) == 0);
  CHECK(bc::g_paired_devices[0].address_type == 0 && !bc::g_paired_devices[0].trusted);
  CHECK(strcmp(bc::g_paired_devices[0].name, "c8:2b:96:0a:1b:2c") == 0);
  CHECK(memcmp(bc::g_paired_devices[1].address, b.getBase()->val, 6) == 0);
  CHECK(strcmp(bc::g_paired_devices[1].name, "kitchen tablet") == 0);
  CHECK(bc::g_paired_devices[1].connection_count == 7 && bc::g_paired_devices[1].trusted);
  CHECK(bc::g_paired_devices[1].address_type == 0);         // the bond store's type
  CHECK(memcmp(bc::g_paired_devices[2].address, d.getBase()->val, 6) == 0);
  CHECK(bc::g_paired_devices[2].address_type == 1);
  CHECK(bc::g_paired_devices[2].security == bc::SEC_BONDED);
  CHECK(host_sim::main_nvs.count("bt_paired_id") == 1);
  CHECK(host_sim::main_nvs["bt_paired"].size() == 3 * sizeof(bc::PairedDevice));
  CHECK(health_detail("Paired list rebuilt from the bond store") == "1 kept, 2 added, 2 dropped");

  // Remove the phone by the identity GET /paired prints: its bond goes.
  bc::Command remove = cmd_of(bc::BT_CMD_PAIRED_REMOVE);
  CHECK(bc::parse_address("C8:2B:96:0A:1B:2C", remove.address));
  CHECK(rest(remove).r.ok && bc::g_paired_count == 2);
  CHECK(!NimBLEDevice::isBonded(a.getIdAddress()) && host_sim::bonds.size() == 2);

  // Once: the next boot loads the saved list as it is, even with a bond
  // the store no longer has.
  host_sim::bonds = {b};
  boot(/*bring_up=*/true, /*wipe=*/false);
  CHECK(bc::g_paired_count == 2);
  CHECK(health_says("Paired list rebuilt from the bond store") == 0);
  std::printf("PASS an_old_paired_list_is_rebuilt_from_the_bond_store\n");
}

// ── A link while Bluetooth is off; a dropped link event (F173, F169) ────

// A phone whose connection was in flight when Bluetooth was turned off
// comes up anyway. Before F173 its connect was applied, and since a
// disabled channel's update() returns early, nothing dropped it: GET
// /api/bluetooth read "state": "connected" beside "enabled": false until
// the phone left. Now the loop task drops it and records nothing, and a
// Numeric Comparison it asks for meanwhile is answered no (nothing would
// time it out while disabled). Its end leaves Bluetooth off.
void test_a_link_while_bluetooth_is_off_is_refused() {
  boot();
  CHECK(rest(cmd_of(bc::BT_CMD_DISABLE)).r.ok && bc::g_state == bc::BT_DISABLED);
  const uint32_t connections = bc::g_total_connections;
  NimBLEConnInfo phone = link(61, 0xD6);
  host_sim::server->peers = {61};
  host_sim::server->link_up(phone);
  host_sim::calls.clear();
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  on_nimble([&] { host_sim::server->callbacks()->onConfirmPassKey(phone, 112233); });
  loop_pass();
  CHECK(!bc::g_connection.connected && bc::g_connection_handle == 0xFFFF);
  CHECK(bc::g_state == bc::BT_DISABLED && bc::g_total_connections == connections);
  CHECK(host_sim::count("disconnect", "loop") == 1);
  CHECK(host_sim::server->disconnected.size() == 1 && host_sim::server->disconnected[0] == 61);
  CHECK(host_sim::passkey_answers.size() == 1 && !host_sim::passkey_answers[0].accept);
  CHECK(host_sim::passkey_answers[0].task == "loop" && host_sim::passkey_answers[0].handle == 61);
  CHECK(bc::g_pairing.state == bc::PAIR_NONE && !bc::g_pending_pair_active);
  CHECK(host_sim::conn_heap == 0);
  CHECK(health_says("BLE link refused: Bluetooth is off") == 1);
  bc::BluetoothStatus st;
  bc::read_status(&st);
  CHECK(!st.connected && st.state == bc::BT_DISABLED && !st.enabled);
  host_sim::server->link_down(61);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x16); });
  loop_pass();
  CHECK(bc::g_state == bc::BT_DISABLED && !host_sim::advertising.isAdvertising());
  CHECK(host_sim::count("adv_start") == 0);
  std::printf("PASS a_link_while_bluetooth_is_off_is_refused\n");
}

// The loop task stalls long enough for a link's room in the queue to fill
// (here with scan ends, which post at the full limit too), and the phone's
// disconnect finds no room. Before F169 the connection stayed recorded:
// "connected": true, no advertising, until another link's events or a
// reboot. Now the next pass sees a link's event was dropped, asks the stack
// for the recorded handle, finds it gone, and ends it as the disconnect
// would have: advertising resumes, and the health log says why.
void test_a_dropped_disconnect_heals() {
  boot();
  NimBLEConnInfo phone = link(71, 0xE7);
  host_sim::server->peers = {71};
  host_sim::server->link_up(phone);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  loop_pass();
  CHECK(bc::g_connection.connected && !host_sim::advertising.isAdvertising());
  while (bc::g_events.waiting() < bc::EVENT_SLOTS) {
    on_nimble([&] { host_sim::scan.callbacks()->onScanEnd(NimBLEScanResults(), 0); });
  }
  host_sim::server->link_down(71);
  host_sim::server->peers.clear();
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  CHECK(bc::g_events.dropped_reserved() == 1);
  loop_pass();
  CHECK(!bc::g_connection.connected && bc::g_connection_handle == 0xFFFF);
  CHECK(host_sim::advertising.isAdvertising() && bc::g_state == bc::BT_ADVERTISING);
  CHECK(health_says("BLE link gone, its end dropped: ended from the stack's record") == 1);
  CHECK(health_level("BLE link gone, its end dropped: ended from the stack's record") == SCV_LOG_WARNING);
  CHECK(presence_on("loop", "console_off") == 1);
  // A pass with no new drop asks nothing more.
  g_health.clear();
  loop_pass();
  CHECK(health_says("BLE link gone, its end dropped: ended from the stack's record") == 0);

  // Both lost: the phone's end and the next phone's start on the same
  // handle. The stack's record of the handle names another address, so the
  // recorded link ended, and the one the stack holds is recorded.
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  host_sim::server->peers = {71};
  host_sim::server->link_up(phone);
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 71);
  while (bc::g_events.waiting() < bc::EVENT_SLOTS) {
    on_nimble([&] { host_sim::scan.callbacks()->onScanEnd(NimBLEScanResults(), 0); });
  }
  NimBLEConnInfo next = link(71, 0xE9);
  host_sim::server->link_up(next);                          // the handle is the next phone's now
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), next); });
  CHECK(bc::g_events.dropped_reserved() == 3);
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 71);
  CHECK(memcmp(bc::g_connection.address, next.getAddress().getBase()->val, 6) == 0);
  CHECK(health_says("BLE link gone, its end dropped: ended from the stack's record") == 1);
  CHECK(health_says("BLE link up, its start dropped: recorded from the stack's record") == 1);
  std::printf("PASS a_dropped_disconnect_heals\n");
}

// The other way: a phone's connect finds no room. Before F169 its live link
// went unrecorded (no inactivity timeout, advertising on beside it). Now the
// next pass records it from the stack's own record, as its connect would.
void test_a_dropped_connect_is_recorded_from_the_stack() {
  boot();
  while (bc::g_events.waiting() < bc::EVENT_SLOTS) {
    on_nimble([&] { host_sim::scan.callbacks()->onScanEnd(NimBLEScanResults(), 0); });
  }
  NimBLEConnInfo phone = link(72, 0xE8);
  host_sim::server->peers = {72};
  host_sim::server->link_up(phone);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  CHECK(bc::g_events.dropped_reserved() == 1);
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 72);
  CHECK(strcmp(bc::g_connection.name, phone.getAddress().toString().c_str()) == 0);
  CHECK(bc::g_state == bc::BT_CONNECTED && !host_sim::advertising.isAdvertising());
  CHECK(health_says("BLE link up, its start dropped: recorded from the stack's record") == 1);
  // Its end, reported as usual, ends it.
  host_sim::server->link_down(72);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  loop_pass();
  CHECK(!bc::g_connection.connected && bc::g_state == bc::BT_ADVERTISING);
  std::printf("PASS a_dropped_connect_is_recorded_from_the_stack\n");
}

// Only the recorded link's end ends the record. A second phone that comes
// up while one is recorded (on the FULL profile Opera re-advertises after
// a connect, F171 put its links in front of the channel) is recorded in its
// place; the first phone's end, on its own handle, or on the same handle
// for another address, leaves the second recorded. Before, any disconnect
// cleared the record while the second link was up.
void test_only_the_recorded_links_end_ends_it() {
  boot();
  NimBLEConnInfo a = link(81, 0xF1);
  NimBLEConnInfo b = link(82, 0xF2);
  host_sim::server->peers = {81, 82};
  host_sim::server->link_up(a);
  host_sim::server->link_up(b);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), a); });
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), b); });
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 82);
  host_sim::server->link_down(81);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), a, 0x13); });
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 82);
  CHECK(bc::g_state == bc::BT_CONNECTED && !host_sim::advertising.isAdvertising());
  NimBLEConnInfo stranger = link(82, 0xF9);                // another address on b's handle
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), stranger, 0x13); });
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 82);
  host_sim::server->link_down(82);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), b, 0x13); });
  loop_pass();
  CHECK(!bc::g_connection.connected && bc::g_state == bc::BT_ADVERTISING);
  std::printf("PASS only_the_recorded_links_end_ends_it\n");
}

// The reconciliation leaves a record the stack still holds alone, compares
// the handle as well as the address, and asks the stack only after a new
// drop (the F169 review's three surviving mutants). A drop while the
// recorded link is up and a second link's connect was the one lost: the
// record, its time and the count stay (before the guard, the second link
// was recorded over it and counted again). A disconnect naming the
// recorded address on another handle (the phone's old link, reported after
// the phone came back on a new handle) leaves the record. And a pass after
// a drop was answered asks the stack nothing.
void test_a_reconcile_leaves_a_live_record_alone() {
  boot();
  NimBLEConnInfo a = link(11, 0xA1);
  host_sim::server->peers = {11};
  host_sim::server->link_up(a);
  host_sim::now_ms += 1000;
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), a); });
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 11);
  const uint32_t since = bc::g_connection.connected_since_ms;
  const uint32_t total = bc::g_total_connections;
  while (bc::g_events.waiting() < bc::EVENT_SLOTS) {
    on_nimble([&] { host_sim::scan.callbacks()->onScanEnd(NimBLEScanResults(), 0); });
  }
  NimBLEConnInfo b = link(12, 0xB2);                         // its connect finds no room
  host_sim::server->peers = {11, 12};
  host_sim::server->link_up(b);
  host_sim::now_ms += 1000;
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), b); });
  CHECK(bc::g_events.dropped_reserved() == 1);
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 11);
  CHECK(memcmp(bc::g_connection.address, a.getAddress().getBase()->val, 6) == 0);
  CHECK(bc::g_connection.connected_since_ms == since && bc::g_total_connections == total);
  CHECK(health_says("BLE link up, its start dropped: recorded from the stack's record") == 0);
  CHECK(health_says("BLE link gone, its end dropped: ended from the stack's record") == 0);

  // The drop was answered: the next pass asks the stack nothing.
  const unsigned info_calls = host_sim::server->peer_info_calls;
  const unsigned devices_calls = host_sim::server->peer_devices_calls;
  loop_pass();
  loop_pass();
  CHECK(host_sim::server->peer_info_calls == info_calls);
  CHECK(host_sim::server->peer_devices_calls == devices_calls);

  // The recorded phone's address on another handle (its old link's end,
  // reported late): the record stays.
  NimBLEConnInfo old_link = link(13, 0xA1);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), old_link, 0x08); });
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 11);
  CHECK(bc::g_connection.connected_since_ms == since);
  std::printf("PASS a_reconcile_leaves_a_live_record_alone\n");
}

// Another link's encryption is not the recorded link's (the F171 review).
// On FULL a second link can come up beside the recorded one (Opera
// advertises through a link), and the newest is recorded. The first link's
// bonded encryption marked the recorded (second, unauthenticated) link
// bonded on the connection card and ended its pairing as complete; its
// failed pairing ended the owner's pairing mode as failed. Now only the
// recorded link's events touch the card and the pairing's state; the bond
// still reaches the paired list, by its identity.
void test_another_links_encryption_leaves_the_record() {
  boot();
  opera_init();
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok && bc::g_pairing.state == bc::PAIR_INITIATED);
  NimBLEConnInfo a = link(7, 0xA7);
  NimBLEConnInfo b = link(8, 0xB8);
  host_sim::server->peers = {7, 8};
  host_sim::server->link_up(a);
  host_sim::server->link_up(b);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), a); });
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), b); });
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 8);
  CHECK(bc::g_connection.security == bc::SEC_NONE);

  NimBLEConnInfo failed = a;                                 // a's pairing fails
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(failed); });
  loop_pass();
  CHECK(health_says("Pairing failed") == 1);
  CHECK(bc::g_pairing.state == bc::PAIR_INITIATED);         // the owner's pairing mode goes on

  a.encrypted = a.authenticated = a.bonded = true;           // then bonds
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(a); });
  loop_pass();
  CHECK(bc::g_paired_count == 1);
  CHECK(memcmp(bc::g_paired_devices[0].address, a.getIdAddress().getBase()->val, 6) == 0);
  CHECK(bc::g_connection_handle == 8 && bc::g_connection.security == bc::SEC_NONE);
  CHECK(bc::g_pairing.state == bc::PAIR_INITIATED);
  bc::BluetoothStatus st;
  bc::read_status(&st);
  CHECK(st.connection.security == bc::SEC_NONE && st.pairing.state == bc::PAIR_INITIATED);

  b.encrypted = b.authenticated = true;                      // the recorded link's own
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(b); });
  loop_pass();
  CHECK(bc::g_connection.security == bc::SEC_AUTHENTICATED);
  CHECK(bc::g_pairing.state == bc::PAIR_COMPLETE);
  CHECK(bc::g_paired_count == 1);                            // not bonded: not listed
  std::printf("PASS another_links_encryption_leaves_the_record\n");
}

// How far the channel's per-link rules reach on FULL, where since F171 it
// sees every link on the shared server: a phone's, and Opera's GATT
// clients, a BLE OTA, provisioning or console session (whose
// characteristics post no activity to the channel). The inactivity timeout
// measures the recorded link and drops only it (the F171 review: it dropped
// every link, so a second link was cut with an idle phone). The owner's
// Disconnect drops every link, and Bluetooth off refuses every link (F173),
// Opera's clients included: both stated, and pinned here.
void test_full_profile_the_timeout_ends_the_recorded_link_only() {
  boot();
  opera_init();
  const uint32_t opera_now = ble_opera::getConnectedNow();
  NimBLEConnInfo client = link(7, 0xC7);                     // Opera's client, say
  NimBLEConnInfo phone = link(8, 0xD8);
  host_sim::server->peers = {7, 8};
  host_sim::server->link_up(client);
  host_sim::server->link_up(phone);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), client); });
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  loop_pass();
  CHECK(bc::g_connection.connected && bc::g_connection_handle == 8);
  CHECK(ble_opera::getConnectedNow() == opera_now + 2);
  host_sim::now_ms += bc::g_settings.inactivity_timeout_ms;
  loop_pass();
  CHECK(health_says("Disconnecting due to inactivity") == 1);
  CHECK(host_sim::server->disconnected == std::vector<uint16_t>{8});

  // The owner's Disconnect: every link on the server (the command's two
  // calls, ahead of the still-idle record's timeout on the same pass).
  host_sim::server->disconnected.clear();
  CHECK(rest(cmd_of(bc::BT_CMD_DISCONNECT)).r.ok);
  const std::vector<uint16_t>& dropped = host_sim::server->disconnected;
  CHECK(dropped.size() >= 2 && dropped[0] == 7 && dropped[1] == 8);
  std::printf("PASS full_profile_the_timeout_ends_the_recorded_link_only\n");
}

// Bluetooth off on FULL: Opera's advertising can still let a client in (a
// NEW decision of this sweep), Opera counts it, and the channel drops it
// (F173), whatever service it came for.
void test_full_profile_off_refuses_every_link() {
  boot();
  opera_init();
  CHECK(rest(cmd_of(bc::BT_CMD_DISABLE)).r.ok && !bc::is_enabled());
  const uint32_t opera_total = ble_opera::getConnectionsTotal();
  NimBLEConnInfo client = link(9, 0xC9);
  host_sim::server->peers = {9};
  host_sim::server->link_up(client);
  host_sim::server->disconnected.clear();
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), client); });
  CHECK(ble_opera::getConnectionsTotal() == opera_total + 1);
  loop_pass();
  CHECK(host_sim::server->disconnected == std::vector<uint16_t>{9});
  CHECK(!bc::g_connection.connected && bc::g_state == bc::BT_DISABLED);
  CHECK(health_says("BLE link refused: Bluetooth is off") == 1);
  std::printf("PASS full_profile_off_refuses_every_link\n");
}

// ── The state is what runs (F170) ───────────────────────────────────────

// What GET /api/bluetooth shows: the state and the advertising flag.
std::pair<bc::BluetoothState, bool> shown() {
  bc::BluetoothStatus st;
  bc::read_status(&st);
  return {st.state, st.advertising};
}

// A scan ends (the owner's stop, its timeout or the stack's end) while
// something else goes on: the state names what goes on. Before F170 each
// end set idle (or connected) whatever it was, so with advertising on the
// route read "state": "idle" beside "advertising": true, and a scan run in
// pairing mode left the state idle with pairing mode still on.
void test_the_state_after_a_scan_is_what_runs() {
  boot();
  CHECK(host_sim::advertising.isAdvertising());
  bc::Command scan = cmd_of(bc::BT_CMD_SCAN_START);
  scan.duration_ms = 5000;
  CHECK(rest(scan).r.ok && bc::g_state == bc::BT_SCANNING);
  CHECK(rest(cmd_of(bc::BT_CMD_SCAN_STOP)).r.ok);
  CHECK((shown() == std::pair<bc::BluetoothState, bool>{bc::BT_ADVERTISING, true}));
  CHECK(rest(scan).r.ok);
  on_nimble([&] { host_sim::scan.callbacks()->onScanEnd(NimBLEScanResults(), 0); });
  loop_pass();
  CHECK((shown() == std::pair<bc::BluetoothState, bool>{bc::BT_ADVERTISING, true}));
  CHECK(rest(scan).r.ok);
  host_sim::now_ms += 5000;                                  // its timeout
  loop_pass();
  CHECK(!bc::g_scanning && bc::g_state == bc::BT_ADVERTISING);

  // In pairing mode: the scan ends back in pairing mode.
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok && bc::g_state == bc::BT_PAIRING);
  CHECK(rest(scan).r.ok && bc::g_state == bc::BT_SCANNING);
  on_nimble([&] { host_sim::scan.callbacks()->onScanEnd(NimBLEScanResults(), 0); });
  loop_pass();
  CHECK(bc::g_state == bc::BT_PAIRING);
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_CANCEL)).r.ok);

  // Nothing else running: idle, as before.
  CHECK(rest(cmd_of(bc::BT_CMD_ADVERTISE_STOP)).r.ok);
  CHECK(rest(scan).r.ok);
  CHECK(rest(cmd_of(bc::BT_CMD_SCAN_STOP)).r.ok);
  CHECK((shown() == std::pair<bc::BluetoothState, bool>{bc::BT_IDLE, false}));
  std::printf("PASS the_state_after_a_scan_is_what_runs\n");
}

// Pairing mode starts advertising; canceling it (or its timeout) leaves the
// advertising on, and the state says so (before F170: idle). A link that
// ends while a scan runs leaves the state scanning (before: idle, then
// advertising once the restart ran).
void test_the_state_after_pairing_or_a_link_is_what_runs() {
  boot();
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok && bc::g_state == bc::BT_PAIRING);
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_CANCEL)).r.ok);
  CHECK((shown() == std::pair<bc::BluetoothState, bool>{bc::BT_ADVERTISING, true}));
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok);
  host_sim::now_ms += bc::PAIRING_TIMEOUT_MS;
  loop_pass();
  CHECK(bc::g_pairing.state == bc::PAIR_NONE && bc::g_state == bc::BT_ADVERTISING);

  NimBLEConnInfo phone = link(91, 0xA9);
  host_sim::server->peers = {91};
  host_sim::server->link_up(phone);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  loop_pass();
  bc::Command scan = cmd_of(bc::BT_CMD_SCAN_START);
  scan.duration_ms = 5000;
  CHECK(rest(scan).r.ok && bc::g_state == bc::BT_SCANNING);
  host_sim::server->link_down(91);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  loop_pass();
  CHECK(!bc::g_connection.connected && bc::g_scanning && bc::g_state == bc::BT_SCANNING);
  CHECK(rest(cmd_of(bc::BT_CMD_SCAN_STOP)).r.ok && bc::g_state == bc::BT_ADVERTISING);

  // A scan stopped while a phone is connected: connected, as before. A link
  // that ends during a scan in pairing mode: scanning, then pairing.
  host_sim::server->link_up(phone);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), phone); });
  loop_pass();
  CHECK(rest(scan).r.ok && rest(cmd_of(bc::BT_CMD_SCAN_STOP)).r.ok);
  CHECK(bc::g_connection.connected && bc::g_state == bc::BT_CONNECTED);
  CHECK(rest(cmd_of(bc::BT_CMD_PAIR_START)).r.ok && rest(scan).r.ok);
  host_sim::server->link_down(91);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  loop_pass();
  CHECK(bc::g_state == bc::BT_SCANNING);
  CHECK(rest(cmd_of(bc::BT_CMD_SCAN_STOP)).r.ok && bc::g_state == bc::BT_PAIRING);
  std::printf("PASS the_state_after_pairing_or_a_link_is_what_runs\n");
}

// On the FULL profile Opera's onDisconnect restarts advertising on the
// NimBLE host task before the loop task applies the link's end, so the
// channel's own restart finds it on and set no state: GET /api/bluetooth
// read idle beside "advertising": true (F170, reachable since F171 put
// the links in front of the channel there). Now advertising.
void test_a_link_ends_on_full_into_advertising() {
  boot();
  opera_init();
  host_sim::server->peers = {92};
  NimBLEConnInfo other = link(92, 0xAA);
  host_sim::server->link_up(other);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), other); });
  loop_pass();
  CHECK(bc::g_state == bc::BT_CONNECTED);
  host_sim::server->link_down(92);
  host_sim::advertising.stop();                              // off as the link ends: the restart is Opera's
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), other, 0x13); });
  CHECK(host_sim::advertising.isAdvertising());              // Opera's restart, on the NimBLE task
  loop_pass();
  CHECK((shown() == std::pair<bc::BluetoothState, bool>{bc::BT_ADVERTISING, true}));
  std::printf("PASS a_link_ends_on_full_into_advertising\n");
}

// When the stack keeps the bond after all (another task started the
// advertiser again inside the delete: Opera's onConnect on the NimBLE host
// task, a chirp), Remove keeps the entry and says so: before the F172
// review it answered ok, dropped the entry and saved the list, and the
// phone, its bond kept, came back encrypted with no owner asked and was
// listed again. The owner's retry, the radio quiet, forgets it. Clear-all
// keeps the entries of the bonds the stack kept, drops the rest, and says
// so; its retry clears them.
void test_a_bond_the_stack_keeps_keeps_its_entry() {
  boot();
  NimBLEConnInfo phone = rpa_phone(31, 0x31);
  rpa_phone_bonds(phone);
  host_sim::server->link_down(31);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  loop_pass();
  const NimBLEAddress identity = phone.getIdAddress();
  CHECK(bc::g_paired_count == 1 && NimBLEDevice::isBonded(identity));
  const std::vector<uint8_t> saved = host_sim::main_nvs["bt_paired"];

  host_sim::before_unpair = [] { host_sim::advertising.start(); };
  bc::Command remove = cmd_of(bc::BT_CMD_PAIRED_REMOVE);
  memcpy(remove.address, identity.getBase()->val, 6);
  Rest r = rest(remove);
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && r.r.refusal == bc::BT_REFUSED_BOND_KEPT);
  CHECK(host_sim::bonds_busy.size() == 1 && NimBLEDevice::isBonded(identity));
  CHECK(bc::g_paired_count == 1 && memcmp(bc::g_paired_devices[0].address, identity.getBase()->val, 6) == 0);
  CHECK(host_sim::main_nvs["bt_paired"] == saved);            // nothing saved
  CHECK(health_says("Paired device not removed: the stack kept its bond") == 1);
  CHECK(health_says("Paired device removed") == 0);
  bc::PairedView paired;
  bc::read_paired(&paired);
  CHECK(paired.count == 1);                                   // the list still shows it
  CHECK(host_sim::advertising.isAdvertising() && host_sim::presence_disc);

  host_sim::before_unpair = nullptr;                          // the owner tries again
  r = rest(remove);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && r.r.refusal == bc::BT_REFUSED_NONE);
  CHECK(bc::g_paired_count == 0 && !NimBLEDevice::isBonded(identity));

  // Clear-all: the phone (IRK) and a tablet with one public address.
  NimBLEConnInfo again = rpa_phone(32, 0x32);
  rpa_phone_bonds(again);
  host_sim::server->link_down(32);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), again, 0x13); });
  loop_pass();
  NimBLEConnInfo tablet = link(33, 0xE3);
  tablet.encrypted = tablet.authenticated = tablet.bonded = true;
  host_sim::server->peers = {33};
  host_sim::server->link_up(tablet);
  on_nimble([&] { host_sim::server->callbacks()->onConnect(host_sim::server.get(), tablet); });
  on_nimble([&] { host_sim::server->callbacks()->onAuthenticationComplete(tablet); });
  host_sim::store_bond(tablet.getIdAddress(), /*irk=*/false);
  loop_pass();
  CHECK(bc::g_paired_count == 2 && host_sim::bonds.size() == 2);
  host_sim::before_unpair = [] { host_sim::advertising.start(); };
  r = rest(cmd_of(bc::BT_CMD_PAIRED_CLEAR));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && r.r.refusal == bc::BT_REFUSED_BOND_KEPT);
  CHECK(host_sim::bonds.size() == 1 && NimBLEDevice::isBonded(identity));
  CHECK(bc::g_paired_count == 1 && memcmp(bc::g_paired_devices[0].address, identity.getBase()->val, 6) == 0);
  CHECK(health_detail("Paired devices not all cleared: the stack kept bonds") == "1");
  host_sim::before_unpair = nullptr;
  r = rest(cmd_of(bc::BT_CMD_PAIRED_CLEAR));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && bc::g_paired_count == 0 && host_sim::bonds.empty());
  CHECK(health_says("All paired devices cleared") == 1);
  CHECK(host_sim::advertising.isAdvertising() && host_sim::presence_disc);
  none_on_httpd();
  std::printf("PASS a_bond_the_stack_keeps_keeps_its_entry\n");
}

// A Remove while the owner's scan runs: a discovery, so the scan ends first
// (its state from what goes on, the scanner back to the presence loop, which
// pauses for the delete and resumes after), and the bond goes.
void test_a_remove_during_a_scan_ends_the_scan_first() {
  boot();
  NimBLEConnInfo phone = rpa_phone(35, 0x35);
  rpa_phone_bonds(phone);
  host_sim::server->link_down(35);
  on_nimble([&] { host_sim::server->callbacks()->onDisconnect(host_sim::server.get(), phone, 0x13); });
  loop_pass();
  bc::Command scan = cmd_of(bc::BT_CMD_SCAN_START);
  scan.duration_ms = 10000;
  CHECK(rest(scan).r.ok && bc::g_scanning && host_sim::scan.isScanning());
  CHECK(!host_sim::presence_disc);                            // paused for the owner's scan
  bc::Command remove = cmd_of(bc::BT_CMD_PAIRED_REMOVE);
  memcpy(remove.address, phone.getIdAddress().getBase()->val, 6);
  const Rest r = rest(remove);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && bc::g_paired_count == 0);
  CHECK(host_sim::bonds_busy.empty() && !NimBLEDevice::isBonded(phone.getIdAddress()));
  CHECK(!bc::g_scanning && !host_sim::scan.isScanning());
  CHECK((shown() == std::pair<bc::BluetoothState, bool>{bc::BT_ADVERTISING, true}));
  CHECK(host_sim::presence_disc);                             // the presence loop back
  none_on_httpd();
  std::printf("PASS a_remove_during_a_scan_ends_the_scan_first\n");
}

// On FULL the shared advertiser carries Opera's fleet-link beacon, on the
// air through a phone's link too: a Remove of a connected phone (its IRK
// bond) stops it for the delete, the bond goes, and the beacon is back on
// the air after, Opera's own flag untouched.
void test_full_profile_remove_brings_the_beacon_back() {
  boot();
  opera_init();
  NimBLEConnInfo phone = rpa_phone(37, 0x37);
  rpa_phone_bonds(phone);
  CHECK(bc::g_paired_count == 1 && bc::g_connection.connected);
  CHECK(host_sim::advertising.isAdvertising() && ble_opera::isAdvertising());
  CHECK(host_sim::presence_disc);
  host_sim::calls.clear();
  bc::Command remove = cmd_of(bc::BT_CMD_PAIRED_REMOVE);
  memcpy(remove.address, phone.getIdAddress().getBase()->val, 6);
  const Rest r = rest(remove);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && bc::g_paired_count == 0);
  CHECK(host_sim::bonds_busy.empty() && host_sim::bonds.empty());
  CHECK(host_sim::count("adv_stop", "loop") == 1 && host_sim::count("adv_start", "loop") == 1);
  CHECK(host_sim::advertising.isAdvertising() && ble_opera::isAdvertising());
  CHECK(host_sim::presence_disc);
  CHECK(host_sim::server->ended_by_unpair == std::vector<uint16_t>{37});
  none_on_httpd();
  std::printf("PASS full_profile_remove_brings_the_beacon_back\n");
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
    {"a_link_is_named_by_its_address", test_a_link_is_named_by_its_address},
    {"a_pairing_ends_with_its_link", test_a_pairing_ends_with_its_link},
    {"a_reused_handle_never_takes_the_old_yes", test_a_reused_handle_never_takes_the_old_yes},
    {"an_answer_goes_only_to_its_own_link", test_an_answer_goes_only_to_its_own_link},
    {"turning_bluetooth_off_ends_a_pairing", test_turning_bluetooth_off_ends_a_pairing},
    {"gatt_activity_never_takes_the_links_room", test_gatt_activity_never_takes_the_links_room},
    {"a_busy_room_is_no_warning", test_a_busy_room_is_no_warning},
    {"a_links_security_is_what_the_stack_said", test_a_links_security_is_what_the_stack_said},
    {"gatt_activity_counts_and_keeps_a_link", test_gatt_activity_counts_and_keeps_a_link},
    {"a_passkey_to_show_reaches_the_owner", test_a_passkey_to_show_reaches_the_owner},
    {"a_scan_result_keeps_what_the_advertisement_said",
     test_a_scan_result_keeps_what_the_advertisement_said},
    {"times_are_the_callbacks", test_times_are_the_callbacks},
    {"a_links_requests_go_out_from_the_loop_task", test_a_links_requests_go_out_from_the_loop_task},
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
    {"full_profile_the_owner_answers_every_pairing", test_full_profile_the_owner_answers_every_pairing},
    {"full_profile_either_init_order", test_full_profile_either_init_order},
    {"no_pairing_owner_fails_closed", test_no_pairing_owner_fails_closed},
    {"remove_forgets_the_bond_by_its_identity", test_remove_forgets_the_bond_by_its_identity},
    {"an_address_prints_most_significant_first_and_round_trips",
     test_an_address_prints_most_significant_first_and_round_trips},
    {"an_old_paired_list_is_rebuilt_from_the_bond_store", test_an_old_paired_list_is_rebuilt_from_the_bond_store},
    {"a_link_while_bluetooth_is_off_is_refused", test_a_link_while_bluetooth_is_off_is_refused},
    {"a_dropped_disconnect_heals", test_a_dropped_disconnect_heals},
    {"a_dropped_connect_is_recorded_from_the_stack", test_a_dropped_connect_is_recorded_from_the_stack},
    {"only_the_recorded_links_end_ends_it", test_only_the_recorded_links_end_ends_it},
    {"the_state_after_a_scan_is_what_runs", test_the_state_after_a_scan_is_what_runs},
    {"the_state_after_pairing_or_a_link_is_what_runs", test_the_state_after_pairing_or_a_link_is_what_runs},
    {"a_link_ends_on_full_into_advertising", test_a_link_ends_on_full_into_advertising},
    {"a_reconcile_leaves_a_live_record_alone", test_a_reconcile_leaves_a_live_record_alone},
    {"another_links_encryption_leaves_the_record", test_another_links_encryption_leaves_the_record},
    {"full_profile_the_timeout_ends_the_recorded_link_only",
     test_full_profile_the_timeout_ends_the_recorded_link_only},
    {"full_profile_off_refuses_every_link", test_full_profile_off_refuses_every_link},
    {"a_bond_the_stack_keeps_keeps_its_entry", test_a_bond_the_stack_keeps_keeps_its_entry},
    {"a_remove_during_a_scan_ends_the_scan_first", test_a_remove_during_a_scan_ends_the_scan_first},
    {"full_profile_remove_brings_the_beacon_back", test_full_profile_remove_brings_the_beacon_back},
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
