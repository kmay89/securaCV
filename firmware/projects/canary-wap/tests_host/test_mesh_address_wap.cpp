// Host test: which address canary-wap sends a member's frames to, and what
// may change it — run against the REAL mesh_network.cpp, not a model.
//
// mesh_network.cpp is #included below and compiled on the host against the
// stubs in stubs/mesh_net (ESP-NOW peer list and send log, an in-memory NVS,
// real Ed25519 / SHA-256 / ChaCha20-Poly1305 from OpenSSL) plus the staged
// pairing stubs (real X25519). mesh_net_sim.h runs several devices on that
// one image by swapping the file's statics, and moves frames between them
// as bytes through each device's ESP-NOW receive callback and update(). So
// every frame here was built and signed by the sender's own send path, and
// every verdict is the receiver's own receive path's. An outsider E only
// records frames and re-sends them from another address.
//
// Host-tested only: the stubs stand in for the radio, so this says nothing
// about two real boards (U1 Track C2), and the Arduino compile of the file
// is CI's (firmware.yml's canary-wap legs).
//
// Build: see the MESH_ADDR block in the Makefile.

#ifndef MESH_NETWORK_CPP
#error "MESH_NETWORK_CPP (absolute path to mesh_network.cpp) must be defined"
#endif
// The firmware file is held to the device build's warnings, not to this
// Makefile's -Wextra -Wpedantic -Werror: the host flags its int/size_t
// length compare, handler parameters it does not read, two helpers it does
// not call and two bounded strncpy copies. Those warnings are off for the
// included file only; this test's own code keeps all of them.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#if defined(__GNUC__) && !defined(__clang__)   // a GCC-only warning name
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
#include MESH_NETWORK_CPP
#pragma GCC diagnostic pop
#include "mesh_net_sim.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// The sketch provides these on a device (canary_wap.ino, chirp_channel.cpp).
// health_log keeps what the code under test logged, for the tests to read.
std::vector<std::string> g_health;
void health_log(LogLevel, LogCategory, const char* message) { g_health.push_back(message); }
void log_health(LogLevel, LogCategory, const char*, const char*) {}
namespace chirp_channel {
void dispatch_espnow_message(const uint8_t*, const uint8_t*, int, int8_t) {}
}

