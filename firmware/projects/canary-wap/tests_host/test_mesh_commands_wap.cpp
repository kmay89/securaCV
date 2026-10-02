// Host test: the owner's mesh commands run on the loop task (sweep F96),
// against the REAL mesh_network.cpp on the test_mesh_address_wap harness
// (stubs/mesh_net, mesh_net_sim.h). mesh_network.cpp is #included below.
//
// canary_wap.ino's handle_mesh_* REST handlers called remove_peer,
// leave_opera, start_pairing_*, cancel_pairing, confirm_pairing,
// set_enabled, set_opera_name and clear_alerts straight from
// esp_http_server's task, while update() read and wrote the same peer
// table, pairing session, opera config (and its one g_prefs NVS handle)
// and alert history on the loop task. Those functions are now internal to
// mesh_network.cpp; a handler hands a Command to mesh_network::submit(),
// which posts it to a ring that update() drains on the loop task, and
// waits for the result.
//
// The harness is one thread, so "the HTTP server's task" is a role the
// test plays: rest() sets host_sim::on_httpd_task and calls submit() the
// way a handler does. submit() waits in vTaskDelay, and the stub's
// vTaskDelay is where the test gives the loop task its turn (one update()
// pass, with on_httpd_task cleared). Every NVS write and ESP-NOW call made
// while on_httpd_task is set is counted (host_sim::httpd_side_effects): a
// command that ran on the handler's task shows up there, and the handlers'
// own source is held by firmware/scripts/check_wap_loop_commands.py.
//
// Host-tested only: the stubs stand in for the radio, the flash and the
// scheduler; the Arduino compile is CI's (firmware.yml's canary-wap legs).
//
// Run: ./test_mesh_commands_wap [name]

#ifndef MESH_NETWORK_CPP
#error "MESH_NETWORK_CPP (absolute path to mesh_network.cpp) must be defined"
#endif
// The firmware file is held to the device build's warnings, not to this
// Makefile's -Wextra -Wpedantic -Werror (see test_mesh_address_wap.cpp);
// this test's own code keeps all of them.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#include MESH_NETWORK_CPP
#pragma GCC diagnostic pop
#include "mesh_net_sim.h"
#include "http_status_line.h"   // the status line http_send_error() sends

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The sketch provides these on a device (canary_wap.ino, chirp_channel.cpp).
std::vector<std::string> g_health;
void health_log(LogLevel, LogCategory, const char* message) { g_health.push_back(message); }
void log_health(LogLevel, LogCategory, const char*, const char*) {}
namespace chirp_channel {
void dispatch_espnow_message(const uint8_t*, const uint8_t*, int, int8_t) {}
}

namespace commands {

int g_checks = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)

using namespace mesh_net_sim;
namespace mn = mesh_network;
namespace lcr = loop_command_ring;
using Frame = std::vector<uint8_t>;

Device A, B, C, J;
const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Helpers ─────────────────────────────────────────────────────────────

void fresh_device(Device& d) {
  d.nvs.clear();
  d.espnow = host_sim::EspNow();
  boot(d);
}

// The devices as NVS holds an opera after pairing, then booted (the
// test_mesh_liveness_wap fixture).
void fresh_opera(const std::vector<Device*>& all) {
  for (Device* d : all) {
    d->nvs.clear();
    d->espnow = host_sim::EspNow();
  }
  g_cur = nullptr;
  uint8_t secret[mn::OPERA_SECRET_SIZE];
  host_sim::fill_random(secret, sizeof secret);
  for (Device* d : all) {
    boot(*d);
    memcpy(mn::g_opera_config.opera_secret, secret, sizeof secret);
    mn::compute_opera_id(secret, mn::g_opera_config.opera_id);
    mn::g_opera_config.configured = true;
    mn::g_opera_config.enabled = true;
    strcpy(mn::g_opera_config.opera_name, "test");
    for (Device* o : all) {
      if (o != d) CHECK(mn::add_peer(o->pub, o->mac, o->name));
    }
    CHECK(mn::persist_opera_config());
    CHECK(mn::persist_peers());
  }
  for (Device* d : all) boot(*d);
}

