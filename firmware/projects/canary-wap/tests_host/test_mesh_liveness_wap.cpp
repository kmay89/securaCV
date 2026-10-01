// Host test: does a canary-wap opera come up, stay reachable, and say so
// when a pairing fails — run against the REAL mesh_network.cpp on the
// test_mesh_address_wap harness. mesh_network.cpp is #included below and
// compiled against stubs/mesh_net (ESP-NOW peer list and send log, an
// in-memory NVS, OpenSSL's Ed25519 / SHA-256 / ChaCha20-Poly1305) and
// stubs/pair_crypto (OpenSSL's X25519); mesh_net_sim.h runs several devices
// on that one image and moves frames between them as bytes, through each
// receiver's ESP-NOW callback and update(). Every frame here was built by
// the sender's own send path and judged by the receiver's own receive path.
//
// Sweep items F71, F74 and F75: each was a way the opera went
// quiet, or a pairing went wrong, with nothing reporting it.
//   F71  a rebooted device's frames dropped as replays at every member
//        that had heard it (its send counters restarted at 1);
//   F74  a joiner's DISCOVER went nowhere once the ESP-NOW broadcast peer
//        was gone (mesh_network relied on other modules to register it,
//        and its own channel-change listener deleted it);
//   F75  a pairing finished only if the initiator's owner confirmed first;
//
// Host-tested only: the stubs stand in for the radio and the flash, so
// this says nothing about two real boards (U1 Track C2), and the Arduino
// compile of the file is CI's (firmware.yml's canary-wap legs).
//
// Run: ./test_mesh_liveness_wap [name]  runs the tests whose name contains
// `name` (every test without one). Build: see the MESH_LIVE block in the
// Makefile.

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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The sketch provides these on a device (canary_wap.ino, chirp_channel.cpp).
std::vector<std::string> g_health;
void health_log(LogLevel, LogCategory, const char* message) { g_health.push_back(message); }
void log_health(LogLevel, LogCategory, const char*, const char*) {}
namespace chirp_channel {
void dispatch_espnow_message(const uint8_t*, const uint8_t*, int, int8_t) {}
}

// Named, not anonymous: a helper only some tests use is not an unused
// static under -Werror.
namespace liveness {

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

Device A, B, C, J, K;
const uint8_t E_MAC[6] = {0x02, 0xEE, 0xEE, 0xEE, 0xEE, 0x01};
const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// The PlatformIO tree's COUNTER_RESERVE_BLOCK, and canary-wap's since F71.
constexpr uint64_t kBlock = 1024;
const char* const kTxKey = "mesh/tx_ctrs";

// ── Helpers ─────────────────────────────────────────────────────────────

// What the pairing callback reported, and on which device.
struct PairEvent {
  std::string device;
  mn::PairingRole role;
  uint32_t code;
  bool success;
};
std::vector<PairEvent> g_pair_events;
void record_pairing(mn::PairingRole role, uint32_t code, bool success) {
  g_pair_events.push_back({g_cur != nullptr ? g_cur->name : "", role, code, success});
}
const PairEvent* last_event_of(const Device& d) {
  for (size_t i = g_pair_events.size(); i-- > 0;) {
    if (g_pair_events[i].device == d.name) return &g_pair_events[i];
  }
  return nullptr;
}

bool logged(const char* message) {
  for (const std::string& m : g_health) {
    if (m.find(message) != std::string::npos) return true;
  }
  return false;
}

void fresh_device(Device& d) {
  d.nvs.clear();
  d.espnow = host_sim::EspNow();
  boot(d);
}

// The devices as NVS holds an opera after pairing, then booted: each holds
// every other (PEER_OFFLINE after boot), the opera is MESH_CONNECTING, and
// nothing has been heard.
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

// Fill `d`'s opera to MAX_OPERA_SIZE with members that never speak, and
// save it.
void fill_opera(Device& d) {
  become(d);
  while (mn::g_peer_count < mn::MAX_OPERA_SIZE) {
    uint8_t priv[32], pub[32];
    host_sim::fill_random(priv, sizeof priv);
    Ed25519::derivePublicKey(pub, priv);
    const uint8_t mac[6] = {0x02, 0x10, 0x00, 0x00, 0x00, mn::g_peer_count};
    CHECK(mn::add_peer(pub, mac, "filler"));
  }
  CHECK(mn::persist_peers());
}

uint8_t nvs_peer_count(Device& d) {
  become(d);
  mn::g_prefs.begin(mn::NVS_NS, true);
  const uint8_t n = mn::g_prefs.getUChar(mn::NVS_PEER_COUNT, 0);
  mn::g_prefs.end();
  return n;
}

std::vector<uint8_t> nvs_value(const Device& d, const std::string& key) {
  const auto it = d.nvs.find(key);
  return it == d.nvs.end() ? std::vector<uint8_t>() : it->second;
}

// `self`'s next heartbeat, as send_heartbeat sends it; returns its frame
// to `to`.
Frame heartbeat_to(Device& self, const Device& to) {
  become(self);
  const size_t before = sent_to(self, to.mac).size();
  mn::send_heartbeat();
  const auto frames = sent_to(self, to.mac);
  CHECK(frames.size() == before + 1);
  return frames.back();
}

// One signed frame from `self` to the member `to`, straight through
// send_to_peer (no airtime governor; the clock steps past the storm gate).
// Returns false if send_to_peer refused it.
bool frame_to(Device& self, const Device& to) {
  host_sim::now_ms += 20;
  mn::OperaPeer* p = entry(self, to);
  CHECK(p != nullptr);
  mn::HeartbeatPayload hb = {};
  return mn::send_to_peer(p, mn::MSG_HEARTBEAT, reinterpret_cast<const uint8_t*>(&hb), sizeof hb);
}

bool is_pair_frame(const Frame& f, uint8_t type) {
  return !f.empty() && f.size() < 102 && f[0] == type;
}

size_t pair_frames(const Device& from, const uint8_t to[6], uint8_t type) {
  size_t n = 0;
  for (const Frame& f : sent_to(from, to)) n += is_pair_frame(f, type) ? 1 : 0;
  return n;
}

// The DISCOVER, OFFER and ACCEPT, each carried as bytes, after which both
// screens show the same code.
void pair_to_codes(Device& ini, Device& joi) {
  g_pair_events.clear();
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
  const uint32_t code = mn::g_pairing.confirmation_code;
  become(joi);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);
  CHECK(mn::g_pairing.confirmation_code == code);
}