namespace {

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
using Frame = std::vector<uint8_t>;

Device A, B, C;
const uint8_t E_MAC[6] = {0x02, 0xEE, 0xEE, 0xEE, 0xEE, 0x01};

// A, B and C as NVS holds an opera after pairing, then booted: each holds
// the other two (PEER_OFFLINE after boot, which broadcasts include), and
// none has heard anything yet.
void fresh_opera() {
  Device* all[3] = {&A, &B, &C};
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
    CHECK(mn::compute_opera_id(secret, mn::g_opera_config.opera_id));
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

// B's next heartbeat, as B's broadcast_message sends it: one frame to each
// member, each under B's counter for that member. Returns B's frame to `to`.
Frame b_heartbeat_to(const Device& to) {
  become(B);
  mn::send_heartbeat();
  const auto frames = sent_to(B, to.mac);
  CHECK(!frames.empty());
  return frames.back();
}

// What A holds for B, read in one place.
struct BView {
  bool bound_to_b;        // A's entry for B carries B's address
  bool espnow_b;          // B's address is on A's ESP-NOW list
  bool espnow_e;          // E's address is
  uint64_t rx;            // A's last-seen counter for B
  uint32_t received;      // A's verified frames, all members
  uint32_t auth_failures;
};
BView a_view_of_b() {
  mn::OperaPeer* pb = entry(A, B);
  CHECK(pb != nullptr);
  return BView{same_mac(pb->mac_addr, B.mac), A.espnow.has(B.mac), A.espnow.has(E_MAC),
               pb->msg_counter_rx, mn::g_messages_received, mn::g_auth_failures};
}

// Where A's next heartbeat goes: true if one of its frames went to `mac`.
bool a_heartbeat_reaches(const uint8_t mac[6]) {
  become(A);
  A.espnow.sent.clear();
  mn::send_heartbeat();
  return !sent_to(A, mac).empty();
}

// ── The receive path's existing gates, end to end ───────────────────────

void test_a_frame_from_the_bound_address_is_heard() {
  fresh_opera();
  const Frame f = b_heartbeat_to(A);
  CHECK(counter_of(f) == 1);                     // the first counter signed is 1
  const BView before = a_view_of_b();
  deliver(A, B.mac, f);
  const BView after = a_view_of_b();
  CHECK(after.bound_to_b && after.espnow_b && !after.espnow_e);
  CHECK(after.rx == 1);
  CHECK(after.received == before.received + 1);
  CHECK(entry(A, B)->state == mn::PEER_CONNECTED);
  CHECK(a_heartbeat_reaches(B.mac));
  CHECK(a_heartbeat_reaches(C.mac));
  std::printf("PASS a_frame_from_the_bound_address_is_heard\n");
}

void test_a_forged_frame_from_another_address_moves_nothing() {
  // The keyless denial of service the ORDER fix closed: a frame carrying
  // B's public fingerprint and opera_id with a signature that does not
  // verify, sent from E's address.
  fresh_opera();
  deliver(A, B.mac, b_heartbeat_to(A));
  Frame forged = b_heartbeat_to(A);
  forged.back() ^= 0x01;
  const BView before = a_view_of_b();
  deliver(A, E_MAC, forged);
  const BView after = a_view_of_b();
  CHECK(after.bound_to_b && after.espnow_b && !after.espnow_e);
  CHECK(after.rx == before.rx);
  CHECK(after.received == before.received);
  CHECK(after.auth_failures == before.auth_failures + 1);
  CHECK(a_heartbeat_reaches(B.mac));
  std::printf("PASS a_forged_frame_from_another_address_moves_nothing\n");
}

void test_a_frame_already_heard_moves_nothing() {
  // The replay gate: a frame A has heard, re-sent from E.
  fresh_opera();
  const Frame f = b_heartbeat_to(A);
  deliver(A, B.mac, f);
  const BView before = a_view_of_b();
  deliver(A, E_MAC, f);
  const BView after = a_view_of_b();
  CHECK(after.bound_to_b && after.espnow_b && !after.espnow_e);
  CHECK(after.rx == before.rx);
  CHECK(after.received == before.received);
  CHECK(a_heartbeat_reaches(B.mac));
  std::printf("PASS a_frame_already_heard_moves_nothing\n");
}


// ── Spec §8.3: a verified frame does not move a member's address ────────
//
// The checks a frame passes (opera_id, a signature under the member's key,
// a counter above the last one heard) prove who signed it, not which radio
// sent it: the envelope signs no address. canary-wap used to re-point the
// member's address, and its ESP-NOW registration, at the source of any
// frame that passed them. So a genuine frame of B's that A had not heard
// yet, re-sent from E's own radio, moved B to E: A's frames for B went to
// E, and B's real address left A's ESP-NOW list. Each test below re-sends
// such a frame from another address and fails on that code. Now a frame
// whose source is not the signer's bound address is dropped before the
// signature check: nothing moves, no counter is spent, nothing is
// dispatched, and it counts as an auth failure.

int g_beacons = 0;
void count_beacon(const uint8_t*, mesh_beacon::BeaconState, const char*) { ++g_beacons; }

// The drop, as A's state shows it.
void check_dropped(const BView& before, const BView& after) {
  CHECK(after.bound_to_b);
  CHECK(after.espnow_b);
  CHECK(!after.espnow_e);
  CHECK(after.rx == before.rx);                  // no counter spent
  CHECK(after.received == before.received);      // not dispatched
  CHECK(after.auth_failures == before.auth_failures + 1);
}

void test_a_missed_frame_from_another_address_moves_nothing() {
  fresh_opera();
  deliver(A, B.mac, b_heartbeat_to(A));          // A hears B's counter 1
  const Frame missed = b_heartbeat_to(A);        // counter 2: A misses it
  CHECK(counter_of(missed) == 2);
  const BView before = a_view_of_b();
  deliver(A, E_MAC, missed);                     // E re-sends it
  check_dropped(before, a_view_of_b());
  CHECK(a_heartbeat_reaches(B.mac));
  CHECK(!a_heartbeat_reaches(E_MAC));
  // No counter was spent, so the frame itself is still good from B.
  deliver(A, B.mac, missed);
  CHECK(a_view_of_b().rx == 2);
  deliver(A, B.mac, b_heartbeat_to(A));
  CHECK(a_view_of_b().rx == 3 && a_view_of_b().bound_to_b);
  std::printf("PASS a_missed_frame_from_another_address_moves_nothing\n");
}

void test_a_missed_frame_from_another_address_is_not_acted_on() {
  fresh_opera();
  mn::set_beacon_event_handler(count_beacon);
  g_beacons = 0;
  become(B);
  CHECK(mn::send_beacon_event(mesh_beacon::BeaconState::ARRIVED, "hall") == 2);
  const Frame ev = sent_to(B, A.mac).back();
  deliver(A, E_MAC, ev);
  CHECK(g_beacons == 0);
  CHECK(a_view_of_b().bound_to_b);
  deliver(A, B.mac, ev);                         // the same frame from B is heard
  CHECK(g_beacons == 1);
  mn::set_beacon_event_handler(nullptr);
  std::printf("PASS a_missed_frame_from_another_address_is_not_acted_on\n");
}

void test_another_members_frame_from_another_address_moves_nothing() {
  // canary-wap counts per destination (send_to_peer takes the peer's own
  // msg_counter_tx), and the envelope names no destination, so A judges a
  // frame B sent C by A's last-seen counter for B. B's counter for C runs
  // ahead of its counter for A whenever B sent C frames A was not sent:
  // the alerts and the Beacon, channel-lock and hub-election sends skip a
  // member in PEER_UNKNOWN or PEER_AUTHENTICATING (a fresh pairing's
  // partner, until it is heard). That is set up directly here: three Beacon
  // events with A at PEER_UNKNOWN in B's table. (This used heartbeats,
  // which skipped such a member too until F76.)
  fresh_opera();
  deliver(A, B.mac, b_heartbeat_to(A));          // A's last-seen for B: 1
  {
    become(B);
    mn::OperaPeer* pa = entry(B, A);
    const mn::PeerState keep = pa->state;
    pa->state = mn::PEER_UNKNOWN;
    for (int i = 0; i < 3; ++i) {
      CHECK(mn::send_beacon_event(mesh_beacon::BeaconState::ARRIVED, "hall") == 1);
    }
    pa->state = keep;
  }
  const Frame ahead = sent_to(B, C.mac).back();
  CHECK(counter_of(ahead) == 4);
  const BView before = a_view_of_b();
  CHECK(before.rx == 1);
  deliver(A, E_MAC, ahead);
  check_dropped(before, a_view_of_b());
  // B is not silenced: its own next frame to A (counter 2) is heard. The
  // old code had moved A's last-seen to 4, so B's frames 2..4 dropped as
  // replays and B stayed bound to E until its counter for A passed 4.
  const Frame next = b_heartbeat_to(A);
  CHECK(counter_of(next) == 2);
  deliver(A, B.mac, next);
  CHECK(a_view_of_b().rx == 2 && a_view_of_b().bound_to_b);
  CHECK(a_heartbeat_reaches(B.mac));
  std::printf("PASS another_members_frame_from_another_address_moves_nothing\n");
}

void test_a_frame_heard_before_a_power_cut_moves_nothing() {
  // The sketch saves the last-seen counters every 5 minutes and before a
  // planned reboot; a power cut loses what A heard since the last save, so
  // those frames are fresh at A again.
  fresh_opera();
  deliver(A, B.mac, b_heartbeat_to(A));
  become(A);
  CHECK(mn::save_replay_counters());             // last-seen 1 saved
  deliver(A, B.mac, b_heartbeat_to(A));
  const Frame heard = b_heartbeat_to(A);
  deliver(A, B.mac, heard);
  CHECK(a_view_of_b().rx == 3);
  boot(A);                                       // power cut, no save since
  const BView before = a_view_of_b();
  CHECK(before.rx == 1);
  deliver(A, E_MAC, heard);
  check_dropped(before, a_view_of_b());
  CHECK(a_heartbeat_reaches(B.mac));
  std::printf("PASS a_frame_heard_before_a_power_cut_moves_nothing\n");
}

void test_a_frame_from_another_members_address_moves_nothing() {
  // ESP-NOW does not authenticate a source, so a radio can copy C's
  // address. B's missed frame from there used to bind B to C's address;
  // B's next real frame then deleted C's ESP-NOW registration while A's
  // entry for C still held it, and A could no longer reach C.
  fresh_opera();
  deliver(A, B.mac, b_heartbeat_to(A));
  const Frame missed = b_heartbeat_to(A);
  const BView before = a_view_of_b();
  deliver(A, C.mac, missed);
  const BView after = a_view_of_b();
  CHECK(after.bound_to_b && after.espnow_b);
  CHECK(after.rx == before.rx && after.received == before.received);
  CHECK(after.auth_failures == before.auth_failures + 1);
  CHECK(same_mac(entry(A, C)->mac_addr, C.mac));
  deliver(A, B.mac, b_heartbeat_to(A));
  CHECK(A.espnow.has(C.mac));
  CHECK(a_heartbeat_reaches(B.mac));
  CHECK(a_heartbeat_reaches(C.mac));
  std::printf("PASS a_frame_from_another_members_address_moves_nothing\n");
}

void test_a_member_whose_address_changed_is_not_heard_until_re_paired() {
  // A changed radio address (B's NVS, which holds its key, moved to
  // another board) now means a re-pair, as on the PlatformIO tree; a
  // swapped module comes back with a new key and joins as a new member.
  // Until then A keeps sending
  // to the address the pairing bound and drops B's frames from the new
  // one. (B reboots to change its address; since F71 its counter for A
  // resumes above every one A has heard, so these frames would pass the
  // replay gate, and only the address drops them.)
  fresh_opera();
  deliver(A, B.mac, b_heartbeat_to(A));
  const uint8_t old_mac[6] = {B.mac[0], B.mac[1], B.mac[2], B.mac[3], B.mac[4], B.mac[5]};
  B.mac[5] = 0xB2;
  boot(B);
  for (int i = 0; i < 2; ++i) {
    const Frame f = b_heartbeat_to(A);
    CHECK(counter_of(f) > 1);                    // fresh to the replay gate
    const uint32_t failures = a_view_of_b().auth_failures;
    deliver(A, B.mac, f);
    CHECK(a_view_of_b().auth_failures == failures + 1);   // the address drops it
  }
  mn::OperaPeer* pb = entry(A, B);
  CHECK(same_mac(pb->mac_addr, old_mac));
  CHECK(pb->msg_counter_rx == 1);
  CHECK(A.espnow.has(old_mac) && !A.espnow.has(B.mac));
  CHECK(a_heartbeat_reaches(old_mac));
  CHECK(!a_heartbeat_reaches(B.mac));
  memcpy(B.mac, old_mac, 6);
  std::printf("PASS a_member_whose_address_changed_is_not_heard_until_re_paired\n");
}

// ── A re-pair re-binds the member it already holds ──────────────────────

const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// A WAP-to-WAP pairing through the real handlers, every frame carried as
// bytes from one device's send log to the other's receive path, in two
// halves. To the codes: the joiner's DISCOVER (sent by update() on its 2 s
// tick), the initiator's OFFER and the joiner's ACCEPT, after which both
// screens show a code. Then the owners: both confirm the code (the
// initiator's first), the CONFIRMs cross, and the initiator's COMPLETE.
void pair_to_codes(Device& ini, Device& joi) {
  become(ini);
  CHECK(mn::start_pairing_initiator(nullptr));
  become(joi);
  CHECK(mn::start_pairing_joiner());
  ini.espnow.sent.clear();
  joi.espnow.sent.clear();
  host_sim::now_ms += 2001;
  become(joi);
  mn::update();
  const auto disc = sent_to(joi, BROADCAST);
  CHECK(!disc.empty());
  deliver(ini, joi.mac, disc.back());
  const auto offer = sent_to(ini, joi.mac);
  CHECK(offer.size() == 1);
  deliver(joi, ini.mac, offer.back());
  const auto accept = sent_to(joi, ini.mac);
  CHECK(accept.size() == 1);
  deliver(ini, joi.mac, accept.back());
  become(ini);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);
  const uint32_t code_ini = mn::g_pairing.confirmation_code;
  become(joi);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);
  const uint32_t code_joi = mn::g_pairing.confirmation_code;
  CHECK(code_ini == code_joi);                   // the two screens agree
}

void pair_confirm(Device& ini, Device& joi) {
  become(ini);
  CHECK(mn::confirm_pairing());
  become(joi);
  CHECK(mn::confirm_pairing());
  const Frame confirm_ini = sent_to(ini, joi.mac).back();
  const Frame confirm_joi = sent_to(joi, ini.mac).back();
  deliver(ini, joi.mac, confirm_joi);            // the initiator adds the joiner
  const Frame complete = sent_to(ini, joi.mac).back();
  deliver(joi, ini.mac, confirm_ini);
  deliver(joi, ini.mac, complete);               // the joiner adds the initiator
  become(ini);
  CHECK(mn::g_mesh_state == mn::MESH_ACTIVE);
  become(joi);
  CHECK(mn::g_mesh_state == mn::MESH_ACTIVE);
}

void run_pairing(Device& ini, Device& joi) {
  pair_to_codes(ini, joi);
  pair_confirm(ini, joi);
}

