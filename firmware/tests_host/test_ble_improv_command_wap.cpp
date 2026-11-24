// canary-wap's Bluetooth setup door: the command handler's verdicts and the
// health line each WIFI_SETTINGS write leaves (sweep F471).
//
// handle_command() — with same_addr() and note_link_wrote(), the helpers it
// calls — is cut verbatim out of
// firmware/projects/canary-wap/arduino/canary_wap/ble_improv.cpp by
// cut_functions.awk (the Makefile writes BLEIMPW_INC), so the file's NimBLE
// half never compiles here. It runs over the sketch's staged improv_core.h
// (byte-identical to firmware/common/network/improv_core.h,
// check_improv_sync.sh) and its health_log.h, whose log_health()
// declaration the stand-in below must match to link. The rest are
// stand-ins: the mailbox the host-task write callback fills, publish_error()
// / publish_state() (which record the byte each would notify),
// answer_device_info() / start_scan(), and the sketch's bridges
// (ble_improv_submit_join() among them, its verdict the test's).
//
// The rule it holds is the one test_improv_glue.cpp's secondwifi holds on
// the common glue: the WIFI_SETTINGS line's level and words are this
// write's verdict, not the session's state. A second write mid-join is
// refused while the first one's join keeps the session Provisioning, so
// it logs WARNING "refused" and counts in s_refused, never INFO "accepted,
// joining"; an accepted write whose join the sketch would not start has
// left Provisioning and logs "refused" too.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "health_log.h"    // the sketch's, for log_health()'s declaration
#include "improv_core.h"   // the sketch's staged copy

// NimBLE's address (host/ble_hs.h's ble_addr_t): what the mailbox carries.
typedef struct {
  uint8_t type;
  uint8_t val[6];
} ble_addr_t;

// ── The sketch's side ──────────────────────────────────────────────────────

struct LogLine {
  LogLevel level;
  LogCategory category;
  std::string message;
  bool detail_null;
};
static std::vector<LogLine> g_log;
void log_health(LogLevel level, LogCategory category, const char* message, const char* detail) {
  g_log.push_back({level, category, message ? message : "", detail == nullptr});
}
void health_log(LogLevel level, LogCategory category, const char* message) {
  log_health(level, category, message, nullptr);
}

static bool g_submit_takes = true;   // ble_improv_submit_join()'s verdict
static int g_submits = 0;
static std::string g_submit_ssid, g_submit_pass;
static int g_activity = 0;
static int g_identify = 0;
bool ble_improv_submit_join(const char* ssid, const char* password) {
  ++g_submits;
  g_submit_ssid = ssid ? ssid : "";
  g_submit_pass = password ? password : "";
  return g_submit_takes;
}
void ble_improv_note_activity() { ++g_activity; }
void ble_improv_identify() { ++g_identify; }

// ── ble_improv.cpp's file statics, as handle_command() names them ─────────

namespace ble_improv {
namespace {

namespace improv = canary::net::improv;
using improv::Command;
using improv::Error;
using improv::Parse;
using improv::State;

constexpr uint16_t NO_CONN = 0xFFFF;

improv::Session s_session;
improv::Timing s_timing;

uint8_t  s_rx[improv::MAX_COMMAND_LEN];
volatile size_t   s_rx_len = 0;
volatile uint16_t s_rx_conn = NO_CONN;
ble_addr_t        s_rx_addr = {};
volatile bool     s_rx_pending = false;
volatile bool     s_rx_unencrypted = false;

uint16_t   s_door_conn = NO_CONN;
ble_addr_t s_door_addr = {};

uint16_t   s_provisioning_conn = NO_CONN;
ble_addr_t s_provisioning_addr = {};

uint32_t s_accepted = 0;
uint32_t s_refused = 0;

// What each publish would notify (the byte on the characteristic).
std::vector<uint8_t> g_published_state, g_published_error;
void publish_state() { g_published_state.push_back((uint8_t)s_session.state); }
void publish_error() { g_published_error.push_back((uint8_t)s_session.error); }
int g_device_info = 0, g_scans = 0;
void answer_device_info() { ++g_device_info; }
void start_scan(uint32_t) { ++g_scans; }

#include BLEIMPW_INC

}  // namespace
}  // namespace ble_improv

using namespace ble_improv;

