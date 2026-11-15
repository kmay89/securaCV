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
// The GET routes (sweep F138) read the same state: chirp_api.h's status,
// nearby and recent handlers called get_status(), get_nearby_devices() and
// get_recent_chirps() on the HTTP server's task while update() and the
// chirp frames mesh_network::update() hands it rewrote the session, the
// cooldowns and the tables (a row read mid-shift by the prune). They now
// read read_status(), read_nearby() and read_recent(): whole copies of what
// the loop task last published (loop_snapshot.h). The view tests drive real
// frames (a presence beacon, a signed witness, a signed confirmation) through
// dispatch_espnow_message(), and a "drain only" turn (Turn::kDrain) for what
// a read right after a POST's answer must already show. The harness is one
// thread: no test here shows a torn read on the old code, the evidence for
// whole copies is test_loop_snapshot.cpp's threads.
//
// A refused send names its reason (sweep F146): a wall clock not set yet is
// clock_unsynced, no longer a cooldown with 0 seconds left.
//
// A refused confirm names its reason, by name and status, and a dismiss says
// whether its signed suppress vote went out (sweep F174).
//
// The send cooldown is a timer, not a state (sweep F178): a mute no longer
// ends it, a send drained in the pass after it ran out goes out, and the
// status route reads it over at once and its last second as 1 s, not 0. A
// send made while muted leaves the channel muted, and a refused send names
// the check that refused it, read once, so a gate that opens at that moment
// is not answered with no reason (the reviewers of F178).
//
// The cooldown tiers reset after 24 hours with no send, not 24 hours after
// the first of a run, a disable and an enable keep them, and their count
// stops at 255 (sweep F193); the reset and the cooldown count across the
// uptime wrap, a mute and the votes beside the kept tiers leave them be,
// and the tiers let 9 sends out on a first day, then 6 a day.
//
// A presence beacon's age since the session's last send stops at 254
// minutes and stays there across uptime's wrap; it read 5 minutes 2^32 ms
// and 5 minutes after a send (sweep F417). And presence itself reads unmet
// for ten minutes every 2^32 ms of a session on without a break, a known
// limit that fails closed, held here as it is.
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

#include <algorithm>
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
// wall_step_s, when a test sets it, moves it on by that much after each
// read (as host_sim::millis_step_ms does the uptime clock).
namespace host_sim {
inline time_t wall_now = 1760000000;   // 2025-10-09 08:53:20 UTC: day, synced
inline time_t wall_step_s = 0;
inline struct tm wall_tm;
}  // namespace host_sim
inline time_t host_sim_time(time_t* out) {
  const time_t t = host_sim::wall_now;
  host_sim::wall_now += host_sim::wall_step_s;
  if (out != nullptr) *out = t;
  return t;
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
#if defined(__GNUC__) && !defined(__clang__)   // a GCC-only warning name
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
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
  host_sim::wall_step_s = 0;
  host_sim::millis_step_ms = 0;
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
  cc::g_status_view = decltype(cc::g_status_view)();     // nothing published yet
  cc::g_nearby_view = decltype(cc::g_nearby_view)();
  cc::g_recent_view = decltype(cc::g_recent_view)();
  cc::g_tables_changed = true;
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

// How much of a pass the loop task gets at its turn.
enum class Turn {
  kPass,    // a whole update() pass
  kDrain,   // only update()'s first statement, its drain of the commands:
            // on a device the handler answers as soon as the drain posts the
            // result, while the loop task may still be at the rest of the pass
};

// The REST handler submits `cmd` (the HTTP server's task), and the loop task
// gets one turn (`how`) on the handler's `turn_at`-th sleep (never, when 0).
Rest rest(const cc::Command& cmd, unsigned turn_at = 1, Turn how = Turn::kPass) {
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
      if (how == Turn::kPass) {
        cc::update();
      } else {
        cc::g_commands.drain(cc::run_command);
      }
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
  CHECK(cc::get_cooldown_remaining_ms() > 0 && cc::get_status().state == cc::CHIRP_COOLDOWN);
  CHECK(cc::g_state == cc::CHIRP_ACTIVE);              // a timer, not a stored state (F178)
  CHECK(r.r.cooldown_tier == 1 && r.r.refusal == cc::SEND_REFUSED_NONE);
  CHECK(sent_types() == std::vector<uint8_t>{cc::CHIRP_MSG_WITNESS});

  // A neighbor's chirp, confirmed: the signed ACK is the loop task's.
  const uint8_t n1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  neighbor_chirp(n1);
  size_t before = host_sim::espnow->sent.size();
  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, n1));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NONE);
  CHECK(r.sent_before_turn == before);
  CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_ACK});

  // An unknown nonce: confirm and dismiss both answer false, say so by name
  // (sweep F174), and send nothing.
  const uint8_t unknown[8] = {9, 9, 9, 9, 9, 9, 9, 9};
  before = host_sim::espnow->sent.size();
  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, unknown));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NOT_FOUND);
  r = rest(nonce_cmd(cc::CHIRP_CMD_DISMISS, unknown));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NOT_FOUND);
  CHECK(!r.r.vote_sent);
  CHECK(host_sim::espnow->sent.size() == before);

  // Dismissed: marked on the loop task's turn, with its signed suppress vote.
  r = rest(nonce_cmd(cc::CHIRP_CMD_DISMISS, n1));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && r.r.vote_sent);
  CHECK(r.r.confirm_refusal == cc::CONFIRM_REFUSED_NONE);
  CHECK(cc::g_recent_chirps[0].dismissed);
  CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_SUPPRESS_VOTE});

  // Mute: a duration the channel does not offer is refused; 30 min mutes.
  before = host_sim::espnow->sent.size();
  cc::Command m = cmd_of(cc::CHIRP_CMD_MUTE);
  m.duration_minutes = 7;
  r = rest(m);
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && !cc::g_muted);
  CHECK(r.r.mute_refusal == cc::MUTE_REFUSED_DURATION);
  CHECK(std::string(cc::mute_refusal_error(r.r.mute_refusal)) == "invalid_duration");
  CHECK(cc::mute_refusal_status(r.r.mute_refusal) == 200);   // as it always answered (F195)
  m.duration_minutes = 30;
  r = rest(m);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(!r.muted_before_turn && cc::g_muted && cc::g_state == cc::CHIRP_MUTED);
  CHECK(cc::g_mute_until_ms == host_sim::now_ms - r.waited_ms + 5 + 30u * 60000u);
  CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_MUTE});
  r = rest(cmd_of(cc::CHIRP_CMD_UNMUTE));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && r.r.mute_refusal == cc::MUTE_REFUSED_NONE);
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

// The send handler's "error" for a refusal (chirp_api.h answers
// send_refusal_error(r.refusal) and its message; nullptr: no reason named).
std::string error_of(cc::SendRefusal why) {
  const char* e = cc::send_refusal_error(why);
  return e != nullptr ? e : "(none)";
}

// A send that does not go out names why, read on the loop task right after
// the attempt, in the order the send handler always checked: the channel
// off, then the presence requirement, then can_send_chirp()'s two (the
// cooldown, then a wall clock not set yet), then night mode. The clock was
// answered as a cooldown with 0 seconds left (sweep F146); it has its own
// refusal now, clock_unsynced.
void test_a_refused_send_names_why() {
  boot();
  Rest r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);
  CHECK(r.r.refusal == cc::SEND_REFUSED_DISABLED);
  CHECK(error_of(r.r.refusal) == "chirp_disabled");

  enabled_channel(/*present=*/false);
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_PRESENCE);
  CHECK(error_of(r.r.refusal) == "presence_required");

  host_sim::now_ms += cc::PRESENCE_REQUIRED_MS;
  host_sim::wall_now = cc::MIN_UNIX_TIME - 1;          // the clock not set yet
  const size_t sent_before = host_sim::espnow->sent.size();
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_CLOCK_UNSYNCED);
  CHECK(error_of(r.r.refusal) == "clock_unsynced");
  CHECK(std::string(cc::send_refusal_message(r.r.refusal)).find("clock") != std::string::npos);
  CHECK(r.r.cooldown_remaining_ms == 0 && r.r.cooldown_tier == 0);
  CHECK(cc::get_cooldown_remaining_ms() == 0);         // no cooldown started
  for (uint8_t t : sent_types(sent_before)) CHECK(t != cc::CHIRP_MSG_WITNESS);   // no chirp went out
  host_sim::wall_now = cc::MIN_UNIX_TIME;              // the first second it counts as set
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(r.r.ok && r.r.cooldown_tier == 1);
  // Both at once (not on a device: its clock is only ever set forward, and a
  // cooldown needs a send the clock allowed; the order is the handler's): the
  // cooldown comes first, with its own time left.
  host_sim::now_ms += 60000;
  host_sim::wall_now = cc::MIN_UNIX_TIME - 1;
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_COOLDOWN);
  CHECK(error_of(r.r.refusal) == "cooldown");
  CHECK(r.r.cooldown_remaining_ms == cc::COOLDOWN_TIER_1_MS - 60000 - 5);
  CHECK(host_sim::httpd_side_effects == 0);
  boot();                                              // a fresh cooldown for the rest
  enabled_channel();
  host_sim::wall_now = 1760000000;

  // Night (23:00 UTC): a template not allowed at night is refused for it.
  host_sim::wall_now = 1760050800;                     // 2025-10-09 23:00:00 UTC
  r = rest(send_of(cc::TPL_INFRA_INTERNET_DOWN));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_NIGHT);
  CHECK(error_of(r.r.refusal) == "night_restricted");
  host_sim::wall_now = 1760000000;

  r = rest(send_of(cc::TPL_INFRA_INTERNET_DOWN));      // day: it goes out
  CHECK(r.r.ok && r.r.cooldown_tier == 1);
  host_sim::now_ms += 60000;
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_COOLDOWN);
  CHECK(r.r.cooldown_tier == 1);
  CHECK(r.r.cooldown_remaining_ms == cc::COOLDOWN_TIER_1_MS - 60000 - 5);
  CHECK(host_sim::httpd_side_effects == 0);

  // The answers the handler always sent are the ones it sends now, and a
  // send that went out names no reason.
  CHECK(error_of(cc::SEND_REFUSED_NONE) == "(none)" && cc::send_refusal_message(cc::SEND_REFUSED_NONE) == nullptr);
  CHECK(std::string(cc::send_refusal_message(cc::SEND_REFUSED_DISABLED)) == "Chirp channel is not enabled");
  CHECK(std::string(cc::send_refusal_message(cc::SEND_REFUSED_PRESENCE)) ==
        "Must be active for 10 minutes before sending");
  CHECK(std::string(cc::send_refusal_message(cc::SEND_REFUSED_COOLDOWN)) ==
        "Please wait before sending another chirp");
  CHECK(std::string(cc::send_refusal_message(cc::SEND_REFUSED_NIGHT)) ==
        "This template is not available during night hours (10pm-6am)");
  std::printf("PASS a_refused_send_names_why\n");
}

// ── What the owner sent reaches the command ─────────────────────────────

// POST /api/chirp/send's template, urgency, detail and TTL reach the witness
// frame the loop task signs and broadcasts. The dashboard's Info / Caution /
// Urgent choice is the urgency: a run_command() that dropped it would send
// every urgent alert as info. The other tests send INFO, no detail and 15
// minutes, which are also the defaults, so they cannot tell.
void test_a_send_carries_the_owners_fields() {
  boot();
  enabled_channel();
  const size_t before = host_sim::espnow->sent.size();
  cc::Command c = send_of(cc::TPL_INFRA_POWER_OUT, cc::CHIRP_URG_URGENT);
  c.detail = cc::DETAIL_STATUS_SPREADING;
  c.ttl_minutes = 30;
  const Rest r = rest(c);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(r.sent_before_turn == before);
  CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_WITNESS});
  const std::vector<uint8_t>& f = host_sim::espnow->sent[before].bytes;
  CHECK(f.size() >= sizeof(cc::ChirpHeader) + sizeof(cc::ChirpWitnessPayload));
  cc::ChirpWitnessPayload p;
  memcpy(&p, f.data() + sizeof(cc::ChirpHeader), sizeof p);
  CHECK(p.template_id == (uint8_t)cc::TPL_INFRA_POWER_OUT);
  CHECK(p.urgency == (uint8_t)cc::CHIRP_URG_URGENT);
  CHECK(p.detail_slot == (uint8_t)cc::DETAIL_STATUS_SPREADING);
  CHECK(p.ttl_minutes == 30);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_send_carries_the_owners_fields\n");
}

// POST /api/chirp/settings changes only the fields it names: the filter
// alone leaves the relay as it stands, and the relay alone leaves the
// filter. The answer is both, as the command left them.
void test_a_settings_post_changes_only_what_it_names() {
  boot();
  enabled_channel();
  CHECK(cc::g_relay_enabled && cc::g_urgency_filter == cc::CHIRP_URG_INFO);
  cc::Command f = cmd_of(cc::CHIRP_CMD_SETTINGS);
  f.set_filter = true;
  f.urgency_filter = cc::CHIRP_URG_CAUTION;
  f.relay_enabled = false;                  // not named: ignored
  Rest r = rest(f);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(cc::g_relay_enabled && r.r.relay_enabled);
  CHECK(cc::g_urgency_filter == cc::CHIRP_URG_CAUTION && r.r.urgency_filter == cc::CHIRP_URG_CAUTION);

  cc::Command relay = cmd_of(cc::CHIRP_CMD_SETTINGS);
  relay.set_relay = true;
  relay.relay_enabled = false;
  relay.urgency_filter = cc::CHIRP_URG_URGENT;   // not named: ignored
  r = rest(relay);
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok);
  CHECK(!cc::g_relay_enabled && !r.r.relay_enabled);
  CHECK(cc::g_urgency_filter == cc::CHIRP_URG_CAUTION && r.r.urgency_filter == cc::CHIRP_URG_CAUTION);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_settings_post_changes_only_what_it_names\n");
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

// ── The status routes read what the loop task published (sweep F138) ─────

// GET /api/chirp, /nearby and /recent (chirp_api.h) read
// chirp_channel::read_status(), read_nearby() and read_recent() on the HTTP
// server's task. Each read here plays that task: it must not wait for the
// loop task, take a lock it leaves held, or touch the radio or the flash.
template <typename F>
void as_httpd(F&& read) {
  const unsigned delays = host_sim::task_delays;
  const unsigned effects = host_sim::httpd_side_effects;
  host_sim::on_httpd_task = true;
  read();
  host_sim::on_httpd_task = false;
  CHECK(host_sim::task_delays == delays);          // it never waits for the loop task
  CHECK(host_sim::httpd_side_effects == effects);
  CHECK(host_sim::mux_depth == 0);
}
cc::StatusView status_read() {
  cc::StatusView v;
  memset(&v, 0xA5, sizeof v);
  as_httpd([&] { cc::read_status(&v); });
  return v;
}
cc::NearbyTable nearby_read() {
  cc::NearbyTable t;
  memset(&t, 0xA5, sizeof t);
  as_httpd([&] { cc::read_nearby(&t); });
  return t;
}
cc::RecentTable recent_read() {
  cc::RecentTable t;
  memset(&t, 0xA5, sizeof t);
  as_httpd([&] { cc::read_recent(&t); });
  return t;
}
std::string reason_of(const cc::StatusView& v) {
  const char* why = cc::cannot_send_reason(v);
  return why != nullptr ? why : "(none)";
}

// Another chirp device in range: its own session key, as it would derive it.
// `display` is its session id's emoji (generate_emoji_string(), spec §2.3),
// what a canary-wap shows for it; `emoji` is what its presence beacon
// carries: the same display unless the test forges the field (sweep F213:
// the beacon is unsigned, and the row no longer takes the field).
struct Neighbor {
  uint8_t priv[32];
  uint8_t pub[32];
  uint8_t sid[cc::SESSION_ID_SIZE];
  uint8_t mac[6];
  std::string display;
  std::string emoji;
};
Neighbor neighbor_of(uint8_t seed, const char* beacon_emoji = nullptr) {
  Neighbor n;
  memset(n.priv, seed, sizeof n.priv);
  n.priv[0] ^= 0x5A;
  Ed25519::derivePublicKey(n.pub, n.priv);
  CHECK(cc::session_id_from_pubkey(n.pub, n.sid));
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, seed};
  memcpy(n.mac, mac, sizeof mac);
  char shown[cc::EMOJI_DISPLAY_SIZE];
  cc::generate_emoji_string(n.sid, shown);
  n.display = shown;
  n.emoji = beacon_emoji != nullptr ? beacon_emoji : n.display;
  return n;
}
cc::ChirpHeader header_of(const Neighbor& n, cc::ChirpMsgType type, uint8_t nonce_byte) {
  cc::ChirpHeader h;
  memset(&h, 0, sizeof h);
  h.magic = cc::CHIRP_MAGIC;
  h.version = cc::PROTOCOL_VERSION;
  h.msg_type = type;
  memcpy(h.session_id, n.sid, sizeof h.session_id);
  h.timestamp = (uint32_t)host_sim::wall_now;
  memset(h.nonce, nonce_byte, sizeof h.nonce);
  return h;
}
template <typename P>
std::vector<uint8_t> frame_of(const cc::ChirpHeader& h, const P& p) {
  std::vector<uint8_t> f(sizeof h + sizeof p);
  memcpy(f.data(), &h, sizeof h);
  memcpy(f.data() + sizeof h, &p, sizeof p);
  return f;
}
// mesh_network::update() hands every ESP-NOW frame to the channel on the
// loop task, before the sketch's loop() runs chirp_channel::update().
void deliver(const Neighbor& n, const std::vector<uint8_t>& f, int8_t rssi = -60) {
  cc::dispatch_espnow_message(n.mac, f.data(), (int)f.size(), rssi);
}
std::vector<uint8_t> presence_of(const Neighbor& n) {
  cc::ChirpPresencePayload p;
  memset(&p, 0, sizeof p);
  // The field whole, as a sender may fill it (31 bytes, no terminator).
  memcpy(p.emoji, n.emoji.data(), std::min(n.emoji.size(), sizeof p.emoji));
  p.listening = 1;
  p.last_chirp_age_min = 255;
  return frame_of(header_of(n, cc::CHIRP_MSG_PRESENCE, 0x70), p);
}
std::vector<uint8_t> witness_of(const Neighbor& n, cc::ChirpTemplate tpl, cc::ChirpUrgency urg,
                                cc::ChirpDetailSlot detail, uint8_t nonce_byte) {
  const cc::ChirpHeader h = header_of(n, cc::CHIRP_MSG_WITNESS, nonce_byte);
  cc::ChirpWitnessPayload p;
  memset(&p, 0, sizeof p);
  p.template_id = (uint8_t)tpl;
  p.detail_slot = (uint8_t)detail;
  p.urgency = (uint8_t)urg;
  p.ttl_minutes = 15;
  memcpy(p.session_pubkey, n.pub, sizeof p.session_pubkey);
  uint8_t canonical[256];
  const size_t cl = cc::build_witness_canonical(&h, &p, n.pub, canonical, sizeof canonical);
  CHECK(cl > 0);
  Ed25519::sign(p.signature, n.priv, n.pub, canonical, cl);
  return frame_of(h, p);
}
std::vector<uint8_t> confirm_of(const Neighbor& n, const uint8_t nonce[8]) {
  const cc::ChirpHeader h = header_of(n, cc::CHIRP_MSG_ACK, 0x71);
  cc::ChirpAckPayload p;
  memset(&p, 0, sizeof p);
  memcpy(p.original_nonce, nonce, 8);
  p.ack_type = cc::CHIRP_ACK_CONFIRMED;
  memcpy(p.confirmer_session_pubkey, n.pub, sizeof p.confirmer_session_pubkey);
  uint8_t canonical[128];
  const size_t cl = cc::build_ack_canonical(nonce, cc::CHIRP_ACK_CONFIRMED, n.pub, canonical,
                                            sizeof canonical);
  CHECK(cl > 0);
  Ed25519::sign(p.signature, n.priv, n.pub, canonical, cl);
  return frame_of(h, p);
}