void test_a_re_pair_re_binds_the_member_it_holds() {
  fresh_opera();
  // A has heard B's frames 1..3 and sent B two heartbeats, so A's counters
  // for B are past where they start: a re-pair that reset them would show.
  for (int i = 0; i < 3; ++i) deliver(A, B.mac, b_heartbeat_to(A));
  CHECK(a_heartbeat_reaches(B.mac));
  CHECK(a_heartbeat_reaches(B.mac));
  const mn::OperaPeer held = *entry(A, B);
  CHECK(held.msg_counter_rx == 3 && held.msg_counter_tx == 3);
  CHECK(held.state == mn::PEER_CONNECTED);
  uint8_t old_mac[6];
  memcpy(old_mac, B.mac, 6);
  B.mac[5] = 0xB2;                               // B's radio address changed
  boot(B);
  g_health.clear();
  run_pairing(A, B);
  // The move is logged on A (the only side whose member moved).
  size_t moved_logs = 0;
  for (const std::string& m : g_health) {
    if (m == "opera: a re-pair moved a member to a new radio address") ++moved_logs;
  }
  CHECK(moved_logs == 1);
  // A holds B once, at the address the pairing completed from, and the
  // ESP-NOW list follows; B holds A once.
  become(A);
  CHECK(mn::g_peer_count == 2);
  mn::OperaPeer* pb = entry(A, B);
  CHECK(same_mac(pb->mac_addr, B.mac));
  CHECK(A.espnow.has(B.mac) && !A.espnow.has(old_mac));
  CHECK(same_mac(entry(A, C)->mac_addr, C.mac));
  // The re-pair kept what A knew about B: the last counter heard (so no
  // frame of B's that A has heard becomes fresh again), the next counter A
  // signs for B, the name A's owner gave B ("New Device" is what a new
  // member gets) and B's state, which keeps B in A's broadcasts.
  CHECK(pb->msg_counter_rx == held.msg_counter_rx);
  CHECK(pb->msg_counter_tx >= held.msg_counter_tx);
  CHECK(strcmp(pb->name, "B") == 0);
  CHECK(pb->state == held.state);
  CHECK(a_heartbeat_reaches(B.mac));             // before A hears B again
  CHECK(!a_heartbeat_reaches(old_mac));
  become(B);
  CHECK(mn::g_peer_count == 2);
  CHECK(same_mac(entry(B, A)->mac_addr, A.mac));
  // B rebooted to change its address. Its counter for A resumes one past
  // the block it reserved before the reboot (F71, test_mesh_liveness_wap),
  // so A hears B's very first frame after the re-pair, from B's new
  // address. (Until F71 B's counter restarted at 1, and this pinned that
  // A dropped B's frames 1..3 as replays and first heard frame 4.)
  {
    const Frame f = b_heartbeat_to(A);
    CHECK(counter_of(f) == mn::TX_COUNTER_RESERVE_BLOCK + 1);
    const uint32_t received = a_view_of_b().received;
    deliver(A, B.mac, f);
    CHECK(a_view_of_b().received == received + 1);
    CHECK(entry(A, B)->msg_counter_rx == mn::TX_COUNTER_RESERVE_BLOCK + 1);
  }
  // A frame from the old address now drops like one from any other.
  const uint64_t rx = entry(A, B)->msg_counter_rx;
  deliver(A, old_mac, b_heartbeat_to(A));
  CHECK(entry(A, B)->msg_counter_rx == rx);
  CHECK(same_mac(entry(A, B)->mac_addr, B.mac));
  // The move is in A's NVS: a reboot binds the new address.
  boot(A);
  CHECK(mn::g_peer_count == 2);
  CHECK(same_mac(entry(A, B)->mac_addr, B.mac));
  CHECK(A.espnow.has(B.mac) && !A.espnow.has(old_mac));
  memcpy(B.mac, old_mac, 6);
  std::printf("PASS a_re_pair_re_binds_the_member_it_holds\n");
}

void test_a_re_pair_the_radio_cannot_register_moves_nothing() {
  // ESP-NOW holds 20 addresses. When the new one cannot be registered the
  // member stays where it was, registration and all: deleting the old one
  // first, or moving anyway, would leave A unable to reach B at either.
  fresh_opera();
  become(A);
  for (uint8_t i = 0; A.espnow.peers.size() < ESP_NOW_MAX_TOTAL_PEER_NUM; ++i) {
    esp_now_peer_info_t p = {};
    const uint8_t m[6] = {0x02, 0x33, 0x00, 0x00, 0x00, i};
    memcpy(p.peer_addr, m, 6);
    CHECK(esp_now_add_peer(&p) == ESP_OK);
  }
  const mn::PeerState state = entry(A, B)->state;
  const uint8_t moved[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xB2};
  g_health.clear();
  become(A);
  CHECK(!mn::add_peer(B.pub, moved, "B"));
  CHECK(mn::g_peer_count == 2);
  CHECK(same_mac(entry(A, B)->mac_addr, B.mac));
  CHECK(entry(A, B)->state == state);
  CHECK(A.espnow.has(B.mac) && !A.espnow.has(moved));
  CHECK(g_health.empty());                       // nothing moved, nothing logged
  CHECK(a_heartbeat_reaches(B.mac));
  std::printf("PASS a_re_pair_the_radio_cannot_register_moves_nothing\n");
}

void test_a_re_pair_is_not_refused_by_a_full_opera() {
  fresh_opera();
  become(A);
  while (mn::g_peer_count < mn::MAX_OPERA_SIZE) {
    uint8_t priv[32], pub[32];
    host_sim::fill_random(priv, sizeof priv);
    Ed25519::derivePublicKey(pub, priv);
    const uint8_t mac[6] = {0x02, 0x10, 0x00, 0x00, 0x00, mn::g_peer_count};
    CHECK(mn::add_peer(pub, mac, "filler"));
  }
  const uint8_t moved[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xB2};
  CHECK(mn::add_peer(B.pub, moved, "B"));
  CHECK(mn::g_peer_count == mn::MAX_OPERA_SIZE);
  CHECK(same_mac(entry(A, B)->mac_addr, moved));
  CHECK(A.espnow.has(moved) && !A.espnow.has(B.mac));
  std::printf("PASS a_re_pair_is_not_refused_by_a_full_opera\n");
}

void test_a_re_pair_cannot_take_another_members_address() {
  fresh_opera();
  become(A);
  CHECK(!mn::add_peer(B.pub, C.mac, "B"));
  CHECK(mn::g_peer_count == 2);
  CHECK(same_mac(entry(A, B)->mac_addr, B.mac));
  CHECK(same_mac(entry(A, C)->mac_addr, C.mac));
  CHECK(A.espnow.has(B.mac) && A.espnow.has(C.mac));
  std::printf("PASS a_re_pair_cannot_take_another_members_address\n");
}

// ── One address, one member (sweep F98) ─────────────────────────────────
//
// Only the re-pair (rebind_peer) refused an address another member holds;
// add_peer appended a NEW key at one. The two entries then shared one
// ESP-NOW registration, removing either deleted it for both, and A sent the
// other nothing (host-probed with the #1761 harness: A's next heartbeat to
// C was not sent). add_peer now refuses it too, and NVS an older firmware
// wrote with such a pair keeps the registration while either entry holds
// the address.

// A key no device in this file holds.
void new_key(uint8_t pub[32]) {
  uint8_t priv[32];
  host_sim::fill_random(priv, sizeof priv);
  Ed25519::derivePublicKey(pub, priv);
}

void test_a_new_key_at_another_members_address_is_refused() {
  fresh_opera();
  uint8_t pub[32];
  new_key(pub);
  become(A);
  CHECK(!mn::add_peer(pub, C.mac, "K"));
  CHECK(mn::g_peer_count == 2);
  CHECK(same_mac(entry(A, C)->mac_addr, C.mac));
  CHECK(A.espnow.has(C.mac));
  CHECK(a_heartbeat_reaches(C.mac));
  // The same key at an address no member holds is a new member as before.
  const uint8_t free_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xE1};
  become(A);
  CHECK(mn::add_peer(pub, free_mac, "K"));
  CHECK(mn::g_peer_count == 3);
  CHECK(A.espnow.has(free_mac) && A.espnow.has(C.mac));
  std::printf("PASS a_new_key_at_another_members_address_is_refused\n");
}

// A's NVS as an older firmware's add_peer could leave it: a member K at
// C's address beside C. Returns K's key.
void two_members_at_cs_address(uint8_t k_pub[32]) {
  fresh_opera();
  new_key(k_pub);
  become(A);
  mn::OperaPeer& k = mn::g_peers[mn::g_peer_count++];
  memset(&k, 0, sizeof k);
  memcpy(k.pubkey, k_pub, 32);
  CHECK(mn::compute_fingerprint(k_pub, k.fingerprint));
  memcpy(k.mac_addr, C.mac, 6);
  strcpy(k.name, "K");
  k.msg_counter_tx = 1;
  CHECK(mn::persist_peers());
  boot(A);
  CHECK(mn::g_peer_count == 3);
  CHECK(A.espnow.has(C.mac));
}

void test_removing_a_member_keeps_an_address_another_member_holds() {
  uint8_t k_pub[32];
  two_members_at_cs_address(k_pub);
  uint8_t fp[mn::FINGERPRINT_SIZE];
  CHECK(mn::compute_fingerprint(k_pub, fp));
  become(A);
  CHECK(mn::remove_peer(fp));
  CHECK(A.espnow.has(C.mac));                   // C still holds it
  CHECK(a_heartbeat_reaches(C.mac));
  // The last holder's removal does release it.
  CHECK(mn::compute_fingerprint(C.pub, fp));
  become(A);
  CHECK(mn::remove_peer(fp));
  CHECK(!A.espnow.has(C.mac));
  std::printf("PASS removing_a_member_keeps_an_address_another_member_holds\n");
}

