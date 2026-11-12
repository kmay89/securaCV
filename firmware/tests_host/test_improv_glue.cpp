// The Sense and Vision Improv glue, driven on the host (sweep F472).
//
// firmware/common/network/improv_ble.cpp is the NimBLE half of the Bluetooth
// setup door: the write callbacks that fill the mailboxes on the host task,
// the connect generation that stamps each write with its link, the loop-task
// handlers that answer them, the words they log, the scan stream, the link
// drops and the first-boot window's record across a reset. Its rules are
// improv_core.h's, host-tested in test_improv_core.cpp; this suite compiles
// the glue itself, unedited, over the stand-ins in stubs/improv_glue/ (the
// NimBLE-Arduino 2.5.0 shape and Arduino-ESP32 3.3.8's scan rules for
// ESP_ARDUINO_VERSION_MAJOR 3, 1.4.3 and 2.0.17's for 2; the Makefile builds
// both) and plays the radio's part: connects, drops and writes in the order
// the stack delivers them, the stack's disconnect event after the glue asks
// for one, the Wi-Fi driver's sweep, and the portal's join.
//
// One scenario per process: the glue's state is file-static and begin()
// runs once a boot. The Makefile runs every name `--list` prints, and fails
// when that list and its own differ. A scenario that runs no check fails.
// `reset` forks a child per boot it plays (a previous boot, then one per
// reset reason), so each child's begin() is a boot of its own.
//
// What it holds the glue to (each scenario was proved red by a mutant of
// improv_ble.cpp when it was committed):
//   race        the join's link is the host task's stamp on the write
//               (s_rx_link), never s_conn_gen read on the loop task after
//               the sender dropped and another link connected;
//   wrap        the connect generation is 32 bits: 256 or 65536 reconnects
//               later is not the link the join is bound to;
//   beside      link_of() answers 0 for a handle that is not the link the
//               server callbacks last saw connect;
//   secondwifi  a WIFI_SETTINGS refused mid-join logs "refused", never
//               "accepted, joining", and leaves the join on its link;
//   drop, nodrop  HUB from the provisioning link only;
//   portalrefused an accepted write the portal refused is "refused", no join;
//   joinfail    the portal's Fail verdict: back to Authorized, UnableToConnect,
//               "Join failed: <reason>", no hand-off, a retry joins;
//   storerefused the board's store_hub refusing reads back Rejected and is
//               logged "not stored"; a board with none takes no hand-off;
//   unencrypted both mailboxes refuse a write from an unencrypted link;
//   commands    the capabilities byte, IDENTIFY, GET_DEVICE_INFO, an unknown
//               command, and an empty or oversize write answered InvalidRpc;
//   scan        GET_WIFI_NETWORKS streams the sweep one row a pass, at most
//               20, and closes it empty; a sweep the driver refuses, a silent
//               one (the glue's 30 s on core 3, the core's own 6 s on core
//               2), one the portal harvested first, one already running
//               (handed back first) and one under a join;
//   linger      the glue's own drops (180 s idle, 20 s after the verdict):
//               asked once, and the right advert back on air after the
//               stack's disconnect event;
//   reset       the first-boot window across a reset: a software reset, a
//               panic, a watchdog or a deep-sleep wake carries what a
//               previous boot spent; a power-on, a brownout, an unnameable
//               reset (and on IDF 5.x a USB / JTAG reset or a power glitch)
//               clears it; a record without the magic is ignored; stored
//               credentials clear it;
//   companion   1.4.x's per-service start refusing the companion service
//               leaves the Wi-Fi door working and no HUB to write;
//   startfail, init  a refused GATT start or NimBLE that will not come up.
// Not driven: the reset reasons the glue's comment names no rule for (EXT,
// SDIO, eFuse, CPU lockup: kept today, by the switch's default), reach_url
// (both families pass null), a stack that refuses to drop a link, and a
// disconnect event that lands after the loop's next pass (host_turn()
// delivers it before every pass).
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_system.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "network/improv_ble.h"
#include "network/setup_portal.h"

SerialT Serial;
WiFiT WiFi;

namespace canary {
namespace net {
// setup_portal.h as the glue reaches it: the join is the test's to steer.
static SetupPortalJoin g_join = SetupPortalJoin::Idle;
static bool g_portal_takes = true;   // setup_portal_submit_join()'s verdict
static int g_submits = 0;
static int g_acks = 0;
static std::string g_submitted_ssid;
bool setup_portal_join_in_flight() { return g_join == SetupPortalJoin::Connecting; }
bool setup_portal_submit_join(const char* ssid, const char*, uint32_t) {
  ++g_submits;
  g_submitted_ssid = ssid ? ssid : "";
  if (!g_portal_takes) return false;
  g_join = SetupPortalJoin::Connecting;
  return true;
}
SetupPortalJoin setup_portal_join_state() { return g_join; }
const char* setup_portal_fail_reason() { return g_join == SetupPortalJoin::Fail ? "wrong password" : ""; }
void setup_portal_note_acked(uint32_t) { ++g_acks; }
}  // namespace net
}  // namespace canary

using namespace canary::net;

// A forked child's checks are named with what it played (a reset reason).
static const char* g_ctx = "";
static int g_fail = 0, g_checks = 0;
#define CHECK(c, m)                                   \
  do {                                                \
    ++g_checks;                                       \
    if (!(c)) {                                       \
      ++g_fail;                                       \
      printf("FAIL %d: %s%s\n", __LINE__, g_ctx, m);  \
    }                                                 \
  } while (0)

// The board's hooks, as the families set them (canary-sense and
// canary-vision main.cpp): an identify blink and a hub store, whose verdict
// is the test's.
static int g_stored = 0;
static bool g_store_ok = true;
static bool store(const hub_handoff::HubHandoff&) {
  ++g_stored;
  return g_store_ok;
}
static bool (*g_store_hook)(const hub_handoff::HubHandoff&) = store;
static int g_identified = 0;
static void identify() { ++g_identified; }

static NimBLECharacteristic* find(const char* uuid) {
  if (!NimBLEDevice::g_server) return nullptr;
  for (auto* s : NimBLEDevice::g_server->svcs)
    for (auto* c : s->chars)
      if (c->uuid == uuid) return c;
  return nullptr;
}