// The order a pairing finished in before F75: the initiator's owner
// confirms, then the joiner's; the CONFIRMs cross; the initiator answers
// the joiner's with COMPLETE.
void confirm_initiator_first(Device& ini, Device& joi) {
  ini.espnow.sent.clear();
  joi.espnow.sent.clear();
  become(ini);
  CHECK(mn::confirm_pairing());
  const auto from_ini = sent_to(ini, joi.mac);    // its CONFIRM, if the radio took it
  become(joi);
  CHECK(mn::confirm_pairing());
  const auto from_joi = sent_to(joi, ini.mac);
  CHECK(from_joi.size() == 1 && is_pair_frame(from_joi[0], mn::MSG_PAIR_CONFIRM));
  ini.espnow.sent.clear();
  deliver(ini, joi.mac, from_joi[0]);
  const auto reply = sent_to(ini, joi.mac);       // the COMPLETE, if it completed
  for (const Frame& f : from_ini) deliver(joi, ini.mac, f);
  for (const Frame& f : reply) deliver(joi, ini.mac, f);
}

// A pairing finished on `d`: it is no longer pairing, it holds `partner`
// at the address the pairing ran from, and its callback reported success.
// (Not "MESH_ACTIVE": the update() pass that takes the last frame also runs
// the 5 s peer check, which puts an opera that has heard no member yet at
// MESH_CONNECTING until one is heard.)
bool completed(Device& d, const Device& partner) {
  become(d);
  if (mn::is_pairing()) return false;
  if (mn::g_mesh_state != mn::MESH_ACTIVE && mn::g_mesh_state != mn::MESH_CONNECTING) return false;
  const mn::OperaPeer* p = entry(d, partner);
  const PairEvent* ev = last_event_of(d);
  return p != nullptr && same_mac(p->mac_addr, partner.mac) && ev != nullptr && ev->success;
}

// Signed frames each device sent each other device during run().
struct Traffic {
  std::map<std::pair<const Device*, const Device*>, int> frames;
  int of(const Device& from, const Device& to) const {
    const auto it = frames.find({&from, &to});
    return it == frames.end() ? 0 : it->second;
  }
};