void test_a_re_pair_away_from_a_shared_address_keeps_it_for_the_other() {
  // C re-pairs from a new address; K, at the address C leaves, keeps its
  // registration. rebind_peer used to delete the old address's.
  uint8_t k_pub[32];
  two_members_at_cs_address(k_pub);
  const uint8_t moved[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xC2};
  become(A);
  CHECK(mn::add_peer(C.pub, moved, "C"));
  CHECK(same_mac(entry(A, C)->mac_addr, moved));
  CHECK(A.espnow.has(moved) && A.espnow.has(C.mac));
  CHECK(a_heartbeat_reaches(C.mac));            // K's frames still go out
  std::printf("PASS a_re_pair_away_from_a_shared_address_keeps_it_for_the_other\n");
}

// ── Only a pairing both owners confirmed binds an address ──────────────
//
// Pairing frames are unsigned (they come before membership), and a
// re-pair re-binds a member this device already holds. So a pairing
// handler that runs ahead of its owner, or that a stranger can restart,
// moves a member as surely as the receive path used to. Outsider X runs
// this firmware on its own radio; where it sends a frame the firmware
// would not, the test builds it with the firmware's own functions.

Device X, J;

// An ACCEPT built against an OVERHEARD offer: X's own ephemeral key, so X
// holds the session key it leads to (written to `session_key`).
Frame accept_from(Device& x, const Frame& offer, const uint8_t to[6], uint8_t session_key[32]) {
  CHECK(offer.size() == 1 + sizeof(mn::PairOfferPayload) && offer[0] == mn::MSG_PAIR_OFFER);
  mn::PairOfferPayload heard;
  memcpy(&heard, offer.data() + 1, sizeof heard);
  become(x);
  uint8_t eph_pub[32], eph_priv[32];
  CHECK(mesh_pair_crypto::generate_keypair(eph_pub, eph_priv, esp_fill_random));
  CHECK(mn::derive_session_key(eph_priv, heard.ephemeral_pubkey, session_key));
  mn::PairOfferPayload acc = {};
  memcpy(acc.ephemeral_pubkey, eph_pub, 32);
  memcpy(acc.device_pubkey, x.pub, 32);
  if (!esp_now_is_peer_exist(to)) {
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, to, 6);
    CHECK(esp_now_add_peer(&p) == ESP_OK);
  }
  x.espnow.sent.clear();
  CHECK(mn::send_pair_frame(to, mn::MSG_PAIR_ACCEPT, &acc, sizeof acc));
  return sent_to(x, to).back();
}

// The joiner's CONFIRM that session key leads to (hash of the key and its
// code, for the joiner's role, F94: the one an initiator takes).
Frame confirm_from(Device& x, const uint8_t session_key[32], const uint8_t to[6]) {
  become(x);
  const uint32_t code = mesh_pair_crypto::confirmation_code(session_key);
  mn::PairConfirmPayload cf;
  mesh_pair_crypto::confirm_hash(mesh_pair_crypto::ConfirmSender::JOINER, session_key, code,
                                 cf.confirmation_hash);
  x.espnow.sent.clear();
  CHECK(mn::send_pair_frame(to, mn::MSG_PAIR_CONFIRM, &cf, sizeof cf));
  return sent_to(x, to).back();
}

// The COMPLETE X's firmware sends only after both CONFIRMs, sent at once:
// the opera_secret X holds, sealed under X's current session key.
Frame early_complete(Device& x, const uint8_t to[6]) {
  become(x);
  mn::PairCompletePayload c;
  uint8_t nonce[mn::NONCE_SIZE], tag[16];
  CHECK(mn::encrypt_message(mn::g_pairing.session_key, mn::g_opera_config.opera_secret,
                            mn::OPERA_SECRET_SIZE, c.encrypted_secret, nonce, tag));
  memcpy(c.nonce, nonce, mn::NONCE_SIZE);
  memcpy(c.encrypted_secret + mn::OPERA_SECRET_SIZE, tag, 16);
  x.espnow.sent.clear();
  CHECK(mn::send_pair_frame(to, mn::MSG_PAIR_COMPLETE, &c, sizeof c));
  return sent_to(x, to).back();
}

void fresh_device(Device& d) {
  d.nvs.clear();
  d.espnow = host_sim::EspNow();
  boot(d);
}

void test_a_joiner_does_not_complete_before_its_owner_confirms() {
  // B, which holds A and C, starts a join (to add a member, or to re-pair).
  // X answers B's DISCOVER presenting A's public key, which every DISCOVER
  // and OFFER of A's carries, and sends COMPLETE straight after B's
  // ACCEPT. B's owner has confirmed nothing, and A's screen shows no code.
  // This used to complete: with the opera_secret X re-bound A to X's radio
  // (a re-pair, since #1761), and without it B's opera was replaced by X's.
  uint8_t x_pub[32];
  memcpy(x_pub, X.pub, sizeof x_pub);
  for (int knows_secret = 0; knows_secret < 2; ++knows_secret) {
    fresh_opera();
    become(A);
    const mn::OperaConfig a_cfg = mn::g_opera_config;
    memcpy(X.pub, A.pub, sizeof X.pub);
    fresh_device(X);
    if (knows_secret) mn::g_opera_config = a_cfg;
    CHECK(mn::start_pairing_initiator(nullptr));
    become(B);
    uint8_t opera_id[mn::OPERA_ID_SIZE];
    memcpy(opera_id, mn::g_opera_config.opera_id, sizeof opera_id);
    CHECK(mn::start_pairing_joiner());
    B.espnow.sent.clear();
    X.espnow.sent.clear();
    host_sim::now_ms += 2001;
    become(B);
    mn::update();
    deliver(X, B.mac, sent_to(B, BROADCAST).back());
    deliver(B, X.mac, sent_to(X, B.mac).back());   // X's OFFER
    deliver(X, B.mac, sent_to(B, X.mac).back());   // B's ACCEPT: both show a code
    become(B);
    CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);
    CHECK(!mn::g_pairing.code_confirmed);
    g_health.clear();
    deliver(B, X.mac, early_complete(X, B.mac));
    become(B);
    CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);   // still waiting on its owner
    CHECK(memcmp(opera_id, mn::g_opera_config.opera_id, sizeof opera_id) == 0);
    CHECK(mn::g_peer_count == 2);
    CHECK(same_mac(entry(B, A)->mac_addr, A.mac));
    CHECK(B.espnow.has(A.mac));
    CHECK(g_health.empty());
    mn::cancel_pairing();                          // B's owner walks away
    become(A);
    A.espnow.sent.clear();
    mn::send_heartbeat();
    become(B);
    const uint32_t received = mn::g_messages_received;
    deliver(B, A.mac, sent_to(A, B.mac).back());
    become(B);
    CHECK(mn::g_messages_received == received + 1);  // B still hears A
  }
  memcpy(X.pub, x_pub, sizeof x_pub);
  std::printf("PASS a_joiner_does_not_complete_before_its_owner_confirms\n");
}

void test_a_joiner_takes_the_first_offer_only() {
  // Once B shows a code, a later OFFER (X answering the DISCOVER it also
  // heard) used to re-key B's pairing to X: a new code, B's CONFIRM sent to
  // X. An owner who compared the first code and then pressed confirm would
  // have confirmed X's.
  fresh_opera();
  fresh_device(X);
  CHECK(mn::start_pairing_initiator(nullptr));
  pair_to_codes(A, B);
  become(B);
  const uint32_t code = mn::g_pairing.confirmation_code;
  const Frame disc = sent_to(B, BROADCAST).back();
  X.espnow.sent.clear();
  deliver(X, B.mac, disc);
  const auto x_offer = sent_to(X, B.mac);
  CHECK(x_offer.size() == 1);
  B.espnow.sent.clear();
  deliver(B, X.mac, x_offer.back());
  become(B);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);
  CHECK(mn::g_pairing.confirmation_code == code);
  CHECK(same_mac(mn::g_pairing.peer_mac, A.mac));
  CHECK(sent_to(B, X.mac).empty());              // no ACCEPT went to X
  pair_confirm(A, B);
  become(B);
  CHECK(mn::g_peer_count == 2);
  CHECK(same_mac(entry(B, A)->mac_addr, A.mac));
  std::printf("PASS a_joiner_takes_the_first_offer_only\n");
}

void test_an_initiator_takes_one_accept_from_where_its_offer_went() {
  // X overheard A's OFFER to B. An ACCEPT from X's own address used to be
  // taken (A showed X's code), and so did a second ACCEPT after A showed
  // a code (a new code under the owner's eyes). The OFFER's address is the
  // joiner's, and the first ACCEPT from it is the one.
  fresh_opera();
  fresh_device(X);
  become(A);
  CHECK(mn::start_pairing_initiator(nullptr));
  become(B);
  CHECK(mn::start_pairing_joiner());
  A.espnow.sent.clear();
  B.espnow.sent.clear();
  host_sim::now_ms += 2001;
  become(B);
  mn::update();
  deliver(A, B.mac, sent_to(B, BROADCAST).back());
  const Frame offer = sent_to(A, B.mac).back();
  uint8_t x_key[32];
  deliver(A, X.mac, accept_from(X, offer, A.mac, x_key));
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_INIT);   // not taken
  deliver(B, A.mac, offer);
  deliver(A, B.mac, sent_to(B, A.mac).back());       // B's own ACCEPT
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);
  const uint32_t code = mn::g_pairing.confirmation_code;
  uint8_t key[mn::SESSION_KEY_SIZE];
  memcpy(key, mn::g_pairing.session_key, sizeof key);
  deliver(A, B.mac, accept_from(X, offer, A.mac, x_key));  // ESP-NOW does not authenticate a source
  become(A);
  CHECK(mn::g_pairing.confirmation_code == code);
  CHECK(memcmp(mn::g_pairing.session_key, key, sizeof key) == 0);
  become(B);
  CHECK(mn::g_pairing.confirmation_code == code);
  pair_confirm(A, B);
  std::printf("PASS an_initiator_takes_one_accept_from_where_its_offer_went\n");
}

