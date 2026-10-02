// Host test: the owner's Chirp commands run on the loop task (sweep F111),
// against the REAL chirp_channel.cpp, compiled on the host over stubs/mesh_net
// (an ESP-NOW peer list and send log, an in-memory NVS, OpenSSL's Ed25519 and
// SHA-256) with the real airtime governor linked. chirp_channel.cpp is
// #included below.
//
// chirp_api.h's POST handlers called chirp_channel::enable, disable,
// send_chirp, confirm_chirp, dismiss_chirp, mute, unmute, set_relay_enabled
// and set_urgency_filter straight from esp_http_server's task, while
// chirp_channel::update() read and wrote the same session, cooldowns, recent
// chirps, mute and relay state on the loop task (canary_wap.ino calls it
// right after mesh_network::update(), which hands it the ESP-NOW frames), and
// chirp_channel.cpp takes no lock. Those functions are now internal to
// chirp_channel.cpp; a handler hands a Command to chirp_channel::submit(),
// which posts it to a ring (loop_command_ring.h, as the mesh does since F96)
// that update() drains on the loop task, and waits for its Result.
//
// The harness is one thread, so "the HTTP server's task" is a role the test
// plays: rest() sets host_sim::on_httpd_task and calls submit() the way a
// handler does. submit() waits in vTaskDelay, and the stub's vTaskDelay is
// where the test gives the loop task its turn (one update() pass, with
// on_httpd_task cleared). Every NVS write and ESP-NOW call made while
// on_httpd_task is set is counted (host_sim::httpd_side_effects): a command
// that ran on the handler's task shows up there. The handlers' own source is
// held by firmware/scripts/check_wap_loop_commands.py (ArduinoJson, which
// they build their answers with, is not on the host).
//
// The wall clock chirp_channel.cpp reads (time(), localtime()) is the test's
// (host_sim::wall_now, UTC), so night mode and an unsynced clock are chosen,
// not inherited from the machine running the test. nvs_store.h is replaced
// by the two calls chirp_channel.cpp makes (nvs_get_u8 / nvs_set_u8 on the
// chirp namespace), over the same in-memory NVS.
//
// Host-tested only: the stubs stand in for the radio, the flash and the
// scheduler; the Arduino compile is CI's (firmware.yml's canary-wap legs).
//
// Run: ./test_chirp_commands_wap [name]

#ifndef CHIRP_CHANNEL_CPP
#error "CHIRP_CHANNEL_CPP (absolute path to chirp_channel.cpp) must be defined"
#endif

// Everything chirp_channel.cpp and its stubs include, first, so the clock
// macros below rename only chirp_channel.cpp's own calls.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include "Arduino.h"
#include "Preferences.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The wall clock chirp_channel.cpp reads: synced (past MIN_UNIX_TIME), UTC.
namespace host_sim {
inline time_t wall_now = 1760000000;   // 2025-10-09 08:53:20 UTC: day, synced
inline struct tm wall_tm;
}  // namespace host_sim
inline time_t host_sim_time(time_t* out) {
  if (out != nullptr) *out = host_sim::wall_now;
  return host_sim::wall_now;
}
inline struct tm* host_sim_localtime(const time_t* t) {
  return gmtime_r(t, &host_sim::wall_tm);
}

// nvs_store.h's two chirp-namespace calls, over the stub NVS (whose writes
// count against the handler's task while it is played).
#define SECURACV_NVS_STORE_H
inline bool nvs_get_u8(const char* key, uint8_t* out_val) {
  Preferences p;
  p.begin("chirp", true);
  if (!p.isKey(key)) return false;
  *out_val = p.getUChar(key, 0);
  return true;
}
inline bool nvs_set_u8(const char* key, uint8_t val) {
  Preferences p;
  p.begin("chirp", false);
  return p.putUChar(key, val) == 1;
}

#define time(p) host_sim_time(p)
#define localtime(p) host_sim_localtime(p)
// The firmware file is held to the device build's warnings, not to this
// Makefile's -Wextra -Wpedantic -Werror (see test_mesh_address_wap.cpp);
// this test's own code keeps all of them.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include CHIRP_CHANNEL_CPP
#pragma GCC diagnostic pop
#undef time
#undef localtime

#include "http_status_line.h"   // the status line a chirp not-run answer sends

// The sketch provides this on a device (canary_wap.ino).
std::vector<std::string> g_health;
void health_log(LogLevel, LogCategory, const char* message) { g_health.push_back(message); }

namespace chirp_commands {

int g_checks = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)