// What one REST call saw while its handler waited.
struct Rest {
  lcr::Wait wait = lcr::Wait::kBusy;
  bool ok = false;
  unsigned sleeps = 0;          // vTaskDelay calls the handler made
  unsigned loop_turns = 0;      // update() passes the loop task ran meanwhile
  uint32_t waited_ms = 0;
  // Before the loop task's first turn: the peer count, the pairing state
  // and the opera name the handler's device held.
  int peers_before_turn = -1;
  mn::MeshState state_before_turn = mn::MESH_ERROR;
  std::string name_before_turn;
};

// `d`'s REST handler submits `cmd` (the HTTP server's task), and the loop
// task gets one update() pass on the handler's `turn_at`-th sleep (never,
// when 0). Returns what the handler saw.
Rest rest(Device& d, const mn::Command& cmd, unsigned turn_at = 1) {
  become(d);
  Rest r;
  const uint32_t start = host_sim::now_ms;
  host_sim::on_task_delay = [&](uint32_t ms) {
    host_sim::now_ms += ms;
    ++r.sleeps;
    if (r.sleeps == 1) {
      r.peers_before_turn = mn::g_peer_count;
      r.state_before_turn = mn::g_mesh_state;
      r.name_before_turn = mn::g_opera_config.opera_name;
    }
    if (turn_at != 0 && r.sleeps == turn_at) {
      host_sim::on_httpd_task = false;   // the loop task's turn
      mn::update();
      ++r.loop_turns;
      host_sim::on_httpd_task = true;
    }
  };
  host_sim::on_httpd_task = true;
  r.wait = mn::submit(cmd, &r.ok);
  host_sim::on_httpd_task = false;
  host_sim::on_task_delay = nullptr;
  r.waited_ms = host_sim::now_ms - start;
  return r;
}

mn::Command cmd_of(mn::CommandType t) { return mn::make_command(t); }

mn::Command remove_of(const Device& member) {
  mn::Command c = mn::make_command(mn::MESH_CMD_REMOVE_PEER);
  mn::compute_fingerprint(member.pub, c.fingerprint);
  return c;
}

uint8_t nvs_peer_count(Device& d) {
  become(d);
  mn::g_prefs.begin(mn::NVS_NS, true);
  const uint8_t n = mn::g_prefs.getUChar(mn::NVS_PEER_COUNT, 0);
  mn::g_prefs.end();
  return n;
}

std::string nvs_opera_name(Device& d) {
  become(d);
  mn::g_prefs.begin(mn::NVS_NS, true);
  const String s = mn::g_prefs.getString(mn::NVS_FLEET_NAME, "");
  mn::g_prefs.end();
  return s.c_str();
}

bool revoked(Device& self, const Device& other) {
  become(self);
  uint8_t fp[mn::FINGERPRINT_SIZE];
  mn::compute_fingerprint(other.pub, fp);
  return mesh_revocation::contains(mn::g_revoked, fp, host_sim::now_ms);
}

// ── The command waits for the loop task ─────────────────────────────────