void test_an_initiator_forgets_a_finished_pairing() {
  // A pairs a new device J. X overheard A's OFFER to J and, after the
  // pairing, sends A an ACCEPT and the CONFIRM its own session key leads
  // to, from J's address. A used to keep the finished pairing's state (its
  // ephemeral key, its confirmed code) until the 2-minute timeout, take
  // both, and seal the opera_secret under X's session key in a COMPLETE
  // anyone in range can read. No owner did anything.
  fresh_opera();
  fresh_device(X);
  fresh_device(J);
  pair_to_codes(A, J);
  const Frame offer = sent_to(A, J.mac).front();
  pair_confirm(A, J);
  become(A);
  CHECK(mn::g_peer_count == 3);
  uint8_t x_key[32];
  const Frame acc = accept_from(X, offer, A.mac, x_key);
  const Frame cf = confirm_from(X, x_key, A.mac);
  A.espnow.sent.clear();
  deliver(A, J.mac, acc);
  deliver(A, J.mac, cf);
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_ACTIVE);
  CHECK(mn::g_peer_count == 3);
  for (const host_sim::Sent& f : A.espnow.sent) {
    CHECK(f.bytes.empty() || f.bytes[0] != mn::MSG_PAIR_COMPLETE);
  }
  // Nothing of the pairing is left to take: no role, no keys, no code.
  static const uint8_t zero[32] = {};
  CHECK(mn::g_pairing.role == mn::PAIR_ROLE_NONE);
  CHECK(!mn::g_pairing.code_confirmed);
  CHECK(memcmp(mn::g_pairing.ephemeral_privkey, zero, sizeof zero) == 0);
  CHECK(memcmp(mn::g_pairing.session_key, zero, sizeof zero) == 0);
  std::printf("PASS an_initiator_forgets_a_finished_pairing\n");
}

// ── An entry the old re-pair duplicated ─────────────────────────────────
//
// Before #1761, a re-pair with a member this device already held appended
// a second entry for the same key, at the address that pairing came from,
// and persist_peers saved both. Lookups reached only the first; the frame
// re-bind kept it current. Without that re-bind the first entry strands
// the member at the address it left, and the re-pair that should fix it
// is refused, since the duplicate holds the new address.

size_t merge_logs() {
  size_t n = 0;
  for (const std::string& m : g_health) {
    if (m == "opera: folded a duplicate member entry into one") ++n;
  }
  return n;
}

// A's NVS as the old add_peer left it: B's entry, C's, and a second entry
// for B at `dup_mac`, with the last-seen counters saved for each.
void seed_duplicate(const uint8_t dup_mac[6]) {
  become(A);
  mn::OperaPeer dup = *entry(A, B);
  memcpy(dup.mac_addr, dup_mac, 6);
  strcpy(dup.name, "New Device");
  dup.msg_counter_rx = 0;
  become(A);
  mn::g_peers[mn::g_peer_count++] = dup;
  CHECK(mn::g_peer_count == 3);
  CHECK(mn::persist_peers());
  CHECK(mn::save_replay_counters());
}

void test_an_old_duplicate_entry_folds_into_one_at_boot() {
  fresh_opera();
  deliver(A, B.mac, b_heartbeat_to(A));          // A's last-seen for B: 1
  uint8_t old_mac[6];
  memcpy(old_mac, B.mac, 6);
  const uint8_t new_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xB2};
  seed_duplicate(new_mac);
  g_health.clear();
  boot(A);
  CHECK(mn::g_peer_count == 2);
  mn::OperaPeer* pb = entry(A, B);
  CHECK(same_mac(pb->mac_addr, new_mac));        // the later pairing's address
  CHECK(strcmp(pb->name, "B") == 0);             // the first entry's name
  CHECK(pb->msg_counter_rx == 1);                // and its last-seen counter
  CHECK(same_mac(entry(A, C)->mac_addr, C.mac));
  CHECK(A.espnow.has(new_mac) && !A.espnow.has(old_mac) && A.espnow.has(C.mac));
  CHECK(merge_logs() == 1);
  become(A);                                     // the fold is saved
  mn::g_prefs.begin(mn::NVS_NS, true);
  CHECK(mn::g_prefs.getUChar(mn::NVS_PEER_COUNT, 0) == 2);
  mn::g_prefs.end();
  // ... and the slot it freed goes (sweep F137): peer_2 held the duplicate,
  // B's key under the later pairing's address and name.
  CHECK(A.nvs.count("mesh/peer_0") == 1 && A.nvs.count("mesh/peer_1") == 1);
  CHECK(A.nvs.count("mesh/peer_2") == 0);
  boot(A);                                       // so the next boot has nothing to fold
  CHECK(mn::g_peer_count == 2);
  CHECK(same_mac(entry(A, B)->mac_addr, new_mac));
  CHECK(merge_logs() == 1);
  memcpy(B.mac, new_mac, 6);                     // B is heard at its new address
  deliver(A, B.mac, b_heartbeat_to(A));
  CHECK(entry(A, B)->msg_counter_rx == 2);
  memcpy(B.mac, old_mac, 6);
  std::printf("PASS an_old_duplicate_entry_folds_into_one_at_boot\n");
}

void test_an_old_duplicate_at_another_members_address_is_dropped() {
  // One address, one member: a duplicate whose address C holds is dropped,
  // and B keeps the address it had.
  fresh_opera();
  seed_duplicate(C.mac);
  g_health.clear();
  boot(A);
  CHECK(mn::g_peer_count == 2);
  CHECK(same_mac(entry(A, B)->mac_addr, B.mac));
  CHECK(same_mac(entry(A, C)->mac_addr, C.mac));
  CHECK(A.espnow.has(B.mac) && A.espnow.has(C.mac));
  CHECK(merge_logs() == 1);
  CHECK(a_heartbeat_reaches(B.mac) && a_heartbeat_reaches(C.mac));
  std::printf("PASS an_old_duplicate_at_another_members_address_is_dropped\n");
}

// ── A SHA-256 step that answers an error ────────────────────────────────

// Nothing is signed over a hash that failed and nothing verified against
// one: no frame goes out, a frame heard is dropped, and neither side of the
// AUTH exchange opens a session. A failed finish leaves the true digest
// behind, so a caller that dropped the answer would pass here.
void test_a_refused_hash_signs_and_verifies_nothing() {
  using stub_sha256::Out;
  using stub_sha256::Step;
  const struct { Step step; int update; } fails[] = {
      {Step::Starts, 0}, {Step::Update, 0}, {Step::Update, 1}, {Step::Finish, 0}};
  fresh_opera();

  // Verify: B's heartbeat at A.
  const Frame hb = b_heartbeat_to(A);
  const BView before = a_view_of_b();
  for (const auto& f : fails) {
    stub_sha256::arm(f.step, 0, f.update, Out::Digest);
    deliver(A, B.mac, hb);
    CHECK(stub_sha256::fired && stub_sha256::live == 0);
    stub_sha256::disarm();
    const BView after = a_view_of_b();
    CHECK(after.rx == before.rx && after.received == before.received);
  }
  deliver(A, B.mac, hb);
  CHECK(a_view_of_b().rx == counter_of(hb));

  // Sign: A's frame to B.
  mn::OperaPeer* b_at_a = entry(A, B);
  for (const auto& f : fails) {
    A.espnow.sent.clear();
    stub_sha256::arm(f.step, 0, f.update, Out::Digest);
    CHECK(!mn::send_to_peer(b_at_a, mn::MSG_HEARTBEAT, nullptr, 0));
    CHECK(stub_sha256::fired && stub_sha256::live == 0);
    CHECK(A.espnow.sent.empty());
  }
  stub_sha256::disarm();
  CHECK(mn::send_to_peer(b_at_a, mn::MSG_HEARTBEAT, nullptr, 0));

  // AUTH_CHALLENGE at B: the frame's verify is hash 0, the challenge's
  // hash 1, the membership proof's hash 2.
  mn::AuthChallengePayload ch;
  host_sim::fill_random(ch.nonce, sizeof ch.nonce);
  memcpy(ch.pubkey, A.pub, 32);
  for (int hash = 1; hash <= 2; ++hash) {
    for (const auto& f : fails) {
      become(A);
      host_sim::now_ms += 20;
      CHECK(mn::send_to_peer(b_at_a, mn::MSG_AUTH_CHALLENGE,
                             reinterpret_cast<const uint8_t*>(&ch), sizeof ch));
      const Frame challenge = sent_to(A, B.mac).back();
      mn::OperaPeer* a_at_b = entry(B, A);
      stub_sha256::arm(f.step, hash, f.update, Out::Digest);
      deliver(B, A.mac, challenge);
      CHECK(stub_sha256::fired && stub_sha256::live == 0);
      stub_sha256::disarm();
      CHECK(!a_at_b->session_established);
    }
  }

  // AUTH_RESPONSE at A (handed in: the frame is over ESP-NOW's 250 bytes).
  mn::AuthResponsePayload resp;
  memset(&resp, 0, sizeof resp);
  memcpy(resp.pubkey, B.pub, 32);
  become(A);
  uint8_t proof_hash[32];
  CHECK(mn::sha256_domain(mn::DOMAIN_AUTH, mn::g_opera_config.opera_id, mn::OPERA_ID_SIZE, proof_hash));
  Ed25519::sign(resp.opera_proof, B.priv, B.pub, proof_hash, 32);
  for (const auto& f : fails) {
    stub_sha256::arm(f.step, 0, f.update, Out::Digest);
    mn::handle_auth_response(b_at_a, reinterpret_cast<const uint8_t*>(&resp), sizeof resp);
    CHECK(stub_sha256::fired && stub_sha256::live == 0);
    CHECK(!b_at_a->session_established);
  }
  stub_sha256::disarm();
  mn::handle_auth_response(b_at_a, reinterpret_cast<const uint8_t*>(&resp), sizeof resp);
  CHECK(b_at_a->session_established);

  // The helper itself: false and zeros, the context freed.
  for (const auto& f : fails) {
    uint8_t out[32];
    memset(out, 0x5C, sizeof out);
    stub_sha256::arm(f.step, 0, f.update, Out::Digest);
    CHECK(!mn::sha256_domain(mn::DOMAIN_AUTH, A.pub, 32, out));
    CHECK(stub_sha256::fired && stub_sha256::live == 0);
    for (uint8_t byte : out) CHECK(byte == 0);
  }
  stub_sha256::disarm();
  std::printf("PASS a_refused_hash_signs_and_verifies_nothing\n");
}