namespace cc = chirp_channel;
namespace lcr = loop_command_ring;

const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Helpers ─────────────────────────────────────────────────────────────

// A device powering up: RAM gone, flash kept unless `wipe`. chirp's file
// statics are reset to what a boot leaves, then the real init() runs, as
// canary_wap.ino's setup() does (the channel stays disabled).
void boot(bool wipe = true) {
  if (wipe) host_sim::nvs->clear();
  *host_sim::espnow = host_sim::EspNow();
  host_sim::wall_now = 1760000000;
  cc::g_state = cc::CHIRP_DISABLED;
  cc::g_initialized = false;
  cc::g_relay_enabled = true;
  cc::g_urgency_filter = cc::CHIRP_URG_INFO;
  memset(&cc::g_cooldown, 0, sizeof(cc::g_cooldown));
  cc::g_session_start_ms = 0;
  cc::g_last_presence_ms = 0;
  cc::g_last_chirp_sent_ms = 0;
  cc::g_muted = false;
  cc::g_mute_until_ms = 0;
  cc::g_commands = decltype(cc::g_commands)();
  host_sim::httpd_side_effects = 0;
  host_sim::on_httpd_task = false;
  host_sim::on_task_delay = nullptr;
  CHECK(cc::init());
  CHECK(cc::g_state == cc::CHIRP_DISABLED);
}

// What one REST call saw while its handler waited.
struct Rest {
  lcr::Wait wait = lcr::Wait::kBusy;
  cc::Result r = {};
  unsigned sleeps = 0;          // vTaskDelay calls the handler made
  unsigned loop_turns = 0;      // update() passes the loop task ran meanwhile
  uint32_t waited_ms = 0;
  // Before the loop task's first turn: the channel's state, mute and relay
  // flags, and the ESP-NOW frames sent so far.
  cc::ChirpState state_before_turn = cc::CHIRP_INITIALIZING;
  bool muted_before_turn = false;
  bool relay_before_turn = false;
  size_t sent_before_turn = 0;
};

// The REST handler submits `cmd` (the HTTP server's task), and the loop task
// gets one update() pass on the handler's `turn_at`-th sleep (never, when 0).
Rest rest(const cc::Command& cmd, unsigned turn_at = 1) {
  Rest out;
  const uint32_t start = host_sim::now_ms;
  host_sim::on_task_delay = [&](uint32_t ms) {
    host_sim::now_ms += ms;
    ++out.sleeps;
    if (out.sleeps == 1) {
      out.state_before_turn = cc::g_state;
      out.muted_before_turn = cc::g_muted;
      out.relay_before_turn = cc::g_relay_enabled;
      out.sent_before_turn = host_sim::espnow->sent.size();
    }
    if (turn_at != 0 && out.sleeps == turn_at) {
      host_sim::on_httpd_task = false;   // the loop task's turn
      cc::update();
      ++out.loop_turns;
      host_sim::on_httpd_task = true;
    }
  };
  host_sim::on_httpd_task = true;
  out.wait = cc::submit(cmd, &out.r);
  host_sim::on_httpd_task = false;
  host_sim::on_task_delay = nullptr;
  out.waited_ms = host_sim::now_ms - start;
  return out;
}

cc::Command cmd_of(cc::CommandType t) { return cc::make_command(t); }

cc::Command send_of(cc::ChirpTemplate tpl, cc::ChirpUrgency urg = cc::CHIRP_URG_INFO) {
  cc::Command c = cc::make_command(cc::CHIRP_CMD_SEND);
  c.template_id = tpl;
  c.urgency = urg;
  c.detail = cc::DETAIL_NONE;
  c.ttl_minutes = 15;
  return c;
}

cc::Command nonce_cmd(cc::CommandType t, const uint8_t nonce[8]) {
  cc::Command c = cc::make_command(t);
  memcpy(c.nonce, nonce, 8);
  return c;
}

// The ESP-NOW broadcasts sent since `from`, as chirp message types.
std::vector<uint8_t> sent_types(size_t from = 0) {
  std::vector<uint8_t> types;
  const auto& sent = host_sim::espnow->sent;
  for (size_t i = from; i < sent.size(); ++i) {
    CHECK(memcmp(sent[i].to.data(), BROADCAST, 6) == 0);
    CHECK(sent[i].bytes.size() >= sizeof(cc::ChirpHeader));
    const cc::ChirpHeader* h = reinterpret_cast<const cc::ChirpHeader*>(sent[i].bytes.data());
    CHECK(h->magic == cc::CHIRP_MAGIC);
    types.push_back(h->msg_type);
  }
  return types;
}