// POST /api/mesh/remove: nothing the removal changes moves while the
// handler waits; the loop task's update() pass makes every change (the
// peer table, NVS, the ESP-NOW registration, the deny-list, the rekey);
// the handler reports what remove_peer() returned.
void test_a_removal_runs_on_the_loop_task() {
  fresh_opera({&A, &B, &C});
  host_sim::httpd_side_effects = 0;
  A.espnow.sent.clear();
  become(A);
  CHECK(mn::g_peer_count == 2);
  // A session with B, so the removal's rekey has a member to send to.
  mn::OperaPeer* b = entry(A, B);
  b->session_established = true;
  host_sim::fill_random(b->session_key, sizeof b->session_key);

  const Rest r = rest(A, remove_of(C), /*turn_at=*/2);
  CHECK(r.wait == lcr::Wait::kDone && r.ok);
  CHECK(r.sleeps == 2 && r.loop_turns == 1);
  CHECK(r.peers_before_turn == 2);              // still there while the handler waited
  CHECK(host_sim::httpd_side_effects == 0);     // every write was the loop task's
  become(A);
  CHECK(mn::g_peer_count == 1);
  CHECK(entry(A, C) == nullptr && entry(A, B) != nullptr);
  CHECK(nvs_peer_count(A) == 1);
  CHECK(!A.espnow.has(C.mac));
  CHECK(revoked(A, C));
  become(A);
  CHECK(mn::g_rekey.active);
  CHECK(!sent_to(A, B.mac).empty());            // the REKEY to the survivor
  std::printf("PASS a_removal_runs_on_the_loop_task\n");
}

// A removal of a fingerprint that is not a member: the handler hears
// remove_peer()'s false (400 remove_failed), not the 503.
void test_a_failed_removal_reports_the_commands_false() {
  fresh_opera({&A, &B});
  const Rest r = rest(A, remove_of(C));
  CHECK(r.wait == lcr::Wait::kDone);
  CHECK(!r.ok);
  become(A);
  CHECK(mn::g_peer_count == 1);
  std::printf("PASS a_failed_removal_reports_the_commands_false\n");
}

// Every command, through the queue, does what its function did, and makes
// no write from the handler's task.
void test_every_command_runs_on_the_loop_task() {
  fresh_opera({&A, &B});
  host_sim::httpd_side_effects = 0;

  // Rename: the name lands in RAM and NVS on the loop task.
  mn::Command rn = cmd_of(mn::MESH_CMD_RENAME);
  strcpy(rn.name, "attic");
  Rest r = rest(A, rn);
  CHECK(r.wait == lcr::Wait::kDone && r.ok);
  CHECK(r.name_before_turn == "test");
  become(A);
  CHECK(std::string(mn::g_opera_config.opera_name) == "attic");
  CHECK(nvs_opera_name(A) == "attic");

  // Pair as the initiator; a second start while pairing is refused (the
  // handler's 400 pairing_failed); cancel; join; cancel.
  r = rest(A, cmd_of(mn::MESH_CMD_PAIR_START));
  CHECK(r.wait == lcr::Wait::kDone && r.ok);
  CHECK(r.state_before_turn != mn::MESH_PAIRING_INIT);
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_INIT);
  CHECK(std::string(mn::g_opera_config.opera_name) == "attic");   // an opera keeps its name
  r = rest(A, cmd_of(mn::MESH_CMD_PAIR_START));
  CHECK(r.wait == lcr::Wait::kDone && !r.ok);
  r = rest(A, cmd_of(mn::MESH_CMD_PAIR_CONFIRM));                 // no code shown yet
  CHECK(r.wait == lcr::Wait::kDone && !r.ok);
  r = rest(A, cmd_of(mn::MESH_CMD_PAIR_CANCEL));
  CHECK(r.wait == lcr::Wait::kDone && r.ok);
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_CONNECTING);
  r = rest(A, cmd_of(mn::MESH_CMD_PAIR_JOIN));
  CHECK(r.wait == lcr::Wait::kDone && r.ok);
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_JOIN);
  r = rest(A, cmd_of(mn::MESH_CMD_PAIR_CANCEL));
  CHECK(r.wait == lcr::Wait::kDone && r.ok);

  // Alerts: one stored, then cleared on the loop task.
  become(A);
  mn::MeshAlert alert = {};
  alert.type = mn::ALERT_TAMPER;
  mn::store_alert(&alert);
  size_t n = 0;
  mn::get_alerts(&n);
  CHECK(n == 1);
  r = rest(A, cmd_of(mn::MESH_CMD_CLEAR_ALERTS));
  CHECK(r.wait == lcr::Wait::kDone && r.ok);
  mn::get_alerts(&n);
  CHECK(n == 0);

  // Off, and back on.
  mn::Command off = cmd_of(mn::MESH_CMD_SET_ENABLED);
  off.flag = false;
  r = rest(A, off);
  CHECK(r.wait == lcr::Wait::kDone && r.ok);
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_DISABLED && !mn::is_enabled());
  mn::Command on = cmd_of(mn::MESH_CMD_SET_ENABLED);
  on.flag = true;
  r = rest(A, on);
  CHECK(r.wait == lcr::Wait::kDone && r.ok);
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_CONNECTING && mn::is_enabled());

  // Leave: the opera and its members go, in RAM and NVS.
  r = rest(A, cmd_of(mn::MESH_CMD_LEAVE));
  CHECK(r.wait == lcr::Wait::kDone && r.ok);
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_NO_OPERA);
  CHECK(mn::g_peer_count == 0 && !mn::g_opera_config.configured);
  CHECK(nvs_peer_count(A) == 0);

  // Start on a device with no opera founds one, named as asked.
  mn::Command st = cmd_of(mn::MESH_CMD_PAIR_START);
  st.flag = true;
  strcpy(st.name, "garage");
  r = rest(A, st);
  CHECK(r.wait == lcr::Wait::kDone && r.ok);
  become(A);
  CHECK(mn::g_opera_config.configured);
  CHECK(std::string(mn::g_opera_config.opera_name) == "garage");
  r = rest(A, cmd_of(mn::MESH_CMD_PAIR_CANCEL));
  CHECK(r.wait == lcr::Wait::kDone);

  CHECK(host_sim::httpd_side_effects == 0);
  CHECK(host_sim::mux_depth == 0);              // every critical section closed
  std::printf("PASS every_command_runs_on_the_loop_task\n");
}

