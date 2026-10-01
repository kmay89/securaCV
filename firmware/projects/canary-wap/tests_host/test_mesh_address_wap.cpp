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

}  // namespace

int main() {
  make_device(A, "A", 0xA1);
  make_device(B, "B", 0xB1);
  make_device(C, "C", 0xC1);
  test_a_frame_from_the_bound_address_is_heard();
  test_a_forged_frame_from_another_address_moves_nothing();
  test_a_frame_already_heard_moves_nothing();
  std::printf("ALL %d mesh address checks PASSED\n", g_checks);
  return 0;
}