// The channel turned on through its own command (the loop task's turn on
// the first sleep), with the presence requirement met when `present`: ten
// minutes on, and the loop task's pass that sends the presence beacon then
// due.
void enabled_channel(bool present = true) {
  const Rest e = rest(cmd_of(cc::CHIRP_CMD_ENABLE));
  CHECK(e.wait == lcr::Wait::kDone && e.r.ok);
  if (present) {
    host_sim::now_ms += cc::PRESENCE_REQUIRED_MS;
    cc::update();
  }
}

// A chirp a neighbor sent, as handle_witness() stores it: another session's
// pubkey, nonce `nonce`.
void neighbor_chirp(const uint8_t nonce[8]) {
  cc::ReceivedChirp c;
  memset(&c, 0, sizeof(c));
  memcpy(c.nonce, nonce, 8);
  memset(c.sender_pubkey, 0x5A, sizeof(c.sender_pubkey));
  c.template_id = cc::TPL_INFRA_POWER_OUT;
  c.received_ms = host_sim::now_ms;
  cc::g_recent_chirps[cc::g_recent_chirp_count++] = c;
}

// ── A command waits for the loop task ───────────────────────────────────

// POST /api/chirp/enable on a fresh device, whose channel is disabled (as
// every device boots): nothing changes while the handler waits; the loop
// task's update() pass (its drain comes before the disabled channel's early
// return) makes the session and sends the first presence beacon; the
// handler answers with that session's emoji.
void test_enable_runs_on_the_loop_task() {
  boot();
  const Rest r = rest(cmd_of(cc::CHIRP_CMD_ENABLE));
  CHECK(r.wait == lcr::Wait::kDone);
  CHECK(r.loop_turns == 1);
  CHECK(r.state_before_turn == cc::CHIRP_DISABLED);   // nothing moved before the turn
  CHECK(r.sent_before_turn == 0);
  CHECK(r.r.ok);
  CHECK(cc::g_state == cc::CHIRP_ACTIVE);
  CHECK(r.r.session_emoji[0] != '\0');
  CHECK(strcmp(r.r.session_emoji, cc::get_session_emoji()) == 0);
  CHECK(sent_types() == std::vector<uint8_t>{cc::CHIRP_MSG_PRESENCE});
  CHECK(host_sim::httpd_side_effects == 0);
  CHECK(host_sim::mux_depth == 0);
  std::printf("PASS enable_runs_on_the_loop_task\n");
}