// The neighbors in `ns` keep beaconing, once a minute, until the rows their
// first beacons put in the nearby table are PRESENCE_REQUIRED_MS old, so a
// confirmation from any of them counts (spec §3.4 rule 4; sweep F311). The
// wall clock moves with uptime and the loop task runs a pass each minute, as
// on a device.
void stay_present(const std::vector<Neighbor>& ns) {
  for (uint32_t t = 0; t < cc::PRESENCE_REQUIRED_MS; t += 60000) {
    host_sim::now_ms += 60000;
    host_sim::wall_now += 60;
    for (const Neighbor& n : ns) deliver(n, presence_of(n));
    cc::update();
  }
}

// The loop task changes the channel mid-pass: a read shows the last pass it
// published, whole, until the pass ends and publishes the change. A read of
// the live state (the routes before F138) would show it at once.
void test_a_status_read_is_the_last_published_pass() {
  boot();
  cc::StatusView v = status_read();                    // what init() published
  CHECK(v.state == cc::CHIRP_DISABLED && v.session_emoji[0] == '\0');
  CHECK(v.relay_enabled && !v.muted && !v.can_send && reason_of(v) == "disabled");
  enabled_channel();
  v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && !v.muted);
  CHECK(strcmp(v.session_emoji, cc::get_session_emoji()) == 0);

  CHECK(cc::mute(30, nullptr));                        // the loop task, mid-pass
  CHECK(cc::g_state == cc::CHIRP_MUTED && cc::g_muted);
  v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && !v.muted && v.mute_remaining_ms == 0);   // not yet published
  cc::update();                                        // the pass ends
  v = status_read();
  CHECK(v.state == cc::CHIRP_MUTED && v.muted && v.mute_remaining_ms == 30u * 60000u);

  // A table the loop task rewrites mid-pass (the prune's shift, a frame's
  // row) is read as it was published, count and rows from one pass.
  const uint8_t n1[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  neighbor_chirp(n1);
  cc::g_tables_changed = true;
  cc::update();
  cc::RecentTable r = recent_read();
  CHECK(r.count == 1 && memcmp(r.chirps[0].nonce, n1, 8) == 0);
  cc::g_recent_chirp_count = 0;                        // the loop task, mid-pass
  r = recent_read();
  CHECK(r.count == 1 && memcmp(r.chirps[0].nonce, n1, 8) == 0);
  CHECK(status_read().recent_chirp_count == 1);
  cc::g_tables_changed = true;
  cc::update();
  CHECK(recent_read().count == 0 && status_read().recent_chirp_count == 0);

  // Before anything is published (the HTTP server can start before init()
  // runs): a disabled channel, as get_status() said then.
  cc::g_status_view = decltype(cc::g_status_view)();
  cc::g_nearby_view = decltype(cc::g_nearby_view)();
  cc::g_recent_view = decltype(cc::g_recent_view)();
  v = status_read();
  CHECK(v.state == cc::CHIRP_DISABLED && v.session_emoji[0] == '\0' && v.nearby_count == 0);
  CHECK(v.relay_enabled && !v.can_send && v.last_chirp_sent_ms == 0);
  CHECK(nearby_read().count == 0 && recent_read().count == 0);
  std::printf("PASS a_status_read_is_the_last_published_pass\n");
}

// init() publishes the first view, with the settings it loaded: the HTTP
// server can answer before loop() runs a pass, and a relay the owner turned
// off must not read as on (the unpublished default) until then.
void test_init_publishes_the_stored_settings() {
  boot();
  cc::Command s = cmd_of(cc::CHIRP_CMD_SETTINGS);
  s.set_relay = true;
  s.relay_enabled = false;
  CHECK(rest(s).r.ok);
  boot(/*wipe=*/false);                               // a reboot: the flash keeps it
  CHECK(!cc::g_relay_enabled);
  const cc::StatusView v = status_read();             // no pass has run
  CHECK(v.state == cc::CHIRP_DISABLED && !v.relay_enabled);
  std::printf("PASS init_publishes_the_stored_settings\n");
}

// A read right after a POST's answer shows what the command did. The loop
// task gets only its drain here: on a device the handler answers as soon as
// the drain posts the result, before the pass reaches its end, so the view
// must already be published (run_command()), not just at the pass's end.
void test_a_read_right_after_a_post_shows_what_it_did() {
  boot();
  Rest e = rest(cmd_of(cc::CHIRP_CMD_ENABLE), 1, Turn::kDrain);
  CHECK(e.wait == lcr::Wait::kDone && e.r.ok);
  cc::StatusView v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && strcmp(v.session_emoji, e.r.session_emoji) == 0);
  CHECK(v.session_start_ms != 0 && !v.presence_met && reason_of(v) == "presence_required");

  cc::Command m = cmd_of(cc::CHIRP_CMD_MUTE);
  m.duration_minutes = 60;
  CHECK(rest(m, 1, Turn::kDrain).r.ok);
  v = status_read();
  CHECK(v.state == cc::CHIRP_MUTED && v.muted && v.mute_remaining_ms > 59u * 60000u);
  CHECK(rest(cmd_of(cc::CHIRP_CMD_UNMUTE), 1, Turn::kDrain).r.ok);
  v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && !v.muted && v.mute_remaining_ms == 0);

  cc::Command s = cmd_of(cc::CHIRP_CMD_SETTINGS);
  s.set_relay = true;
  s.relay_enabled = false;
  CHECK(rest(s, 1, Turn::kDrain).r.ok);
  CHECK(!status_read().relay_enabled);

  // The dashboard reloads the list right after a dismiss.
  const uint8_t n1[8] = {3, 1, 4, 1, 5, 9, 2, 6};
  neighbor_chirp(n1);
  cc::g_tables_changed = true;
  cc::update();
  cc::RecentTable r = recent_read();
  CHECK(r.count == 1 && !r.chirps[0].dismissed);
  CHECK(rest(nonce_cmd(cc::CHIRP_CMD_DISMISS, n1), 1, Turn::kDrain).r.ok);
  r = recent_read();
  CHECK(r.count == 1 && r.chirps[0].dismissed);       // the route leaves it out

  // A send starts the cooldown the status shows at once.
  host_sim::now_ms += cc::PRESENCE_REQUIRED_MS;
  CHECK(status_read().presence_met);                  // counted at the read
  Rest sent = rest(send_of(cc::TPL_INFRA_POWER_OUT), 1, Turn::kDrain);
  CHECK(sent.r.ok);
  v = status_read();
  CHECK(v.state == cc::CHIRP_COOLDOWN && v.cooldown_tier == 1 && !v.can_send);
  CHECK(v.last_chirp_sent_ms == cc::g_cooldown.last_chirp_ms && v.last_chirp_sent_ms != 0);
  CHECK(v.cooldown_remaining_ms == cc::COOLDOWN_TIER_1_MS - (host_sim::now_ms - v.last_chirp_sent_ms));
  CHECK(reason_of(v) == "cooldown");

  // Off: the session and both tables go with it.
  CHECK(rest(cmd_of(cc::CHIRP_CMD_DISABLE), 1, Turn::kDrain).r.ok);
  v = status_read();
  CHECK(v.state == cc::CHIRP_DISABLED && v.session_emoji[0] == '\0');
  CHECK(v.recent_chirp_count == 0 && v.nearby_count == 0 && reason_of(v) == "disabled");
  CHECK(recent_read().count == 0 && nearby_read().count == 0);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_read_right_after_a_post_shows_what_it_did\n");
}

// What update() itself changes (a mute that ran out, a cooldown that ended)
// shows once the pass that changed it ends.
void test_the_pass_publishes_what_it_changed() {
  boot();
  enabled_channel();
  cc::Command m = cmd_of(cc::CHIRP_CMD_MUTE);
  m.duration_minutes = 15;
  CHECK(rest(m).r.ok);
  CHECK(status_read().state == cc::CHIRP_MUTED);
  host_sim::now_ms += 15u * 60000u;
  cc::StatusView v = status_read();
  CHECK(v.state == cc::CHIRP_MUTED && v.mute_remaining_ms == 0);   // ran out; the pass has not run
  cc::update();
  v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && !v.muted && v.can_send);

  // A cooldown is a timer (sweep F178): it ends at the read that finds it
  // run out, before any pass. The pass that came after it said so before.
  CHECK(rest(send_of(cc::TPL_INFRA_POWER_OUT)).r.ok);
  CHECK(status_read().state == cc::CHIRP_COOLDOWN);
  host_sim::now_ms += cc::COOLDOWN_TIER_1_MS;
  v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && v.cooldown_remaining_ms == 0 && v.can_send);
  CHECK(reason_of(v) == "(none)");
  cc::update();
  v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && v.can_send && v.cooldown_tier == 1);
  CHECK(reason_of(v) == "(none)");
  std::printf("PASS the_pass_publishes_what_it_changed\n");
}

// The chirp frames mesh_network::update() hands the channel change the
// nearby and recent tables; chirp_channel::update(), later in the same
// loop() pass, publishes them. A real presence beacon, a signed witness and
// a signed confirmation, each through dispatch_espnow_message().
void test_a_frame_shows_in_the_tables_after_its_pass() {
  boot();
  enabled_channel();
  const Neighbor N = neighbor_of(1);
  const Neighbor M = neighbor_of(2);
  deliver(N, presence_of(N), -48);
  CHECK(cc::g_nearby_count == 1);
  CHECK(nearby_read().count == 0 && status_read().nearby_count == 0);   // not yet published
  cc::update();
  cc::NearbyTable t = nearby_read();
  CHECK(t.count == 1 && status_read().nearby_count == 1);
  CHECK(std::string(t.devices[0].emoji) == N.display && t.devices[0].rssi == -48);
  CHECK(t.devices[0].listening && t.devices[0].last_seen_ms == host_sim::now_ms);
  deliver(M, presence_of(M), -71);
  cc::update();
  t = nearby_read();
  CHECK(t.count == 2 && std::string(t.devices[1].emoji) == M.display && t.devices[1].rssi == -71);
  stay_present({N, M});   // M's confirmation below counts (spec §3.4 rule 4; F311)

  const size_t sent_before = host_sim::espnow->sent.size();
  deliver(N, witness_of(N, cc::TPL_EMERG_FIRE_VISIBLE, cc::CHIRP_URG_URGENT,
                        cc::DETAIL_STATUS_SPREADING, 0x11));
  CHECK(cc::g_recent_chirp_count == 1);
  CHECK(recent_read().count == 0);
  cc::update();
  cc::RecentTable r = recent_read();
  CHECK(r.count == 1 && status_read().recent_chirp_count == 1);
  const cc::RecentView& w = r.chirps[0];
  CHECK(w.template_id == cc::TPL_EMERG_FIRE_VISIBLE && w.urgency == cc::CHIRP_URG_URGENT);
  CHECK(w.detail == cc::DETAIL_STATUS_SPREADING && w.hop_count == 0);
  uint8_t nonce[8];
  memset(nonce, 0x11, sizeof nonce);
  CHECK(memcmp(w.nonce, nonce, 8) == 0 && w.received_ms == host_sim::now_ms);
  CHECK(strcmp(w.sender_emoji, cc::g_recent_chirps[0].sender_emoji) == 0 && w.sender_emoji[0] != '\0');
  CHECK(w.confirm_count == 0 && !w.validated && !w.relayed);
  CHECK(std::string(cc::get_validation_status(&w)) == "awaiting_confirmation");

  // M saw it too: one confirmation validates a safety template, and the
  // relay re-signs and sends it.
  deliver(M, confirm_of(M, nonce));
  CHECK(cc::g_recent_chirps[0].validated && cc::g_recent_chirps[0].relayed);
  CHECK(!recent_read().chirps[0].validated);
  cc::update();
  r = recent_read();
  CHECK(r.chirps[0].confirm_count == 1 && r.chirps[0].validated && r.chirps[0].relayed);
  CHECK(std::string(cc::get_validation_status(&r.chirps[0])) == "validated");
  bool relayed = false;
  for (uint8_t type : sent_types(sent_before)) relayed = relayed || type == cc::CHIRP_MSG_WITNESS;
  CHECK(relayed);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_frame_shows_in_the_tables_after_its_pass\n");
}

// ── The recent route names an all-clear chirp's category (sweep F307) ───

// The category each template's name places it in (mesh_network.h's
// ranges: authority 0x00-0x0F up to mutual aid 0x40-0x4F, all clear
// 0x80-0x8F), by a switch with no default: -Wswitch (in -Wall, an error in
// this build) fails the compile on a TPL_* this list does not place.
// TPL_INVALID is no template.
bool named_category(cc::ChirpTemplate t, cc::ChirpCategory* out) {
  switch (t) {
    case cc::TPL_AUTH_POLICE_ACTIVITY:
    case cc::TPL_AUTH_HEAVY_RESPONSE:
    case cc::TPL_AUTH_ROAD_BLOCKED_LE:
    case cc::TPL_AUTH_HELICOPTER:
      *out = cc::CHIRP_CAT_AUTHORITY;
      return true;
    case cc::TPL_INFRA_POWER_OUT:
    case cc::TPL_INFRA_WATER_ISSUE:
    case cc::TPL_INFRA_GAS_SMELL:
    case cc::TPL_INFRA_INTERNET_DOWN:
    case cc::TPL_INFRA_ROAD_CLOSED:
      *out = cc::CHIRP_CAT_INFRA;
      return true;
    case cc::TPL_EMERG_FIRE_VISIBLE:
    case cc::TPL_EMERG_MEDICAL_SCENE:
    case cc::TPL_EMERG_MULTIPLE_AMBULANCE:
    case cc::TPL_EMERG_EVACUATION:
    case cc::TPL_EMERG_SHELTER_IN_PLACE:
      *out = cc::CHIRP_CAT_EMERGENCY;
      return true;
    case cc::TPL_WX_SEVERE_WARNING:
    case cc::TPL_WX_TORNADO:
    case cc::TPL_WX_FLOOD:
    case cc::TPL_WX_LIGHTNING_CLOSE:
      *out = cc::CHIRP_CAT_WEATHER;
      return true;
    case cc::TPL_AID_WELFARE_CHECK:
    case cc::TPL_AID_SUPPLIES_NEEDED:
    case cc::TPL_AID_OFFERING_HELP:
      *out = cc::CHIRP_CAT_MUTUAL_AID;
      return true;
    case cc::TPL_CLR_RESOLVED:
    case cc::TPL_CLR_SAFE:
    case cc::TPL_CLR_FALSE_ALARM:
      *out = cc::CHIRP_CAT_ALL_CLEAR;
      return true;
    case cc::TPL_INVALID:
      return false;
  }
  return false;   // an id no enumerator names (0x04, the reserved one, among them)
}

// GET /api/chirp/recent answered `category` from the template id's high
// nibble. The all-clear templates are 0x80 to 0x82, nibble 8, while
// CHIRP_CAT_ALL_CLEAR is 5, so category_name() named every all-clear chirp
// "unknown" (the dashboard printed it under the chirp). The route now asks
// template_category(), the template table's mapping handle_ack() counts
// confirmations by (check_wap_loop_commands.py holds chirp_api.h to that
// expression; ArduinoJson is not on the host). Every one of the 256 ids:
// the table holds exactly the enumerators, each in its name's range, and
// template_category() names that range's category; then a neighbor's
// signed all-clear chirp, through the real receive path and the published
// copy, reads "all_clear" by the route's expression.
void test_the_recent_route_names_every_category() {
  unsigned placed = 0;
  for (unsigned id = 0; id < 256; ++id) {
    const cc::ChirpTemplate t = (cc::ChirpTemplate)id;
    cc::ChirpCategory want = cc::CHIRP_CAT_AUTHORITY;
    const bool named = named_category(t, &want);
    CHECK(cc::is_valid_template(t) == named);
    if (!named) continue;
    ++placed;
    const unsigned range = want == cc::CHIRP_CAT_ALL_CLEAR ? 0x80u : (unsigned)want << 4;
    CHECK((id & 0xF0u) == range);
    CHECK(cc::template_category(t) == want);
    CHECK(std::string(cc::category_name(cc::template_category(t))) != "unknown");
  }
  CHECK(placed == cc::TEMPLATE_COUNT && placed == 24);
  const cc::ChirpTemplate clear[] = {cc::TPL_CLR_RESOLVED, cc::TPL_CLR_SAFE, cc::TPL_CLR_FALSE_ALARM};
  for (cc::ChirpTemplate t : clear) {
    CHECK(std::string(cc::category_name(cc::template_category(t))) == "all_clear");
    // What the route's old derivation answered for it.
    CHECK(std::string(cc::category_name((cc::ChirpCategory)((uint8_t)t >> 4))) == "unknown");
  }

  boot();
  enabled_channel();
  const Neighbor N = neighbor_of(0x61);
  deliver(N, presence_of(N));
  deliver(N, witness_of(N, cc::TPL_CLR_SAFE, cc::CHIRP_URG_INFO, cc::DETAIL_NONE, 0x62));
  deliver(N, witness_of(N, cc::TPL_WX_FLOOD, cc::CHIRP_URG_CAUTION, cc::DETAIL_NONE, 0x63));
  cc::update();
  const cc::RecentTable r = recent_read();
  CHECK(r.count == 2);
  const cc::RecentView* chirps = r.chirps;   // the route's names for the copy and its rows
  CHECK(chirps[0].template_id == cc::TPL_CLR_SAFE && chirps[1].template_id == cc::TPL_WX_FLOOD);
  CHECK(std::string(cc::category_name(cc::template_category(chirps[0].template_id))) == "all_clear");
  CHECK(std::string(cc::category_name(cc::template_category(chirps[1].template_id))) == "weather");
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS the_recent_route_names_every_category\n");
}