// ── A fingerprint or opera id that does not hash (sweep F364) ───────────
//
// compute_fingerprint() and compute_opera_id() zero their output when the
// hash fails, and they used to hand the zeros on as a fingerprint or an
// opera id: a member added, loaded or joined under one, an opera created or
// rotated to one, and a deny-list that answered "not removed" for a key it
// could not hash. Each caller now refuses and leaves nothing half done.
// Each test fails every step of the helper's hash (its start, either update,
// its finish; a failed finish leaving the true digest) at the call it
// names, counted from the knob's arm(), then runs the same path with the
// knob off: a hash that works is never refused. Each fails on the code
// before the change (the commit message has the run).

using stub_sha256::Out;
using stub_sha256::Step;
struct HashFail { Step step; int update; };
const HashFail kHashFails[] = {
    {Step::Starts, 0}, {Step::Update, 0}, {Step::Update, 1}, {Step::Finish, 0}};

size_t logged(const char* line) {
  size_t n = 0;
  for (const std::string& m : g_health) n += m == line ? 1 : 0;
  return n;
}

bool all_zero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) if (p[i] != 0) return false;
  return true;
}

// No member `self` holds has a zeroed fingerprint.
bool no_zero_fingerprint(Device& self) {
  become(self);
  for (uint8_t i = 0; i < mn::g_peer_count; ++i) {
    if (all_zero(mn::g_peers[i].fingerprint, mn::FINGERPRINT_SIZE)) return false;
  }
  return true;
}

// Any frame of `type` among `d`'s sends.
bool sent_type(const Device& d, uint8_t type) {
  for (const host_sim::Sent& s : d.espnow.sent) {
    if (s.bytes.size() >= 2 && s.bytes[1] == type) return true;
  }
  return false;
}

const char kNotAdded[] = "opera: a member's key did not hash; not added";

void test_a_key_that_does_not_hash_is_not_added() {
  // add_peer: hash 0 is its deny-list check (is_revoked_pubkey), hash 1
  // its own fingerprint. Either one failing refuses the key, and the slot
  // past the last member, the ESP-NOW list and NVS are as they were.
  fresh_opera();
  fresh_device(J);
  for (int hash = 0; hash <= 1; ++hash) {
    for (const HashFail& f : kHashFails) {
      become(A);
      const uint8_t count = mn::g_peer_count;
      mn::OperaPeer slots[mn::MAX_OPERA_SIZE];
      memcpy(slots, mn::g_peers, sizeof slots);
      const auto espnow_peers = A.espnow.peers;
      const host_sim::NvsStore nvs = A.nvs;
      g_health.clear();
      stub_sha256::arm(f.step, hash, f.update, Out::Digest);
      CHECK(!mn::add_peer(J.pub, J.mac, "J"));
      CHECK(stub_sha256::fired && stub_sha256::live == 0);
      stub_sha256::disarm();
      CHECK(mn::g_peer_count == count);
      CHECK(memcmp(slots, mn::g_peers, sizeof slots) == 0);
      CHECK(A.espnow.peers == espnow_peers && !A.espnow.has(J.mac));
      CHECK(A.nvs == nvs);
      CHECK(logged(kNotAdded) == (hash == 1 ? 1u : 0u));  // the deny-list check is silent
    }
  }
  become(A);
  stub_sha256::arm();                             // count, fail nothing
  CHECK(mn::add_peer(J.pub, J.mac, "J"));
  CHECK(stub_sha256::hashes == 2);                // the two hashes above
  stub_sha256::disarm();
  CHECK(mn::g_peer_count == 3 && A.espnow.has(J.mac));
  CHECK(entry(A, J) != nullptr && no_zero_fingerprint(A));
  std::printf("PASS a_key_that_does_not_hash_is_not_added\n");
}

const char kNotLoaded[] = "opera: a stored member's key did not hash; not loaded this boot";

void test_a_stored_member_whose_key_does_not_hash_is_not_loaded() {
  // A boot's hashes: hash 0 is the device's own key (init), then one per
  // stored member in slot order (load_peers): B's slot 0 is hash 1, C's
  // slot 1 hash 2. The member is not loaded, nor registered with ESP-NOW,
  // and nothing is saved over its slot: a slot above the count, which a
  // boot that loads every slot removes (F137), stays for that boot.
  for (int hash = 1; hash <= 2; ++hash) {
    for (const HashFail& f : kHashFails) {
      fresh_opera();
      become(A);
      A.nvs["mesh/peer_2"] = A.nvs.at("mesh/peer_1");   // an older firmware's left slot
      const host_sim::NvsStore nvs = A.nvs;
      const Device& skipped = hash == 1 ? B : C;
      const Device& loaded = hash == 1 ? C : B;
      g_health.clear();
      stub_sha256::arm(f.step, hash, f.update, Out::Digest);
      boot(A);
      CHECK(stub_sha256::fired && stub_sha256::live == 0);
      stub_sha256::disarm();
      become(A);
      CHECK(mn::g_initialized);
      CHECK(mn::g_peer_count == 1);
      CHECK(memcmp(mn::g_peers[0].pubkey, loaded.pub, 32) == 0);
      CHECK(no_zero_fingerprint(A));
      CHECK(entry(A, skipped) == nullptr && entry(A, loaded) != nullptr);
      CHECK(!A.espnow.has(skipped.mac) && A.espnow.has(loaded.mac));
      CHECK(A.nvs == nvs);                          // nothing saved over it
      CHECK(logged(kNotLoaded) == 1);
    }
  }
  // The next boot that hashes loads both, and only then tidies the slot.
  g_health.clear();
  boot(A);
  CHECK(mn::g_peer_count == 2 && entry(A, B) != nullptr && entry(A, C) != nullptr);
  CHECK(A.espnow.has(B.mac) && A.espnow.has(C.mac));
  CHECK(A.nvs.count("mesh/peer_2") == 0);
  CHECK(logged(kNotLoaded) == 0);
  std::printf("PASS a_stored_member_whose_key_does_not_hash_is_not_loaded\n");
}

const char kNotStarted[] = "mesh: this device's key did not hash; mesh not started";

void test_a_device_whose_own_key_does_not_hash_starts_no_mesh() {
  // init(): hash 0 is the device's own fingerprint. The mesh does not
  // start: no ESP-NOW, no member loaded, nothing written.
  for (const HashFail& f : kHashFails) {
    fresh_opera();
    const host_sim::NvsStore nvs = A.nvs;
    g_health.clear();
    stub_sha256::arm(f.step, 0, f.update, Out::Digest);
    boot(A);
    CHECK(stub_sha256::fired && stub_sha256::live == 0);
    stub_sha256::disarm();
    become(A);
    CHECK(!mn::g_initialized && !mn::g_espnow_initialized);
    CHECK(mn::g_mesh_state == mn::MESH_ERROR);
    CHECK(!A.espnow.inited && A.espnow.recv_cb == nullptr && A.espnow.peers.empty());
    CHECK(mn::g_peer_count == 0);
    CHECK(all_zero(mn::g_device_fingerprint, mn::FINGERPRINT_SIZE));
    uint8_t self[mn::FINGERPRINT_SIZE];
    CHECK(!mn::get_self_fingerprint(self));
    CHECK(A.nvs == nvs);
    CHECK(logged(kNotStarted) == 1);
  }
  g_health.clear();
  boot(A);
  CHECK(mn::g_initialized && mn::g_mesh_state == mn::MESH_CONNECTING);
  CHECK(mn::g_peer_count == 2);
  uint8_t self[mn::FINGERPRINT_SIZE], want[mn::FINGERPRINT_SIZE];
  CHECK(mn::get_self_fingerprint(self));
  CHECK(mn::compute_fingerprint(A.pub, want));
  CHECK(memcmp(self, want, sizeof self) == 0);
  CHECK(logged(kNotStarted) == 0);
  std::printf("PASS a_device_whose_own_key_does_not_hash_starts_no_mesh\n");
}

