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
  CHECK(cc::g_state == cc::CHIRP_ACTIVE);              // no cooldown started
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
struct Neighbor {
  uint8_t priv[32];
  uint8_t pub[32];
  uint8_t sid[cc::SESSION_ID_SIZE];
  uint8_t mac[6];
  std::string emoji;
};
Neighbor neighbor_of(uint8_t seed, const char* emoji) {
  Neighbor n;
  memset(n.priv, seed, sizeof n.priv);
  n.priv[0] ^= 0x5A;
  Ed25519::derivePublicKey(n.pub, n.priv);
  cc::session_id_from_pubkey(n.pub, n.sid);
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, seed};
  memcpy(n.mac, mac, sizeof mac);
  n.emoji = emoji;
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
  strncpy(p.emoji, n.emoji.c_str(), sizeof p.emoji - 1);
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

const char* const BEE = "\xF0\x9F\x90\x9D\xF0\x9F\x8C\xB8";   // two of EMOJI_SET's
const char* const TREE = "\xF0\x9F\x8C\xB3";

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

  CHECK(cc::mute(30));                                 // the loop task, mid-pass
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

  CHECK(rest(send_of(cc::TPL_INFRA_POWER_OUT)).r.ok);
  CHECK(status_read().state == cc::CHIRP_COOLDOWN);
  host_sim::now_ms += cc::COOLDOWN_TIER_1_MS;
  v = status_read();
  CHECK(v.state == cc::CHIRP_COOLDOWN && v.cooldown_remaining_ms == 0 && !v.can_send);
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
  const Neighbor N = neighbor_of(1, BEE);
  const Neighbor M = neighbor_of(2, TREE);
  deliver(N, presence_of(N), -48);
  CHECK(cc::g_nearby_count == 1);
  CHECK(nearby_read().count == 0 && status_read().nearby_count == 0);   // not yet published
  cc::update();
  cc::NearbyTable t = nearby_read();
  CHECK(t.count == 1 && status_read().nearby_count == 1);
  CHECK(std::string(t.devices[0].emoji) == BEE && t.devices[0].rssi == -48);
  CHECK(t.devices[0].listening && t.devices[0].last_seen_ms == host_sim::now_ms);
  deliver(M, presence_of(M), -71);
  cc::update();
  t = nearby_read();
  CHECK(t.count == 2 && std::string(t.devices[1].emoji) == TREE && t.devices[1].rssi == -71);

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

// update()'s 30-second prune drops a neighbor not heard for 3 minutes and a
// chirp older than 30; the pass that prunes publishes the shorter tables.
void test_the_prune_shows_in_the_tables() {
  boot();
  enabled_channel();
  const Neighbor N = neighbor_of(3, BEE);
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
  const Neighbor N = neighbor_of(6, BEE);
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
  v.state = cc::CHIRP_COOLDOWN;
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
  const Neighbor N = neighbor_of(4, BEE);
  const Neighbor M = neighbor_of(5, TREE);
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
  const Neighbor N = neighbor_of(7, BEE);
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

struct Test {
  const char* name;
  void (*fn)();
};
const Test kTests[] = {
    {"enable_runs_on_the_loop_task", test_enable_runs_on_the_loop_task},
    {"every_command_runs_on_the_loop_task", test_every_command_runs_on_the_loop_task},
    {"a_refused_send_names_why", test_a_refused_send_names_why},
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
    {"the_prune_shows_in_the_tables", test_the_prune_shows_in_the_tables},
    {"an_idle_pass_does_not_rebuild_the_tables", test_an_idle_pass_does_not_rebuild_the_tables},
    {"a_status_read_counts_time_at_the_read", test_a_status_read_counts_time_at_the_read},
    {"cannot_send_reason_names_the_clock", test_cannot_send_reason_names_the_clock},
    {"every_field_the_routes_show_is_the_live_one", test_every_field_the_routes_show_is_the_live_one},
    {"the_tables_publish_into_their_block", test_the_tables_publish_into_their_block},
    {"the_view_sizes", test_the_view_sizes},
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