// Sweep F213: a presence beacon is unsigned, and handle_presence() copied
// its 31 emoji bytes into the nearby table as sent, so any device in
// ESP-NOW range put quotes, control bytes or markup (or 31 bytes with no
// terminator) into every neighbor's GET /api/chirp/nearby, which serializes
// the row whole since F196. The row's emoji is now the display of the
// session id it is keyed on (spec §2.3), derived by generate_emoji_string()
// as a witness's sender emoji is: five of EMOJI_SET's sixteen, whatever the
// beacon's field says. An honest beacon carries that same display, so a
// canary-wap neighbor shows the emoji it sends.
// Whether `e` is exactly `n` of EMOJI_SET's entries, one after another.
bool emoji_set_only(const std::string& e, size_t n) {
  size_t pos = 0, count = 0;
  while (pos < e.size()) {
    size_t step = 0;
    for (const char* entry : cc::EMOJI_SET) {
      const size_t len = strlen(entry);
      if (e.compare(pos, len, entry) == 0) { step = len; break; }
    }
    if (step == 0) return false;
    pos += step;
    ++count;
  }
  return count == n;
}
void test_a_beacon_emoji_is_its_session_display() {
  boot();
  enabled_channel();
  std::string control;                                 // every control byte but NUL, then DEL
  for (int c = 1; c < 30; ++c) control.push_back((char)c);
  control.push_back('\x7f');
  const std::string forged_fields[] = {
      std::string(30, '"'),                            // the F196 harness's quotes
      "\"},\"emoji\":\"\\u0000\",\"x\":\"\\\\",       // quotes and backslashes that close the field
      control,
      "<img src=x onerror=alert(1)>",                  // markup
      "</script><script>alert(1)",
      std::string(31, 'A'),                            // the whole field, no terminator
  };
  std::vector<Neighbor> senders;
  uint8_t seed = 0x51;
  for (const std::string& field : forged_fields) senders.push_back(neighbor_of(seed++, field.c_str()));
  const Neighbor honest = neighbor_of(seed++);
  CHECK(honest.emoji == honest.display && emoji_set_only(honest.display, 5));
  for (const Neighbor& n : senders) deliver(n, presence_of(n));
  deliver(honest, presence_of(honest));
  cc::update();
  const cc::NearbyTable t = nearby_read();
  CHECK(t.count == senders.size() + 1);
  for (size_t i = 0; i < t.count && i < senders.size() + 1; ++i) {
    const Neighbor& n = i < senders.size() ? senders[i] : honest;
    const size_t len = strnlen(t.devices[i].emoji, sizeof t.devices[i].emoji);
    CHECK(len < sizeof t.devices[i].emoji);            // terminated in its field
    const std::string shown(t.devices[i].emoji, len);
    CHECK(shown == n.display);                         // the session's display...
    CHECK(emoji_set_only(shown, 5));                   // ...five of the set's, nothing else
    CHECK(shown.find_first_of("\"\\<>") == std::string::npos);
    CHECK(std::none_of(shown.begin(), shown.end(), [](char c) { return (unsigned char)c < 0x20 || c == 0x7f; }));
    CHECK(std::string(cc::g_nearby_devices[i].emoji) == n.display);   // the table the view copies
  }
  // The same session's next beacon carries other bytes: the row keeps its display.
  Neighbor again = senders[0];
  again.emoji = "<b>bold</b>";
  deliver(again, presence_of(again));
  cc::update();
  CHECK(std::string(nearby_read().devices[0].emoji) == senders[0].display);
  // This device's own beacon carries its session's display, as a neighbor derives it.
  char own[cc::EMOJI_DISPLAY_SIZE];
  cc::generate_emoji_string(cc::g_session.session_id, own);
  CHECK(strcmp(own, cc::get_session_emoji()) == 0 && emoji_set_only(own, 5));
  std::printf("PASS a_beacon_emoji_is_its_session_display\n");
}

// Sweep F231: spec §2.3 built a session's display from three emoji and §3.2
// gave the beacon's field as 3 to 12 bytes, while generate_emoji_string()
// renders five (audit C11 lifted it from three, 12 bits, to five, 20 bits,
// on purpose; the spec's changelog says so). The spec now says five. What
// selects them is the low nibble of session id bytes 0 to 4, so every input
// that picks a display is one of 16^5 nibble tuples, and each is checked
// here: all five of its emoji, in order, none dropped by the field's guard,
// between 15 bytes (five ⭐, 3 bytes each) and 30 (five ☀️, 6 each), and
// terminated inside the 31-byte field with nothing written past the NUL.
// The high nibbles and bytes 5 to 7 choose nothing. A beacon carries the
// display NUL-padded to the field's end (strncpy). And the session id the
// display is drawn from is SHA-256 over the 25 bytes of
// "securacv:chirp:session:v0" and the pubkey, as spec §2.3 and §3.1 write
// it; §12.2's pseudocode hashed 26, a NUL the code never hashes.
void test_every_session_display_is_five_emoji_in_its_field() {
  boot();
  enabled_channel();
  size_t lens[16];
  for (size_t i = 0; i < 16; ++i) lens[i] = strlen(cc::EMOJI_SET[i]);
  CHECK(*std::min_element(lens, lens + 16) == 3);    // ⭐
  CHECK(*std::max_element(lens, lens + 16) == 6);    // ☀️ (with its variation selector)

  // Every tuple of five nibbles, under high nibbles and tail bytes that move
  // with it: the display is exactly the five emoji the nibbles name.
  size_t shortest = SIZE_MAX, longest = 0, wrong = 0;
  uint32_t first_wrong = 0;
  for (uint32_t t = 0; t < (1u << 20); ++t) {
    uint8_t sid[cc::SESSION_ID_SIZE];
    for (uint32_t k = 0; k < 5; ++k) {
      sid[k] = (uint8_t)(((t >> (4 * k)) & 0x0Fu) | (((t * 37u) + (k * 101u)) & 0xF0u));
    }
    for (uint32_t k = 5; k < cc::SESSION_ID_SIZE; ++k) sid[k] = (uint8_t)((t * 13u) + k);
    char out[cc::EMOJI_DISPLAY_SIZE + 4];
    memset(out, 0x5A, sizeof out);
    cc::generate_emoji_string(sid, out);
    std::string expect;
    for (uint32_t k = 0; k < 5; ++k) expect += cc::EMOJI_SET[(t >> (4 * k)) & 0x0Fu];
    const size_t n = strnlen(out, sizeof out);
    const bool ok = n < cc::EMOJI_DISPLAY_SIZE && std::string(out, n) == expect &&
                    std::all_of(out + n + 1, out + sizeof out, [](char c) { return c == 0x5A; });
    if (!ok && wrong++ == 0) first_wrong = t;
    shortest = std::min(shortest, n);
    longest = std::max(longest, n);
  }
  if (wrong != 0) std::fprintf(stderr, "first wrong nibble tuple: %05x\n", (unsigned)first_wrong);
  CHECK(wrong == 0);
  CHECK(shortest == 15);                              // five ⭐
  CHECK(longest == 30);                               // five ☀️: the field's 31 with its NUL
  CHECK(longest == cc::EMOJI_DISPLAY_SIZE - 1);

  // Each byte value at each position: only bytes 0 to 4 choose, by their low nibble.
  for (uint32_t pos = 0; pos < cc::SESSION_ID_SIZE; ++pos) {
    for (uint32_t b = 0; b < 256; ++b) {
      uint8_t sid[cc::SESSION_ID_SIZE] = {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87};
      sid[pos] = (uint8_t)b;
      char out[cc::EMOJI_DISPLAY_SIZE];
      cc::generate_emoji_string(sid, out);
      std::string expect;
      for (uint32_t k = 0; k < 5; ++k) expect += cc::EMOJI_SET[sid[k] & 0x0F];
      CHECK(std::string(out) == expect);
    }
  }

  // The beacon's field: the display, then NULs to the end of the 31 bytes.
  for (const uint8_t pick : {(uint8_t)0x0B, (uint8_t)0x04, (uint8_t)0x00}) {   // ☀️, ⭐, 🐦
    memset(cc::g_session.session_id, pick, sizeof cc::g_session.session_id);
    cc::generate_emoji_string(cc::g_session.session_id, cc::g_session.emoji_display);
    const std::string display = cc::g_session.emoji_display;
    CHECK(display.size() == 5 * strlen(cc::EMOJI_SET[pick]));
    const size_t from = host_sim::espnow->sent.size();
    cc::send_presence();
    CHECK(host_sim::espnow->sent.size() == from + 1);
    const std::vector<uint8_t>& f = host_sim::espnow->sent.back().bytes;
    CHECK(f.size() == sizeof(cc::ChirpHeader) + sizeof(cc::ChirpPresencePayload));
    const cc::ChirpPresencePayload* p =
        reinterpret_cast<const cc::ChirpPresencePayload*>(f.data() + sizeof(cc::ChirpHeader));
    CHECK(sizeof p->emoji == cc::EMOJI_DISPLAY_SIZE);
    CHECK(memcmp(p->emoji, display.data(), display.size()) == 0);
    CHECK(std::all_of(p->emoji + display.size(), p->emoji + sizeof p->emoji,
                      [](char c) { return c == '\0'; }));
  }

  // The session id: the first 8 bytes of SHA-256(25-byte domain || pubkey).
  const Neighbor n = neighbor_of(0x6E);
  static const char kDomain[] = "securacv:chirp:session:v0";
  CHECK(sizeof kDomain - 1 == 25);
  for (const size_t domain_len : {sizeof kDomain - 1, sizeof kDomain}) {   // 25, then 26 with the NUL
    std::vector<uint8_t> in(kDomain, kDomain + domain_len);
    in.insert(in.end(), n.pub, n.pub + sizeof n.pub);
    uint8_t md[32];
    unsigned int md_len = 0;
    CHECK(EVP_Digest(in.data(), in.size(), md, &md_len, EVP_sha256(), nullptr) == 1 && md_len == 32);
    const bool same = memcmp(md, n.sid, cc::SESSION_ID_SIZE) == 0;
    CHECK(same == (domain_len == 25));
  }
  std::printf("PASS every_session_display_is_five_emoji_in_its_field\n");
}

// update()'s 30-second prune drops a neighbor not heard for 3 minutes and a
// chirp older than 30; the pass that prunes publishes the shorter tables.
void test_the_prune_shows_in_the_tables() {
  boot();
  enabled_channel();
  const Neighbor N = neighbor_of(3);
  deliver(N, presence_of(N));
  deliver(N, witness_of(N, cc::TPL_INFRA_POWER_OUT, cc::CHIRP_URG_INFO, cc::DETAIL_NONE, 0x22));
  cc::update();
  CHECK(nearby_read().count == 1 && recent_read().count == 1);

  host_sim::now_ms += cc::NEARBY_TIMEOUT_MS + 31000;
  cc::update();                                        // the prune's pass
  CHECK(cc::g_nearby_count == 0 && cc::g_recent_chirp_count == 1);
  CHECK(nearby_read().count == 0 && status_read().nearby_count == 0);
  CHECK(recent_read().count == 1);
  host_sim::now_ms += cc::DEFAULT_DISPLAY_MS;
  cc::update();
  CHECK(cc::g_recent_chirp_count == 0);
  CHECK(recent_read().count == 0 && status_read().recent_chirp_count == 0);
  std::printf("PASS the_prune_shows_in_the_tables\n");
}

// The tables publish on change, not every pass (the brief: not a whole-table
// copy every loop pass). A pass that nothing marked (no chirp frame, no
// prune, no command) leaves the published tables as they were, even when
// the live rows differ: it never read them. The live rows are written here
// behind the channel's back, which nothing on a device does, so the only
// way a read can show the write is a pass that rebuilt the tables anyway.
// The next frame's pass, which is marked, shows it.
void test_an_idle_pass_does_not_rebuild_the_tables() {
  boot();
  enabled_channel();
  const Neighbor N = neighbor_of(6);
  deliver(N, presence_of(N), -50);
  deliver(N, witness_of(N, cc::TPL_INFRA_POWER_OUT, cc::CHIRP_URG_INFO, cc::DETAIL_NONE, 0x55));
  cc::update();                                        // marked: publishes both tables
  CHECK(!cc::g_tables_changed);
  CHECK(recent_read().chirps[0].hop_count == 0 && nearby_read().devices[0].rssi == -50);

  cc::g_recent_chirps[0].hop_count = 2;                // unmarked writes
  cc::g_nearby_devices[0].rssi = -90;
  for (int pass = 0; pass < 3; ++pass) {
    host_sim::now_ms += 1000;                           // well inside the 30-second prune
    cc::update();
    CHECK(!cc::g_tables_changed);
    CHECK(recent_read().chirps[0].hop_count == 0);
    CHECK(nearby_read().devices[0].rssi == -50);
  }

  deliver(N, presence_of(N), -60);                     // a frame marks them
  cc::update();
  CHECK(recent_read().chirps[0].hop_count == 2 && nearby_read().devices[0].rssi == -60);
  std::printf("PASS an_idle_pass_does_not_rebuild_the_tables\n");
}

// What counts in time is counted at the read, from what the loop task
// published: the cooldown and mute left, the presence requirement, the wall
// clock (and with it night mode and can_send), as the live readers counted
// them. No pass runs between these reads.
void test_a_status_read_counts_time_at_the_read() {
  boot();
  enabled_channel(/*present=*/false);
  cc::StatusView v = status_read();
  CHECK(!v.presence_met && reason_of(v) == "presence_required");
  host_sim::now_ms += cc::PRESENCE_REQUIRED_MS - 1;
  CHECK(!status_read().presence_met);
  host_sim::now_ms += 1;
  v = status_read();
  CHECK(v.presence_met && v.can_send && reason_of(v) == "(none)");

  // A wall clock not set yet: the route names it (sweep F146; it named no
  // reason, and the dashboard said Ready to a send that would be refused).
  host_sim::wall_now = cc::MIN_UNIX_TIME - 1;
  v = status_read();
  CHECK(!v.clock_synced && !v.can_send && v.night_mode);   // night: the conservative answer
  CHECK(reason_of(v) == "clock_unsynced");
  host_sim::wall_now = 1760050800;                        // 23:00 UTC
  v = status_read();
  CHECK(v.clock_synced && v.night_mode && v.can_send);
  host_sim::wall_now = 1760000000;                        // 08:53 UTC
  CHECK(!status_read().night_mode);

  CHECK(rest(send_of(cc::TPL_INFRA_POWER_OUT)).r.ok);
  v = status_read();
  const uint32_t left = v.cooldown_remaining_ms;
  CHECK(left > 0 && left <= cc::COOLDOWN_TIER_1_MS);
  host_sim::now_ms += 10000;
  CHECK(status_read().cooldown_remaining_ms == left - 10000);
  cc::Command m = cmd_of(cc::CHIRP_CMD_MUTE);             // a mute, in the cooldown
  m.duration_minutes = 15;
  CHECK(rest(m).r.ok);
  const uint32_t mute_left = status_read().mute_remaining_ms;
  host_sim::now_ms += 60000;
  CHECK(status_read().mute_remaining_ms == mute_left - 60000);
  std::printf("PASS a_status_read_counts_time_at_the_read\n");
}

// cannot_send_reason() checks in the route's order, the clock last.
void test_cannot_send_reason_names_the_clock() {
  cc::StatusView v;
  memset(&v, 0, sizeof v);
  v.state = cc::CHIRP_ACTIVE;
  v.presence_met = true;
  v.clock_synced = true;
  v.can_send = true;
  CHECK(reason_of(v) == "(none)");
  v.can_send = false;
  v.clock_synced = false;
  CHECK(reason_of(v) == "clock_unsynced");
  v.presence_met = false;
  CHECK(reason_of(v) == "presence_required");
  // The cooldown is its timer, whatever the state reads (sweep F178): muted
  // in it, it still names the cooldown.
  v.cooldown_remaining_ms = 1;
  CHECK(reason_of(v) == "cooldown");
  v.state = cc::CHIRP_MUTED;
  CHECK(reason_of(v) == "cooldown");
  v.state = cc::CHIRP_DISABLED;
  CHECK(reason_of(v) == "disabled");
  std::printf("PASS cannot_send_reason_names_the_clock\n");
}

// Every field the routes show is the live one, as the pass that published
// it left it: the status against get_status() and the live readers, each
// row against the live table's.
void test_every_field_the_routes_show_is_the_live_one() {
  boot();
  enabled_channel();
  const Neighbor N = neighbor_of(4);
  const Neighbor M = neighbor_of(5);
  deliver(N, presence_of(N), -41);
  deliver(M, presence_of(M), -88);
  cc::g_nearby_devices[1].listening = false;
  deliver(N, witness_of(N, cc::TPL_WX_FLOOD, cc::CHIRP_URG_CAUTION, cc::DETAIL_DIR_EAST, 0x33));
  deliver(M, witness_of(M, cc::TPL_AID_OFFERING_HELP, cc::CHIRP_URG_INFO, cc::DETAIL_NONE, 0x44));
  CHECK(cc::g_recent_chirp_count == 2);
  cc::ReceivedChirp& a = cc::g_recent_chirps[0];       // as later frames would leave them
  a.hop_count = 2;
  a.confirm_count = 3;
  a.validated = true;
  a.relayed = true;
  cc::ReceivedChirp& b = cc::g_recent_chirps[1];
  b.suppressed = true;
  b.dismissed = true;
  cc::g_tables_changed = true;
  CHECK(rest(send_of(cc::TPL_INFRA_POWER_OUT)).r.ok);  // a cooldown, tier 1
  cc::Command m = cmd_of(cc::CHIRP_CMD_MUTE);
  m.duration_minutes = 120;
  CHECK(rest(m).r.ok);
  cc::Command s = cmd_of(cc::CHIRP_CMD_SETTINGS);
  s.set_relay = true;
  s.relay_enabled = false;
  CHECK(rest(s).r.ok);
  host_sim::now_ms += 1234;

  const cc::StatusView v = status_read();
  const cc::ChirpStatus live = cc::get_status();
  CHECK(v.state == live.state && strcmp(v.session_emoji, live.session_emoji) == 0);
  CHECK(v.nearby_count == live.nearby_count && v.nearby_count == 2);
  CHECK(v.recent_chirp_count == live.recent_chirp_count && v.recent_chirp_count == 2);
  CHECK(v.last_chirp_sent_ms == live.last_chirp_sent_ms);
  CHECK(v.cooldown_remaining_ms == live.cooldown_remaining_ms);
  CHECK(v.relay_enabled == live.relay_enabled && !v.relay_enabled);
  CHECK(v.muted == live.muted && v.muted);
  CHECK(v.mute_remaining_ms == live.mute_remaining_ms && v.mute_remaining_ms > 0);
  CHECK(v.cooldown_tier == cc::get_cooldown_tier() && v.cooldown_tier == 1);
  CHECK(v.presence_met == cc::has_presence_requirement());
  CHECK(v.can_send == cc::can_send_chirp() && v.night_mode == cc::is_night_mode());

  const cc::NearbyTable t = nearby_read();
  CHECK(t.count == cc::g_nearby_count);
  for (size_t i = 0; i < t.count; i++) {
    const cc::NearbyDevice& d = cc::g_nearby_devices[i];
    CHECK(strcmp(t.devices[i].emoji, d.emoji) == 0);
    CHECK(t.devices[i].rssi == d.rssi && t.devices[i].listening == d.listening);
    CHECK(t.devices[i].last_seen_ms == d.last_seen_ms);
  }
  CHECK(t.devices[0].listening && !t.devices[1].listening);

  const cc::RecentTable r = recent_read();
  CHECK(r.count == cc::g_recent_chirp_count);
  for (size_t i = 0; i < r.count; i++) {
    const cc::ReceivedChirp& c = cc::g_recent_chirps[i];
    const cc::RecentView& w = r.chirps[i];
    CHECK(strcmp(w.sender_emoji, c.sender_emoji) == 0);
    CHECK(w.template_id == c.template_id && w.detail == c.detail && w.urgency == c.urgency);
    CHECK(w.hop_count == c.hop_count && w.confirm_count == c.confirm_count);
    CHECK(w.validated == c.validated && w.suppressed == c.suppressed);
    CHECK(w.relayed == c.relayed && w.dismissed == c.dismissed);
    CHECK(memcmp(w.nonce, c.nonce, 8) == 0 && w.received_ms == c.received_ms);
    CHECK(strcmp(cc::get_validation_status(&w), cc::get_validation_status(&c)) == 0);
  }
  CHECK(r.chirps[0].template_id == cc::TPL_WX_FLOOD && r.chirps[0].detail == cc::DETAIL_DIR_EAST);
  CHECK(r.chirps[0].hop_count == 2 && r.chirps[0].confirm_count == 3);
  CHECK(r.chirps[0].validated && r.chirps[0].relayed && !r.chirps[0].dismissed);
  CHECK(r.chirps[1].suppressed && r.chirps[1].dismissed && r.chirps[1].urgency == cc::CHIRP_URG_INFO);
  CHECK(std::string(cc::get_validation_status(&r.chirps[1])) == "suppressed");
  std::printf("PASS every_field_the_routes_show_is_the_live_one\n");
}