// A and B hold one session key (nothing opens one on canary-wap, F95: the
// liveness suite's rotation test stands in for one the same way), so a
// removal's rotation reaches B.
void share_a_session(Device& x, Device& y) {
  uint8_t key[mn::SESSION_KEY_SIZE];
  host_sim::fill_random(key, sizeof key);
  for (const auto& side : {std::make_pair(&x, &y), std::make_pair(&y, &x)}) {
    mn::OperaPeer* p = entry(*side.first, *side.second);
    memcpy(p->session_key, key, sizeof key);
    p->session_established = true;
  }
}

// A removes `gone`; the REKEY A sent B, if any.
Frame remove_at_a(const Device& gone) {
  uint8_t fp[mn::FINGERPRINT_SIZE];
  CHECK(mn::compute_fingerprint(gone.pub, fp));
  become(A);
  A.espnow.sent.clear();
  CHECK(mn::remove_peer(fp));
  for (const Frame& f : sent_to(A, B.mac)) {
    if (f.size() >= 2 && f[1] == mn::MSG_OPERA_REKEY) return f;
  }
  return Frame();
}

const char kRekeyRefused[] = "opera: rekey refused: the new opera id did not hash (no ACK sent)";

void test_a_rekey_whose_opera_id_does_not_hash_is_refused_before_its_ack() {
  // B's REKEY receive: hash 0 verifies the frame, hash 1 is the new
  // opera_id, hash 2 signs the ACK. The id now comes before the ACK, and a
  // failed one sends no ACK and installs nothing: B keeps the opera it had,
  // in RAM and NVS, and its session. (It used to ACK first and then install
  // the new secret under a zeroed opera_id; here, on that code, hash 1 was
  // the ACK's signature and the rekey went in.)
  for (const HashFail& f : kHashFails) {
    fresh_opera();
    share_a_session(A, B);
    const Frame rekey = remove_at_a(C);
    CHECK(!rekey.empty());
    become(B);
    const mn::OperaConfig cfg = mn::g_opera_config;
    const host_sim::NvsStore nvs = B.nvs;
    B.espnow.sent.clear();
    g_health.clear();
    stub_sha256::arm(f.step, 1, f.update, Out::Digest);
    deliver(B, A.mac, rekey);
    CHECK(stub_sha256::fired && stub_sha256::live == 0);
    stub_sha256::disarm();
    become(B);
    CHECK(!sent_type(B, mn::MSG_OPERA_REKEY_ACK));
    CHECK(memcmp(&mn::g_opera_config, &cfg, sizeof cfg) == 0);
    CHECK(B.nvs == nvs);
    CHECK(entry(B, A)->session_established);
    CHECK(entry(B, A)->msg_counter_rx == counter_of(rekey));   // the frame itself was heard
    CHECK(logged(kRekeyRefused) == 1);
  }
  // With the hash working: the ACK, then the new id, A's candidate.
  fresh_opera();
  share_a_session(A, B);
  const Frame rekey = remove_at_a(C);
  become(A);
  std::vector<uint8_t> pending(mn::g_rekey.pending_opera_id,
                               mn::g_rekey.pending_opera_id + mn::OPERA_ID_SIZE);
  B.espnow.sent.clear();
  stub_sha256::arm();
  deliver(B, A.mac, rekey);
  CHECK(stub_sha256::hashes >= 3);                // verify, the id, the ACK (and update()'s
                                                  // heartbeat, when one is due)
  stub_sha256::disarm();
  become(B);
  CHECK(sent_type(B, mn::MSG_OPERA_REKEY_ACK));
  CHECK(memcmp(mn::g_opera_config.opera_id, pending.data(), mn::OPERA_ID_SIZE) == 0);
  CHECK(!all_zero(mn::g_opera_config.opera_id, mn::OPERA_ID_SIZE));
  std::printf("PASS a_rekey_whose_opera_id_does_not_hash_is_refused_before_its_ack\n");
}

const char kRekeyNotStarted[] = "opera: rekey not started: the new opera id did not hash";

void test_a_rotation_whose_opera_id_does_not_hash_does_not_start() {
  // remove_peer's rotation: hash 0 is the candidate opera_id (the removal
  // before it hashes nothing), hash 1 signs the REKEY to B. A failed id
  // starts no rotation: no REKEY sent, g_rekey as it was (one already in
  // flight carries on), and A's opera unchanged, then and at the timeout.
  // The removal itself stands.
  for (int in_flight = 0; in_flight < 2; ++in_flight) {
    for (const HashFail& f : kHashFails) {
      fresh_opera();
      share_a_session(A, B);
      if (in_flight) {
        become(A);
        CHECK(mn::add_peer(J.pub, J.mac, "J"));
        CHECK(!remove_at_a(J).empty());           // a rotation in flight
      }
      become(A);
      const mn::RekeyState rekey = mn::g_rekey;
      const mn::OperaConfig cfg = mn::g_opera_config;
      CHECK(rekey.active == (in_flight != 0));
      uint8_t fp[mn::FINGERPRINT_SIZE];
      memcpy(fp, entry(A, C)->fingerprint, sizeof fp);
      become(A);
      A.espnow.sent.clear();
      g_health.clear();
      stub_sha256::arm(f.step, 0, f.update, Out::Digest);
      CHECK(mn::remove_peer(fp));
      CHECK(stub_sha256::fired && stub_sha256::live == 0);
      stub_sha256::disarm();
      CHECK(!sent_type(A, mn::MSG_OPERA_REKEY));
      CHECK(memcmp(&mn::g_rekey, &rekey, sizeof rekey) == 0);
      CHECK(memcmp(&mn::g_opera_config, &cfg, sizeof cfg) == 0);
      CHECK(entry(A, C) == nullptr && mn::g_peer_count == 1);   // removed
      become(A);
      CHECK(mn::is_revoked_pubkey(C.pub));                      // and deny-listed
      CHECK(logged(kRekeyNotStarted) == 1);
      if (!in_flight) {
        host_sim::now_ms += 61000;                // past REKEY_TIMEOUT_MS
        mn::update();
        CHECK(memcmp(mn::g_opera_config.opera_id, cfg.opera_id, mn::OPERA_ID_SIZE) == 0);
      }
    }
  }
  // With the hash working: a rotation to a real id, sent to B.
  fresh_opera();
  share_a_session(A, B);
  stub_sha256::arm();
  CHECK(!remove_at_a(C).empty());
  CHECK(stub_sha256::hashes == 3);                // C's fingerprint here, the id, the REKEY
  stub_sha256::disarm();
  become(A);
  CHECK(mn::g_rekey.active && !all_zero(mn::g_rekey.pending_opera_id, mn::OPERA_ID_SIZE));
  uint8_t want[mn::OPERA_ID_SIZE];
  CHECK(mn::compute_opera_id(mn::g_rekey.pending_secret, want));
  CHECK(memcmp(want, mn::g_rekey.pending_opera_id, sizeof want) == 0);
  std::printf("PASS a_rotation_whose_opera_id_does_not_hash_does_not_start\n");
}

const char kNotCreated[] = "opera: not created: its opera id did not hash";

void test_an_opera_whose_id_does_not_hash_is_not_created() {
  // start_pairing_initiator() on a device with no opera: hash 0 is the new
  // opera's id. Refused, nothing changes: no opera, no pairing, no NVS.
  for (const HashFail& f : kHashFails) {
    fresh_device(J);
    become(J);
    const mn::OperaConfig cfg = mn::g_opera_config;
    const mn::PairingSession pairing = mn::g_pairing;
    const mn::MeshState state = mn::g_mesh_state;
    const host_sim::NvsStore nvs = J.nvs;
    CHECK(!cfg.configured);
    g_health.clear();
    stub_sha256::arm(f.step, 0, f.update, Out::Digest);
    CHECK(!mn::start_pairing_initiator(nullptr));
    CHECK(stub_sha256::fired && stub_sha256::live == 0);
    stub_sha256::disarm();
    CHECK(memcmp(&mn::g_opera_config, &cfg, sizeof cfg) == 0);
    CHECK(memcmp(&mn::g_pairing, &pairing, sizeof pairing) == 0);
    CHECK(mn::g_mesh_state == state);
    CHECK(J.nvs == nvs);
    CHECK(logged(kNotCreated) == 1);
  }
  become(J);
  stub_sha256::arm();
  CHECK(mn::start_pairing_initiator(nullptr));
  CHECK(stub_sha256::hashes == 1);
  stub_sha256::disarm();
  CHECK(mn::g_opera_config.configured && mn::g_mesh_state == mn::MESH_PAIRING_INIT);
  uint8_t want[mn::OPERA_ID_SIZE];
  CHECK(mn::compute_opera_id(mn::g_opera_config.opera_secret, want));
  CHECK(memcmp(want, mn::g_opera_config.opera_id, sizeof want) == 0);
  mn::cancel_pairing();
  std::printf("PASS an_opera_whose_id_does_not_hash_is_not_created\n");
}