// update() drains the commands before its early return, so a disabled mesh
// still turns back on: the drain after that return would never run while
// the mesh is off, and the handler would answer 503 forever.
void test_a_disabled_mesh_still_takes_the_enable_command() {
  fresh_opera({&A, &B});
  mn::Command off = cmd_of(mn::MESH_CMD_SET_ENABLED);
  CHECK(rest(A, off).ok);
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_DISABLED);
  mn::Command on = cmd_of(mn::MESH_CMD_SET_ENABLED);
  on.flag = true;
  const Rest r = rest(A, on);
  CHECK(r.wait == lcr::Wait::kDone && r.ok);
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_CONNECTING);
  std::printf("PASS a_disabled_mesh_still_takes_the_enable_command\n");
}

// ── Bounded waits ───────────────────────────────────────────────────────

// The loop task never gets to it: the handler stops waiting at
// COMMAND_WAIT_MS, the command is withdrawn (the 503 mesh_timeout is true:
// nothing happened), and a later update() does not run it. The handler's
// answer is the PlatformIO tree's for the case: 503 mesh_timeout, with a
// 503 status line.
void test_a_command_the_loop_task_does_not_reach_is_withdrawn() {
  fresh_opera({&A, &B, &C});
  host_sim::httpd_side_effects = 0;
  const Rest r = rest(A, remove_of(C), /*turn_at=*/0);
  CHECK(r.wait == lcr::Wait::kWithdrawn);
  CHECK(!r.ok);
  CHECK(mn::not_run_status(r.wait) == 503);
  CHECK(std::strcmp(mn::not_run_error(r.wait), "mesh_timeout") == 0);
  CHECK(std::strcmp(http_status_line(mn::not_run_status(r.wait)), "503 Service Unavailable") == 0);
  CHECK(r.waited_ms == mn::COMMAND_WAIT_MS);
  CHECK(r.sleeps == mn::COMMAND_WAIT_MS / mn::COMMAND_POLL_MS);
  become(A);
  CHECK(mn::g_commands.queued() == 0);
  host_sim::now_ms += 10;
  mn::update();
  CHECK(mn::g_peer_count == 2 && entry(A, C) != nullptr);
  CHECK(nvs_peer_count(A) == 2);
  CHECK(!revoked(A, C));
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_command_the_loop_task_does_not_reach_is_withdrawn\n");
}