// The two tables' published copies live in the block init() allocates
// (g_view_tables: PSRAM on a device, csi_mem.h), beside the scratch, not in
// the static objects that hold their locks. The PSRAM diet moved these
// tables out of internal SRAM for the BLE stack's heap; a static copy of
// each would have put 2.1 KB of it back. A read returns the block's bytes,
// and the static objects are a lock, a pointer and a flag.
void test_the_tables_publish_into_their_block() {
  static_assert(sizeof(cc::g_nearby_view) < sizeof(cc::NearbyTable) / 8,
                "the nearby table's published copy is not in the static object");
  static_assert(sizeof(cc::g_recent_view) < sizeof(cc::RecentTable) / 8,
                "the recent table's published copy is not in the static object");
  boot();
  CHECK(cc::g_view_tables != nullptr);
  enabled_channel();
  const Neighbor N = neighbor_of(7);
  deliver(N, presence_of(N), -52);
  deliver(N, witness_of(N, cc::TPL_INFRA_POWER_OUT, cc::CHIRP_URG_INFO, cc::DETAIL_NONE, 0x66));
  cc::update();
  cc::NearbyTable t = nearby_read();
  cc::RecentTable r = recent_read();
  CHECK(t.count == 1 && r.count == 1);
  CHECK(memcmp(&t, &cc::g_view_tables->nearby, sizeof t) == 0);
  CHECK(memcmp(&r, &cc::g_view_tables->recent, sizeof r) == 0);
  cc::g_view_tables->nearby.devices[0].rssi = -99;     // the block is what a read copies
  cc::g_view_tables->recent.chirps[0].hop_count = 3;
  CHECK(nearby_read().devices[0].rssi == -99 && recent_read().chirps[0].hop_count == 3);

  // Not attached (as before init()): nothing to read, an empty table.
  cc::g_nearby_view.attach(nullptr);
  cc::g_recent_view.attach(nullptr);
  CHECK(nearby_read().count == 0 && recent_read().count == 0);
  CHECK(!cc::g_nearby_view.publish(t));
  std::printf("PASS the_tables_publish_into_their_block\n");
}

// What the view costs: the status's published copy is static (internal
// SRAM on a device), the tables' copies and their scratch are one PSRAM
// allocation (the_tables_publish_into_their_block). The sizes
// the docs quote, on this host's layout (the device's 32-bit layout of these
// structs is the same: no pointer, size_t or 8-byte member in any).
void test_the_view_sizes() {
  static_assert(sizeof(cc::StatusView) == 68, "StatusView size the docs quote");
  static_assert(sizeof(cc::NearbyView) == 40 && sizeof(cc::NearbyTable) == 1284,
                "NearbyTable size the docs quote");
  static_assert(sizeof(cc::RecentView) == 52 && sizeof(cc::RecentTable) == 836,
                "RecentTable size the docs quote");
  static_assert(sizeof(cc::ViewScratch) == 1284, "the scratch is the larger table");
  static_assert(sizeof(cc::ViewTables) == 1284 + 836 + 1284, "the PSRAM block: two copies and the scratch");
  std::printf("PASS the_view_sizes (StatusView %zu B, NearbyTable %zu B, RecentTable %zu B; "
              "the live tables they copy from: %zu B and %zu B)\n",
              sizeof(cc::StatusView), sizeof(cc::NearbyTable), sizeof(cc::RecentTable),
              cc::NEARBY_BYTES, cc::RECENT_CHIRPS_BYTES);
}

// Spec §12.3's memory budget, row by row and its total, on this host's
// layout. The device's 32-bit layout of these structs is the same: no
// pointer, size_t or 8-byte member in any, and no member aligned past 4.
// scripts/tests/test_chirp_spec_tables.py holds each row of the spec's
// table to the line here that names it, and the total to the sum.
// The nearby row grew 4 bytes for first_seen_ms (sweep F311); the recent
// row's TTL byte took the padding before received_ms (F306), so it did not.
static_assert(sizeof(cc::ChirpSession) == 112, "§12.3 Session identity");
static_assert(sizeof(cc::ReceivedChirp) == 964 &&
                  cc::RECENT_CHIRPS_BYTES == cc::MAX_RECENT_CHIRPS * sizeof(cc::ReceivedChirp) &&
                  cc::RECENT_CHIRPS_BYTES == 16 * 964,
              "§12.3 Recent chirps");
static_assert(offsetof(cc::ReceivedChirp, ttl_minutes) == offsetof(cc::ReceivedChirp, hop_count) + 1 &&
                  offsetof(cc::ReceivedChirp, received_ms) == offsetof(cc::ReceivedChirp, ttl_minutes) + 1,
              "the TTL takes the byte of padding between hop_count and received_ms");
static_assert(2 * cc::BLOOM_BYTES == 2 * 4096, "§12.3 Nonce dedup (Bloom filter, two halves)");
static_assert(sizeof(cc::NearbyDevice) == 52 &&
                  cc::NEARBY_BYTES == cc::MAX_NEARBY_CACHE * sizeof(cc::NearbyDevice) &&
                  cc::NEARBY_BYTES == 32 * 52,
              "§12.3 Nearby (presence) table");
static_assert(sizeof(cc::PubkeyRateEntry) == 40 && cc::PUBKEY_RATE_BYTES == 32 * 40,
              "§12.3 Per-key witness rate table");
static_assert(sizeof(cc::SelfTestSeenEntry) == 40 && cc::SELFTEST_SEEN_BYTES == 32 * 40,
              "§12.3 Self-test seen table");
static_assert(sizeof(cc::ViewTables) == 3404, "§12.3 Route views (two copies and a scratch)");
static_assert(sizeof(cc::ChirpSession) + cc::RECENT_CHIRPS_BYTES + 2 * cc::BLOOM_BYTES + cc::NEARBY_BYTES +
                      cc::PUBKEY_RATE_BYTES + cc::SELFTEST_SEEN_BYTES + sizeof(cc::ViewTables) ==
                  31356,
              "§12.3 Total");
void test_the_memory_budget_is_the_specs() {
  std::printf("PASS the_memory_budget_is_the_specs (ReceivedChirp %zu B, NearbyDevice %zu B)\n",
              sizeof(cc::ReceivedChirp), sizeof(cc::NearbyDevice));
}

// ── The send cooldown is a timer (sweep F178) ───────────────────────────

// The presence beacons the loop task sent since `from`: each one's
// `listening` flag, in order.
std::vector<uint8_t> presence_listening(size_t from) {
  std::vector<uint8_t> out;
  const auto& sent = host_sim::espnow->sent;
  for (size_t i = from; i < sent.size(); ++i) {
    const cc::ChirpHeader* h = reinterpret_cast<const cc::ChirpHeader*>(sent[i].bytes.data());
    if (h->msg_type != cc::CHIRP_MSG_PRESENCE) continue;
    CHECK(sent[i].bytes.size() >= sizeof(cc::ChirpHeader) + sizeof(cc::ChirpPresencePayload));
    cc::ChirpPresencePayload p;
    memcpy(&p, sent[i].bytes.data() + sizeof(cc::ChirpHeader), sizeof p);
    out.push_back(p.listening);
  }
  return out;
}

// No witness frame went out since `from`.
bool no_witness_since(size_t from) {
  for (uint8_t t : sent_types(from)) {
    if (t == cc::CHIRP_MSG_WITNESS) return false;
  }
  return true;
}

// A mute does not end the send cooldown (spec 2.5.4: escalating, counted
// from the last send). The cooldown was a state, CHIRP_COOLDOWN, which
// mute() overwrote with CHIRP_MUTED, and unmute() and the mute's timeout
// returned to CHIRP_ACTIVE; can_send_chirp() and the send's refusal read
// only the state. So a send one second after the first, muted in between,
// went out, a tier up. Now each refusal is the timer's, with its time left,
// and GET /api/chirp names it whatever the state reads.
void test_a_mute_does_not_end_the_cooldown() {
  boot();
  enabled_channel();
  Rest r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(r.r.ok && r.r.cooldown_tier == 1);
  const uint32_t first_at = cc::g_cooldown.last_chirp_ms;
  host_sim::now_ms += 1000;
  cc::Command m15 = cmd_of(cc::CHIRP_CMD_MUTE);
  m15.duration_minutes = 15;
  CHECK(rest(m15).r.ok && cc::g_state == cc::CHIRP_MUTED);

  // Muted: refused for the cooldown, with the timer's time left, tier 1.
  size_t before = host_sim::espnow->sent.size();
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_COOLDOWN && r.r.cooldown_tier == 1);
  CHECK(r.r.cooldown_remaining_ms == cc::COOLDOWN_TIER_1_MS - (host_sim::now_ms - first_at));
  CHECK(no_witness_since(before) && cc::g_cooldown.chirps_sent_today == 1);
  cc::StatusView v = status_read();
  CHECK(v.state == cc::CHIRP_MUTED && v.muted && !v.can_send);
  CHECK(reason_of(v) == "cooldown" && v.cooldown_remaining_ms == r.r.cooldown_remaining_ms);
  CHECK(cc::seconds_left(v.cooldown_remaining_ms) == (cc::COOLDOWN_TIER_1_MS - 1000 - 10) / 1000 + 1);

  // Unmuted: still refused, and the state reads "cooldown" again.
  CHECK(rest(cmd_of(cc::CHIRP_CMD_UNMUTE)).r.ok && cc::g_state == cc::CHIRP_ACTIVE);
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_COOLDOWN && r.r.cooldown_remaining_ms > 0);
  CHECK(no_witness_since(before));
  v = status_read();
  CHECK(v.state == cc::CHIRP_COOLDOWN && !v.can_send && reason_of(v) == "cooldown");
  CHECK(cc::get_status().state == cc::CHIRP_COOLDOWN && !cc::can_send_chirp());

  // The mute's own timeout: tier 3's hour outlasts a 15-minute mute.
  host_sim::now_ms = first_at + cc::COOLDOWN_TIER_1_MS;
  CHECK(rest(send_of(cc::TPL_INFRA_POWER_OUT)).r.cooldown_tier == 2);
  host_sim::now_ms = cc::g_cooldown.last_chirp_ms + cc::COOLDOWN_TIER_2_MS;
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(r.r.ok && r.r.cooldown_tier == 3);
  const uint32_t third_at = cc::g_cooldown.last_chirp_ms;
  CHECK(rest(m15).r.ok && cc::g_state == cc::CHIRP_MUTED);
  host_sim::now_ms += 15u * 60000u;
  before = host_sim::espnow->sent.size();
  cc::update();                                       // the mute runs out on this pass
  CHECK(!cc::g_muted && cc::g_state == cc::CHIRP_ACTIVE);
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_COOLDOWN && r.r.cooldown_tier == 3);
  CHECK(r.r.cooldown_remaining_ms == cc::COOLDOWN_TIER_3_MS - (host_sim::now_ms - third_at));
  CHECK(no_witness_since(before) && cc::g_cooldown.chirps_sent_today == 3);
  CHECK(reason_of(status_read()) == "cooldown");

  // The pass the mute ran out on beaconed "listening": the cooldown still
  // runs, and a device in its cooldown takes chirps (sweep F194; it said
  // "not listening" here, as it did when the cooldown was the state). And
  // after the cooldown, "listening" still.
  CHECK(presence_listening(before) == std::vector<uint8_t>{1});
  host_sim::now_ms = third_at + cc::COOLDOWN_TIER_3_MS;
  before = host_sim::espnow->sent.size();
  cc::update();
  CHECK(presence_listening(before) == std::vector<uint8_t>{1});
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_mute_does_not_end_the_cooldown\n");
}

// A send made while muted leaves the channel muted (the reviewers of F178).
// A mute silences what comes in (handle_witness() drops chirps while
// g_muted), not the owner, so the send goes out. send_chirp() then stored
// CHIRP_COOLDOWN over CHIRP_MUTED: GET /api/chirp's state and MQTT's
// chirp_state (state_name(get_status().state), canary_wap.ino) read
// "cooldown", and when update() ended the cooldown it set CHIRP_ACTIVE, so
// with the mute still running and chirps still dropped the channel read
// "active" and its presence beacon said "listening". Now the state stays
// CHIRP_MUTED until the mute runs out: "muted" over the running cooldown
// (cannot_send_reason "cooldown"), "muted" and "not listening" after it,
// then "active" and "listening".
void test_a_send_while_muted_stays_muted() {
  boot();
  enabled_channel();
  cc::Command m120 = cmd_of(cc::CHIRP_CMD_MUTE);
  m120.duration_minutes = 120;
  CHECK(rest(m120).r.ok && cc::g_state == cc::CHIRP_MUTED);
  const uint32_t mute_ends = cc::g_mute_until_ms;
  size_t before = host_sim::espnow->sent.size();
  Rest r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(r.r.ok && r.r.cooldown_tier == 1);
  CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_WITNESS});
  const uint32_t sent_at = cc::g_cooldown.last_chirp_ms;

  // Right after it: muted, over the cooldown it started.
  cc::StatusView v = status_read();
  CHECK(v.state == cc::CHIRP_MUTED && v.muted && !v.can_send && reason_of(v) == "cooldown");
  CHECK(v.cooldown_remaining_ms > 0);
  CHECK(cc::get_status().state == cc::CHIRP_MUTED);
  CHECK(std::string(cc::state_name(cc::get_status().state)) == "muted");   // MQTT's chirp_state

  // The cooldown over, the mute not: muted still, and not listening.
  host_sim::now_ms = sent_at + cc::COOLDOWN_TIER_1_MS;
  before = host_sim::espnow->sent.size();
  cc::update();
  CHECK(cc::g_muted && cc::g_state == cc::CHIRP_MUTED && cc::get_status().state == cc::CHIRP_MUTED);
  v = status_read();
  CHECK(v.state == cc::CHIRP_MUTED && v.muted && v.cooldown_remaining_ms == 0);
  CHECK(v.can_send && reason_of(v) == "(none)");
  CHECK(presence_listening(before) == std::vector<uint8_t>{0});

  // The mute runs out: active, and listening.
  host_sim::now_ms = mute_ends;
  before = host_sim::espnow->sent.size();
  cc::update();
  CHECK(!cc::g_muted && cc::g_state == cc::CHIRP_ACTIVE && cc::get_status().state == cc::CHIRP_ACTIVE);
  v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && !v.muted && v.can_send);
  CHECK(presence_listening(before) == std::vector<uint8_t>{1});
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_send_while_muted_stays_muted\n");
}

// The cooldown is over the moment its timer runs out, whatever pass the
// loop task is at. update() drains the owner's commands first and ended
// CHIRP_COOLDOWN only after them, so a send drained in the pass after the
// timer ran out was refused as a cooldown with 0 s left, and GET /api/chirp,
// read before that pass ended, said cannot_send_reason "cooldown" with
// cooldown_remaining_sec 0 (the card said Ready, with Send on). And in the
// cooldown's last second the refused send and the route both said 0 s:
// they say 1 now (seconds_left() rounds up), so "cooldown" never reads 0.
void test_a_send_just_after_the_cooldown_goes_out() {
  boot();
  enabled_channel();
  CHECK(rest(send_of(cc::TPL_INFRA_POWER_OUT)).r.ok);
  const uint32_t sent_at = cc::g_cooldown.last_chirp_ms;

  // Its last second: 400 ms left at the loop task's turn (one poll in).
  host_sim::now_ms = sent_at + cc::COOLDOWN_TIER_1_MS - 400 - cc::COMMAND_POLL_MS;
  size_t before = host_sim::espnow->sent.size();
  Rest r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_COOLDOWN);
  CHECK(r.r.cooldown_remaining_ms == 400 && cc::seconds_left(r.r.cooldown_remaining_ms) == 1);
  CHECK(no_witness_since(before));
  cc::StatusView v = status_read();
  CHECK(reason_of(v) == "cooldown" && !v.can_send && v.cooldown_remaining_ms == 400);
  CHECK(cc::seconds_left(v.cooldown_remaining_ms) == 1);

  // Run out, and no pass since: the route reads it over at once.
  host_sim::now_ms = sent_at + cc::COOLDOWN_TIER_1_MS;
  v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && v.can_send && v.cooldown_remaining_ms == 0);
  CHECK(reason_of(v) == "(none)" && cc::seconds_left(v.cooldown_remaining_ms) == 0);

  // A send drained by the next pass (its drain comes first) goes out.
  host_sim::now_ms = sent_at + cc::COOLDOWN_TIER_1_MS - cc::COMMAND_POLL_MS;
  before = host_sim::espnow->sent.size();
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(r.r.ok && r.r.refusal == cc::SEND_REFUSED_NONE && r.r.cooldown_tier == 2);
  CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_WITNESS});
  CHECK(host_sim::httpd_side_effects == 0);

  // seconds_left() rounds up, so only a timer that ran out reads 0.
  CHECK(cc::seconds_left(0) == 0 && cc::seconds_left(1) == 1 && cc::seconds_left(999) == 1);
  CHECK(cc::seconds_left(1000) == 1 && cc::seconds_left(1001) == 2);
  CHECK(cc::seconds_left(cc::COOLDOWN_TIER_4_MS) == cc::COOLDOWN_TIER_4_MS / 1000);
  CHECK(cc::seconds_left(0xFFFFFFFFu) == 4294968u);
  std::printf("PASS a_send_just_after_the_cooldown_goes_out\n");
}

// A send at a gate's edge goes out or names that gate (the reviewers of
// F178). run_command() named a refused send's reason after send_chirp()
// came back false, from fresh reads of the clocks, so a gate that opened
// between the send's own check and those reads left the answer naming the
// next gate, or none: a cooldown that ran out read as no reason at all (the
// route answers {"success":false} alone, the card "Failed: undefined") or
// as a cooldown with 0 s left; the presence requirement met a millisecond
// late as no reason; a clock set in between as night (MIN_UNIX_TIME is
// 22:13 UTC); 06:00 striking in between as no reason. The refusal is now
// the check that refused the send, read once. Here each millis() call moves
// the uptime clock 1 ms on and each time() call the wall clock 1 s on, and
// the send runs on the loop task (run_command(), as update()'s drain runs
// it) 1 to 8 steps before each edge.
cc::Result send_while_the_clocks_run(cc::ChirpTemplate tpl) {
  host_sim::millis_step_ms = 1;
  host_sim::wall_step_s = 1;
  const cc::Result r = cc::run_command(send_of(tpl));
  host_sim::millis_step_ms = 0;
  host_sim::wall_step_s = 0;
  return r;
}