// `devs` for `ms` of simulated time: each step advances the clock and runs
// every device's update() once, then carries every frame sent since to its
// destination (a unicast to the device with that address, a broadcast to
// every other device), including the frames those deliveries cause.
Traffic run(const std::vector<Device*>& devs, uint32_t ms, uint32_t step_ms = 500) {
  Traffic t;
  std::map<Device*, size_t> seen;
  for (Device* d : devs) seen[d] = d->espnow.sent.size();
  for (uint32_t elapsed = 0; elapsed < ms; elapsed += step_ms) {
    host_sim::now_ms += step_ms;
    for (Device* d : devs) {
      become(*d);
      mn::update();
    }
    for (bool moved = true; moved;) {
      moved = false;
      for (Device* d : devs) {
        while (seen[d] < d->espnow.sent.size()) {
          const host_sim::Sent s = d->espnow.sent[seen[d]++];
          moved = true;
          for (Device* r : devs) {
            if (r == d) continue;
            if (!same_mac(s.to.data(), BROADCAST) && !same_mac(s.to.data(), r->mac)) continue;
            if (s.bytes.size() >= 102) ++t.frames[{d, r}];
            deliver(*r, d->mac, s.bytes);
          }
        }
      }
    }
  }
  return t;
}

// ── F71: a reboot resumes above every counter a member can have seen ────
//
// canary-wap counts per destination: each member has its own
// msg_counter_tx. Receivers keep and persist their last-seen counter for
// each sender and drop anything not above it. load_peers used to start
// every counter at 1 again, so after a reboot every member that had heard
// this device dropped its frames as replays until the counter for that
// member climbed back past what it last saw: one heartbeat per 30 s.
// Each member's counter is now reserved ahead in NVS (the PlatformIO
// tree's F33 part 3, per member): before the first counter above its
// reservation is signed, a new reservation 1024 ahead is stored, and a
// boot resumes above the stored one.

void test_a_rebooted_member_is_heard_at_once() {
  fresh_opera({&A, &B, &C});
  for (int i = 0; i < 5; ++i) deliver(A, B.mac, heartbeat_to(B, A));
  CHECK(entry(A, B)->msg_counter_rx == 5);
  boot(B);                                         // B reboots; A remembers 5
  become(A);
  const uint32_t received = mn::g_messages_received;
  const Frame f = heartbeat_to(B, A);
  CHECK(counter_of(f) > 5);                        // was 1, dropped as a replay
  deliver(A, B.mac, f);
  become(A);
  CHECK(mn::g_messages_received == received + 1);
  CHECK(entry(A, B)->msg_counter_rx == counter_of(f));
  std::printf("PASS a_rebooted_member_is_heard_at_once\n");
}

void test_no_counter_is_signed_twice_across_reboots() {
  // A boot resumes one past the stored reservation, so a counter signed
  // before the reboot is never signed again, whatever happened after the
  // reservation was stored (a crash before the frame went out included):
  // the cost is a gap, which receivers accept, since they need only
  // "higher".
  fresh_opera({&A, &B, &C});
  std::vector<uint64_t> seen;
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 2 + round; ++i) {
      const Frame f = heartbeat_to(B, A);
      seen.push_back(counter_of(f));
      become(A);
      const uint32_t received = mn::g_messages_received;
      deliver(A, B.mac, f);
      become(A);
      CHECK(mn::g_messages_received == received + 1);   // every one heard
    }
    boot(B);
  }
  const std::vector<uint64_t> want = {1, 2, kBlock + 1, kBlock + 2, kBlock + 3,
                                      2 * kBlock + 1, 2 * kBlock + 2, 2 * kBlock + 3,
                                      2 * kBlock + 4};
  CHECK(seen == want);
  std::printf("PASS no_counter_is_signed_twice_across_reboots\n");
}

void test_the_reservation_costs_one_write_per_block() {
  // Wear: one NVS write per 1024 frames to a member, not one per frame.
  fresh_opera({&A, &B, &C});
  host_sim::nvs_writes.clear();
  for (int i = 0; i < 2100; ++i) CHECK(frame_to(B, A));   // counters 1..2100
  CHECK(host_sim::nvs_writes[kTxKey] == 3);               // at 1, 1025 and 2049
  for (int i = 0; i < 1100; ++i) CHECK(frame_to(B, C));   // 1..1100 for C
  CHECK(host_sim::nvs_writes[kTxKey] == 5);               // C's at 1 and 1025
  // One record holds both members' reservations: fingerprint + u64 each.
  CHECK(nvs_value(B, kTxKey).size() == 2 * (mn::FINGERPRINT_SIZE + 8));
  boot(B);
  B.espnow.sent.clear();
  CHECK(frame_to(B, A) && frame_to(B, C));
  CHECK(counter_of(sent_to(B, A.mac).back()) == 3 * kBlock + 1);
  CHECK(counter_of(sent_to(B, C.mac).back()) == 2 * kBlock + 1);
  std::printf("PASS the_reservation_costs_one_write_per_block\n");
}