const char kJoinRefused[] = "opera: pairing refused: the opera id did not hash";

// A pairs new device J up to J's COMPLETE, which it returns undelivered.
Frame pair_up_to_complete() {
  fresh_opera();
  fresh_device(J);
  pair_to_codes(A, J);
  become(A);
  CHECK(mn::confirm_pairing());
  become(J);
  CHECK(mn::confirm_pairing());
  const Frame confirm_ini = sent_to(A, J.mac).back();
  const Frame confirm_joi = sent_to(J, A.mac).back();
  deliver(A, J.mac, confirm_joi);                 // A adds J
  Frame complete;                                 // (A's first heartbeat to J follows it)
  for (const Frame& fr : sent_to(A, J.mac)) {
    if (!fr.empty() && fr[0] == mn::MSG_PAIR_COMPLETE) complete = fr;
  }
  CHECK(!complete.empty());
  deliver(J, A.mac, confirm_ini);
  return complete;
}

void test_a_join_whose_opera_id_or_partner_does_not_hash_fails() {
  // J's COMPLETE (pair frames carry no signature): hash 0 is the opera_id
  // it joins, hash 1 add_peer's deny-list check on A's key, hash 2 A's
  // fingerprint. Any of them failing ends the pairing with J as it was
  // before it: no opera, no member, nothing stored.
  for (int hash = 0; hash <= 2; ++hash) {
    for (const HashFail& f : kHashFails) {
      const Frame complete = pair_up_to_complete();
      become(J);
      const mn::OperaConfig cfg = mn::g_opera_config;
      const host_sim::NvsStore nvs = J.nvs;
      const auto espnow_peers = J.espnow.peers;
      CHECK(!cfg.configured);
      g_health.clear();
      stub_sha256::arm(f.step, hash, f.update, Out::Digest);
      deliver(J, A.mac, complete);
      CHECK(stub_sha256::fired && stub_sha256::live == 0);
      stub_sha256::disarm();
      become(J);
      CHECK(memcmp(&mn::g_opera_config, &cfg, sizeof cfg) == 0);
      CHECK(mn::g_peer_count == 0);
      CHECK(J.nvs == nvs);
      CHECK(J.espnow.peers == espnow_peers);
      CHECK(mn::g_mesh_state == mn::MESH_NO_OPERA);
      CHECK(mn::g_pairing.role == mn::PAIR_ROLE_NONE);
      CHECK(logged(kJoinRefused) == (hash == 0 ? 1u : 0u));
      CHECK(logged(kNotAdded) == (hash == 2 ? 1u : 0u));
    }
  }
  const Frame complete = pair_up_to_complete();
  stub_sha256::arm();
  deliver(J, A.mac, complete);
  CHECK(stub_sha256::hashes >= 3);                // the three above (and update()'s heartbeat,
                                                  // when one is due: F76)
  stub_sha256::disarm();
  become(A);
  const std::vector<uint8_t> a_id(mn::g_opera_config.opera_id,
                                  mn::g_opera_config.opera_id + mn::OPERA_ID_SIZE);
  become(J);
  CHECK(mn::g_mesh_state == mn::MESH_ACTIVE && mn::g_opera_config.configured);
  CHECK(memcmp(mn::g_opera_config.opera_id, a_id.data(), mn::OPERA_ID_SIZE) == 0);
  CHECK(entry(J, A) != nullptr && no_zero_fingerprint(J));
  std::printf("PASS a_join_whose_opera_id_or_partner_does_not_hash_fails\n");
}

void test_a_key_that_does_not_hash_counts_as_removed() {
  // is_revoked_pubkey() fails closed: a key it cannot hash counts as
  // removed, where its zeroed fingerprint was on no list and counted as
  // not removed. So a DISCOVER from it gets no OFFER, and an OFFER from it
  // no ACCEPT (hash 0 at each: pair frames carry no signature).
  fresh_opera();
  fresh_device(J);
  become(A);
  for (const HashFail& f : kHashFails) {
    stub_sha256::arm(f.step, 0, f.update, Out::Digest);
    CHECK(mn::is_revoked_pubkey(J.pub));
    CHECK(stub_sha256::fired && stub_sha256::live == 0);
    stub_sha256::disarm();
  }
  CHECK(!mn::is_revoked_pubkey(J.pub));
  CHECK(!mn::is_revoked_pubkey(B.pub));
  // The DISCOVER, at A.
  become(A);
  CHECK(mn::start_pairing_initiator(nullptr));
  become(J);
  CHECK(mn::start_pairing_joiner());
  J.espnow.sent.clear();
  host_sim::now_ms += 2001;
  mn::update();
  const Frame disc = sent_to(J, BROADCAST).back();
  for (const HashFail& f : kHashFails) {
    A.espnow.sent.clear();
    stub_sha256::arm(f.step, 0, f.update, Out::Digest);
    deliver(A, J.mac, disc);
    CHECK(stub_sha256::fired && stub_sha256::live == 0);
    stub_sha256::disarm();
    CHECK(sent_to(A, J.mac).empty());
    become(A);
    CHECK(mn::g_mesh_state == mn::MESH_PAIRING_INIT);
  }
  A.espnow.sent.clear();
  deliver(A, J.mac, disc);
  const auto offer = sent_to(A, J.mac);
  CHECK(offer.size() == 1);
  // The OFFER, at J.
  for (const HashFail& f : kHashFails) {
    J.espnow.sent.clear();
    stub_sha256::arm(f.step, 0, f.update, Out::Digest);
    deliver(J, A.mac, offer.back());
    CHECK(stub_sha256::fired && stub_sha256::live == 0);
    stub_sha256::disarm();
    CHECK(sent_to(J, A.mac).empty());
    become(J);
    CHECK(mn::g_mesh_state == mn::MESH_PAIRING_JOIN && all_zero(mn::g_pairing.peer_pubkey, 32));
  }
  J.espnow.sent.clear();
  deliver(J, A.mac, offer.back());
  CHECK(sent_to(J, A.mac).size() == 1);           // the ACCEPT
  become(J);
  mn::cancel_pairing();
  become(A);
  mn::cancel_pairing();
  std::printf("PASS a_key_that_does_not_hash_counts_as_removed\n");
}

}  // namespace

int main() {
  make_device(A, "A", 0xA1);
  make_device(B, "B", 0xB1);
  make_device(C, "C", 0xC1);
  make_device(X, "X", 0xEE);
  make_device(J, "J", 0xD1);
  test_a_frame_from_the_bound_address_is_heard();
  test_a_forged_frame_from_another_address_moves_nothing();
  test_a_frame_already_heard_moves_nothing();
  test_a_missed_frame_from_another_address_moves_nothing();
  test_a_missed_frame_from_another_address_is_not_acted_on();
  test_another_members_frame_from_another_address_moves_nothing();
  test_a_frame_heard_before_a_power_cut_moves_nothing();
  test_a_frame_from_another_members_address_moves_nothing();
  test_a_member_whose_address_changed_is_not_heard_until_re_paired();
  test_a_re_pair_re_binds_the_member_it_holds();
  test_a_re_pair_the_radio_cannot_register_moves_nothing();
  test_a_re_pair_is_not_refused_by_a_full_opera();
  test_a_re_pair_cannot_take_another_members_address();
  test_a_new_key_at_another_members_address_is_refused();
  test_removing_a_member_keeps_an_address_another_member_holds();
  test_a_re_pair_away_from_a_shared_address_keeps_it_for_the_other();
  test_a_joiner_does_not_complete_before_its_owner_confirms();
  test_a_joiner_takes_the_first_offer_only();
  test_an_initiator_takes_one_accept_from_where_its_offer_went();
  test_an_initiator_forgets_a_finished_pairing();
  test_an_old_duplicate_entry_folds_into_one_at_boot();
  test_an_old_duplicate_at_another_members_address_is_dropped();
  test_a_refused_hash_signs_and_verifies_nothing();
  test_a_key_that_does_not_hash_is_not_added();
  test_a_stored_member_whose_key_does_not_hash_is_not_loaded();
  test_a_device_whose_own_key_does_not_hash_starts_no_mesh();
  test_a_rekey_whose_opera_id_does_not_hash_is_refused_before_its_ack();
  test_a_rotation_whose_opera_id_does_not_hash_does_not_start();
  test_an_opera_whose_id_does_not_hash_is_not_created();
  test_a_join_whose_opera_id_or_partner_does_not_hash_fails();
  test_a_key_that_does_not_hash_counts_as_removed();
  std::printf("ALL %d mesh address checks PASSED\n", g_checks);
  return 0;
}