void test_a_send_at_an_edge_names_why() {
  unsigned refused[4] = {0, 0, 0, 0};
  for (uint32_t n = 1; n <= 8; ++n) {
    // The cooldown's end: tier 1's five minutes, n ms short.
    boot();
    enabled_channel();
    CHECK(rest(send_of(cc::TPL_INFRA_POWER_OUT)).r.ok);
    host_sim::now_ms = cc::g_cooldown.last_chirp_ms + cc::COOLDOWN_TIER_1_MS - n;
    size_t before = host_sim::espnow->sent.size();
    cc::Result r = send_while_the_clocks_run(cc::TPL_INFRA_POWER_OUT);
    if (r.ok) {
      CHECK(r.refusal == cc::SEND_REFUSED_NONE && r.cooldown_tier == 2);
      CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_WITNESS});
    } else {
      ++refused[0];
      CHECK(r.refusal == cc::SEND_REFUSED_COOLDOWN && r.cooldown_tier == 1);
      CHECK(r.cooldown_remaining_ms > 0 && r.cooldown_remaining_ms <= n);
      CHECK(cc::seconds_left(r.cooldown_remaining_ms) == 1 && no_witness_since(before));
    }

    // The presence requirement: ten minutes on, n ms short.
    boot();
    enabled_channel(/*present=*/false);
    host_sim::now_ms = cc::g_session_start_ms + cc::PRESENCE_REQUIRED_MS - n;
    before = host_sim::espnow->sent.size();
    r = send_while_the_clocks_run(cc::TPL_INFRA_POWER_OUT);
    if (r.ok) {
      CHECK(r.refusal == cc::SEND_REFUSED_NONE && r.cooldown_tier == 1);
    } else {
      ++refused[1];
      CHECK(r.refusal == cc::SEND_REFUSED_PRESENCE && r.cooldown_remaining_ms == 0);
      CHECK(no_witness_since(before));
    }

    // The wall clock set: n s before MIN_UNIX_TIME (a template allowed at
    // night: MIN_UNIX_TIME is 22:13 UTC).
    boot();
    enabled_channel();
    host_sim::wall_now = cc::MIN_UNIX_TIME - n;
    before = host_sim::espnow->sent.size();
    r = send_while_the_clocks_run(cc::TPL_INFRA_POWER_OUT);
    if (r.ok) {
      CHECK(r.refusal == cc::SEND_REFUSED_NONE);
    } else {
      ++refused[2];
      CHECK(r.refusal == cc::SEND_REFUSED_CLOCK_UNSYNCED && r.cooldown_remaining_ms == 0);
      CHECK(no_witness_since(before));
    }

    // Night's end: n s before 06:00 UTC, a template not allowed at night.
    boot();
    enabled_channel();
    host_sim::wall_now = 1760076000 - (time_t)n;          // 2025-10-10 06:00:00 UTC, n s short
    before = host_sim::espnow->sent.size();
    r = send_while_the_clocks_run(cc::TPL_INFRA_INTERNET_DOWN);
    if (r.ok) {
      CHECK(r.refusal == cc::SEND_REFUSED_NONE);
    } else {
      ++refused[3];
      CHECK(r.refusal == cc::SEND_REFUSED_NIGHT && r.cooldown_remaining_ms == 0);
      CHECK(no_witness_since(before));
    }
  }
  // Each edge was met shut at least once (the sweep straddles it).
  for (unsigned k : refused) CHECK(k > 0);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_send_at_an_edge_names_why\n");
}

// ── A refused confirm says why; a dismiss says if its vote went (F174) ──

// The confirm route's answer to a refusal (chirp_api.h's send_confirm_answer
// sets the status and the error and message from these lookups).
struct Named {
  int status;
  std::string error;
  std::string message;
};
Named named(cc::ConfirmRefusal why) {
  return Named{cc::confirm_refusal_status(why), cc::confirm_refusal_error(why),
               cc::confirm_refusal_message(why)};
}

// The signed confirmations (ACK frames) sent since `from`.
size_t acks_since(size_t from) {
  size_t n = 0;
  for (uint8_t t : sent_types(from)) n += t == cc::CHIRP_MSG_ACK ? 1 : 0;
  return n;
}

// ── The beacon says listening through a cooldown (sweep F194) ───────────

// The presence beacon's `listening` is whether this device takes a chirp,
// and handle_witness() drops one only while the mute runs. send_presence()
// set it from what the state read as, and an active channel in its send
// cooldown reads CHIRP_COOLDOWN, so the beacon said "not listening" for the
// cooldown's 15 minutes to 4 hours while the device still took every chirp;
// neighbors' nearby lists showed it deaf. Now the flag is the mute alone:
// listening through a cooldown, not while a mute runs (cooldown or not), and
// listening again when the mute ends with the cooldown still running. Each
// beacon is checked against a real witness frame taken or dropped in the
// same state.
void test_the_beacon_says_listening_through_a_cooldown() {
  boot();
  enabled_channel();
  const Neighbor bee = neighbor_of(0x41);
  deliver(bee, presence_of(bee));
  // Three sends, each as the last cooldown ends: tier 3's hour outlasts a
  // 15-minute mute.
  CHECK(rest(send_of(cc::TPL_INFRA_POWER_OUT)).r.ok);
  host_sim::now_ms = cc::g_cooldown.last_chirp_ms + cc::COOLDOWN_TIER_1_MS;
  CHECK(rest(send_of(cc::TPL_INFRA_POWER_OUT)).r.ok);
  host_sim::now_ms = cc::g_cooldown.last_chirp_ms + cc::COOLDOWN_TIER_2_MS;
  CHECK(rest(send_of(cc::TPL_INFRA_POWER_OUT)).r.cooldown_tier == 3);
  deliver(bee, presence_of(bee));                     // still nearby
  const uint32_t sent_at = cc::g_cooldown.last_chirp_ms;

  // The cooldown runs, no mute: the next beacon says listening, and a
  // neighbor's chirp is taken.
  host_sim::now_ms = sent_at + cc::PRESENCE_INTERVAL_MS;
  size_t before = host_sim::espnow->sent.size();
  cc::update();
  CHECK(cc::get_cooldown_remaining_ms() > 0 && cc::get_status().state == cc::CHIRP_COOLDOWN);
  CHECK(presence_listening(before) == std::vector<uint8_t>{1});
  deliver(bee, witness_of(bee, cc::TPL_INFRA_POWER_OUT, cc::CHIRP_URG_INFO, cc::DETAIL_NONE, 0x61));
  CHECK(cc::g_recent_chirp_count == 1);

  // Muted in the cooldown: not listening, and a chirp is dropped.
  cc::Command m15 = cmd_of(cc::CHIRP_CMD_MUTE);
  m15.duration_minutes = 15;
  CHECK(rest(m15).r.ok && cc::g_state == cc::CHIRP_MUTED);
  host_sim::now_ms += cc::PRESENCE_INTERVAL_MS;
  before = host_sim::espnow->sent.size();
  cc::update();
  CHECK(presence_listening(before) == std::vector<uint8_t>{0});
  deliver(bee, witness_of(bee, cc::TPL_INFRA_WATER_ISSUE, cc::CHIRP_URG_INFO, cc::DETAIL_NONE, 0x62));
  CHECK(cc::g_recent_chirp_count == 1);

  // The mute runs out, the cooldown does not: listening again, chirps taken.
  host_sim::now_ms = cc::g_mute_until_ms;
  before = host_sim::espnow->sent.size();
  cc::update();
  CHECK(!cc::g_muted && cc::get_cooldown_remaining_ms() > 0);
  CHECK(presence_listening(before) == std::vector<uint8_t>{1});
  deliver(bee, witness_of(bee, cc::TPL_INFRA_GAS_SMELL, cc::CHIRP_URG_INFO, cc::DETAIL_NONE, 0x63));
  CHECK(cc::g_recent_chirp_count == 2);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS the_beacon_says_listening_through_a_cooldown\n");
}

// ── A mute needs a channel that is on (sweep F192) ──────────────────────

cc::Command mute_of(uint8_t minutes) {
  cc::Command c = cc::make_command(cc::CHIRP_CMD_MUTE);
  c.duration_minutes = minutes;
  return c;
}

// The presence beacons sent since `from`: each one's session id and emoji.
struct Beacon {
  std::array<uint8_t, cc::SESSION_ID_SIZE> sid;
  std::string emoji;
};
std::vector<Beacon> beacons_since(size_t from) {
  std::vector<Beacon> out;
  const auto& sent = host_sim::espnow->sent;
  for (size_t i = from; i < sent.size(); ++i) {
    const cc::ChirpHeader* h = reinterpret_cast<const cc::ChirpHeader*>(sent[i].bytes.data());
    if (h->msg_type != cc::CHIRP_MSG_PRESENCE) continue;
    cc::ChirpPresencePayload p;
    memcpy(&p, sent[i].bytes.data() + sizeof(cc::ChirpHeader), sizeof p);
    Beacon b;
    memcpy(b.sid.data(), h->session_id, b.sid.size());
    b.emoji.assign(p.emoji, strnlen(p.emoji, sizeof p.emoji));
    out.push_back(b);
  }
  return out;
}
bool all_zero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (p[i] != 0) return false;
  }
  return true;
}

// A mute on a channel that is off turned it on with no session: mute() set
// CHIRP_MUTED whatever the state, so is_enabled() read true, GET /api/chirp
// read "muted" and, once the mute ran out, "active", both with an empty
// emoji, the passes ran (taking frames, sending beacons) under the all-zero
// session id, the mute's own frame went out under it, and a later enable
// answered success with an empty emoji: enable() makes a session only from
// CHIRP_DISABLED. Now a mute and an unmute on a channel that is off are
// refused by name (MUTE_REFUSED_DISABLED, which chirp_api.h answers 409
// chirp_disabled), the channel off before a duration it lacks, and nothing
// moves; the enable that follows makes a real session.
void test_a_mute_needs_a_channel_that_is_on() {
  boot();
  // Turned on once and off again, so ESP-NOW is up and a frame would show.
  enabled_channel(false);
  CHECK(rest(cmd_of(cc::CHIRP_CMD_DISABLE)).r.ok && cc::g_state == cc::CHIRP_DISABLED);
  host_sim::espnow->sent.clear();

  Rest r = rest(mute_of(30));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && r.r.mute_refusal == cc::MUTE_REFUSED_DISABLED);
  CHECK(cc::g_state == cc::CHIRP_DISABLED && !cc::is_enabled() && !cc::g_muted);
  CHECK(host_sim::espnow->sent.empty());                 // no mute frame under a zero session id
  r = rest(mute_of(7));                                  // the channel off is named first
  CHECK(!r.r.ok && r.r.mute_refusal == cc::MUTE_REFUSED_DISABLED);
  r = rest(cmd_of(cc::CHIRP_CMD_UNMUTE));
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok && r.r.mute_refusal == cc::MUTE_REFUSED_DISABLED);
  CHECK(cc::g_state == cc::CHIRP_DISABLED);
  CHECK(std::string(cc::mute_refusal_error(r.r.mute_refusal)) == "chirp_disabled");
  CHECK(cc::mute_refusal_status(r.r.mute_refusal) == 409);
  // The words the route answers and the dashboard shows (it falls back to the
  // bare code when a message is empty), and none for a mute that ran.
  CHECK(std::string(cc::mute_refusal_message(cc::MUTE_REFUSED_DISABLED)) == "Chirp channel is not enabled");
  CHECK(std::string(cc::mute_refusal_message(cc::MUTE_REFUSED_DURATION)) ==
        "Duration must be 15, 30, 60, or 120 minutes");
  CHECK(cc::mute_refusal_message(cc::MUTE_REFUSED_NONE) == nullptr &&
        cc::mute_refusal_error(cc::MUTE_REFUSED_NONE) == nullptr);
  cc::StatusView v = status_read();
  CHECK(v.state == cc::CHIRP_DISABLED && !v.muted && v.mute_remaining_ms == 0);
  CHECK(v.session_emoji[0] == '\0' && reason_of(v) == "disabled");

  // Past the 30 minutes and a beacon interval: still off, nothing sent.
  host_sim::now_ms += 31u * 60000u + cc::PRESENCE_INTERVAL_MS;
  cc::update();
  CHECK(cc::g_state == cc::CHIRP_DISABLED && host_sim::espnow->sent.empty());
  CHECK(status_read().state == cc::CHIRP_DISABLED);

  // The enable makes a real session, and its beacon carries it.
  r = rest(cmd_of(cc::CHIRP_CMD_ENABLE));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && cc::g_state == cc::CHIRP_ACTIVE);
  CHECK(r.r.session_emoji[0] != '\0' && cc::g_session.valid);
  CHECK(!all_zero(cc::get_session_id(), cc::SESSION_ID_SIZE));
  const std::vector<Beacon> b = beacons_since(0);
  CHECK(b.size() == 1);
  CHECK(memcmp(b[0].sid.data(), cc::get_session_id(), cc::SESSION_ID_SIZE) == 0);
  CHECK(b[0].emoji == r.r.session_emoji);
  v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && strcmp(v.session_emoji, r.r.session_emoji) == 0);

  // On, the same commands run: a 30-minute mute, an unmute.
  r = rest(mute_of(30));
  CHECK(r.r.ok && r.r.mute_refusal == cc::MUTE_REFUSED_NONE && cc::g_state == cc::CHIRP_MUTED);
  r = rest(cmd_of(cc::CHIRP_CMD_UNMUTE));
  CHECK(r.r.ok && r.r.mute_refusal == cc::MUTE_REFUSED_NONE && cc::g_state == cc::CHIRP_ACTIVE);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_mute_needs_a_channel_that_is_on\n");
}

// A disable ends a running mute (sweep F192). The mute outlived it: the next
// enable's new session read "active", its beacon said listening, and the
// status said muted with the old mute's time left, while handle_witness()
// still dropped every chirp until the old mute ran out.
void test_a_disable_ends_the_mute() {
  boot();
  enabled_channel(false);
  CHECK(rest(mute_of(120)).r.ok && cc::g_muted && cc::g_state == cc::CHIRP_MUTED);
  CHECK(rest(cmd_of(cc::CHIRP_CMD_DISABLE)).r.ok);
  CHECK(!cc::g_muted && cc::g_mute_until_ms == 0 && cc::g_state == cc::CHIRP_DISABLED);
  CHECK(!status_read().muted);

  host_sim::now_ms += 60000u;
  const size_t before = host_sim::espnow->sent.size();
  Rest r = rest(cmd_of(cc::CHIRP_CMD_ENABLE));
  CHECK(r.r.ok && cc::g_state == cc::CHIRP_ACTIVE && !cc::is_muted());
  cc::StatusView v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && !v.muted && v.mute_remaining_ms == 0);
  CHECK(presence_listening(before) == std::vector<uint8_t>{1});

  // The new session takes a neighbor's chirp: no mute drops it.
  host_sim::now_ms += cc::PRESENCE_REQUIRED_MS;
  const Neighbor bee = neighbor_of(0x31);
  deliver(bee, presence_of(bee));
  deliver(bee, witness_of(bee, cc::TPL_INFRA_POWER_OUT, cc::CHIRP_URG_INFO, cc::DETAIL_NONE, 0x51));
  CHECK(cc::g_recent_chirp_count == 1);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_disable_ends_the_mute\n");
}

// ── A mute counts across the uptime wrap (sweep F361) ───────────────────

// mute() stores its deadline, the time it ran plus the duration, and
// update(), is_muted(), get_status() and read_status() compared it with
// millis() as an absolute time. A deadline past uptime's 49.7-day wrap
// wraps to a small number and read as long past: a 15-minute mute set 5
// minutes before the wrap was ended by the very pass that ran it (update()
// drains the command, then ends a mute whose deadline it reads as passed),
// and is_muted() read false from the start, so handle_witness() would take
// the chirps it was set to drop and the beacon said listening. Each reader
// now counts the time to the deadline across the wrap (mute_left_ms()).
// These tests put the clock back when they end.
void test_a_mute_set_before_the_wrap_runs_its_length() {
  const uint32_t clock_was = host_sim::now_ms;
  constexpr uint32_t MIN_MS = 60000;
  host_sim::now_ms = 0u - 15u * MIN_MS;
  boot();
  enabled_channel();                                   // on, ten minutes present
  const Neighbor bee = neighbor_of(0x42);
  deliver(bee, presence_of(bee));
  const uint32_t before_mute = host_sim::now_ms;
  CHECK(before_mute == 0u - 5u * MIN_MS + cc::COMMAND_POLL_MS);   // 2^32 - 5 min, and the drain's 5 ms
  CHECK(rest(mute_of(15)).r.ok);
  CHECK(cc::g_muted && cc::g_state == cc::CHIRP_MUTED);   // the pass that ran it kept it
  const uint32_t until = cc::g_mute_until_ms;
  CHECK(until == before_mute + cc::COMMAND_POLL_MS + 15u * MIN_MS);   // the drain ran on the first sleep
  CHECK(until < before_mute);                          // the deadline wrapped
  const uint32_t left = until - host_sim::now_ms;
  CHECK(left == 15u * MIN_MS);

  // Muted, with its 15 minutes left, live and in the published view.
  CHECK(cc::is_muted());
  cc::ChirpStatus live = cc::get_status();
  CHECK(live.muted && live.mute_remaining_ms == left);
  cc::StatusView v = status_read();
  CHECK(v.state == cc::CHIRP_MUTED && v.muted && v.mute_remaining_ms == left);
  CHECK(v.mute_remaining_ms / 1000 == 900);            // the route's mute_remaining_sec

  // A pass keeps it; a neighbor's chirp is dropped; the beacon says deaf.
  cc::update();
  CHECK(cc::g_muted && cc::g_state == cc::CHIRP_MUTED);
  deliver(bee, witness_of(bee, cc::TPL_INFRA_POWER_OUT, cc::CHIRP_URG_INFO, cc::DETAIL_NONE, 0x43));
  CHECK(cc::g_recent_chirp_count == 0);
  size_t before = host_sim::espnow->sent.size();
  host_sim::now_ms += cc::PRESENCE_INTERVAL_MS;        // the next beacon is due
  cc::update();
  CHECK(presence_listening(before) == std::vector<uint8_t>{0});
  CHECK(status_read().mute_remaining_ms == left - cc::PRESENCE_INTERVAL_MS);

  // Past the wrap: a millisecond before the deadline it still runs.
  host_sim::now_ms = 2u * MIN_MS;
  cc::update();
  CHECK(cc::is_muted() && cc::g_state == cc::CHIRP_MUTED);
  CHECK(status_read().mute_remaining_ms == until - 2u * MIN_MS);
  host_sim::now_ms = until - 1;
  cc::update();
  CHECK(cc::is_muted() && cc::get_status().mute_remaining_ms == 1);
  v = status_read();
  CHECK(v.state == cc::CHIRP_MUTED && v.muted && v.mute_remaining_ms == 1);

  // At it, the mute is over, and the pass ends it.
  host_sim::now_ms = until;
  CHECK(!cc::is_muted() && cc::get_status().mute_remaining_ms == 0);
  CHECK(status_read().mute_remaining_ms == 0);          // counted at the read, before the pass
  before = host_sim::espnow->sent.size();
  host_sim::now_ms = until + cc::PRESENCE_INTERVAL_MS;  // a beacon due on the pass that ends it
  cc::update();
  CHECK(!cc::g_muted && cc::g_state == cc::CHIRP_ACTIVE);
  v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && !v.muted && v.mute_remaining_ms == 0);
  CHECK(presence_listening(before) == std::vector<uint8_t>{1});
  deliver(bee, presence_of(bee));
  deliver(bee, witness_of(bee, cc::TPL_INFRA_POWER_OUT, cc::CHIRP_URG_INFO, cc::DETAIL_NONE, 0x44));
  CHECK(cc::g_recent_chirp_count == 1);
  CHECK(host_sim::httpd_side_effects == 0);
  host_sim::now_ms = clock_was;
  std::printf("PASS a_mute_set_before_the_wrap_runs_its_length\n");
}