// The radio's part, in the order each NimBLE release calls the glue: 2.x
// hands one NimBLEConnInfo; 1.4.x calls the plain overload, then the one
// with the connection descriptor (NimBLEServer.cpp and
// NimBLECharacteristic.cpp handleGapEvent). The disconnect reason is the
// host's (BLE_HS_ERR_HCI_BASE + the HCI code); the glue reads none.
constexpr int REASON_REMOTE_USER = 0x213;   // the phone hung up
constexpr int REASON_TIMEOUT = 0x208;       // supervision timeout: it walked away
constexpr int REASON_LOCAL = 0x216;         // this side asked (ble_gap_terminate)
static std::vector<uint16_t> g_live;        // the links the stack holds open
static bool live(uint16_t h) { return std::find(g_live.begin(), g_live.end(), h) != g_live.end(); }
static void connect(uint16_t h) {
  NimBLEServer* sv = NimBLEDevice::g_server;
  g_live.push_back(h);
  // A central connecting ends the connectable advert (the controller leaves
  // the advertising state; the host then reports the connect).
  NimBLEDevice::getAdvertising()->on = false;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  NimBLEConnInfo i;
  i.m_desc.conn_handle = h;
  sv->cb->onConnect(sv, i);
#else
  ble_gap_conn_desc d{};
  d.conn_handle = h;
  sv->cb->onConnect(sv);
  sv->cb->onConnect(sv, &d);
#endif
}
static void disconnect(uint16_t h, int reason = REASON_REMOTE_USER) {
  NimBLEServer* sv = NimBLEDevice::g_server;
  g_live.erase(std::remove(g_live.begin(), g_live.end(), h), g_live.end());
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  NimBLEConnInfo i;
  i.m_desc.conn_handle = h;
  sv->cb->onDisconnect(sv, i, reason);
#else
  (void)reason;   // 1.4.x hands the descriptor, not the reason
  ble_gap_conn_desc d{};
  d.conn_handle = h;
  sv->cb->onDisconnect(sv);
  sv->cb->onDisconnect(sv, &d);
  // 1.4.3's server then puts the last advert back on air itself
  // (m_advertiseOnDisconnect, true by default; the glue leaves it so).
  NimBLEDevice::getAdvertising()->start();
#endif
}
// The host task's turn: the disconnect event for each link the glue asked
// the stack to drop (NimBLEServer::disconnect() only starts the terminate).
// A second ask for a link already going raises no second event.
static size_t g_drops_seen = 0;
static void host_turn() {
  NimBLEServer* sv = NimBLEDevice::g_server;
  if (!sv) return;
  while (g_drops_seen < sv->disconnects.size()) {
    const uint16_t h = sv->disconnects[g_drops_seen++];
    if (live(h)) disconnect(h, REASON_LOCAL);
  }
}
// The stack stores the written bytes as the characteristic's value, then
// calls the callback.
static void gatt_write(const char* uuid, uint16_t h, const std::string& bytes, bool encrypted = true) {
  NimBLECharacteristic* c = find(uuid);
  if (!c || !c->cb) {
    printf("no characteristic %s\n", uuid);
    exit(2);
  }
  c->value = bytes;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  NimBLEConnInfo i;
  i.m_desc.conn_handle = h;
  i.m_desc.sec_state.encrypted = encrypted ? 1 : 0;
  c->cb->onWrite(c, i);
#else
  ble_gap_conn_desc d{};
  d.conn_handle = h;
  d.sec_state.encrypted = encrypted ? 1 : 0;
  c->cb->onWrite(c);
  c->cb->onWrite(c, &d);
#endif
}
static void wifi(uint16_t h, bool encrypted = true) {
  uint8_t f[improv::MAX_COMMAND_LEN] = {};
  const size_t n = improv::build_wifi_settings(f, sizeof f, "home", "password1");
  gatt_write(improv::RPC_COMMAND_UUID, h, std::string((const char*)f, n), encrypted);
}
static void command(uint16_t h, improv::Command cmd) {
  uint8_t f[8] = {};
  const size_t n = improv::build_command(f, sizeof f, cmd, nullptr, 0);
  gatt_write(improv::RPC_COMMAND_UUID, h, std::string((const char*)f, n));
}
static const std::string DOC = "{\"host\":\"hub.local\",\"port\":1883}";
static void hub(uint16_t h, bool encrypted = true) { gatt_write(hub_handoff::HUB_UUID, h, DOC, encrypted); }

// One loop pass, after the host task's turn; millis() reads the same clock.
static uint32_t g_now = 1000;
static bool g_no_creds = true;   // tick's and begin()'s `no_credentials`
static void tick(uint32_t step = 50) {
  host_turn();
  g_now += step;
  g_millis = g_now;
  improv_ble::tick(g_now, g_no_creds);
}
static void tick_to(uint32_t t, uint32_t step = 100) {
  while ((int32_t)(t - g_now) > 0) tick((int32_t)(t - g_now) < (int32_t)step ? t - g_now : step);
}

// The glue's lines since mark(), and the hand-off lines among them.
static size_t g_mark = 0;
static void mark() { g_mark = Serial.lines.size(); }
static int lines_with(const char* needle, std::string* last = nullptr) {
  int n = 0;
  for (size_t i = g_mark; i < Serial.lines.size(); ++i)
    if (Serial.lines[i].find(needle) != std::string::npos) {
      ++n;
      if (last) *last = Serial.lines[i];
    }
  return n;
}
static int lines_equal(const std::string& want) {
  int n = 0;
  for (size_t i = g_mark; i < Serial.lines.size(); ++i)
    if (Serial.lines[i] == want) ++n;
  return n;
}
// The "[IMPROV] Wi-Fi settings <word>" lines since mark().
static int wifi_lines(const char* word) { return lines_equal(std::string("[IMPROV] Wi-Fi settings ") + word); }

// The last byte the error characteristic notified, and every one since a mark.
static int last_error() {
  NimBLECharacteristic* c = find(improv::ERROR_STATE_UUID);
  if (!c || c->notified.empty() || c->notified.back().size() != 1) return -1;
  return (uint8_t)c->notified.back()[0];
}
static size_t errors_mark() {
  NimBLECharacteristic* c = find(improv::ERROR_STATE_UUID);
  return c ? c->notified.size() : 0;
}
static std::vector<int> errors_since(size_t from) {
  std::vector<int> out;
  NimBLECharacteristic* c = find(improv::ERROR_STATE_UUID);
  if (!c) return out;
  for (size_t i = from; i < c->notified.size(); ++i)
    out.push_back(c->notified[i].size() == 1 ? (uint8_t)c->notified[i][0] : -1);
  return out;
}
static bool only(const std::vector<int>& v, improv::Error e) { return v.size() == 1 && v[0] == (int)e; }