// The loop task reaches it on the handler's last sleep before the timeout:
// it runs, and the handler reports it.
void test_a_command_reached_just_in_time_reports_its_result() {
  fresh_opera({&A, &B, &C});
  const unsigned last = mn::COMMAND_WAIT_MS / mn::COMMAND_POLL_MS;
  const Rest r = rest(A, remove_of(C), last);
  CHECK(r.wait == lcr::Wait::kDone && r.ok);
  become(A);
  CHECK(entry(A, C) == nullptr);
  std::printf("PASS a_command_reached_just_in_time_reports_its_result\n");
}

// Four commands already waiting: a fifth handler answers 409 mesh_busy at
// once (the PlatformIO tree's answer, with a 409 status line), without
// waiting, and its command never runs.
void test_a_full_ring_answers_busy_at_once() {
  fresh_opera({&A, &B, &C});
  become(A);
  for (size_t i = 0; i < mn::COMMAND_SLOTS; ++i) {
    CHECK(mn::g_commands.post(cmd_of(mn::MESH_CMD_CLEAR_ALERTS)) != 0);
  }
  const Rest r = rest(A, remove_of(C), /*turn_at=*/1);
  CHECK(r.wait == lcr::Wait::kBusy && !r.ok);
  CHECK(r.sleeps == 0 && r.waited_ms == 0);
  CHECK(mn::not_run_status(r.wait) == 409);
  CHECK(std::strcmp(mn::not_run_error(r.wait), "mesh_busy") == 0);
  CHECK(std::strcmp(http_status_line(mn::not_run_status(r.wait)), "409 Conflict") == 0);
  become(A);
  mn::update();                                 // runs the four, not the fifth
  CHECK(mn::g_commands.queued() == 0);
  CHECK(entry(A, C) != nullptr);
  std::printf("PASS a_full_ring_answers_busy_at_once\n");
}

// Commands run in the order they were posted: a start then a cancel leaves
// no pairing; a cancel then a start leaves one.
void test_commands_run_in_the_order_they_were_posted() {
  fresh_opera({&A, &B});
  become(A);
  CHECK(mn::g_commands.post(cmd_of(mn::MESH_CMD_PAIR_START)) != 0);
  CHECK(mn::g_commands.post(cmd_of(mn::MESH_CMD_PAIR_CANCEL)) != 0);
  mn::update();
  CHECK(!mn::is_pairing());
  CHECK(mn::g_commands.post(cmd_of(mn::MESH_CMD_PAIR_CANCEL)) != 0);
  CHECK(mn::g_commands.post(cmd_of(mn::MESH_CMD_PAIR_START)) != 0);
  mn::update();
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_INIT);
  std::printf("PASS commands_run_in_the_order_they_were_posted\n");
}

// ── End to end ──────────────────────────────────────────────────────────