// Every command, one REST call each: the state each changes is untouched
// until the loop task's turn, every frame and NVS write is the loop task's,
// and each answer is what the command did, read on the loop task.
void test_every_command_runs_on_the_loop_task() {
  boot();
  enabled_channel();
  host_sim::espnow->sent.clear();
  host_sim::httpd_side_effects = 0;

  // A send: the witness frame goes out on the loop task's turn, and the
  // cooldown starts there.
  Rest r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.state_before_turn == cc::CHIRP_ACTIVE && r.sent_before_turn == 0);
  CHECK(cc::g_state == cc::CHIRP_COOLDOWN);
  CHECK(r.r.cooldown_tier == 1 && r.r.refusal == cc::SEND_REFUSED_NONE);
  CHECK(sent_types() == std::vector<uint8_t>{cc::CHIRP_MSG_WITNESS});

  // A neighbor's chirp, confirmed: the signed ACK is the loop task's.
  const uint8_t n1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  neighbor_chirp(n1);
  size_t before = host_sim::espnow->sent.size();
  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, n1));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.sent_before_turn == before);
  CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_ACK});

  // An unknown nonce: confirm and dismiss both answer false, and send nothing.
  const uint8_t unknown[8] = {9, 9, 9, 9, 9, 9, 9, 9};
  before = host_sim::espnow->sent.size();
  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, unknown));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);
  r = rest(nonce_cmd(cc::CHIRP_CMD_DISMISS, unknown));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);
  CHECK(host_sim::espnow->sent.size() == before);

  // Dismissed: marked on the loop task's turn, with its signed suppress vote.
  r = rest(nonce_cmd(cc::CHIRP_CMD_DISMISS, n1));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(cc::g_recent_chirps[0].dismissed);
  CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_SUPPRESS_VOTE});

  // Mute: a duration the channel does not offer is refused; 30 min mutes.
  before = host_sim::espnow->sent.size();
  cc::Command m = cmd_of(cc::CHIRP_CMD_MUTE);
  m.duration_minutes = 7;
  r = rest(m);
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && !cc::g_muted);
  m.duration_minutes = 30;
  r = rest(m);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(!r.muted_before_turn && cc::g_muted && cc::g_state == cc::CHIRP_MUTED);
  CHECK(cc::g_mute_until_ms == host_sim::now_ms - r.waited_ms + 5 + 30u * 60000u);
  CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_MUTE});
  r = rest(cmd_of(cc::CHIRP_CMD_UNMUTE));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.muted_before_turn && !cc::g_muted && cc::g_state == cc::CHIRP_ACTIVE);

  // Settings: relay off and the urgent filter, stored (NVS) on the loop
  // task's turn; the answer is the settings as the command left them.
  cc::Command s = cmd_of(cc::CHIRP_CMD_SETTINGS);
  s.set_relay = true;
  s.relay_enabled = false;
  s.set_filter = true;
  s.urgency_filter = cc::CHIRP_URG_URGENT;
  r = rest(s);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.relay_before_turn && !cc::g_relay_enabled);
  CHECK(!r.r.relay_enabled && r.r.urgency_filter == cc::CHIRP_URG_URGENT);
  uint8_t v = 0xEE;
  CHECK(nvs_get_u8("chirp_relay", &v) && v == 0);
  CHECK(nvs_get_u8("chirp_filter", &v) && v == (uint8_t)cc::CHIRP_URG_URGENT);
  // Neither field: nothing changes, and the answer is the settings as they stand.
  r = rest(cmd_of(cc::CHIRP_CMD_SETTINGS));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(!r.r.relay_enabled && r.r.urgency_filter == cc::CHIRP_URG_URGENT);

  // Disable: the session is dropped on the loop task's turn.
  r = rest(cmd_of(cc::CHIRP_CMD_DISABLE));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.state_before_turn == cc::CHIRP_ACTIVE && cc::g_state == cc::CHIRP_DISABLED);
  CHECK(cc::get_session_emoji()[0] == '\0');

  CHECK(host_sim::httpd_side_effects == 0);
  CHECK(host_sim::mux_depth == 0);
  std::printf("PASS every_command_runs_on_the_loop_task\n");
}

// A send that does not go out names why, read on the loop task right after
// the attempt, in the order the send handler always checked: the channel
// off, then the presence requirement, then can_send_chirp() (the cooldown,
// and, as before, an unsynced wall clock), then night mode.
void test_a_refused_send_names_why() {
  boot();
  Rest r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);
  CHECK(r.r.refusal == cc::SEND_REFUSED_DISABLED);

  enabled_channel(/*present=*/false);
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_PRESENCE);

  host_sim::now_ms += cc::PRESENCE_REQUIRED_MS;
  host_sim::wall_now = cc::MIN_UNIX_TIME - 1;          // the clock not synced yet
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_COOLDOWN);
  CHECK(r.r.cooldown_remaining_ms == 0);
  host_sim::wall_now = 1760000000;

  // Night (23:00 UTC): a template not allowed at night is refused for it.
  host_sim::wall_now = 1760050800;                     // 2025-10-09 23:00:00 UTC
  r = rest(send_of(cc::TPL_INFRA_INTERNET_DOWN));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_NIGHT);
  host_sim::wall_now = 1760000000;

  r = rest(send_of(cc::TPL_INFRA_INTERNET_DOWN));      // day: it goes out
  CHECK(r.r.ok && r.r.cooldown_tier == 1);
  host_sim::now_ms += 60000;
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_COOLDOWN);
  CHECK(r.r.cooldown_tier == 1);
  CHECK(r.r.cooldown_remaining_ms == cc::COOLDOWN_TIER_1_MS - 60000 - 5);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_refused_send_names_why\n");
}

// ── The ring's edges, through the real submit() and update() ────────────

// The loop task never gets to it (busy, or the device still booting): after
// COMMAND_WAIT_MS the handler withdraws the command, answers that it did not
// run (with a zeroed Result), and no later pass runs it.
void test_a_command_the_loop_never_reaches_is_withdrawn() {
  boot();
  const Rest r = rest(cmd_of(cc::CHIRP_CMD_ENABLE), /*turn_at=*/0);
  CHECK(r.wait == lcr::Wait::kWithdrawn);
  CHECK(r.waited_ms >= cc::COMMAND_WAIT_MS && r.waited_ms < cc::COMMAND_WAIT_MS + 10);
  CHECK(!r.r.ok && r.r.session_emoji[0] == '\0');
  CHECK(cc::g_commands.queued() == 0);
  cc::update();
  cc::update();
  CHECK(cc::g_state == cc::CHIRP_DISABLED);
  CHECK(host_sim::espnow->sent.empty());
  CHECK(cc::not_run_status(r.wait) == 503);
  CHECK(strcmp(cc::not_run_error(r.wait), "chirp_timeout") == 0);
  CHECK(strcmp(http_status_line(503), "503 Service Unavailable") == 0);
  std::printf("PASS a_command_the_loop_never_reaches_is_withdrawn\n");
}