// Every result notified since `from` on the RPC result characteristic, as
// its command and strings (improv::parse_result, the client's walk).
struct Result {
  improv::Command cmd;
  std::vector<std::string> strings;
};
static std::vector<Result> results_since(size_t from) {
  std::vector<Result> out;
  NimBLECharacteristic* c = find(improv::RPC_RESULT_UUID);
  if (!c) return out;
  for (size_t i = from; i < c->notified.size(); ++i) {
    Result r{improv::Command::Unknown, {}};
    const std::string& f = c->notified[i];
    const int n = improv::parse_result((const uint8_t*)f.data(), f.size(), r.cmd,
                                       [&](int, const char* s, size_t len) { r.strings.emplace_back(s, len); });
    if (n < 0) r.cmd = improv::Command::Unknown;
    out.push_back(r);
  }
  return out;
}
static size_t results_mark() {
  NimBLECharacteristic* c = find(improv::RPC_RESULT_UUID);
  return c ? c->notified.size() : 0;
}
static bool closes_empty(const std::vector<Result>& r) {
  return r.size() == 1 && r[0].cmd == improv::Command::GetWifiNetworks && r[0].strings.empty();
}

static improv_ble::Identity identity() {
  improv_ble::Identity id{};
  id.firmware_name = "canary-sense";
  id.firmware_version = "0";
  id.hardware = "h";
  id.device_name = "d";
  id.adv_name = "Sense-AB12";
  id.reach_url = nullptr;   // as both families: no page to send the phone to
  id.identify = identify;
  id.require_encryption = true;
  id.store_hub = g_store_hook;
  return id;
}
static void boot() {
  if (!improv_ble::begin(identity(), g_no_creds, g_now)) {
    printf("begin failed\n");
    exit(2);
  }
  tick();
}