// A pairing the two owners drive over REST: start and join through the
// queue, the frames carried as bytes, both confirms through the queue (the
// joiner's owner first, F75's harder order). Both devices finish holding
// each other, and no handler wrote anything.
void test_a_pairing_driven_over_rest_completes() {
  fresh_device(A);
  fresh_device(J);
  host_sim::httpd_side_effects = 0;
  CHECK(rest(A, cmd_of(mn::MESH_CMD_PAIR_START)).ok);
  CHECK(rest(J, cmd_of(mn::MESH_CMD_PAIR_JOIN)).ok);
  A.espnow.sent.clear();
  J.espnow.sent.clear();
  host_sim::now_ms += 2001;
  become(J);
  mn::update();
  const auto disc = sent_to(J, BROADCAST);
  CHECK(!disc.empty());
  deliver(A, J.mac, disc.back());
  const auto offer = sent_to(A, J.mac);
  CHECK(offer.size() == 1);
  deliver(J, A.mac, offer.back());
  const auto accept = sent_to(J, A.mac);
  CHECK(accept.size() == 1);
  deliver(A, J.mac, accept.back());
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);

  // The joiner's owner confirms first: its CONFIRM goes out on J's loop.
  J.espnow.sent.clear();
  CHECK(rest(J, cmd_of(mn::MESH_CMD_PAIR_CONFIRM)).ok);
  const auto j_confirm = sent_to(J, A.mac);
  CHECK(j_confirm.size() == 1);
  deliver(A, J.mac, j_confirm[0]);
  // Then the initiator's: the next pass (initiator_step) sends COMPLETE.
  A.espnow.sent.clear();
  CHECK(rest(A, cmd_of(mn::MESH_CMD_PAIR_CONFIRM)).ok);
  const auto complete = sent_to(A, J.mac);
  CHECK(!complete.empty());
  for (const Frame& f : complete) deliver(J, A.mac, f);

  for (Device* d : {&A, &J}) {
    become(*d);
    CHECK(!mn::is_pairing());
    CHECK(mn::g_peer_count == 1);
  }
  CHECK(entry(A, J) != nullptr && entry(J, A) != nullptr);
  CHECK(nvs_peer_count(A) == 1 && nvs_peer_count(J) == 1);
  CHECK(host_sim::httpd_side_effects == 0);
  std::printf("PASS a_pairing_driven_over_rest_completes\n");
}

// ── The pre-reboot replay save ──────────────────────────────────────────

// What one pre-reboot save saw, called as the hook calls it on the task the
// test plays (the HTTP server's when `httpd`), with the loop task getting
// one update() pass on the caller's `turn_at`-th sleep (never, when 0).
struct RebootSave {
  bool ok = false;
  unsigned sleeps = 0;
  unsigned loop_turns = 0;
  uint32_t waited_ms = 0;
};
RebootSave reboot_save(Device& d, bool httpd, unsigned turn_at) {
  become(d);
  RebootSave r;
  const uint32_t start = host_sim::now_ms;
  host_sim::on_task_delay = [&](uint32_t ms) {
    host_sim::now_ms += ms;
    ++r.sleeps;
    if (turn_at != 0 && r.sleeps == turn_at) {
      host_sim::on_httpd_task = false;
      mn::update();
      ++r.loop_turns;
      host_sim::on_httpd_task = true;
    }
  };
  host_sim::on_httpd_task = httpd;
  r.ok = mn::save_replay_counters_before_reboot();
  host_sim::on_httpd_task = false;
  host_sim::on_task_delay = nullptr;
  r.waited_ms = host_sim::now_ms - start;
  return r;
}

// The first member's msg_counter_rx in d's saved replay blob (0: none).
uint64_t saved_rx_counter(Device& d) {
  become(d);
  uint8_t blob[mn::MAX_OPERA_SIZE * mn::REPLAY_ENTRY_SIZE] = {};
  mn::g_prefs.begin(mn::NVS_NS, true);
  const size_t got = mn::g_prefs.getBytes(mn::NVS_REPLAY_KEY, blob, sizeof(blob));
  mn::g_prefs.end();
  if (got < mn::REPLAY_ENTRY_SIZE) return 0;
  uint64_t ctr = 0;
  std::memcpy(&ctr, blob + mn::FINGERPRINT_SIZE, sizeof(ctr));
  return ctr;
}