void test_a_reservation_that_cannot_be_stored_refuses_the_frame() {
  // A counter is never signed before its reservation is durable: when NVS
  // refuses the write, the frame is not sent, and the counter is not spent.
  fresh_opera({&A, &B, &C});
  B.espnow.sent.clear();
  host_sim::nvs_writes_fail = true;
  CHECK(!frame_to(B, A));                          // the first needs a reservation
  host_sim::nvs_writes_fail = false;
  CHECK(sent_to(B, A.mac).empty());
  CHECK(frame_to(B, A));
  CHECK(counter_of(sent_to(B, A.mac).back()) == 1);
  for (uint64_t i = 2; i <= kBlock; ++i) CHECK(frame_to(B, A));
  host_sim::nvs_writes_fail = true;
  CHECK(!frame_to(B, A));                          // 1025 crosses the reservation
  CHECK(!frame_to(B, C));                          // and C's first needs one
  host_sim::nvs_writes_fail = false;
  CHECK(sent_to(B, C.mac).empty());
  CHECK(counter_of(sent_to(B, A.mac).back()) == kBlock);
  CHECK(frame_to(B, A));
  CHECK(counter_of(sent_to(B, A.mac).back()) == kBlock + 1);
  boot(B);
  CHECK(frame_to(B, A));
  CHECK(counter_of(sent_to(B, A.mac).back()) == 2 * kBlock + 1);
  std::printf("PASS a_reservation_that_cannot_be_stored_refuses_the_frame\n");
}

void test_a_damaged_reservation_record_is_logged_and_replaced() {
  // A record that is not whole entries is not trusted for any member: the
  // counters start at 1 (as before F71), the boot says so, and the next
  // reservation writes a whole record again.
  fresh_opera({&A, &B, &C});
  CHECK(frame_to(B, A));
  CHECK(nvs_value(B, kTxKey).size() == mn::FINGERPRINT_SIZE + 8);
  B.nvs[kTxKey].pop_back();
  g_health.clear();
  boot(B);
  CHECK(logged("opera: send-counter reservations unreadable"));
  B.espnow.sent.clear();
  CHECK(frame_to(B, A));
  CHECK(counter_of(sent_to(B, A.mac).back()) == 1);
  CHECK(nvs_value(B, kTxKey).size() == mn::FINGERPRINT_SIZE + 8);
  std::printf("PASS a_damaged_reservation_record_is_logged_and_replaced\n");
}

// ── F74: the DISCOVER registers the broadcast peer it is sent to ────────
//
// ESP-NOW sends only to a registered address, the broadcast one included
// (ESP_ERR_ESPNOW_NOT_FOUND otherwise). mesh_network sent its DISCOVER to
// broadcast relying on csi_probe::init, chirp_channel or beacon_channel to
// have registered it, and its channel-change listener deleted it, which
// only chirp's and Beacon's sends put back. mesh_net_sim's boot() used to
// register it for every device, standing in for those modules; it no
// longer does.

size_t broadcast_entries(const Device& d) {
  size_t n = 0;
  for (const host_sim::Mac& m : d.espnow.peers) n += same_mac(m.data(), BROADCAST) ? 1 : 0;
  return n;
}

void test_a_discover_goes_out_with_no_broadcast_peer_registered() {
  fresh_device(J);
  become(J);
  if (esp_now_is_peer_exist(BROADCAST)) CHECK(esp_now_del_peer(BROADCAST) == ESP_OK);
  CHECK(broadcast_entries(J) == 0);
  CHECK(mn::start_pairing_joiner());
  J.espnow.sent.clear();
  host_sim::now_ms += 2001;
  become(J);
  mn::update();
  CHECK(sent_to(J, BROADCAST).size() == 1);
  CHECK(broadcast_entries(J) == 1);
  // Already registered (by itself, or by chirp, Beacon or the CSI probe,
  // all with channel 0 and no encryption): ESP_ERR_ESPNOW_EXIST counts as
  // registered, and the list holds it once.
  host_sim::now_ms += 2001;
  mn::update();
  CHECK(sent_to(J, BROADCAST).size() == 2);
  CHECK(broadcast_entries(J) == 1);
  mn::cancel_pairing();
  std::printf("PASS a_discover_goes_out_with_no_broadcast_peer_registered\n");
}