// Runs `fn` in a child process, a boot of its own (begin() runs once a
// process), and answers how it ended: 0 when it ran checks and all passed.
static int in_child(void (*fn)()) {
  fflush(stdout);
  const pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    exit(2);
  }
  if (pid == 0) {
    g_checks = 0;
    g_fail = 0;
    fn();
    fflush(stdout);
    _exit(g_checks == 0 ? 3 : (g_fail ? 1 : 0));
  }
  int st = 0;
  if (waitpid(pid, &st, 0) != pid) {
    perror("waitpid");
    exit(2);
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static void expect_refused(const char* what, int stored_before) {
  std::string last;
  const int lines = lines_with("hub hand-off", &last);
  char m[256];
  snprintf(m, sizeof m, "%s: Rejected", what);
  CHECK(improv_ble::hub_status() == hub_handoff::Status::Rejected, m);
  snprintf(m, sizeof m, "%s: nothing stored", what);
  CHECK(g_stored == stored_before, m);
  snprintf(m, sizeof m, "%s: exactly one hand-off line (got %d: '%s')", what, lines, last.c_str());
  CHECK(lines == 1 && last.find("refused") != std::string::npos, m);
}
static void expect_stored(const char* what, int stored_before) {
  std::string last;
  const int lines = lines_with("hub hand-off", &last);
  char m[256];
  snprintf(m, sizeof m, "%s: Stored", what);
  CHECK(improv_ble::hub_status() == hub_handoff::Status::Stored, m);
  snprintf(m, sizeof m, "%s: stored once", what);
  CHECK(g_stored == stored_before + 1, m);
  snprintf(m, sizeof m, "%s: one stored line (got %d: '%s')", what, lines, last.c_str());
  CHECK(lines == 1 && last.find("stored") != std::string::npos, m);
}

// ── Scenarios ────────────────────────────────────────────────────────────

static void drop() {
  boot();
  connect(1); tick();                         // link N
  wifi(1); tick();
  CHECK(improv_ble::state() == improv::State::Provisioning, "N's Wi-Fi accepted, joining");
  disconnect(1, REASON_TIMEOUT); tick();      // N walks away mid-join
  connect(2); tick();                         // N+1 connects: the door is still on air
  mark(); hub(2); tick();
  expect_refused("N+1's HUB, mid-join", 0);
  g_join = SetupPortalJoin::Success; tick();
  CHECK(improv_ble::state() == improv::State::Provisioned, "joined");
  CHECK(g_acks == 1, "the verdict over the door is acked to the portal once");
  mark(); hub(2); tick();
  expect_refused("N+1's HUB, joined", 0);
  mark(); hub(9); tick();                     // a handle that is not the connected link's
  expect_refused("a foreign handle, joined", 0);
}

static void nodrop() {
  boot();
  connect(1); tick();
  wifi(1); tick();
  CHECK(improv_ble::state() == improv::State::Provisioning, "N's Wi-Fi accepted, joining");
  CHECK(g_submits == 1 && g_submitted_ssid == "home", "the portal got N's join");
  mark(); hub(1); tick();
  expect_stored("N's HUB, mid-join", 0);
  g_join = SetupPortalJoin::Success; tick();
  CHECK(improv_ble::state() == improv::State::Provisioned, "joined");
  mark(); hub(1); tick();
  expect_stored("N's HUB, joined", 1);
}

static void race() {
  // WIFI_SETTINGS lands, N drops and N+1 connects and writes HUB, all
  // before the loop task's turn: the stamp is the host task's.
  boot();
  connect(1); tick();
  wifi(1); disconnect(1); connect(2); hub(2);
  mark(); tick();
  CHECK(improv_ble::state() == improv::State::Provisioning, "N's Wi-Fi, taken late, still joins");
  expect_refused("N+1's HUB in the same pass", 0);
  g_join = SetupPortalJoin::Success; tick();
  mark(); hub(2); tick();
  expect_refused("N+1's HUB, joined", 0);
}

static void secondwifi() {
  boot();
  connect(1); tick();
  mark(); wifi(1); tick();
  CHECK(wifi_lines("accepted, joining") == 1 && wifi_lines("refused") == 0,
        "N's Wi-Fi: one 'accepted, joining' line");
  disconnect(1); connect(2); tick();
  mark(); wifi(2); tick();                    // N+1's own credentials mid-join: refused
  CHECK(wifi_lines("accepted, joining") == 0, "N+1's refused Wi-Fi mid-join: no 'accepted, joining' line");
  CHECK(wifi_lines("refused") == 1, "N+1's refused Wi-Fi mid-join: one 'Wi-Fi settings refused' line");
  CHECK(last_error() == (int)improv::Error::InvalidRpc, "N+1's refused Wi-Fi mid-join: InvalidRpc published");
  CHECK(improv_ble::state() == improv::State::Provisioning, "still N's join");
  CHECK(g_submits == 1, "no second join reaches the portal");
  mark(); hub(2); tick();
  expect_refused("N+1 after its refused Wi-Fi, mid-join", 0);
  g_join = SetupPortalJoin::Success; tick();
  mark(); hub(2); tick();
  expect_refused("N+1 after its refused Wi-Fi, joined", 0);
}

static void wrap() {
  // N sends the Wi-Fi and drops; then 2^16 more links come and go: none
  // of them, at 256 or 65536 reconnects, is N again.
  boot();
  connect(1); tick();
  wifi(1); tick();
  disconnect(1); tick();
  int taken = 0;
  for (unsigned i = 1; i <= 65536u; ++i) {
    connect(2);
    if (i == 255 || i == 256 || i == 257 || i == 65535 || i == 65536) {
      const int before = g_stored;
      hub(2); tick();
      if (g_stored != before || improv_ble::hub_status() != hub_handoff::Status::Rejected) {
        ++taken;
        printf("taken at reconnect %u\n", i);
      }
    }
    disconnect(2);
  }
  CHECK(taken == 0, "no reconnect comes round to N (255, 256, 257, 65535, 65536)");
}

static void beside() {
  // A second link that slipped in BEFORE the provisioning one, beside it:
  // its HUB write must not borrow the newer link's generation.
  boot();
  connect(2); tick();                         // F, first
  connect(1); tick();                         // N, beside it, the latest connect
  wifi(1); tick();
  CHECK(improv_ble::state() == improv::State::Provisioning, "N's Wi-Fi accepted");
  mark(); hub(2); tick();
  expect_refused("F's HUB beside N, mid-join", 0);
  mark(); hub(1); tick();
  expect_stored("N's HUB, mid-join", 0);
}

static void portalrefused() {
  // The core accepts the credentials, the portal refuses to start the join
  // (a wizard join is testing): the write's word is "refused", the session
  // is back to Authorized with UnableToConnect, and there is no join to
  // hand a hub over for.
  boot();
  g_portal_takes = false;
  connect(1); tick();
  mark(); wifi(1); tick();
  CHECK(g_submits == 1, "the portal was asked once");
  CHECK(wifi_lines("refused") == 1 && wifi_lines("accepted, joining") == 0,
        "the portal's refusal: one 'Wi-Fi settings refused' line");
  CHECK(improv_ble::state() == improv::State::Authorized, "back to Authorized");
  CHECK(last_error() == (int)improv::Error::UnableToConnect, "UnableToConnect published");
  mark(); hub(1); tick();
  expect_refused("HUB after the portal refused the join", 0);
  CHECK(lines_with("no join this session") == 1, "refused as no join");
}

static void joinfail() {
  // The join ran and the portal says it failed (a wrong password): the
  // phone hears UnableToConnect at once, the door stays open for a retry,
  // and there is no join to hand a hub over for until one succeeds.
  boot();
  connect(1); tick();
  wifi(1); tick();
  CHECK(improv_ble::state() == improv::State::Provisioning, "joining");
  mark();
  const size_t e0 = errors_mark();
  const size_t r0 = results_mark();
  g_join = SetupPortalJoin::Fail; tick();
  CHECK(improv_ble::state() == improv::State::Authorized, "a wrong password: back to Authorized at once");
  CHECK(only(errors_since(e0), improv::Error::UnableToConnect), "UnableToConnect published, once");
  CHECK(lines_equal("[IMPROV] Join failed: wrong password") == 1, "one 'Join failed' line with the portal's reason");
  CHECK(results_since(r0).empty(), "no WIFI_SETTINGS result: there is no address to send");
  CHECK(g_acks == 0, "no verdict over the door to ack to the portal");
  mark(); hub(1); tick();
  expect_refused("HUB after the failed join", 0);
  CHECK(lines_with("no join this session") == 1, "refused as no join");
  // The person retries, past the cooldown, and this time it joins.
  tick(3000);
  mark(); wifi(1); tick();
  CHECK(improv_ble::state() == improv::State::Provisioning && g_submits == 2, "the retry starts a second join");
  g_join = SetupPortalJoin::Success; tick();
  CHECK(improv_ble::state() == improv::State::Provisioned && g_acks == 1, "the retry joined");
  mark(); hub(1); tick();
  expect_stored("the retry's HUB", 0);
}

static void nostore_board() {
  // Identity::store_hub null: the board takes no hand-off.
  g_store_hook = nullptr;
  boot();
  connect(1); tick();
  wifi(1); tick();
  mark(); hub(1); tick();
  CHECK(improv_ble::hub_status() == hub_handoff::Status::Rejected, "no store_hub: Rejected");
  CHECK(lines_equal("[IMPROV] hub hand-off refused: this board takes none") == 1,
        "one 'this board takes none' line");
  CHECK(g_stored == 0, "nothing stored");
}

static void storerefused() {
  // First a board with no store at all, in a boot of its own.
  CHECK(in_child(nostore_board) == 0, "a board with no store_hub takes no hand-off");
  // The board's store refuses (its readback did not hold the record): the
  // phone reads Rejected, and the line says the store refused, not the gate.
  boot();
  connect(1); tick();
  wifi(1); tick();
  g_store_ok = false;
  mark(); hub(1); tick();
  CHECK(g_stored == 1, "the board's store was asked");
  CHECK(improv_ble::hub_status() == hub_handoff::Status::Rejected, "the store refused: Rejected");
  CHECK(lines_equal("[IMPROV] hub hand-off not stored (the store refused)") == 1, "one 'not stored' line");
  CHECK(lines_with("hub hand-off stored") == 0, "no 'stored' line");
  NimBLECharacteristic* hb = find(hub_handoff::HUB_UUID);
  CHECK(hb && hb->value == std::string(1, (char)hub_handoff::Status::Rejected), "HUB reads back Rejected");
  // The phone writes again and the store takes it.
  g_store_ok = true;
  mark(); hub(1); tick();
  expect_stored("the next write, the store taking it", 1);
}

static void unencrypted() {
  // The characteristics ask the stack for encryption; the handlers check
  // again, so a write the stack let through unencrypted is refused too.
  boot();
  NimBLECharacteristic* cmd = find(improv::RPC_COMMAND_UUID);
  NimBLECharacteristic* hb = find(hub_handoff::HUB_UUID);
  CHECK(cmd && (cmd->props & WRITE_ENC), "the command write asks for encryption");
  CHECK(hb && (hb->props & WRITE_ENC) && (hb->props & READ_ENC), "HUB asks for encryption both ways");
  connect(1); tick();
  mark(); wifi(1, /*encrypted=*/false); tick();
  CHECK(lines_equal("[IMPROV] write on an unencrypted link refused") == 1, "one unencrypted-write line");
  CHECK(last_error() == (int)improv::Error::NotAuthorized, "NotAuthorized published");
  CHECK(improv_ble::state() == improv::State::Authorized, "the door still waits for credentials");
  CHECK(g_submits == 0, "nothing reached the portal");
  CHECK(wifi_lines("accepted, joining") == 0 && wifi_lines("refused") == 0, "no Wi-Fi settings verdict");
  mark(); hub(1, /*encrypted=*/false); tick();
  CHECK(improv_ble::hub_status() == hub_handoff::Status::Rejected, "the unencrypted HUB: Rejected");
  CHECK(g_stored == 0, "the unencrypted HUB: nothing stored");
  CHECK(lines_equal("[IMPROV] hub hand-off on an unencrypted link refused") == 1, "one unencrypted-HUB line");
  CHECK(hb && hb->value == std::string(1, (char)hub_handoff::Status::Rejected),
        "HUB reads back the status byte, not the document");
  // The same link, encrypted now, is heard.
  wifi(1); tick();
  CHECK(improv_ble::state() == improv::State::Provisioning && g_submits == 1, "the encrypted write joins");
}

static void commands() {
  boot();
  NimBLECharacteristic* caps = find(improv::CAPABILITIES_UUID);
  CHECK(caps && caps->value.size() == 1 &&
            (uint8_t)caps->value[0] == (improv::CAP_IDENTIFY | improv::CAP_DEVICE_INFO | improv::CAP_SCAN_WIFI),
        "the capabilities byte: identify (the board has a blink), device info, the scan");
  connect(1); tick();
  size_t e0 = errors_mark();
  command(1, improv::Command::Identify); tick();
  CHECK(g_identified == 1, "IDENTIFY blinks once");
  CHECK(only(errors_since(e0), improv::Error::None), "IDENTIFY: None published");
  size_t r0 = results_mark();
  command(1, improv::Command::GetDeviceInfo); tick();
  std::vector<Result> r = results_since(r0);
  CHECK(r.size() == 1 && r[0].cmd == improv::Command::GetDeviceInfo &&
            r[0].strings == std::vector<std::string>({"canary-sense", "0", "h", "d"}),
        "GET_DEVICE_INFO: one result, firmware, version, hardware, device name");
  e0 = errors_mark();
  command(1, (improv::Command)0x42); tick();
  CHECK(only(errors_since(e0), improv::Error::UnknownRpc), "a command this device does not speak: UnknownRpc");
  // An empty write, and one a byte past the longest legal command (a
  // checksummed GET_DEVICE_INFO with 99 bytes of data; the stack takes up to
  // BLE_ATT_ATTR_MAX_LEN): each is InvalidRpc, never silence, and nothing
  // is answered or joined.
  e0 = errors_mark();
  gatt_write(improv::RPC_COMMAND_UUID, 1, std::string()); tick();
  CHECK(only(errors_since(e0), improv::Error::InvalidRpc), "an empty write: InvalidRpc");
  uint8_t data[improv::MAX_COMMAND_LEN - 2] = {};
  uint8_t big[improv::MAX_COMMAND_LEN + 1] = {};
  const size_t n = improv::build_command(big, sizeof big, improv::Command::GetDeviceInfo, data, sizeof data);
  CHECK(n == improv::MAX_COMMAND_LEN + 1, "the oversize frame is one byte past MAX_COMMAND_LEN");
  e0 = errors_mark();
  r0 = results_mark();
  gatt_write(improv::RPC_COMMAND_UUID, 1, std::string((const char*)big, n)); tick();
  CHECK(only(errors_since(e0), improv::Error::InvalidRpc), "an oversize write: InvalidRpc");
  CHECK(results_since(r0).empty(), "an oversize write: the device info is not answered");
  CHECK(g_submits == 0 && improv_ble::state() == improv::State::Authorized, "nothing joined");
}

static void scan() {
  boot();
  connect(1); tick();
  // A sweep the driver takes: nothing sent while it runs, then one row a
  // pass (an empty SSID is skipped: that pass sends nothing), then the empty
  // result that closes the list, and the sweep's memory handed back.
  size_t from = results_mark();
  command(1, improv::Command::GetWifiNetworks); tick();
  CHECK(WiFi.scans_started == 1, "GET_WIFI_NETWORKS starts one sweep");
  tick();
  CHECK(results_since(from).empty(), "nothing sent while the sweep runs");
  WiFi.scan_done({{"home", -40, WIFI_AUTH_WPA2_PSK}, {"", -50, WIFI_AUTH_OPEN}, {"cafe", -70, WIFI_AUTH_OPEN}});
  for (int i = 0; i < 6; ++i) tick();
  std::vector<Result> r = results_since(from);
  CHECK(r.size() == 3, "two rows and the closing empty result");
  CHECK(r.size() == 3 && r[0].cmd == improv::Command::GetWifiNetworks && r[0].strings.size() == 3 &&
            r[0].strings[0] == "home" && r[0].strings[1] == "-40" && r[0].strings[2] == "WPA2",
        "row 1: home, -40, WPA2");
  CHECK(r.size() == 3 && r[1].strings.size() == 3 && r[1].strings[0] == "cafe" && r[1].strings[1] == "-70" &&
            r[1].strings[2] == "NO",
        "row 2: cafe, -70, NO (the empty SSID skipped)");
  CHECK(r.size() == 3 && r[2].cmd == improv::Command::GetWifiNetworks && r[2].strings.empty(),
        "the list closes with an empty result");
  CHECK(WiFi.count("scanDelete") == 1, "the sweep is handed back once, after the last row");

  // A sweep the driver refuses to start: the empty answer at once.
  WiFi.driver_takes = false;
  from = results_mark();
  const int asked = WiFi.count("scanNetworks");
  command(1, improv::Command::GetWifiNetworks); tick();
  CHECK(WiFi.count("scanNetworks") == asked + 1 && WiFi.scans_started == 1 && closes_empty(results_since(from)),
        "a sweep the driver refuses: one empty result at once");
  WiFi.driver_takes = true;

  // A sweep that never reports.
  from = results_mark();
  command(1, improv::Command::GetWifiNetworks); tick();
  const uint32_t t0 = g_now;
  int deleted = WiFi.count("scanDelete");
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // Core 3.3.8 waits 60 s for the driver; the glue gives up at its own 30 s.
  tick_to(t0 + 29900);
  CHECK(results_since(from).empty(), "29.9 s into a silent sweep: still waiting");
  tick_to(t0 + 30200);
  CHECK(closes_empty(results_since(from)) && WiFi.count("scanDelete") == deleted + 1,
        "past the glue's 30 s: the sweep handed back, one empty result");
#else
  // Core 2.0.17 gives up at max_ms_per_chan * 20, 6 s for the glue's 300 ms,
  // and answers WIFI_SCAN_FAILED: the glue closes the list on that.
  tick_to(t0 + 5900);
  CHECK(results_since(from).empty(), "5.9 s into a silent sweep: still waiting");
  tick_to(t0 + 6200);
  CHECK(closes_empty(results_since(from)) && WiFi.count("scanDelete") == deleted + 1,
        "the core gave up at 6 s: the sweep handed back, one empty result");
#endif
  tick_to(g_now + 40000, 1000);
  CHECK(results_since(from).size() == 1, "and nothing after it");

  // The portal's /scan harvests the sweep first (it reads any sweep that
  // completed and hands it back): the glue finds nothing to read and closes
  // the list empty.
  from = results_mark();
  command(1, improv::Command::GetWifiNetworks); tick();
  WiFi.scan_done({{"home", -40, WIFI_AUTH_WPA2_PSK}});
  WiFi.scanDelete();   // the portal's harvest
  tick();
  CHECK(closes_empty(results_since(from)), "a sweep the portal harvested first: one empty result closes the list");
  tick(); tick();
  CHECK(results_since(from).size() == 1, "and nothing after it");

  // A sweep already running (the portal's own): the glue hands it back
  // before it asks for its own.
  WiFi.scanNetworks(/*async=*/true, false, false, 300);
  const int started = WiFi.scans_started;
  WiFi.calls.clear();
  from = results_mark();
  command(1, improv::Command::GetWifiNetworks); tick();
  CHECK(WiFi.calls.size() >= 3 && WiFi.calls[0] == "scanComplete" && WiFi.calls[1] == "scanDelete" &&
            WiFi.calls[2] == "scanNetworks",
        "a sweep already running is handed back before the glue's own scanNetworks()");
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  CHECK(WiFi.scans_started == started + 1, "core 3: scanDelete() ended it, so a fresh sweep starts");
#else
  CHECK(WiFi.scans_started == started, "core 2: scanDelete() leaves the scanning bit, so the sweep in flight is joined");
#endif
  // More rows than SCAN_MAX (20): twenty of them, then the close.
  std::vector<WiFiT::Row> many;
  for (int i = 0; i < 21; ++i) many.push_back({"net" + std::to_string(i), -40 - i, WIFI_AUTH_WPA2_PSK});
  WiFi.scan_done(many);
  for (int i = 0; i < 30; ++i) tick();
  r = results_since(from);
  CHECK(r.size() == 21 && r[19].strings.size() == 3 && r[19].strings[0] == "net19" && r[20].strings.empty(),
        "21 networks heard: the first 20 rows, then the empty close");

  // Under a join in flight: no sweep (it can fail WiFi.begin), the empty
  // answer at once.
  wifi(1); tick();
  CHECK(improv_ble::state() == improv::State::Provisioning, "a join in flight");
  const int asked2 = WiFi.count("scanNetworks");
  from = results_mark();
  command(1, improv::Command::GetWifiNetworks); tick();
  CHECK(WiFi.count("scanNetworks") == asked2 && closes_empty(results_since(from)),
        "mid-join: no sweep, one empty result");
}

static void linger() {
  // The glue's own drops: a client that asks for nothing for 180 s, and the
  // provisioning link 20 s after its verdict. Each is asked of the stack
  // once; after the stack's disconnect event the right advert is on air.
  boot();
  const uint8_t beacon[4] = {0xFF, 0xFF, 0x01, 0x02};
  improv_ble::advertise(beacon, sizeof beacon);
  NimBLEServer* sv = NimBLEDevice::g_server;
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  connect(1); tick();
  const uint32_t up = g_now;
  mark();
  tick_to(up + 179000, 1000);
  CHECK(sv->disconnects.empty(), "179 s of silence: still connected");
  tick_to(up + 181000, 1000);
  CHECK(sv->disconnects == std::vector<uint16_t>({1}), "past 180 s: the glue asks the stack to drop that link, once");
  CHECK(lines_equal("[IMPROV] link dropped (idle)") == 1, "one 'link dropped (idle)' line");
  CHECK(!live(1), "the link is gone");
  CHECK(adv->on && adv->mode == BLE_GAP_CONN_MODE_UND && adv->adv.name == "Sense-AB12" &&
            adv->scan.services.size() == 1 && adv->scan.services[0] == improv::SERVICE_UUID,
        "the door back on air, connectable: its window still runs");
  // The provisioning link: dropped 20 s after the verdict.
  connect(2); tick();
  wifi(2); tick();
  g_join = SetupPortalJoin::Success; tick();
  CHECK(improv_ble::state() == improv::State::Provisioned, "joined");
  const uint32_t joined = g_now;
  mark();
  tick_to(joined + 19000, 1000);
  CHECK(sv->disconnects.size() == 1, "19 s after the verdict: still connected");
  tick_to(joined + 25000, 1000);
  CHECK(sv->disconnects.size() == 2 && sv->disconnects[1] == 2,
        "20 s after the verdict: the glue drops the provisioning link, once");
  CHECK(lines_equal("[IMPROV] link dropped (joined, verdict delivered)") == 1,
        "one 'link dropped (joined, verdict delivered)' line");
  CHECK(!live(2), "the link is gone");
  CHECK(adv->on && adv->mode == BLE_GAP_CONN_MODE_NON &&
            adv->adv.mfg == std::string((const char*)beacon, sizeof beacon) && adv->adv.name.empty() &&
            adv->scan.services.empty(),
        "the beacon alone back on air, not connectable: a provisioned device offers no door");
}

// ── The first-boot window across a reset ──────────────────────────────────

// improv_ble.cpp's RTC record, as it lays it out: the stand-in's
// RTC_NOINIT_ATTR (esp_attr.h) puts it alone in the rtc_noinit section.
struct WindowRecord {
  uint32_t magic;
  uint32_t used_ms;
};
#if defined(__APPLE__)
extern char rtc_begin[] __asm("section$start$__DATA$rtc_noinit");
extern char rtc_end[] __asm("section$end$__DATA$rtc_noinit");
#else
extern "C" char __start_rtc_noinit[];
extern "C" char __stop_rtc_noinit[];
#define rtc_begin __start_rtc_noinit
#define rtc_end __stop_rtc_noinit
#endif
static WindowRecord rtc_read() {
  WindowRecord r;
  memcpy(&r, rtc_begin, sizeof r);
  return r;
}
static void rtc_write(const WindowRecord& r) { memcpy(rtc_begin, &r, sizeof r); }

static const uint32_t WINDOW_MS = improv::Timing{}.first_boot_window_ms;   // no IMPROV_FIRST_BOOT_WINDOW_MS here
static WindowRecord g_prev{};                      // what the previous boot left
static esp_reset_reason_t g_why = ESP_RST_POWERON;
static int g_pipe = -1;

static void previous_boot() {
  // A power-on, the window running ten seconds, saved as it goes.
  g_esp_reset_reason = ESP_RST_POWERON;
  boot();
  tick_to(g_now + 10000, 1000);
  const WindowRecord r = rtc_read();
  CHECK(write(g_pipe, &r, sizeof r) == (ssize_t)sizeof r, "the record handed to the next boot");
}

static void after_reset_keeps() {
  rtc_write(g_prev);
  g_esp_reset_reason = g_why;
  const uint32_t t0 = g_now;
  mark();
  boot();
  char want[96];
  snprintf(want, sizeof want, "[IMPROV] first-boot window: %lu s already spent before this restart",
           (unsigned long)(g_prev.used_ms / 1000));
  CHECK(lines_equal(want) == 1, "the spent window is carried: one 'already spent' line");
  CHECK(improv_ble::setup_open(), "the door opens for what is left");
  tick_to(t0 + 5000, 1000);
  const WindowRecord r = rtc_read();
  CHECK(r.magic == g_prev.magic && r.used_ms >= g_prev.used_ms + 2000, "and the record keeps counting from it");
  const uint32_t ends = t0 - g_prev.used_ms + WINDOW_MS;
  tick_to(ends - 1000, 1000);
  CHECK(improv_ble::setup_open(), "open until what was left runs out");
  tick_to(ends + 1000, 1000);
  CHECK(!improv_ble::setup_open() && lines_equal("[IMPROV] door shut (window over)") == 1,
        "shut when what was left runs out");
}

// begin() without the first tick, so the record reads as begin() left it.
static void begin_only() {
  if (!improv_ble::begin(identity(), g_no_creds, g_now)) {
    printf("begin failed\n");
    exit(2);
  }
}

static void after_reset_clears() {
  rtc_write(g_prev);
  g_esp_reset_reason = g_why;
  const uint32_t t0 = g_now;
  mark();
  begin_only();
  const WindowRecord r = rtc_read();
  CHECK(r.magic == 0 && r.used_ms == 0, "the record the previous boot left is cleared");
  CHECK(lines_with("already spent") == 0, "no 'already spent' line");
  tick_to(t0 - g_prev.used_ms + WINDOW_MS + 1000, 1000);
  CHECK(improv_ble::setup_open(), "the whole window runs: open past where a carried one would shut");
  tick_to(t0 + WINDOW_MS + 1000, 1000);
  CHECK(!improv_ble::setup_open(), "shut when the whole window runs out");
}

static void garbage_record() {
  // A software reset over RTC bytes that do not carry the glue's magic (an
  // image that never wrote one): they are not a spent window.
  WindowRecord junk = g_prev;
  junk.magic = ~g_prev.magic;
  rtc_write(junk);
  g_esp_reset_reason = ESP_RST_SW;
  const uint32_t t0 = g_now;
  mark();
  boot();
  CHECK(lines_with("already spent") == 0, "no 'already spent' line");
  tick_to(t0 - g_prev.used_ms + WINDOW_MS + 1000, 1000);
  CHECK(improv_ble::setup_open(), "the whole window runs");
}

static void credentials_stored() {
  // A software reset with credentials stored: no window to carry, and the
  // record is cleared so a later factory reset starts a fresh one.
  rtc_write(g_prev);
  g_esp_reset_reason = ESP_RST_SW;
  g_no_creds = false;
  mark();
  boot();
  CHECK(lines_with("already spent") == 0, "no 'already spent' line");
  CHECK(!improv_ble::setup_open(), "credentials stored: the door shut");
  const WindowRecord r = rtc_read();
  CHECK(r.magic == 0 && r.used_ms == 0, "the record cleared on the first pass");
}

static void reset() {
  CHECK((size_t)(rtc_end - rtc_begin) >= sizeof(WindowRecord), "the glue's RTC record is in rtc_noinit");
  int fds[2];
  if (pipe(fds) != 0) {
    perror("pipe");
    exit(2);
  }
  g_pipe = fds[1];
  g_ctx = "previous boot: ";
  const int prev = in_child(previous_boot);
  close(fds[1]);
  const ssize_t got = read(fds[0], &g_prev, sizeof g_prev);
  close(fds[0]);
  g_ctx = "";
  CHECK(prev == 0 && got == (ssize_t)sizeof g_prev, "the previous boot ran and left its record");
  // Ten seconds and a tick spent; the record is at most one save (2 s) behind.
  CHECK(g_prev.magic != 0 && g_prev.used_ms <= 10050 && g_prev.used_ms + 2000 >= 10050,
        "the previous boot's record: the window's spent time, saved every two seconds");
  struct Row {
    esp_reset_reason_t why;
    const char* name;
    bool keeps;
  };
  static const Row ROWS[] = {
      {ESP_RST_SW, "ESP_RST_SW: ", true},
      {ESP_RST_PANIC, "ESP_RST_PANIC: ", true},
      {ESP_RST_INT_WDT, "ESP_RST_INT_WDT: ", true},
      {ESP_RST_TASK_WDT, "ESP_RST_TASK_WDT: ", true},
      {ESP_RST_WDT, "ESP_RST_WDT: ", true},
      {ESP_RST_DEEPSLEEP, "ESP_RST_DEEPSLEEP: ", true},
      {ESP_RST_POWERON, "ESP_RST_POWERON: ", false},
      {ESP_RST_BROWNOUT, "ESP_RST_BROWNOUT: ", false},
      {ESP_RST_UNKNOWN, "ESP_RST_UNKNOWN: ", false},
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
      {ESP_RST_USB, "ESP_RST_USB: ", false},
      {ESP_RST_JTAG, "ESP_RST_JTAG: ", false},
      {ESP_RST_PWR_GLITCH, "ESP_RST_PWR_GLITCH: ", false},
#endif
  };
  for (const Row& row : ROWS) {
    g_why = row.why;
    g_ctx = row.name;
    const int rc = in_child(row.keeps ? after_reset_keeps : after_reset_clears);
    g_ctx = "";
    char m[128];
    snprintf(m, sizeof m, "%s%s", row.name, row.keeps ? "the spent window is carried" : "the window starts over");
    CHECK(rc == 0, m);
  }
  g_ctx = "a record without the magic: ";
  const int junk = in_child(garbage_record);
  g_ctx = "credentials stored: ";
  const int creds = in_child(credentials_stored);
  g_ctx = "";
  CHECK(junk == 0, "a record without the magic is no spent window");
  CHECK(creds == 0, "with credentials stored the record is cleared");
}

static void companion() {
  // The companion service (HUB) is fail-soft: 1.4.x starts each service in
  // its own start(), and a refused companion leaves the Wi-Fi door working
  // with no hand-off this boot. 2.x starts every service with the server's
  // one start(), so there is nothing per service to refuse.
#if ESP_ARDUINO_VERSION_MAJOR < 3
  NimBLEService::g_refuse_start = hub_handoff::SERVICE_UUID;
#endif
  mark();
  boot();
#if ESP_ARDUINO_VERSION_MAJOR < 3
  CHECK(lines_with("[IMPROV] companion service refused") == 1, "1.4.x: the companion's start refused, one line");
  CHECK(find(hub_handoff::HUB_UUID) == nullptr, "and its service removed: no HUB to write");
#else
  CHECK(lines_with("companion service") == 0, "2.x: no per-service refusal line");
  CHECK(find(hub_handoff::HUB_UUID) != nullptr, "2.x: HUB registered beside the door");
#endif
  CHECK(NimBLEDevice::g_server->started, "the server is started");
  CHECK(improv_ble::active() && improv_ble::setup_open(), "the Wi-Fi door is open");
  connect(1); tick();
  wifi(1); tick();
  CHECK(improv_ble::state() == improv::State::Provisioning, "and takes the credentials");
  g_join = SetupPortalJoin::Success; tick();
  CHECK(improv_ble::state() == improv::State::Provisioned, "and joins");
}

static void startfail() {
  // The GATT start refused (2.x: the server's start(); 1.4.x: the Improv
  // service's): begin() says so and the door never opens; the carrier's
  // next beacon goes on air alone, not connectable.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  NimBLEServer::g_start_ok = false;
  const char* word = "[IMPROV] GATT start refused";
#else
  NimBLEService::g_refuse_start = improv::SERVICE_UUID;
  const char* word = "[IMPROV] service registration refused";
#endif
  mark();
  CHECK(!improv_ble::begin(identity(), true, g_now), "begin() refused");
  CHECK(lines_with(word) == 1, "one refusal line");
  CHECK(!improv_ble::active() && !improv_ble::setup_open(), "the door is not open");
  tick();
  const uint8_t beacon[4] = {0xFF, 0xFF, 0x01, 0x02};
  improv_ble::advertise(beacon, sizeof beacon);
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  CHECK(adv->on && adv->mode == BLE_GAP_CONN_MODE_NON, "the beacon is on air, not connectable");
  CHECK(adv->adv.mfg == std::string((const char*)beacon, sizeof beacon), "with the carrier's bytes");
  CHECK(adv->adv.name.empty() && adv->scan.services.empty(), "and no door: no name, no Improv service");
}

static void bring_up() {
  // NimBLE not up (no beacon brought it up): the glue brings it up under
  // the device id, or refuses begin() when it cannot.
  NimBLEDevice::g_init = false;
  NimBLEDevice::g_init_works = false;
  CHECK(!improv_ble::begin(identity(), true, g_now), "NimBLE will not come up: begin() refused");
  CHECK(NimBLEDevice::g_server == nullptr, "no server registered");
  NimBLEDevice::g_init_works = true;
  CHECK(improv_ble::begin(identity(), true, g_now), "NimBLE comes up: begin() works");
  CHECK(NimBLEDevice::g_init_name == "d", "brought up under the device id");
  tick();
  CHECK(improv_ble::active() && improv_ble::setup_open(), "the door is open");
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  CHECK(adv->on && adv->mode == BLE_GAP_CONN_MODE_UND, "connectable on air");
  CHECK(adv->adv.name == "Sense-AB12", "under the door's name");
  CHECK(adv->scan.services.size() == 1 && adv->scan.services[0] == improv::SERVICE_UUID,
        "the scan response names the Improv service");
}

struct Scenario {
  const char* name;
  void (*run)();
};
static const Scenario SCENARIOS[] = {
    {"drop", drop},
    {"nodrop", nodrop},
    {"race", race},
    {"secondwifi", secondwifi},
    {"wrap", wrap},
    {"beside", beside},
    {"portalrefused", portalrefused},
    {"joinfail", joinfail},
    {"storerefused", storerefused},
    {"unencrypted", unencrypted},
    {"commands", commands},
    {"scan", scan},
    {"linger", linger},
    {"reset", reset},
    {"companion", companion},
    {"startfail", startfail},
    {"init", bring_up},
};

int main(int argc, char** argv) {
  const std::string sc = argc > 1 ? argv[1] : "";
  if (sc == "--list") {
    bool first = true;
    for (const Scenario& s : SCENARIOS) {
      printf("%s%s", first ? "" : " ", s.name);
      first = false;
    }
    printf("\n");
    return 0;
  }
  const Scenario* pick = nullptr;
  for (const Scenario& s : SCENARIOS)
    if (sc == s.name) pick = &s;
  if (!pick) {
    printf("usage: %s --list | <scenario> (one of --list's names)\n", argv[0]);
    return 2;
  }
  pick->run();
  if (getenv("DUMP"))
    for (auto& l : Serial.lines) printf("  | %s\n", l.c_str());
  if (g_checks == 0) {
    printf("%s: no checks ran\n", sc.c_str());
    return 1;
  }
  printf("%s (core %d): %d checks, %d FAILED\n", sc.c_str(), ESP_ARDUINO_VERSION_MAJOR, g_checks, g_fail);
  return g_fail ? 1 : 0;
}
