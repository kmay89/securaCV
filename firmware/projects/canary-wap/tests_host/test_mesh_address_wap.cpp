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
#include <vector>

// The sketch provides these on a device (canary_wap.ino, chirp_channel.cpp).
void health_log(LogLevel, LogCategory, const char*) {}
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


// ── A re-pair re-binds the member it already holds ──────────────────────

const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// A WAP-to-WAP pairing through the real handlers, every frame carried as
// bytes from one device's send log to the other's receive path: the
// joiner's DISCOVER (sent by update() on its 2 s tick), the initiator's
// OFFER, the joiner's ACCEPT, both CONFIRMs once each owner has accepted
// the code, and the initiator's COMPLETE.
void run_pairing(Device& ini, Device& joi) {
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
  const uint32_t code_ini = mn::g_pairing.confirmation_code;
  become(joi);
  const uint32_t code_joi = mn::g_pairing.confirmation_code;
  CHECK(code_ini == code_joi);                   // the two screens agree
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

void test_a_re_pair_re_binds_the_member_it_holds() {
  fresh_opera();
  uint8_t old_mac[6];
  memcpy(old_mac, B.mac, 6);
  B.mac[5] = 0xB2;                               // B's radio address changed
  boot(B);
  run_pairing(A, B);
  // A holds B once, at the address the pairing completed from, and the
  // ESP-NOW list follows; B holds A once.
  become(A);
  CHECK(mn::g_peer_count == 2);
  mn::OperaPeer* pb = entry(A, B);
  CHECK(same_mac(pb->mac_addr, B.mac));
  CHECK(pb->msg_counter_rx == 0);                // the re-pair kept the counter
  CHECK(A.espnow.has(B.mac) && !A.espnow.has(old_mac));
  CHECK(same_mac(entry(A, C)->mac_addr, C.mac));
  become(B);
  CHECK(mn::g_peer_count == 2);
  CHECK(same_mac(entry(B, A)->mac_addr, A.mac));
  // B is heard from its new address, and A's frames go there.
  deliver(A, B.mac, b_heartbeat_to(A));
  CHECK(entry(A, B)->msg_counter_rx == 1);
  CHECK(a_heartbeat_reaches(B.mac));
  CHECK(!a_heartbeat_reaches(old_mac));
  // The move is in A's NVS: a reboot binds the new address.
  boot(A);
  CHECK(mn::g_peer_count == 2);
  CHECK(same_mac(entry(A, B)->mac_addr, B.mac));
  CHECK(A.espnow.has(B.mac) && !A.espnow.has(old_mac));
  memcpy(B.mac, old_mac, 6);
  std::printf("PASS a_re_pair_re_binds_the_member_it_holds\n");
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

}  // namespace

int main() {
  make_device(A, "A", 0xA1);
  make_device(B, "B", 0xB1);
  make_device(C, "C", 0xC1);
  test_a_frame_from_the_bound_address_is_heard();
  test_a_forged_frame_from_another_address_moves_nothing();
  test_a_frame_already_heard_moves_nothing();
  test_a_re_pair_re_binds_the_member_it_holds();
  test_a_re_pair_is_not_refused_by_a_full_opera();
  test_a_re_pair_cannot_take_another_members_address();
  std::printf("ALL %d mesh address checks PASSED\n", g_checks);
  return 0;
}