void test_a_channel_change_re_adds_the_broadcast_peer() {
  fresh_opera({&A, &B, &C});
  become(A);
  if (!esp_now_is_peer_exist(BROADCAST)) {         // as csi_probe::init leaves it
    esp_now_peer_info_t bc = {};
    memcpy(bc.peer_addr, BROADCAST, 6);
    CHECK(esp_now_add_peer(&bc) == ESP_OK);
  }
  mesh_channel_policy::RadioState sta = {};
  sta.sta_connected = true;
  sta.sta_channel = 11;
  mesh_channel_policy::set_state_for_tests(sta);
  become(A);
  mn::update();                                     // poll_radio() runs the listener
  CHECK(mesh_channel_policy::current().channel == 11);
  CHECK(broadcast_entries(A) == 1);                 // dropped and re-added, not dropped
  mesh_channel_policy::set_state_for_tests(mesh_channel_policy::RadioState{});
  mn::update();
  CHECK(mesh_channel_policy::current().channel == mesh_channel_policy::MESH_FALLBACK_CHANNEL);
  CHECK(broadcast_entries(A) == 1);
  std::printf("PASS a_channel_change_re_adds_the_broadcast_peer\n");
}

// ── F75: the owners confirm in either order ──────────────────────────────
//
// handle_pair_confirm acted on the joiner's CONFIRM only if the
// initiator's own owner had confirmed already, and dropped it otherwise;
// neither side sent its CONFIRM twice. So a joiner's owner who confirmed
// first left both devices waiting until the 2-minute timeout. Spec §5.2
// asks for a confirm on both devices, in no order. The initiator now keeps
// the joiner's CONFIRM, from the pairing partner's address only, and acts
// on it when its own owner confirms. Every guard #1761 added stays.

void test_the_joiners_owner_may_confirm_first() {
  fresh_device(A);
  fresh_device(J);
  pair_to_codes(A, J);
  g_pair_events.clear();
  become(J);
  CHECK(mn::confirm_pairing());
  deliver(A, J.mac, sent_to(J, A.mac).back());
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);   // waits for its owner
  CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_COMPLETE) == 0);
  A.espnow.sent.clear();
  CHECK(mn::confirm_pairing());
  become(A);
  mn::update();                                     // A's loop sends the COMPLETE
  // One frame: the COMPLETE. (A CONFIRM sent just before it could take the
  // joiner's one-frame receive buffer, and the COMPLETE would be dropped.)
  const auto to_j = sent_to(A, J.mac);
  CHECK(to_j.size() == 1 && is_pair_frame(to_j[0], mn::MSG_PAIR_COMPLETE));
  deliver(J, A.mac, to_j[0]);
  CHECK(completed(A, J));
  CHECK(completed(J, A));
  become(A);
  uint8_t opera_id[mn::OPERA_ID_SIZE];
  memcpy(opera_id, mn::g_opera_config.opera_id, sizeof opera_id);
  become(J);
  CHECK(memcmp(mn::g_opera_config.opera_id, opera_id, sizeof opera_id) == 0);
  std::printf("PASS the_joiners_owner_may_confirm_first\n");
}

void test_the_initiator_still_waits_for_the_joiners_confirm() {
  // The other order, unchanged: the initiator's owner confirms and its
  // CONFIRM goes out, but no COMPLETE until the joiner's CONFIRM arrives.
  fresh_device(A);
  fresh_device(J);
  pair_to_codes(A, J);
  A.espnow.sent.clear();
  become(A);
  CHECK(mn::confirm_pairing());
  CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_CONFIRM) == 1);
  CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_COMPLETE) == 0);
  host_sim::now_ms += 60000;                        // the joiner's owner takes a minute
  become(A);
  mn::update();
  CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_COMPLETE) == 0);
  become(J);
  CHECK(mn::confirm_pairing());
  deliver(A, J.mac, sent_to(J, A.mac).back());
  CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_COMPLETE) == 1);
  deliver(J, A.mac, sent_to(A, J.mac).back());
  CHECK(completed(A, J) && completed(J, A));
  std::printf("PASS the_initiator_still_waits_for_the_joiners_confirm\n");
}