static int g_fail = 0, g_checks = 0;
#define CHECK(c, m)                                   \
  do {                                                \
    ++g_checks;                                       \
    if (!(c)) {                                       \
      ++g_fail;                                       \
      printf("FAIL %d: %s\n", __LINE__, m);           \
    }                                                 \
  } while (0)

static uint32_t g_now = 5000;

// A fresh boot of the door: the session as init() begins it, the counters
// and the stand-ins' records cleared.
static void fresh(bool no_credentials) {
  s_timing = improv::Timing{};
  improv::session_begin(s_session, no_credentials, g_now, s_timing, 0);
  s_accepted = s_refused = 0;
  s_door_conn = s_provisioning_conn = NO_CONN;
  memset(&s_door_addr, 0, sizeof s_door_addr);
  memset(&s_provisioning_addr, 0, sizeof s_provisioning_addr);
  g_log.clear();
  g_published_state.clear();
  g_published_error.clear();
  g_submits = 0;
  g_submit_takes = true;
}

static ble_addr_t addr_of(uint8_t tail) {
  ble_addr_t a = {};
  a.type = 1;
  for (int i = 0; i < 6; ++i) a.val[i] = (uint8_t)(0xA0 + i);
  a.val[5] = tail;
  return a;
}

// What CommandCb::onWrite leaves in the mailbox for one WIFI_SETTINGS
// write, then the loop task's turn (tick() calls handle_command() when the
// mailbox is pending).
static void wifi_write(uint16_t conn, uint8_t addr_tail, bool encrypted = true) {
  s_rx_conn = conn;
  s_rx_addr = addr_of(addr_tail);
  if (!encrypted) {
    s_rx_unencrypted = true;
    s_rx_len = 0;
  } else {
    const size_t n = improv::build_wifi_settings(s_rx, sizeof s_rx, "home", "password1");
    s_rx_len = n;
  }
  s_rx_pending = true;
  g_log.clear();
  g_published_state.clear();
  g_published_error.clear();
  handle_command(g_now);
}

static bool mailbox_handed_back() {
  if (s_rx_pending) return false;
  for (uint8_t b : s_rx)
    if (b != 0) return false;
  return true;
}

// Exactly one line for the write, at `level`, with `message`.
static void expect_line(const char* row, LogLevel level, const char* message) {
  char m[320];
  snprintf(m, sizeof m, "%s: one health line (got %zu)", row, g_log.size());
  CHECK(g_log.size() == 1, m);
  if (g_log.size() != 1) {
    for (const LogLine& l : g_log) printf("  line: %d '%s'\n", (int)l.level, l.message.c_str());
    return;
  }
  const LogLine& l = g_log[0];
  snprintf(m, sizeof m, "%s: '%s' (got '%s')", row, message, l.message.c_str());
  CHECK(l.message == message, m);
  snprintf(m, sizeof m, "%s: level %s (got %s)", row, log_level_name(level), log_level_name(l.level));
  CHECK(l.level == level, m);
  snprintf(m, sizeof m, "%s: Bluetooth, no detail", row);
  CHECK(l.category == SCV_CAT_BLUETOOTH && l.detail_null, m);
}

static const char* ACCEPTED = "Setup door: Wi-Fi settings accepted, joining";
static const char* REFUSED = "Setup door: Wi-Fi settings refused";