// The same count at the clock's edges: a mute of every length the channel
// offers, set at 2^32 - 1, at 2^32 less exactly 15 minutes and a
// millisecond either side (a 15-minute deadline at, before and after the
// wrap), at 0, and at 2^31 + 1 and 2^31 - 1 (where the deadline's sign as an
// int32 turns), and stepped in 1 ms, 1 s and 10 min passes. The 1 ms passes
// cover its first two seconds, two seconds about the wrap when the mute
// runs across it, and two seconds either side of its deadline; the others
// cover its whole length and two strides past it. At every step, before the
// pass and after it, is_muted(), get_status() and read_status() say the time
// left to the deadline (none from the deadline on), and the flag ends
// exactly once, on the first pass at or past the deadline.
void test_a_mute_ends_once_at_its_deadline_across_the_wrap() {
  const uint32_t clock_was = host_sim::now_ms;
  constexpr uint32_t MIN_MS = 60000;
  const uint32_t starts[] = {0xFFFFFFFFu, 0u - 15u * MIN_MS - 1u, 0u - 15u * MIN_MS, 0u - 15u * MIN_MS + 1u,
                             0u, 0x80000001u, 0x7FFFFFFFu};
  const uint8_t lengths[] = {15, 30, 60, 120};
  const uint32_t strides[] = {1, 1000, 10u * MIN_MS};
  unsigned steps = 0;
  for (uint32_t start : starts) {
    for (uint8_t minutes : lengths) {
      const uint32_t dur = minutes * MIN_MS;
      for (uint32_t stride : strides) {
        // The offsets from `start` this run visits, in order.
        std::vector<uint32_t> at;
        if (stride == 1) {
          const uint32_t wrap = 0u - start;            // the offset of 2^32 (0 for start 0)
          for (uint32_t e = 0; e < 2000; ++e) at.push_back(e);
          if (wrap > 2000 && wrap < dur) {
            for (uint32_t e = wrap - 1000; e < wrap + 1000; ++e) at.push_back(e);
          }
          for (uint32_t e = dur - 2000; e < dur + 2000; ++e) at.push_back(e);
          std::sort(at.begin(), at.end());
          at.erase(std::unique(at.begin(), at.end()), at.end());
        } else {
          for (uint32_t e = 0; e <= dur + 2 * stride; e += stride) at.push_back(e);
        }

        host_sim::now_ms = start - MIN_MS;
        boot();
        enabled_channel(false);
        host_sim::now_ms = start;
        // No mute yet: unmuted at `start`, past 2^31 too (its deadline is 0).
        CHECK(!cc::is_muted() && cc::get_status().mute_remaining_ms == 0);
        CHECK(status_read().mute_remaining_ms == 0);
        CHECK(cc::mute(minutes, nullptr));             // the loop task's command, at `start`
        CHECK(cc::g_mute_until_ms == start + dur);
        unsigned ends = 0;
        uint32_t ended_at = 0;
        for (uint32_t e : at) {
          host_sim::now_ms = start + e;
          const uint32_t want = e < dur ? dur - e : 0;
          if (cc::is_muted() != (want > 0)) {
            std::fprintf(stderr, "a %u-minute mute set at %u, %u ms on (stride %u ms): is_muted() %d\n",
                         (unsigned)minutes, start, e, stride, (int)cc::is_muted());
          }
          CHECK(cc::is_muted() == (want > 0));
          CHECK(cc::get_status().mute_remaining_ms == want);
          if (e > 0) CHECK(status_read().mute_remaining_ms == want);   // the last pass's copy, counted now
          const bool was = cc::g_muted;
          cc::update();
          if (was && !cc::g_muted) {
            ++ends;
            ended_at = e;
          }
          CHECK(cc::g_muted == (want > 0));
          CHECK(cc::is_muted() == (want > 0));
          const cc::ChirpStatus live = cc::get_status();
          CHECK(live.muted == (want > 0) && live.mute_remaining_ms == want);
          const cc::StatusView v = status_read();
          CHECK(v.muted == (want > 0) && v.mute_remaining_ms == want);
          CHECK(v.state == (want > 0 ? cc::CHIRP_MUTED : cc::CHIRP_ACTIVE));
          ++steps;
        }
        CHECK(ends == 1);
        CHECK(ended_at == (dur + stride - 1) / stride * stride);   // the first visited offset at or past dur
        CHECK(stride == 10u * MIN_MS || ended_at == dur);
      }
    }
  }
  CHECK(steps > 250000);
  host_sim::now_ms = clock_was;
  std::printf("PASS a_mute_ends_once_at_its_deadline_across_the_wrap (%u steps)\n", steps);
}

// With no mute running the channel reads unmuted at any uptime. The stored
// deadline is then 0 (a boot, an unmute, a disable) or a deadline that has
// passed (a mute that ran out, which update() leaves in place), and the
// difference read as signed says nothing about it: `0 - now` is positive as
// an int32 for every `now` past 2^31 ms (24.8 days of uptime), and so is a
// deadline more than 2^31 ms gone. mute_left_ms() counts only while the flag
// is set; without that, a channel up that long would read muted after any
// unmute, drop every chirp and beacon "not listening". Read at the sign
// boundary, between it and the wrap, and at the wrap: never muted, unmuted,
// disabled and enabled again, and more than 24.8 days after a mute ran out. At each, before and after a pass, is_muted(), get_status() and
// read_status() say no mute, the pass's beacon says listening, and a
// neighbor's chirp is taken. These tests put the clock back when they end.
void test_no_mute_reads_unmuted_past_the_sign_boundary() {
  const uint32_t clock_was = host_sim::now_ms;
  constexpr uint32_t MIN_MS = 60000;
  const uint32_t reads[] = {0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xC0000000u, 0xFFFFFFFFu};
  enum How { kNever, kUnmuted, kEnabledAgain, kRanOut, kHows };
  const char* const names[kHows] = {"never muted", "unmuted", "disabled and enabled again", "a mute that ran out"};
  uint8_t seed = 0x60;
  unsigned cases = 0;
  for (uint32_t at : reads) {
    for (int how = kNever; how < kHows; ++how) {
      // On, ten minutes present, twenty minutes before `at`; for a mute that
      // runs out, 2^31 ms and 15 minutes earlier still, so its deadline is
      // 2^31 ms and about twenty minutes gone at `at`.
      const uint32_t lead = cc::PRESENCE_REQUIRED_MS + 20u * MIN_MS;
      host_sim::now_ms = (how == kRanOut) ? at - 0x80000000u - 15u * MIN_MS - lead : at - lead;
      boot();
      enabled_channel();
      if (how == kUnmuted) {
        CHECK(rest(mute_of(15)).r.ok && cc::is_muted());
        CHECK(rest(cmd_of(cc::CHIRP_CMD_UNMUTE)).r.ok);
        CHECK(!cc::g_muted && cc::g_mute_until_ms == 0);
      } else if (how == kEnabledAgain) {
        CHECK(rest(mute_of(120)).r.ok && cc::is_muted());
        CHECK(rest(cmd_of(cc::CHIRP_CMD_DISABLE)).r.ok);
        CHECK(rest(cmd_of(cc::CHIRP_CMD_ENABLE)).r.ok);
        CHECK(!cc::g_muted && cc::g_mute_until_ms == 0 && cc::g_state == cc::CHIRP_ACTIVE);
      } else if (how == kRanOut) {
        CHECK(rest(mute_of(15)).r.ok && cc::is_muted());
        const uint32_t until = cc::g_mute_until_ms;
        host_sim::now_ms = until;
        cc::update();                                  // the pass at the deadline ends it
        CHECK(!cc::g_muted && cc::g_mute_until_ms == until && cc::g_state == cc::CHIRP_ACTIVE);
        CHECK(at - until > 0x80000000u && at - until < 0x80000000u + lead);   // more than 2^31 ms gone at `at`
      }
      host_sim::now_ms = at;
      for (int pass = 0; pass < 2; ++pass) {
        const bool im = cc::is_muted();
        const cc::ChirpStatus live = cc::get_status();
        const cc::StatusView v = status_read();
        if (im || live.muted || live.mute_remaining_ms != 0 || v.muted || v.mute_remaining_ms != 0) {
          std::fprintf(stderr, "%s, read at %u (%s the pass): is_muted() %d, get_status() %u ms, read_status() %u ms\n",
                       names[how], at, pass == 0 ? "before" : "after", (int)im, live.mute_remaining_ms,
                       v.mute_remaining_ms);
        }
        CHECK(!im && !live.muted && live.mute_remaining_ms == 0);
        CHECK(!v.muted && v.mute_remaining_ms == 0);
        CHECK(live.state == cc::CHIRP_ACTIVE && (pass == 0 || v.state == cc::CHIRP_ACTIVE));
        if (pass == 0) {
          const size_t before = host_sim::espnow->sent.size();
          cc::update();                                // a beacon due: twenty minutes since the last
          CHECK(presence_listening(before) == std::vector<uint8_t>{1});
        }
      }
      const Neighbor nb = neighbor_of(seed++);
      deliver(nb, presence_of(nb));
      deliver(nb, witness_of(nb, cc::TPL_INFRA_POWER_OUT, cc::CHIRP_URG_INFO, cc::DETAIL_NONE, 0x61));
      CHECK(cc::g_recent_chirp_count == 1);           // taken, not dropped as muted
      ++cases;
    }
  }
  CHECK(cases == 20);
  CHECK(host_sim::httpd_side_effects == 0);
  host_sim::now_ms = clock_was;
  std::printf("PASS no_mute_reads_unmuted_past_the_sign_boundary (%u cases)\n", cases);
}

// ── The tiers reset after 24 quiet hours and outlive a disable (F193) ───

// spec 2.5.4: escalating cooldowns, "Reset: 24 hours of no chirps".
// reset_cooldown_if_stale() cleared the tiers 24 hours after the FIRST send
// of the run, whatever went out since, and zeroed the last send's time too,
// so it ended a running cooldown; enable() zeroed them, so a disable and an
// enable cost the 10-minute presence requirement instead of the tier's
// cooldown. Now the reset counts from the last send (expire_stale_tiers(),
// on every pass, on or off, and before a send), an enable keeps the tiers,
// and the count stops at 255. They still live in RAM: boot() here clears
// them as a reboot does, and no send, disable or enable writes NVS.
//
// These tests set the uptime clock to a boot's (the stub's 1000 ms) and put
// it back when they end: a run of 43 days would otherwise take the next
// test's clock across the 49.7-day wrap.

constexpr uint32_t HOUR_MS = 60u * 60000u;

// The cooldown a send that leaves the count at `sent` starts (the spec's
// table, from the constants, not from the code under test).
uint32_t tier_cooldown_ms(uint8_t sent) {
  switch (sent) {
    case 1: return cc::COOLDOWN_TIER_1_MS;    // 5 min
    case 2: return cc::COOLDOWN_TIER_2_MS;    // 15 min
    case 3: return cc::COOLDOWN_TIER_3_MS;    // 1 h
    default: return cc::COOLDOWN_TIER_4_MS;   // 4 h
  }
}

// A send as soon as the running cooldown lets one out (now, when none runs
// or it is over), which goes out.
cc::Result send_when_due() {
  const uint8_t sent = cc::g_cooldown.chirps_sent_today;
  if (sent > 0) {
    const uint32_t due = cc::g_cooldown.last_chirp_ms + tier_cooldown_ms(sent);
    if ((int32_t)(due - host_sim::now_ms) > 0) host_sim::now_ms = due;
  }
  const cc::Result r = rest(send_of(cc::TPL_INFRA_POWER_OUT)).r;
  CHECK(r.ok && r.refusal == cc::SEND_REFUSED_NONE);
  return r;
}

// The last "chirp sent" line the channel logged.
std::string last_sent_log() {
  for (auto it = g_health.rbegin(); it != g_health.rend(); ++it) {
    if (it->rfind("chirp sent: ", 0) == 0) return *it;
  }
  return "";
}

bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// The sweep's own case: a 4-hour cooldown started 22 hours into a run of
// sends is not over 2 hours later. The old reset fired 24 hours after the
// run's first send and ended it. A busy day keeps the top tier; 24 hours
// with no send, and only that, resets it (24 hours exactly does not).
void test_the_tiers_reset_24_quiet_hours_after_the_last_send() {
  const uint32_t clock_was = host_sim::now_ms;
  host_sim::now_ms = 1000;
  boot();
  enabled_channel();
  const host_sim::NvsStore nvs_before = *host_sim::nvs;
  CHECK(send_when_due().cooldown_tier == 1);
  const uint32_t run_start = cc::g_cooldown.last_chirp_ms;
  CHECK(send_when_due().cooldown_tier == 2);
  CHECK(send_when_due().cooldown_tier == 3);
  CHECK(send_when_due().cooldown_tier == 4);
  CHECK(send_when_due().cooldown_tier == 4);          // the fifth send
  // The log names the tier, not the count (it said "tier 5").
  CHECK(ends_with(last_sent_log(), "tier 4)"));
  // Sends every 4 hours, then one 22 hours into the run.
  while (cc::g_cooldown.last_chirp_ms + 2u * cc::COOLDOWN_TIER_4_MS <= run_start + 22u * HOUR_MS) {
    CHECK(send_when_due().cooldown_tier == 4);
  }
  CHECK(cc::g_cooldown.last_chirp_ms + cc::COOLDOWN_TIER_4_MS <= run_start + 22u * HOUR_MS);
  host_sim::now_ms = run_start + 22u * HOUR_MS;
  CHECK(send_when_due().cooldown_tier == 4);
  const uint32_t at_22h = cc::g_cooldown.last_chirp_ms;
  CHECK(at_22h == run_start + 22u * HOUR_MS + cc::COMMAND_POLL_MS);

  // 24 hours and 1 ms into the run: a pass, then a send. The cooldown runs
  // for 2 hours more, at the top tier.
  host_sim::now_ms = run_start + 24u * HOUR_MS + 1;
  cc::update();
  size_t before = host_sim::espnow->sent.size();
  Rest r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_COOLDOWN && r.r.cooldown_tier == 4);
  CHECK(r.r.cooldown_remaining_ms == cc::COOLDOWN_TIER_4_MS - (host_sim::now_ms - at_22h));
  CHECK(r.r.cooldown_remaining_ms > 2u * HOUR_MS - 1000u);
  CHECK(no_witness_since(before));
  cc::StatusView v = status_read();
  CHECK(v.state == cc::CHIRP_COOLDOWN && v.cooldown_tier == 4 && !v.can_send);
  CHECK(reason_of(v) == "cooldown" && v.last_chirp_sent_ms == at_22h);

  // When it runs out the next send goes out, still at the top tier.
  CHECK(send_when_due().cooldown_tier == 4);
  const uint32_t last = cc::g_cooldown.last_chirp_ms;
  CHECK(last == at_22h + cc::COOLDOWN_TIER_4_MS + cc::COMMAND_POLL_MS);

  // 24 quiet hours exactly: not yet.
  host_sim::now_ms = last + cc::COOLDOWN_RESET_MS;
  cc::update();
  v = status_read();
  CHECK(v.cooldown_tier == 4 && v.can_send && v.last_chirp_sent_ms == last);
  CHECK(cc::get_cooldown_tier() == 4);

  // A millisecond more: the tiers reset, and the next send is tier 1's.
  host_sim::now_ms = last + cc::COOLDOWN_RESET_MS + 1;
  cc::update();
  v = status_read();
  CHECK(v.state == cc::CHIRP_ACTIVE && v.can_send && v.cooldown_tier == 0);
  CHECK(v.last_chirp_sent_ms == 0 && v.cooldown_remaining_ms == 0);
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(r.r.ok && r.r.cooldown_tier == 1);
  CHECK(cc::get_cooldown_remaining_ms() == cc::COOLDOWN_TIER_1_MS);
  CHECK(ends_with(last_sent_log(), "tier 1)"));

  CHECK(*host_sim::nvs == nvs_before);                // the tiers are RAM only
  CHECK(host_sim::httpd_side_effects == 0);
  host_sim::now_ms = clock_was;
  std::printf("PASS the_tiers_reset_24_quiet_hours_after_the_last_send\n");
}

// A send drained in the pass that ends 24 quiet hours (update()'s drain
// comes before its own reset) counts from tier 1: send_chirp() expires the
// tiers itself. At 24 hours exactly it counts on.
void test_a_send_that_ends_a_quiet_day_counts_from_tier_1() {
  const uint32_t clock_was = host_sim::now_ms;
  for (const uint32_t past : {0u, 1u}) {
    host_sim::now_ms = 1000;
    boot();
    enabled_channel();
    CHECK(send_when_due().cooldown_tier == 1);
    CHECK(send_when_due().cooldown_tier == 2);
    const uint32_t last = cc::g_cooldown.last_chirp_ms;
    host_sim::now_ms = last + 1000;
    cc::update();                                     // a pass well inside the day
    // The handler's first sleep moves the clock COMMAND_POLL_MS on, and the
    // loop task's turn is only the drain.
    host_sim::now_ms = last + cc::COOLDOWN_RESET_MS + past - cc::COMMAND_POLL_MS;
    const Rest r = rest(send_of(cc::TPL_INFRA_POWER_OUT), 1, Turn::kDrain);
    CHECK(r.r.ok && r.r.refusal == cc::SEND_REFUSED_NONE);
    CHECK(cc::g_cooldown.last_chirp_ms == last + cc::COOLDOWN_RESET_MS + past);
    if (past == 0) {
      CHECK(r.r.cooldown_tier == 3 && cc::g_cooldown.chirps_sent_today == 3);
      CHECK(cc::get_cooldown_remaining_ms() == cc::COOLDOWN_TIER_3_MS);
    } else {
      CHECK(r.r.cooldown_tier == 1 && cc::g_cooldown.chirps_sent_today == 1);
      CHECK(cc::get_cooldown_remaining_ms() == cc::COOLDOWN_TIER_1_MS);
    }
    cc::update();
    CHECK(status_read().cooldown_tier == (past == 0 ? 3 : 1));
  }
  CHECK(host_sim::httpd_side_effects == 0);
  host_sim::now_ms = clock_was;
  std::printf("PASS a_send_that_ends_a_quiet_day_counts_from_tier_1\n");
}

// A disable and an enable keep the tiers: the new session (a new key,
// session id and emoji, spec 2.3) waits out the presence requirement, and
// then the cooldown's rest. enable() zeroed them, so the send 10 minutes
// after the enable went out at tier 1 inside tier 2's 15 minutes. And the
// 24 quiet hours count while the channel is off: its status shows the reset
// when it comes.
void test_a_disable_keeps_the_tiers() {
  const uint32_t clock_was = host_sim::now_ms;
  host_sim::now_ms = 1000;
  boot();
  enabled_channel();
  const host_sim::NvsStore nvs_before = *host_sim::nvs;
  CHECK(send_when_due().cooldown_tier == 1);
  CHECK(send_when_due().cooldown_tier == 2);
  const uint32_t second = cc::g_cooldown.last_chirp_ms;
  uint8_t old_session[sizeof cc::g_session.session_id];
  memcpy(old_session, cc::g_session.session_id, sizeof old_session);

  CHECK(rest(cmd_of(cc::CHIRP_CMD_DISABLE)).r.ok && cc::g_state == cc::CHIRP_DISABLED);
  cc::StatusView v = status_read();
  CHECK(v.state == cc::CHIRP_DISABLED && v.cooldown_tier == 2 && v.last_chirp_sent_ms == second);

  CHECK(rest(cmd_of(cc::CHIRP_CMD_ENABLE)).r.ok);
  CHECK(memcmp(old_session, cc::g_session.session_id, sizeof old_session) != 0);   // a new session
  v = status_read();
  CHECK(v.state == cc::CHIRP_COOLDOWN && v.cooldown_tier == 2 && !v.can_send);
  // The new session's presence requirement first, as before.
  size_t before = host_sim::espnow->sent.size();
  Rest r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_PRESENCE && r.r.cooldown_tier == 2);
  // Ten minutes on: the cooldown's last 5 minutes still run.
  host_sim::now_ms = cc::g_session_start_ms + cc::PRESENCE_REQUIRED_MS;
  cc::update();
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_COOLDOWN && r.r.cooldown_tier == 2);
  CHECK(r.r.cooldown_remaining_ms == cc::COOLDOWN_TIER_2_MS - (host_sim::now_ms - second));
  CHECK(no_witness_since(before));
  v = status_read();
  CHECK(v.state == cc::CHIRP_COOLDOWN && reason_of(v) == "cooldown");
  // When it runs out, the send goes out a tier up.
  CHECK(send_when_due().cooldown_tier == 3);
  const uint32_t third = cc::g_cooldown.last_chirp_ms;

  // Off for 24 hours and a millisecond: the reset lands while off.
  CHECK(rest(cmd_of(cc::CHIRP_CMD_DISABLE)).r.ok);
  host_sim::now_ms = third + cc::COOLDOWN_RESET_MS;
  cc::update();
  CHECK(status_read().cooldown_tier == 3);
  host_sim::now_ms = third + cc::COOLDOWN_RESET_MS + 1;
  cc::update();
  v = status_read();
  CHECK(v.state == cc::CHIRP_DISABLED && v.cooldown_tier == 0 && v.last_chirp_sent_ms == 0);
  enabled_channel();
  CHECK(send_when_due().cooldown_tier == 1);

  CHECK(*host_sim::nvs == nvs_before);                // the tiers are RAM only
  CHECK(host_sim::httpd_side_effects == 0);
  host_sim::now_ms = clock_was;
  std::printf("PASS a_disable_keeps_the_tiers\n");
}

