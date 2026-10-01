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
#pragma GCC diagnostic ignored "-Wstringop-truncation"
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
  // broadcast_message skips a member in PEER_UNKNOWN or
  // PEER_AUTHENTICATING (one paired after B booted, say). That is set up
  // directly here: three heartbeats with A at PEER_UNKNOWN in B's table.
  fresh_opera();
  deliver(A, B.mac, b_heartbeat_to(A));          // A's last-seen for B: 1
  {
    become(B);
    mn::OperaPeer* pa = entry(B, A);
    const mn::PeerState keep = pa->state;
    pa->state = mn::PEER_UNKNOWN;
    for (int i = 0; i < 3; ++i) mn::send_heartbeat();
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

// The CONFIRM that session key leads to (hash of the key and its code).
Frame confirm_from(Device& x, const uint8_t session_key[32], const uint8_t to[6]) {
  become(x);
  const uint32_t code = mesh_pair_crypto::confirmation_code(session_key);
  uint8_t in[mn::SESSION_KEY_SIZE + 4];
  memcpy(in, session_key, mn::SESSION_KEY_SIZE);
  memcpy(in + mn::SESSION_KEY_SIZE, &code, 4);
  mn::PairConfirmPayload cf;
  mn::sha256_domain(mn::DOMAIN_PAIR_CONFIRM, in, sizeof in, cf.confirmation_hash);
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
  test_a_joiner_does_not_complete_before_its_owner_confirms();
  test_a_joiner_takes_the_first_offer_only();
  test_an_initiator_takes_one_accept_from_where_its_offer_went();
  test_an_initiator_forgets_a_finished_pairing();
  test_an_old_duplicate_entry_folds_into_one_at_boot();
  test_an_old_duplicate_at_another_members_address_is_dropped();
  std::printf("ALL %d mesh address checks PASSED\n", g_checks);
  return 0;
}