static void accepted_then_mid_join() {
  fresh(/*no_credentials=*/true);
  CHECK(s_session.state == State::Authorized, "a fresh no-credentials door takes credentials");

  // The first write: accepted, the join starts on its link.
  wifi_write(1, 0x11);
  expect_line("accepted first write", SCV_LOG_INFO, ACCEPTED);
  CHECK(s_session.state == State::Provisioning, "accepted first write: Provisioning");
  CHECK(s_accepted == 1 && s_refused == 0, "accepted first write: s_accepted 1, s_refused 0");
  CHECK(g_submits == 1 && g_submit_ssid == "home" && g_submit_pass == "password1",
        "accepted first write: the sketch's join got the credentials");
  CHECK(s_provisioning_conn == 1 && s_provisioning_addr.val[5] == 0x11,
        "accepted first write: the join's link recorded");
  CHECK(!g_published_error.empty() && g_published_error.back() == (uint8_t)Error::None,
        "accepted first write: None published");
  CHECK(!g_published_state.empty() && g_published_state.back() == (uint8_t)State::Provisioning,
        "accepted first write: Provisioning published");
  CHECK(mailbox_handed_back(), "accepted first write: mailbox wiped and handed back");

  // The same phone writes again before the verdict: refused, the join
  // keeps its link and stays in flight.
  g_now += 3000;
  wifi_write(1, 0x11);
  expect_line("second write mid-join, same link", SCV_LOG_WARNING, REFUSED);
  CHECK(s_refused == 1 && s_accepted == 1, "second write mid-join, same link: counted in s_refused");
  CHECK(s_session.state == State::Provisioning, "second write mid-join, same link: still the first join");
  CHECK(g_submits == 1, "second write mid-join, same link: no second join");
  CHECK(!g_published_error.empty() && g_published_error.back() == (uint8_t)Error::InvalidRpc,
        "second write mid-join, same link: InvalidRpc published");

  // Another phone at the door mid-join: refused the same way, and the join
  // is still the first link's.
  g_now += 3000;
  wifi_write(2, 0x22);
  expect_line("second write mid-join, another link", SCV_LOG_WARNING, REFUSED);
  CHECK(s_refused == 2 && s_accepted == 1, "second write mid-join, another link: counted in s_refused");
  CHECK(s_session.state == State::Provisioning, "second write mid-join, another link: still the first join");
  CHECK(s_provisioning_conn == 1 && s_provisioning_addr.val[5] == 0x11,
        "second write mid-join, another link: the join's link unchanged");
  CHECK(g_submits == 1, "second write mid-join, another link: no second join");
}

static void submit_refused_then_cooldown() {
  fresh(true);
  // The core accepts the write; the sketch will not start its join (a
  // wizard join testing, a scan lock held): back to Authorized.
  g_submit_takes = false;
  wifi_write(1, 0x11);
  expect_line("accepted write, the sketch refused its join", SCV_LOG_WARNING, REFUSED);
  CHECK(g_submits == 1, "the sketch was asked once");
  CHECK(s_refused == 1 && s_accepted == 0, "the sketch's refusal: counted in s_refused");
  CHECK(s_session.state == State::Authorized, "the sketch's refusal: back to Authorized");
  CHECK(!g_published_error.empty() && g_published_error.back() == (uint8_t)Error::UnableToConnect,
        "the sketch's refusal: UnableToConnect published");
  CHECK(s_provisioning_conn == NO_CONN, "the sketch's refusal: no join's link");

  // Straight away again: inside the cooldown, refused before the sketch.
  g_submit_takes = true;
  g_now += 10;
  wifi_write(1, 0x11);
  expect_line("a write inside the cooldown", SCV_LOG_WARNING, REFUSED);
  CHECK(g_submits == 1, "inside the cooldown: the sketch is not asked");
  CHECK(s_refused == 2, "inside the cooldown: counted in s_refused");
}

static void door_shut() {
  fresh(/*no_credentials=*/false);   // credentials stored, no tap: the door is shut
  wifi_write(1, 0x11);
  expect_line("a write to a shut door", SCV_LOG_WARNING, REFUSED);
  CHECK(g_submits == 0 && s_refused == 1 && s_accepted == 0, "a shut door: refused, nothing joined");
  CHECK(!g_published_error.empty() && g_published_error.back() == (uint8_t)Error::NotAuthorized,
        "a shut door: NotAuthorized published");
}

static void unencrypted() {
  fresh(true);
  wifi_write(1, 0x11, /*encrypted=*/false);
  expect_line("a write on an unencrypted link", SCV_LOG_WARNING,
              "Setup door: write on an unencrypted link refused");
  CHECK(!s_rx_unencrypted && !s_rx_pending, "the unencrypted mark and the mailbox handed back");
  CHECK(g_submits == 0 && s_refused == 1, "unencrypted: refused, nothing joined");
  CHECK(s_session.state == State::Authorized, "unencrypted: the door still takes credentials");
  CHECK(!g_published_error.empty() && g_published_error.back() == (uint8_t)Error::NotAuthorized,
        "unencrypted: NotAuthorized published");
}

int main() {
  accepted_then_mid_join();
  submit_refused_then_cooldown();
  door_shut();
  unencrypted();
  if (g_checks == 0) {
    printf("test_ble_improv_command_wap: no checks ran\n");
    return 1;
  }
  printf("test_ble_improv_command_wap: %d checks, %d FAILED\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