// The last_chirp_age_min of each presence beacon this device sent since
// `from`, in order (255: no send).
std::vector<uint8_t> presence_ages(size_t from) {
  std::vector<uint8_t> out;
  const auto& sent = host_sim::espnow->sent;
  for (size_t i = from; i < sent.size(); ++i) {
    const cc::ChirpHeader* h = reinterpret_cast<const cc::ChirpHeader*>(sent[i].bytes.data());
    if (h->msg_type != cc::CHIRP_MSG_PRESENCE) continue;
    CHECK(sent[i].bytes.size() >= sizeof(cc::ChirpHeader) + sizeof(cc::ChirpPresencePayload));
    cc::ChirpPresencePayload p;
    memcpy(&p, sent[i].bytes.data() + sizeof(cc::ChirpHeader), sizeof p);
    out.push_back(p.last_chirp_age_min);
  }
  return out;
}

// A new session beacons no last send (sweep F415). A presence beacon says
// how many minutes ago this device chirped, and send_presence() counted that
// from g_last_chirp_sent_ms, which only a reboot cleared. So after a disable
// and an enable the new session (a new key, session id and emoji, spec 2.3)
// beaconed the old session's last-send age: a send, a disable 7 minutes
// later and an enable, and the new session's first beacon said 7, which
// links the two sessions for any neighbor that heard the old one chirp
// (spec 6.4: no chirp history across sessions). Now its beacons say 255
// until its own first send, and count from that send. The cooldown's timer
// is the device's and stays (sweep F193): the new session's send is a tier up.
void test_a_new_session_beacons_no_last_send() {
  boot();
  enabled_channel();
  size_t before = host_sim::espnow->sent.size();
  host_sim::now_ms += 60000;
  cc::update();
  CHECK(presence_ages(before) == std::vector<uint8_t>{255});   // no send yet
  Rest r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(r.r.ok && r.r.cooldown_tier == 1);
  const uint32_t first = cc::g_cooldown.last_chirp_ms;
  host_sim::now_ms = first + 7u * 60000u;
  before = host_sim::espnow->sent.size();
  cc::update();
  CHECK(presence_ages(before) == std::vector<uint8_t>{7});     // the session's own send

  CHECK(rest(cmd_of(cc::CHIRP_CMD_DISABLE)).r.ok);
  before = host_sim::espnow->sent.size();
  CHECK(rest(cmd_of(cc::CHIRP_CMD_ENABLE)).r.ok);              // a new session
  CHECK(presence_ages(before) == std::vector<uint8_t>{255});   // its first beacon: no send
  for (int m = 0; m < 10; ++m) {                               // nor any until it sends
    host_sim::now_ms += 60000;
    before = host_sim::espnow->sent.size();
    cc::update();
    CHECK(presence_ages(before) == std::vector<uint8_t>{255});
  }
  CHECK(cc::g_cooldown.last_chirp_ms == first);                // the device's timer (F193)
  r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(r.r.ok && r.r.cooldown_tier == 2);
  const uint32_t second = cc::g_cooldown.last_chirp_ms;
  host_sim::now_ms = second + 3u * 60000u;
  before = host_sim::espnow->sent.size();
  cc::update();
  CHECK(presence_ages(before) == std::vector<uint8_t>{3});     // from the new session's send
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_new_session_beacons_no_last_send\n");
}

// The session runs on without a break, the loop task's pass once a minute
// (as on a device, where update() runs every loop and a beacon is due every
// PRESENCE_INTERVAL_MS), from `from` until `until` ms after `t0`, both
// counted in 64 bits so the span can pass 2^32 ms. `each(elapsed)` runs
// after every pass; the send log is emptied as it goes.
template <typename F>
void run_on(uint32_t t0, uint64_t from, uint64_t until, F each) {
  for (uint64_t e = from; e <= until; e += 60000) {
    host_sim::now_ms = t0 + (uint32_t)e;
    host_sim::espnow->sent.clear();
    cc::update();
    each(e);
  }
}

// The beacon's age stops at 254, across uptime's wrap (sweep F417).
// send_presence() counted last_chirp_age_min as the time since this
// session's last send with nothing to stop it, so 2^32 ms (49.7 days) after
// a send it read small again: a send, then a beacon every minute for 2^32 ms
// and 5 minutes, and the last one said 5, which is false. Now every beacon
// says the whole minutes since the send up to 254, the field's cap (spec
// §3.2, 255 being "never"), and 254 from then on, read off a send time
// that each beacon past the cap slides forward to hold it there. The slide
// never writes 0, which reads as "never": a beacon at the uptime where the
// slide would land on it holds 254 too. A disable and an enable say 255
// (F415).
void test_the_beacon_age_stops_at_254_across_the_wrap() {
  boot();
  enabled_channel();
  CHECK(rest(send_of(cc::TPL_INFRA_POWER_OUT)).r.ok);
  const uint32_t sent = cc::g_last_chirp_sent_ms;
  CHECK(sent != 0);
  const uint64_t SPAN = (1ull << 32) + 5u * 60000u;
  size_t beacons = 0, wrong = 0;
  uint8_t last = 0;
  const auto check_beacons = [&](uint64_t e) {
    for (uint8_t age : presence_ages(0)) {
      const uint64_t want = e / 60000 < 254 ? e / 60000 : 254;
      ++beacons;
      if (age != want) ++wrong;
      last = age;
    }
  };
  run_on(sent, 60000, SPAN - 5u * 60000u, check_beacons);   // a minute a pass, up to 2^32 ms
  host_sim::now_ms = sent + (uint32_t)SPAN;                 // and the pass at 2^32 ms + 5 min
  host_sim::espnow->sent.clear();
  cc::g_last_presence_ms = host_sim::now_ms - cc::PRESENCE_INTERVAL_MS;
  cc::update();
  check_beacons(SPAN);
  std::printf("  %zu beacons over 2^32 ms + 5 min after a send; the last says %u minutes\n", beacons,
              (unsigned)last);
  CHECK(beacons > 71000);
  CHECK(last == 254);
  CHECK(wrong == 0);

  // Where the slide would write 0: a send 1 s before the wrap, and a beacon
  // at 254 minutes after it.
  cc::g_last_chirp_sent_ms = 0u - 1000u;
  host_sim::now_ms = 254u * 60000u;
  cc::g_last_presence_ms = host_sim::now_ms - cc::PRESENCE_INTERVAL_MS;
  host_sim::espnow->sent.clear();
  cc::update();
  CHECK(presence_ages(0) == std::vector<uint8_t>{254});
  CHECK(cc::g_last_chirp_sent_ms != 0);
  host_sim::now_ms += cc::PRESENCE_INTERVAL_MS;
  host_sim::espnow->sent.clear();
  cc::update();
  CHECK(presence_ages(0) == std::vector<uint8_t>{254});    // not 255, "never"

  CHECK(rest(cmd_of(cc::CHIRP_CMD_DISABLE)).r.ok);
  const size_t before = host_sim::espnow->sent.size();
  CHECK(rest(cmd_of(cc::CHIRP_CMD_ENABLE)).r.ok);
  CHECK(presence_ages(before) == std::vector<uint8_t>{255});
  std::printf("PASS the_beacon_age_stops_at_254_across_the_wrap\n");
}

// A known limit, written down (sweep F417): presence reads unmet again for
// ten minutes every 2^32 ms. has_presence_requirement() and read_status()'s
// presence_met count the session's age as unsigned elapsed time, as F311's
// nearby rows do, so uptime's wrap never makes it read longer, and a session
// on without a break for 2^32 ms reads as ten minutes new again: its sends
// are refused as presence_required (and its confirmations and votes) for
// those ten minutes, and nothing goes early. This case holds the limit as it
// is, failing closed: met through the whole span, then unmet from 2^32 ms
// to 2^32 ms + 10 minutes less 1 ms after the session started, and met, a
// send going out, at 2^32 ms + 10 minutes. Latching it would publish a new
// StatusView field and move the_view_sizes pin.
void test_presence_reads_new_again_every_2_32_ms() {
  boot();
  enabled_channel();
  const uint32_t start = cc::g_session_start_ms;
  const uint64_t W = 1ull << 32;
  size_t unmet = 0;
  run_on(start, cc::PRESENCE_REQUIRED_MS, W - 60000u, [&](uint64_t) {
    if (!cc::has_presence_requirement()) ++unmet;
  });
  CHECK(unmet == 0);
  for (const uint64_t e : {W, W + 5u * 60000u, W + cc::PRESENCE_REQUIRED_MS - 1}) {
    host_sim::now_ms = start + (uint32_t)e;
    cc::update();
    const cc::StatusView v = status_read();
    CHECK(!cc::has_presence_requirement() && !v.presence_met);
    CHECK(reason_of(v) == "presence_required");
  }
  host_sim::now_ms = start + (uint32_t)(W + 5u * 60000u);
  const Rest refused = rest(send_of(cc::TPL_INFRA_POWER_OUT), 1, Turn::kDrain);
  CHECK(!refused.r.ok && refused.r.refusal == cc::SEND_REFUSED_PRESENCE);
  host_sim::now_ms = start + (uint32_t)(W + cc::PRESENCE_REQUIRED_MS);
  CHECK(cc::has_presence_requirement() && status_read().presence_met);
  CHECK(rest(send_of(cc::TPL_INFRA_POWER_OUT), 1, Turn::kDrain).r.ok);
  std::printf("PASS presence_reads_new_again_every_2_32_ms\n");
}

// The count stops at 255. Kept across days, it reaches 256 after about 42
// days of a send every 4 hours, and a uint8_t that wrapped to 0 read tier 0:
// the next send went out with no cooldown, and the one after it at tier 1.
void test_the_tier_count_stops_at_255() {
  const uint32_t clock_was = host_sim::now_ms;
  host_sim::now_ms = 1000;
  boot();
  enabled_channel();
  for (unsigned n = 1; n <= 260; ++n) {
    const cc::Result r = send_when_due();
    CHECK(r.cooldown_tier == (n < 4 ? n : 4));
    CHECK(cc::get_cooldown_remaining_ms() == tier_cooldown_ms((uint8_t)(n < 4 ? n : 4)));
    CHECK(cc::g_cooldown.chirps_sent_today == (n < 255 ? n : 255));
  }
  // A second after the 260th: refused, the top tier's 4 hours less that second.
  const uint32_t last = cc::g_cooldown.last_chirp_ms;
  host_sim::now_ms = last + 1000;
  const Rest r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_COOLDOWN && r.r.cooldown_tier == 4);
  CHECK(r.r.cooldown_remaining_ms == cc::COOLDOWN_TIER_4_MS - (host_sim::now_ms - last));
  CHECK(ends_with(last_sent_log(), "tier 4)"));
  CHECK(host_sim::httpd_side_effects == 0);
  host_sim::now_ms = clock_was;
  std::printf("PASS the_tier_count_stops_at_255\n");
}

// The tiers across the 49.7-day uptime wrap. Kept across days, a tier set
// while millis() wraps is routine for a steady sender. The reset and the
// cooldown both count the time since the last send, which crosses the wrap;
// a deadline (last + 24 hours) that wrapped to a small number would read as
// long past, and the next pass would reset the tiers and end the cooldown.
void test_the_tiers_count_across_the_uptime_wrap() {
  const uint32_t clock_was = host_sim::now_ms;
  // Three sends before the wrap; tier 3's hour runs across it.
  host_sim::now_ms = 0u - 40u * 60000u;
  boot();
  enabled_channel();                                  // 2^32 - 30 min
  CHECK(send_when_due().cooldown_tier == 1);
  cc::update();                                       // a pass right after it keeps the tier
  CHECK(status_read().cooldown_tier == 1 && cc::get_cooldown_remaining_ms() > 0);
  CHECK(send_when_due().cooldown_tier == 2);          // 2^32 - 25 min
  CHECK(send_when_due().cooldown_tier == 3);          // 2^32 - 10 min
  const uint32_t third = cc::g_cooldown.last_chirp_ms;
  CHECK(third > 0u - 11u * 60000u);
  host_sim::now_ms = 0u - 60000u;                     // a pass 1 minute before the wrap
  cc::update();
  CHECK(status_read().cooldown_tier == 3);
  host_sim::now_ms = 20u * 60000u;                    // and one 20 minutes after it
  cc::update();
  cc::StatusView v = status_read();
  CHECK(v.state == cc::CHIRP_COOLDOWN && v.cooldown_tier == 3 && !v.can_send);
  CHECK(v.cooldown_remaining_ms == cc::COOLDOWN_TIER_3_MS - (host_sim::now_ms - third));
  Rest r = rest(send_of(cc::TPL_INFRA_POWER_OUT));
  CHECK(!r.r.ok && r.r.refusal == cc::SEND_REFUSED_COOLDOWN && r.r.cooldown_tier == 3);
  CHECK(r.r.cooldown_remaining_ms == cc::COOLDOWN_TIER_3_MS - (host_sim::now_ms - third));
  CHECK(send_when_due().cooldown_tier == 4);          // when the hour ends, past the wrap
  const uint32_t fourth = cc::g_cooldown.last_chirp_ms;
  CHECK(fourth == third + cc::COOLDOWN_TIER_3_MS + cc::COMMAND_POLL_MS);
  // The 24 quiet hours count from the last send, not from one before the wrap.
  host_sim::now_ms = third + cc::COOLDOWN_RESET_MS + 1;
  cc::update();
  CHECK(status_read().cooldown_tier == 4);
  host_sim::now_ms = fourth + cc::COOLDOWN_RESET_MS;
  cc::update();
  CHECK(status_read().cooldown_tier == 4);
  host_sim::now_ms = fourth + cc::COOLDOWN_RESET_MS + 1;
  cc::update();
  v = status_read();
  CHECK(v.cooldown_tier == 0 && v.last_chirp_sent_ms == 0);

  // One send a minute before the wrap: tier 1's 5 minutes run across it,
  // and so do the 24 quiet hours.
  host_sim::now_ms = 0u - 11u * 60000u;
  boot();
  enabled_channel();                                  // 2^32 - 1 min
  CHECK(send_when_due().cooldown_tier == 1);
  const uint32_t one = cc::g_cooldown.last_chirp_ms;
  CHECK(one > 0u - 2u * 60000u);
  host_sim::now_ms = one + 2u * 60000u;               // past the wrap
  CHECK(host_sim::now_ms < one);
  cc::update();
  CHECK(cc::get_cooldown_tier() == 1);
  CHECK(cc::get_cooldown_remaining_ms() == cc::COOLDOWN_TIER_1_MS - 2u * 60000u);
  host_sim::now_ms = one + cc::COOLDOWN_RESET_MS;
  cc::update();
  CHECK(status_read().cooldown_tier == 1);
  host_sim::now_ms = one + cc::COOLDOWN_RESET_MS + 1;
  cc::update();
  CHECK(status_read().cooldown_tier == 0);
  CHECK(host_sim::httpd_side_effects == 0);
  host_sim::now_ms = clock_was;
  std::printf("PASS the_tiers_count_across_the_uptime_wrap\n");
}

// A mute beside tiers a disable kept. A mute set inside tier 3's hour ends
// at the disable (sweep F192); the enable's new session reads the cooldown
// at tier 3, not muted; a new mute that runs out leaves the cooldown with
// its exact remainder (sweep F178), and the send when it ends is tier 4's.
// enable() zeroed the tiers, so the new session read active at tier 0.
void test_a_mute_beside_the_kept_tiers() {
  const uint32_t clock_was = host_sim::now_ms;
  host_sim::now_ms = 1000;
  boot();
  enabled_channel();
  CHECK(send_when_due().cooldown_tier == 1);
  CHECK(send_when_due().cooldown_tier == 2);
  CHECK(send_when_due().cooldown_tier == 3);          // a 1-hour cooldown
  const uint32_t third = cc::g_cooldown.last_chirp_ms;
  CHECK(rest(mute_of(15)).r.ok && cc::g_state == cc::CHIRP_MUTED);
  cc::StatusView v = status_read();
  CHECK(v.state == cc::CHIRP_MUTED && v.muted && v.cooldown_tier == 3 && v.cooldown_remaining_ms > 0);

  CHECK(rest(cmd_of(cc::CHIRP_CMD_DISABLE)).r.ok && !cc::g_muted);
  CHECK(rest(cmd_of(cc::CHIRP_CMD_ENABLE)).r.ok);
  v = status_read();
  CHECK(!v.muted && v.mute_remaining_ms == 0);
  CHECK(v.state == cc::CHIRP_COOLDOWN && v.cooldown_tier == 3 && !v.can_send);

  // A mute in the new session runs out; the cooldown runs on.
  CHECK(rest(mute_of(15)).r.ok && cc::g_state == cc::CHIRP_MUTED);
  host_sim::now_ms = cc::g_mute_until_ms;
  cc::update();
  v = status_read();
  CHECK(!v.muted && v.state == cc::CHIRP_COOLDOWN && v.cooldown_tier == 3);
  CHECK(v.cooldown_remaining_ms == cc::COOLDOWN_TIER_3_MS - (host_sim::now_ms - third));
  CHECK(reason_of(v) == "cooldown");
  CHECK(send_when_due().cooldown_tier == 4);
  CHECK(cc::g_cooldown.last_chirp_ms == third + cc::COOLDOWN_TIER_3_MS + cc::COMMAND_POLL_MS);
  CHECK(host_sim::httpd_side_effects == 0);
  host_sim::now_ms = clock_was;
  std::printf("PASS a_mute_beside_the_kept_tiers\n");
}

// The votes beside tiers a disable kept. A dismiss's suppress vote and a
// confirm wait on the channel, the presence requirement and the clock, never
// on the send cooldown, so both go out while the kept tier-2 cooldown runs,
// and neither moves it.
void test_the_votes_beside_the_kept_tiers() {
  const uint32_t clock_was = host_sim::now_ms;
  host_sim::now_ms = 1000;
  boot();
  enabled_channel();
  CHECK(send_when_due().cooldown_tier == 1);
  CHECK(send_when_due().cooldown_tier == 2);          // a 15-minute cooldown
  const uint32_t second = cc::g_cooldown.last_chirp_ms;
  CHECK(rest(cmd_of(cc::CHIRP_CMD_DISABLE)).r.ok);
  CHECK(rest(cmd_of(cc::CHIRP_CMD_ENABLE)).r.ok);
  host_sim::now_ms = cc::g_session_start_ms + cc::PRESENCE_REQUIRED_MS;
  cc::update();
  CHECK(cc::get_cooldown_tier() == 2);
  CHECK(cc::get_cooldown_remaining_ms() == cc::COOLDOWN_TIER_2_MS - (host_sim::now_ms - second));
  CHECK(cc::get_cooldown_remaining_ms() > 4u * 60000u);

  const uint8_t a[8] = {0xE1, 1, 1, 1, 1, 1, 1, 1};
  const uint8_t b[8] = {0xE2, 2, 2, 2, 2, 2, 2, 2};
  neighbor_chirp(a);
  neighbor_chirp(b);
  size_t before = host_sim::espnow->sent.size();
  Rest r = rest(nonce_cmd(cc::CHIRP_CMD_DISMISS, a));
  CHECK(r.r.ok && r.r.vote_sent && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NONE);
  CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_SUPPRESS_VOTE});
  before = host_sim::espnow->sent.size();
  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, b));
  CHECK(r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NONE);
  CHECK(acks_since(before) == 1);

  cc::StatusView v = status_read();
  CHECK(v.state == cc::CHIRP_COOLDOWN && v.cooldown_tier == 2 && v.last_chirp_sent_ms == second);
  CHECK(v.cooldown_remaining_ms == cc::COOLDOWN_TIER_2_MS - (host_sim::now_ms - second));
  CHECK(send_when_due().cooldown_tier == 3);
  CHECK(cc::g_cooldown.last_chirp_ms == second + cc::COOLDOWN_TIER_2_MS + cc::COMMAND_POLL_MS);
  CHECK(host_sim::httpd_side_effects == 0);
  host_sim::now_ms = clock_was;
  std::printf("PASS the_votes_beside_the_kept_tiers\n");
}