void test_a_confirm_from_another_address_does_not_count() {
  // Either order: the joiner's genuine CONFIRM, re-sent from another radio,
  // is not the joiner's. Confirmed first, the initiator used to take it
  // from any address.
  for (int joiner_first = 0; joiner_first < 2; ++joiner_first) {
    fresh_device(A);
    fresh_device(J);
    pair_to_codes(A, J);
    if (!joiner_first) {
      become(A);
      CHECK(mn::confirm_pairing());
    }
    become(J);
    CHECK(mn::confirm_pairing());
    const Frame confirm_j = sent_to(J, A.mac).back();
    deliver(A, E_MAC, confirm_j);
    if (joiner_first) {
      become(A);
      CHECK(mn::confirm_pairing());
    }
    become(A);
    mn::update();
    CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);
    CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_COMPLETE) == 0);
    CHECK(pair_frames(A, E_MAC, mn::MSG_PAIR_COMPLETE) == 0);
    deliver(A, J.mac, confirm_j);                  // from the partner's address
    CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_COMPLETE) == 1);
    deliver(J, A.mac, sent_to(A, J.mac).back());
    CHECK(completed(A, J) && completed(J, A));
  }
  std::printf("PASS a_confirm_from_another_address_does_not_count\n");
}

void test_a_bad_confirm_from_another_address_does_not_end_the_pairing() {
  // A CONFIRM whose hash is wrong ends the pairing (possible MITM) only
  // when it comes from the partner's address. From any other radio it used
  // to cancel the pairing once the initiator's owner had confirmed.
  fresh_device(A);
  fresh_device(J);
  pair_to_codes(A, J);
  become(A);
  CHECK(mn::confirm_pairing());
  mn::PairConfirmPayload bad = {};
  Frame forged(1 + sizeof bad);
  forged[0] = mn::MSG_PAIR_CONFIRM;
  memcpy(forged.data() + 1, &bad, sizeof bad);
  deliver(A, E_MAC, forged);
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);
  become(J);
  CHECK(mn::confirm_pairing());
  deliver(A, J.mac, sent_to(J, A.mac).back());
  deliver(J, A.mac, sent_to(A, J.mac).back());
  CHECK(completed(A, J) && completed(J, A));
  // From the partner's own address it still ends the pairing.
  fresh_device(A);
  fresh_device(J);
  pair_to_codes(A, J);
  deliver(A, J.mac, forged);
  become(A);
  CHECK(!mn::is_pairing());
  std::printf("PASS a_bad_confirm_from_another_address_does_not_end_the_pairing\n");
}

struct Test {
  const char* name;
  void (*fn)();
};
const Test kTests[] = {
    {"a_rebooted_member_is_heard_at_once", test_a_rebooted_member_is_heard_at_once},
    {"no_counter_is_signed_twice_across_reboots", test_no_counter_is_signed_twice_across_reboots},
    {"the_reservation_costs_one_write_per_block", test_the_reservation_costs_one_write_per_block},
    {"a_reservation_that_cannot_be_stored_refuses_the_frame",
     test_a_reservation_that_cannot_be_stored_refuses_the_frame},
    {"a_damaged_reservation_record_is_logged_and_replaced",
     test_a_damaged_reservation_record_is_logged_and_replaced},
    {"a_discover_goes_out_with_no_broadcast_peer_registered",
     test_a_discover_goes_out_with_no_broadcast_peer_registered},
    {"a_channel_change_re_adds_the_broadcast_peer", test_a_channel_change_re_adds_the_broadcast_peer},
    {"the_joiners_owner_may_confirm_first", test_the_joiners_owner_may_confirm_first},
    {"the_initiator_still_waits_for_the_joiners_confirm",
     test_the_initiator_still_waits_for_the_joiners_confirm},
    {"a_confirm_from_another_address_does_not_count", test_a_confirm_from_another_address_does_not_count},
    {"a_bad_confirm_from_another_address_does_not_end_the_pairing",
     test_a_bad_confirm_from_another_address_does_not_end_the_pairing},
};

}  // namespace liveness

int main(int argc, char** argv) {
  using namespace liveness;
  make_device(A, "A", 0xA1);
  make_device(B, "B", 0xB1);
  make_device(C, "C", 0xC1);
  make_device(J, "J", 0xD1);
  make_device(K, "K", 0xD2);
  mn::set_pairing_callback(record_pairing);
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
  std::printf("ALL %d mesh liveness checks PASSED (%d tests)\n", g_checks, ran);
  return 0;
}