// POST /api/reboot and the safe-mode retry run the pre-reboot hook on the
// HTTP server's task. The replay save reads the peer table and writes
// through the mesh's one g_prefs handle, which update() uses on the loop
// task, so from there it is a MESH_CMD_SAVE_REPLAY the loop task runs (the
// F96 review found the hook calling save_replay_counters() in place). On
// the loop task (the safe-mode paths in setup() and loop()) it saves in
// place, and a save the loop task never reaches does not run.
void test_a_reboot_save_from_the_http_task_runs_on_the_loop_task() {
  fresh_opera({&A, &B});
  become(A);
  mn::g_peers[0].msg_counter_rx = 4242;
  host_sim::httpd_side_effects = 0;
  RebootSave r = reboot_save(A, /*httpd=*/true, /*turn_at=*/1);
  CHECK(host_sim::httpd_side_effects == 0);           // nothing written from the HTTP task
  CHECK(r.ok && r.loop_turns == 1);
  CHECK(saved_rx_counter(A) == 4242);                 // the loop task wrote it
  // The loop task never gets to it: the save is withdrawn, not run.
  become(A);
  mn::g_peers[0].msg_counter_rx = 5555;
  r = reboot_save(A, /*httpd=*/true, /*turn_at=*/0);
  CHECK(!r.ok && r.waited_ms == mn::COMMAND_WAIT_MS);
  CHECK(host_sim::httpd_side_effects == 0);
  CHECK(saved_rx_counter(A) == 4242);
  // On the loop task: in place, no hand-over, no wait.
  become(A);
  mn::g_peers[0].msg_counter_rx = 6666;
  r = reboot_save(A, /*httpd=*/false, /*turn_at=*/1);
  CHECK(r.ok && r.sleeps == 0 && r.loop_turns == 0);
  CHECK(saved_rx_counter(A) == 6666);
  std::printf("PASS a_reboot_save_from_the_http_task_runs_on_the_loop_task\n");
}

// The status line of every code an error answer in canary_wap.ino carries
// (http_send_error: 400, 409, 500, 503, and 404). Before F96 every code but
// 400, 404 and 500 went out as "400 Bad Request", the audio self-test's 409
// and the BLE chirp send's 503 among them; an unknown code still does.
void test_every_error_code_has_its_status_line() {
  CHECK(std::strcmp(http_status_line(400), "400 Bad Request") == 0);
  CHECK(std::strcmp(http_status_line(404), "404 Not Found") == 0);
  CHECK(std::strcmp(http_status_line(409), "409 Conflict") == 0);
  CHECK(std::strcmp(http_status_line(500), "500 Internal Server Error") == 0);
  CHECK(std::strcmp(http_status_line(503), "503 Service Unavailable") == 0);
  CHECK(std::strcmp(http_status_line(418), "400 Bad Request") == 0);
  std::printf("PASS every_error_code_has_its_status_line\n");
}

struct Test {
  const char* name;
  void (*fn)();
};
const Test kTests[] = {
    {"a_removal_runs_on_the_loop_task", test_a_removal_runs_on_the_loop_task},
    {"a_failed_removal_reports_the_commands_false", test_a_failed_removal_reports_the_commands_false},
    {"every_command_runs_on_the_loop_task", test_every_command_runs_on_the_loop_task},
    {"a_disabled_mesh_still_takes_the_enable_command", test_a_disabled_mesh_still_takes_the_enable_command},
    {"a_command_the_loop_task_does_not_reach_is_withdrawn",
     test_a_command_the_loop_task_does_not_reach_is_withdrawn},
    {"a_command_reached_just_in_time_reports_its_result",
     test_a_command_reached_just_in_time_reports_its_result},
    {"a_full_ring_answers_busy_at_once", test_a_full_ring_answers_busy_at_once},
    {"commands_run_in_the_order_they_were_posted", test_commands_run_in_the_order_they_were_posted},
    {"a_pairing_driven_over_rest_completes", test_a_pairing_driven_over_rest_completes},
    {"every_error_code_has_its_status_line", test_every_error_code_has_its_status_line},
    {"a_reboot_save_from_the_http_task_runs_on_the_loop_task",
     test_a_reboot_save_from_the_http_task_runs_on_the_loop_task},
};

}  // namespace commands

int main(int argc, char** argv) {
  using namespace commands;
  make_device(A, "A", 0xA1);
  make_device(B, "B", 0xB1);
  make_device(C, "C", 0xC1);
  make_device(J, "J", 0xD1);
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
  std::printf("ALL %d mesh command checks PASSED (%d tests)\n", g_checks, ran);
  return 0;
}