// Spec 2.5.4's count, from the constants: sending as fast as its tiers let
// it, a device sends 9 chirps in its first 24 hours and then one every 4
// hours, 6 a day, and no 24 hours after the first day hold more than 6. The
// old reset, 24 hours after the run's first send, dropped back to tier 1
// once a day, so every day was a first day.
void test_the_tiers_let_9_out_on_the_first_day_then_6() {
  const uint32_t clock_was = host_sim::now_ms;
  host_sim::now_ms = 1000;
  boot();
  enabled_channel();
  std::vector<uint32_t> at;
  for (int n = 0; n < 40; ++n) {
    send_when_due();
    at.push_back(cc::g_cooldown.last_chirp_ms);
  }
  constexpr uint32_t DAY_MS = 24u * HOUR_MS;
  const uint32_t first = at.front();
  CHECK(at.back() - first > 6u * DAY_MS);             // six whole days of sends
  for (uint32_t day = 0; day < 6; ++day) {
    unsigned sends = 0;
    for (const uint32_t t : at) {
      if (t - first >= day * DAY_MS && t - first < (day + 1) * DAY_MS) ++sends;
    }
    CHECK(sends == (day == 0 ? 9u : 6u));
  }
  unsigned most = 0;
  for (size_t i = 0; i < at.size(); ++i) {
    if (at[i] - first < DAY_MS) continue;
    unsigned sends = 0;
    for (size_t j = i; j < at.size() && at[j] - at[i] < DAY_MS; ++j) ++sends;
    most = sends > most ? sends : most;
  }
  CHECK(most == 6);
  CHECK(host_sim::httpd_side_effects == 0);
  host_sim::now_ms = clock_was;
  std::printf("PASS the_tiers_let_9_out_on_the_first_day_then_6\n");
}

// A chirp this device sent, as handle_witness() would store it if a
// neighbor's relay of it arrived once the nonce filter had forgotten it (it
// was cleared whole every 5 minutes before sweep F270; it now holds a nonce
// past the freshness window, so only a wall clock stepped back between the
// send and the relay lets one in): this session's own pubkey as the origin.
void own_chirp(const uint8_t nonce[8]) {
  cc::ReceivedChirp c;
  memset(&c, 0, sizeof(c));
  memcpy(c.nonce, nonce, 8);
  memcpy(c.sender_pubkey, cc::g_session.session_pubkey, sizeof(c.sender_pubkey));
  c.template_id = cc::TPL_INFRA_POWER_OUT;
  c.hop_count = 1;
  c.received_ms = host_sim::now_ms;
  cc::g_recent_chirps[cc::g_recent_chirp_count++] = c;
}

// confirm_chirp() returned false for every refusal, and POST
// /api/chirp/confirm answered each {"error":"not_found","message":"Chirp not
// found or already dismissed"} (/api/chirp/ack with "confirmed" a bare
// success:false). Each refusal is in the Result now, read on the loop task
// right after the attempt, in the order confirm_chirp() checks: the channel
// off, the presence requirement, the wall clock, then the chirp (none with
// that nonce, or this device's own). None of them sends a frame.
void test_a_refused_confirm_names_why() {
  boot();
  const uint8_t n1[8] = {0xC1, 1, 1, 1, 1, 1, 1, 1};
  Rest r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, n1));             // off
  CHECK(r.wait == lcr::Wait::kDone && !r.r.ok);
  CHECK(r.r.confirm_refusal == cc::CONFIRM_REFUSED_DISABLED);
  CHECK(named(r.r.confirm_refusal).error == "chirp_disabled" && named(r.r.confirm_refusal).status == 409);

  enabled_channel(/*present=*/false);
  neighbor_chirp(n1);
  size_t before = host_sim::espnow->sent.size();
  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, n1));                  // on for less than 10 minutes
  CHECK(!r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_PRESENCE);
  CHECK(named(r.r.confirm_refusal).error == "presence_required" && named(r.r.confirm_refusal).status == 409);

  host_sim::now_ms += cc::PRESENCE_REQUIRED_MS;
  host_sim::wall_now = cc::MIN_UNIX_TIME - 1;                      // the clock not set yet
  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, n1));
  CHECK(!r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_CLOCK_UNSYNCED);
  CHECK(named(r.r.confirm_refusal).error == "clock_unsynced" && named(r.r.confirm_refusal).status == 409);
  CHECK(named(r.r.confirm_refusal).message.find("GPS time") != std::string::npos);
  host_sim::wall_now = 1760000000;

  const uint8_t gone[8] = {0xC2, 2, 2, 2, 2, 2, 2, 2};
  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, gone));                // no such chirp
  CHECK(!r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NOT_FOUND);
  CHECK(named(r.r.confirm_refusal).error == "not_found" && named(r.r.confirm_refusal).status == 404);
  CHECK(named(r.r.confirm_refusal).message == "Chirp not found or already dismissed");

  const uint8_t mine[8] = {0xC3, 3, 3, 3, 3, 3, 3, 3};
  own_chirp(mine);
  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, mine));                // this device's own
  CHECK(!r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_OWN_CHIRP);
  CHECK(named(r.r.confirm_refusal).error == "own_chirp" && named(r.r.confirm_refusal).status == 409);
  CHECK(acks_since(before) == 0);                                  // no refusal sent one

  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, n1));                  // all met: it goes out
  CHECK(r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NONE);
  CHECK(acks_since(before) == 1);

  // The other answers: the dashboard reads a 403 as a bad token, so no
  // refusal is one; NONE with a failure (an ACK that could not be signed,
  // which the fixed canonical buffer rules out) is a 500 of its own.
  for (cc::ConfirmRefusal why : {cc::CONFIRM_REFUSED_DISABLED, cc::CONFIRM_REFUSED_PRESENCE,
                                 cc::CONFIRM_REFUSED_CLOCK_UNSYNCED, cc::CONFIRM_REFUSED_NOT_FOUND,
                                 cc::CONFIRM_REFUSED_OWN_CHIRP}) {
    const int st = cc::confirm_refusal_status(why);
    CHECK((st == 404 || st == 409) && strcmp(http_status_line(st), "400 Bad Request") != 0);
  }
  CHECK(named(cc::CONFIRM_REFUSED_NONE).status == 500 && named(cc::CONFIRM_REFUSED_NONE).error == "confirm_failed");
  // Every answer fits its buffer with room for the terminator (chirp_api.h's
  // send_confirm_answer and send_dismiss_answer serialize into 256 bytes,
  // rule CV9): serializeJson() leaves a full char array unterminated, and the
  // refused confirm's old 85-byte answer filled its 64-byte buffer. The
  // longest, a dismiss whose vote waits for the clock, is 161 bytes, past
  // rule CV10's 160-byte floor for other answers with a message.
  for (cc::ConfirmRefusal why : {cc::CONFIRM_REFUSED_NONE, cc::CONFIRM_REFUSED_DISABLED,
                                 cc::CONFIRM_REFUSED_PRESENCE, cc::CONFIRM_REFUSED_CLOCK_UNSYNCED,
                                 cc::CONFIRM_REFUSED_NOT_FOUND, cc::CONFIRM_REFUSED_OWN_CHIRP}) {
    const std::string refused = std::string("{\"success\":false,\"error\":\"") + cc::confirm_refusal_error(why) +
                                "\",\"message\":\"" + cc::confirm_refusal_message(why) + "\"}";
    const std::string unsent = std::string("{\"success\":true,\"vote_sent\":false,\"vote_error\":\"") +
                               cc::confirm_refusal_error(why) + "\",\"message\":\"" +
                               cc::vote_unsent_message(why) + "\"}";
    CHECK(refused.size() < 160 && unsent.size() < 256);
    if (why == cc::CONFIRM_REFUSED_CLOCK_UNSYNCED) CHECK(unsent.size() == 161);
  }
  CHECK(strcmp(http_status_line(500), "500 Internal Server Error") == 0);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_refused_confirm_names_why\n");
}

// A dismiss hides the chirp here whatever the clock, but sends its signed
// suppress vote only with the presence requirement met and the clock set,
// and said nothing when it did not. The Result says whether the vote went
// out, and if not why; a chirp that is not there is not_found, as for a
// confirm.
void test_a_dismiss_says_whether_its_vote_went() {
  boot();
  enabled_channel(/*present=*/false);
  const uint8_t a[8] = {0xD1, 1, 1, 1, 1, 1, 1, 1};
  const uint8_t b[8] = {0xD2, 2, 2, 2, 2, 2, 2, 2};
  const uint8_t c[8] = {0xD3, 3, 3, 3, 3, 3, 3, 3};
  neighbor_chirp(a);
  neighbor_chirp(b);
  neighbor_chirp(c);

  size_t before = host_sim::espnow->sent.size();
  Rest r = rest(nonce_cmd(cc::CHIRP_CMD_DISMISS, a));              // on for less than 10 minutes
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && cc::g_recent_chirps[0].dismissed);
  CHECK(!r.r.vote_sent && r.r.confirm_refusal == cc::CONFIRM_REFUSED_PRESENCE);
  CHECK(std::string(cc::vote_unsent_message(r.r.confirm_refusal)).find("10 minutes") != std::string::npos);
  CHECK(std::string(cc::confirm_refusal_error(r.r.confirm_refusal)) == "presence_required");
  CHECK(sent_types(before).empty());

  host_sim::now_ms += cc::PRESENCE_REQUIRED_MS;
  host_sim::wall_now = cc::MIN_UNIX_TIME - 1;                      // the clock not set yet
  before = host_sim::espnow->sent.size();
  r = rest(nonce_cmd(cc::CHIRP_CMD_DISMISS, b));
  CHECK(r.r.ok && cc::g_recent_chirps[1].dismissed);
  CHECK(!r.r.vote_sent && r.r.confirm_refusal == cc::CONFIRM_REFUSED_CLOCK_UNSYNCED);
  CHECK(std::string(cc::vote_unsent_message(r.r.confirm_refusal)).find("GPS time") != std::string::npos);
  for (uint8_t t : sent_types(before)) CHECK(t != cc::CHIRP_MSG_SUPPRESS_VOTE);
  host_sim::wall_now = 1760000000;

  before = host_sim::espnow->sent.size();
  r = rest(nonce_cmd(cc::CHIRP_CMD_DISMISS, c));                   // both met: the vote goes out
  CHECK(r.r.ok && r.r.vote_sent && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NONE);
  CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_SUPPRESS_VOTE});

  const uint8_t gone[8] = {0xD4, 4, 4, 4, 4, 4, 4, 4};
  r = rest(nonce_cmd(cc::CHIRP_CMD_DISMISS, gone));
  CHECK(!r.r.ok && !r.r.vote_sent && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NOT_FOUND);
  CHECK(named(r.r.confirm_refusal).status == 404 && named(r.r.confirm_refusal).error == "not_found");
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_dismiss_says_whether_its_vote_went\n");
}

// A chirp its owner dismissed is not confirmed from this device (sweep
// F305, the sending side; spec §3.4, §8.1). The dismiss leaves the row in
// the recent table, marked, until update()'s next prune, up to 30 s on, and
// a confirm in those seconds signed and broadcast an ACK that a neighbor
// counted like any other. It is refused not_found, as it is after the
// prune, and sends nothing; a chirp not dismissed is confirmed. Dismissed
// before its owner's ten minutes (no vote went out) it is refused the same.
void test_a_dismissed_chirp_is_not_confirmed() {
  boot();
  enabled_channel();
  const uint8_t a[8] = {0xE1, 1, 1, 1, 1, 1, 1, 1};
  const uint8_t b[8] = {0xE2, 2, 2, 2, 2, 2, 2, 2};
  neighbor_chirp(a);
  neighbor_chirp(b);
  size_t before = host_sim::espnow->sent.size();
  Rest r = rest(nonce_cmd(cc::CHIRP_CMD_DISMISS, a));
  CHECK(r.wait == lcr::Wait::kDone && r.r.ok && r.r.vote_sent);
  CHECK(sent_types(before) == std::vector<uint8_t>{cc::CHIRP_MSG_SUPPRESS_VOTE});

  before = host_sim::espnow->sent.size();
  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, a));
  CHECK(cc::g_recent_chirp_count == 2 && cc::g_recent_chirps[0].dismissed);   // not pruned yet
  CHECK(!r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NOT_FOUND);
  CHECK(named(r.r.confirm_refusal).status == 404 && named(r.r.confirm_refusal).error == "not_found");
  CHECK(named(r.r.confirm_refusal).message == "Chirp not found or already dismissed");
  CHECK(acks_since(before) == 0);

  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, b));                   // the control: not dismissed
  CHECK(r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NONE);
  CHECK(acks_since(before) == 1);

  host_sim::now_ms += 30001;                                       // past the prune: still not_found
  cc::update();
  CHECK(cc::g_recent_chirp_count == 1);
  before = host_sim::espnow->sent.size();
  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, a));
  CHECK(!r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NOT_FOUND);
  CHECK(acks_since(before) == 0);

  boot();
  enabled_channel(/*present=*/false);
  const uint8_t c[8] = {0xE3, 3, 3, 3, 3, 3, 3, 3};
  neighbor_chirp(c);
  r = rest(nonce_cmd(cc::CHIRP_CMD_DISMISS, c));                   // before its ten minutes: no vote
  CHECK(r.r.ok && !r.r.vote_sent && r.r.confirm_refusal == cc::CONFIRM_REFUSED_PRESENCE);
  host_sim::now_ms += cc::PRESENCE_REQUIRED_MS;
  before = host_sim::espnow->sent.size();
  r = rest(nonce_cmd(cc::CHIRP_CMD_CONFIRM, c), 1, Turn::kDrain);
  CHECK(cc::g_recent_chirp_count == 1 && cc::g_recent_chirps[0].dismissed);
  CHECK(!r.r.ok && r.r.confirm_refusal == cc::CONFIRM_REFUSED_NOT_FOUND);
  CHECK(acks_since(before) == 0);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_dismissed_chirp_is_not_confirmed\n");
}

struct Test {
  const char* name;
  void (*fn)();
};
const Test kTests[] = {
    {"enable_runs_on_the_loop_task", test_enable_runs_on_the_loop_task},
    {"every_command_runs_on_the_loop_task", test_every_command_runs_on_the_loop_task},
    {"a_refused_send_names_why", test_a_refused_send_names_why},
    {"a_mute_does_not_end_the_cooldown", test_a_mute_does_not_end_the_cooldown},
    {"a_send_while_muted_stays_muted", test_a_send_while_muted_stays_muted},
    {"a_send_just_after_the_cooldown_goes_out", test_a_send_just_after_the_cooldown_goes_out},
    {"a_send_at_an_edge_names_why", test_a_send_at_an_edge_names_why},
    {"the_beacon_says_listening_through_a_cooldown", test_the_beacon_says_listening_through_a_cooldown},
    {"a_mute_needs_a_channel_that_is_on", test_a_mute_needs_a_channel_that_is_on},
    {"a_disable_ends_the_mute", test_a_disable_ends_the_mute},
    {"a_mute_set_before_the_wrap_runs_its_length", test_a_mute_set_before_the_wrap_runs_its_length},
    {"a_mute_ends_once_at_its_deadline_across_the_wrap", test_a_mute_ends_once_at_its_deadline_across_the_wrap},
    {"no_mute_reads_unmuted_past_the_sign_boundary", test_no_mute_reads_unmuted_past_the_sign_boundary},
    {"the_tiers_reset_24_quiet_hours_after_the_last_send",
     test_the_tiers_reset_24_quiet_hours_after_the_last_send},
    {"a_send_that_ends_a_quiet_day_counts_from_tier_1", test_a_send_that_ends_a_quiet_day_counts_from_tier_1},
    {"a_disable_keeps_the_tiers", test_a_disable_keeps_the_tiers},
    {"a_new_session_beacons_no_last_send", test_a_new_session_beacons_no_last_send},
    {"the_beacon_age_stops_at_254_across_the_wrap", test_the_beacon_age_stops_at_254_across_the_wrap},
    {"presence_reads_new_again_every_2_32_ms", test_presence_reads_new_again_every_2_32_ms},
    {"the_tier_count_stops_at_255", test_the_tier_count_stops_at_255},
    {"the_tiers_count_across_the_uptime_wrap", test_the_tiers_count_across_the_uptime_wrap},
    {"a_mute_beside_the_kept_tiers", test_a_mute_beside_the_kept_tiers},
    {"the_votes_beside_the_kept_tiers", test_the_votes_beside_the_kept_tiers},
    {"the_tiers_let_9_out_on_the_first_day_then_6", test_the_tiers_let_9_out_on_the_first_day_then_6},
    {"a_refused_confirm_names_why", test_a_refused_confirm_names_why},
    {"a_dismiss_says_whether_its_vote_went", test_a_dismiss_says_whether_its_vote_went},
    {"a_dismissed_chirp_is_not_confirmed", test_a_dismissed_chirp_is_not_confirmed},
    {"a_send_carries_the_owners_fields", test_a_send_carries_the_owners_fields},
    {"a_settings_post_changes_only_what_it_names", test_a_settings_post_changes_only_what_it_names},
    {"a_command_the_loop_never_reaches_is_withdrawn", test_a_command_the_loop_never_reaches_is_withdrawn},
    {"a_full_ring_answers_busy", test_a_full_ring_answers_busy},
    {"a_late_turn_still_answers", test_a_late_turn_still_answers},
    {"a_status_read_is_the_last_published_pass", test_a_status_read_is_the_last_published_pass},
    {"init_publishes_the_stored_settings", test_init_publishes_the_stored_settings},
    {"a_read_right_after_a_post_shows_what_it_did", test_a_read_right_after_a_post_shows_what_it_did},
    {"the_pass_publishes_what_it_changed", test_the_pass_publishes_what_it_changed},
    {"a_frame_shows_in_the_tables_after_its_pass", test_a_frame_shows_in_the_tables_after_its_pass},
    {"the_recent_route_names_every_category", test_the_recent_route_names_every_category},
    {"a_beacon_emoji_is_its_session_display", test_a_beacon_emoji_is_its_session_display},
    {"every_session_display_is_five_emoji_in_its_field", test_every_session_display_is_five_emoji_in_its_field},
    {"the_prune_shows_in_the_tables", test_the_prune_shows_in_the_tables},
    {"an_idle_pass_does_not_rebuild_the_tables", test_an_idle_pass_does_not_rebuild_the_tables},
    {"a_status_read_counts_time_at_the_read", test_a_status_read_counts_time_at_the_read},
    {"cannot_send_reason_names_the_clock", test_cannot_send_reason_names_the_clock},
    {"every_field_the_routes_show_is_the_live_one", test_every_field_the_routes_show_is_the_live_one},
    {"the_tables_publish_into_their_block", test_the_tables_publish_into_their_block},
    {"the_view_sizes", test_the_view_sizes},
    {"the_memory_budget_is_the_specs", test_the_memory_budget_is_the_specs},
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