// Four commands already wait (other handlers'): the fifth is refused at once,
// before any wait, and never runs; the four run on the next pass, in the
// order they were posted.
void test_a_full_ring_answers_busy() {
  boot();
  enabled_channel();
  cc::Command m30 = cmd_of(cc::CHIRP_CMD_MUTE);
  m30.duration_minutes = 30;
  cc::Command m60 = cmd_of(cc::CHIRP_CMD_MUTE);
  m60.duration_minutes = 60;
  uint32_t t[cc::COMMAND_SLOTS];
  t[0] = cc::g_commands.post(m30);
  t[1] = cc::g_commands.post(cmd_of(cc::CHIRP_CMD_UNMUTE));
  t[2] = cc::g_commands.post(m60);
  cc::Command relay_off = cmd_of(cc::CHIRP_CMD_SETTINGS);
  relay_off.set_relay = true;
  relay_off.relay_enabled = false;
  t[3] = cc::g_commands.post(relay_off);
  for (uint32_t ticket : t) CHECK(ticket != 0);

  const Rest r = rest(cmd_of(cc::CHIRP_CMD_DISABLE));
  CHECK(r.wait == lcr::Wait::kBusy);
  CHECK(r.sleeps == 0 && r.loop_turns == 0);
  CHECK(cc::not_run_status(r.wait) == 409);
  CHECK(strcmp(cc::not_run_error(r.wait), "chirp_busy") == 0);
  CHECK(strcmp(http_status_line(409), "409 Conflict") == 0);

  const size_t before = host_sim::espnow->sent.size();
  cc::update();
  // Post order: muted for 30, unmuted, muted for 60 (the mute that stands),
  // relay off; the refused DISABLE never ran.
  CHECK(sent_types(before) ==
        (std::vector<uint8_t>{cc::CHIRP_MSG_MUTE, cc::CHIRP_MSG_MUTE}));
  CHECK(cc::g_muted && cc::g_state == cc::CHIRP_MUTED);
  CHECK(cc::g_mute_until_ms == host_sim::now_ms + 60u * 60000u);
  CHECK(!cc::g_relay_enabled);
  for (uint32_t ticket : t) {
    cc::Result res;
    CHECK(cc::g_commands.poll(ticket, &res) == lcr::Poll::kDone && res.ok);
  }
  CHECK(cc::g_commands.queued() == 0);
  std::printf("PASS a_full_ring_answers_busy\n");
}

// A command posted while the loop task is not draining (the handler's own
// first sleeps) runs on the first pass that comes, and its handler collects
// its own result, once.
void test_a_late_turn_still_answers() {
  boot();
  const Rest r = rest(cmd_of(cc::CHIRP_CMD_ENABLE), /*turn_at=*/50);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.sleeps == 50 && r.loop_turns == 1);
  CHECK(r.waited_ms == 50 * cc::COMMAND_POLL_MS);
  CHECK(cc::g_state == cc::CHIRP_ACTIVE);
  CHECK(cc::g_commands.queued() == 0);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_late_turn_still_answers\n");
}

struct Test {
  const char* name;
  void (*fn)();
};
const Test kTests[] = {
    {"enable_runs_on_the_loop_task", test_enable_runs_on_the_loop_task},
    {"every_command_runs_on_the_loop_task", test_every_command_runs_on_the_loop_task},
    {"a_refused_send_names_why", test_a_refused_send_names_why},
    {"a_command_the_loop_never_reaches_is_withdrawn", test_a_command_the_loop_never_reaches_is_withdrawn},
    {"a_full_ring_answers_busy", test_a_full_ring_answers_busy},
    {"a_late_turn_still_answers", test_a_late_turn_still_answers},
};

}  // namespace chirp_commands

int main(int argc, char** argv) {
  using namespace chirp_commands;
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
  std::printf("ALL %d chirp command checks PASSED (%d tests)\n", g_checks, ran);
  return 0;
}
