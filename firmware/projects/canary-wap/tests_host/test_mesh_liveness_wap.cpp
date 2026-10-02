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
// Sweep items F71, F73-F76, F95, F98 and F99: each was a way the opera
// went quiet, or a pairing went wrong, with nothing reporting it.
//   F71  a rebooted device's frames dropped as replays at every member
//        that had heard it (its send counters restarted at 1);
//   F73  a pairing whose partner add_peer refused still persisted, went
//        MESH_ACTIVE and reported success;
//   F74  a joiner's DISCOVER went nowhere once the ESP-NOW broadcast peer
//        was gone (mesh_network relied on other modules to register it,
//        and its own channel-change listener deleted it);
//   F75  a pairing finished only if the initiator's owner confirmed first;
//   F76  an opera whose members it had not heard sent nothing at all;
//   F98  a pairing from an address another member holds added a second
//        member there (test_mesh_address_wap has the add_peer side);
//   F99  a device re-paired after a removal dropped its remover's frames
//        (the remover's counter for a new member started at 1);
//   F95  a removal's rotation reaches no member (open: no session opens,
//        and the AUTH exchange cannot open one as it stands, pinned here),
//        and the rotation set every counter back, so the re-pair that
//        rejoins a survivor, or a reboot before the last-seen save, cost
//        frames until they climbed back (fixed: they carry on).
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

// The last pairing frame of `type` that `from` sent `to` (a heartbeat can
// follow a COMPLETE in the same update() pass, so "the last frame" is not it).
Frame last_pair(const Device& from, const uint8_t to[6], uint8_t type) {
  const auto frames = sent_to(from, to);
  for (size_t i = frames.size(); i-- > 0;) {
    if (is_pair_frame(frames[i], type)) return frames[i];
  }
  CHECK(false);
  return Frame();
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
// boot resumes every member above the highest one stored. With no record
// (NVS from before F71) or an unreadable one, a boot resumes above a floor.

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
  // Wear: one NVS write per 1024 counters spent to a member, not one per
  // frame, and one write covers every member that needs a reservation.
  fresh_opera({&A, &B, &C});
  host_sim::nvs_writes.clear();
  for (int i = 0; i < 2100; ++i) CHECK(frame_to(B, A));   // counters 1..2100
  CHECK(host_sim::nvs_writes[kTxKey] == 3);               // at 1 (C's too), 1025, 2049
  for (int i = 0; i < 1100; ++i) CHECK(frame_to(B, C));   // 1..1100 for C
  CHECK(host_sim::nvs_writes[kTxKey] == 4);               // C's at 1025 only
  // One record holds both members' reservations: fingerprint + u64 each.
  CHECK(nvs_value(B, kTxKey).size() == 2 * (mn::FINGERPRINT_SIZE + 8));
  // The boot sets both one past the highest reservation (A's, 3072), and
  // their first frames cost one write between them.
  boot(B);
  B.espnow.sent.clear();
  CHECK(frame_to(B, A) && frame_to(B, C));
  CHECK(counter_of(sent_to(B, A.mac).back()) == 3 * kBlock + 1);
  CHECK(counter_of(sent_to(B, C.mac).back()) == 3 * kBlock + 1);
  CHECK(host_sim::nvs_writes[kTxKey] == 5);
  // At the heartbeat cadence the members' counters cross a block together:
  // 1024 rounds to both cost one write, not one per member.
  for (uint64_t i = 0; i < kBlock; ++i) {
    host_sim::now_ms += mn::HEARTBEAT_INTERVAL_MS;
    become(B);
    mn::send_heartbeat();
  }
  CHECK(counter_of(sent_to(B, A.mac).back()) == 4 * kBlock + 1);
  CHECK(counter_of(sent_to(B, C.mac).back()) == 4 * kBlock + 1);
  CHECK(host_sim::nvs_writes[kTxKey] == 6);
  // An entry outside the member table has no place in the record, so
  // nothing is signed for it (no caller passes one).
  mn::OperaPeer stray = *entry(B, A);
  stray.msg_counter_tx_reserved = 0;
  const mn::HeartbeatPayload hb = {};
  become(B);
  const size_t sent = B.espnow.sent.size();
  CHECK(!mn::send_to_peer(&stray, mn::MSG_HEARTBEAT, reinterpret_cast<const uint8_t*>(&hb), sizeof hb));
  CHECK(B.espnow.sent.size() == sent && host_sim::nvs_writes[kTxKey] == 6);
  std::printf("PASS the_reservation_costs_one_write_per_block\n");
}

void test_a_boot_aligns_every_members_counter() {
  // Counting is per member, and the envelope names no destination, so a
  // frame B sent C, replayed at A from B's address, is taken there, and
  // A then drops B's frames until B's counter for A catches up (sweep F72,
  // open). Each member resuming from its own reservation could leave the
  // two counters up to a block further apart after a boot: B's for A at
  // 1000 and for C at 1025 resumed at 1025 and 2049, and that replay then
  // silenced B at A for 1025 frames, not 25. The boot now resumes both at
  // one counter.
  fresh_opera({&A, &B, &C});
  for (int i = 0; i < 1000; ++i) CHECK(frame_to(B, A));              // B to A: 1..1000
  for (uint64_t i = 0; i <= kBlock; ++i) CHECK(frame_to(B, C));      // B to C: 1..1025
  deliver(A, B.mac, sent_to(B, A.mac).back());
  deliver(C, B.mac, sent_to(B, C.mac).back());
  boot(B);
  B.espnow.sent.clear();
  CHECK(frame_to(B, A) && frame_to(B, C));
  const Frame to_a = sent_to(B, A.mac).back();
  const Frame to_c = sent_to(B, C.mac).back();
  CHECK(counter_of(to_a) == counter_of(to_c));
  CHECK(counter_of(to_a) == 2 * kBlock + 1);
  // The replay still costs A the frame with the same counter, and no more.
  deliver(A, B.mac, to_c);
  CHECK(entry(A, B)->msg_counter_rx == counter_of(to_c));
  CHECK(frame_to(B, A));
  become(A);
  const uint32_t received = mn::g_messages_received;
  deliver(A, B.mac, sent_to(B, A.mac).back());
  become(A);
  CHECK(mn::g_messages_received == received + 1);
  std::printf("PASS a_boot_aligns_every_members_counter\n");
}

void test_the_first_boot_after_the_update_is_heard_at_once() {
  // NVS from before F71 holds the members and no send-counter record, and
  // its counters restarted at 1 at every boot. The first boot on this
  // firmware resumes every member above 2^40, which no boot of the old one
  // reached, instead of at 1 (members drop those until each counter climbs
  // back past the one they last heard: the old uptime's worth of frames).
  fresh_opera({&A, &B, &C});
  for (int i = 0; i < 5; ++i) deliver(A, B.mac, heartbeat_to(B, A));
  B.nvs.erase(kTxKey);                                   // as the old firmware left it
  g_health.clear();
  boot(B);
  CHECK(logged("opera: no send-counter record"));
  B.espnow.sent.clear();
  const Frame f = heartbeat_to(B, A);
  CHECK(counter_of(f) == (1ULL << 40) + 1);
  become(A);
  const uint32_t received = mn::g_messages_received;
  deliver(A, B.mac, f);
  become(A);
  CHECK(mn::g_messages_received == received + 1);
  // The record is written then, so the next boot resumes from it.
  CHECK(nvs_value(B, kTxKey).size() == 2 * (mn::FINGERPRINT_SIZE + 8));
  g_health.clear();
  boot(B);
  CHECK(!logged("opera: no send-counter record"));
  CHECK(counter_of(heartbeat_to(B, A)) == (1ULL << 40) + kBlock + 1);
  // A device that stored members on this firmware has a record even if it
  // sent nothing (persist_peers writes it), so its first counter is 1.
  fresh_opera({&A, &B, &C});
  CHECK(nvs_value(B, kTxKey).size() == 2 * (mn::FINGERPRINT_SIZE + 8));
  CHECK(counter_of(heartbeat_to(B, A)) == 1);
  std::printf("PASS the_first_boot_after_the_update_is_heard_at_once\n");
}

size_t times_logged(const char* message) {
  size_t n = 0;
  for (const std::string& m : g_health) n += m.find(message) != std::string::npos ? 1 : 0;
  return n;
}

void test_a_fold_on_the_first_boot_keeps_the_floor() {
  // load_peers folds a duplicate member entry the old add_peer left, and
  // saves the list (persist_peers, which writes the send-counter record
  // from RAM). On NVS from before F71 that save would write a record of
  // zeros ahead of the reading, and the boot would resume at 1; the record
  // is read first.
  fresh_opera({&A, &B, &C});
  for (int i = 0; i < 5; ++i) deliver(A, B.mac, heartbeat_to(B, A));
  mn::OperaPeer dup = *entry(B, A);                 // the old re-pair's second entry
  become(B);
  mn::g_peers[mn::g_peer_count++] = dup;
  CHECK(mn::persist_peers());
  B.nvs.erase(kTxKey);                              // as the old firmware left it
  g_health.clear();
  boot(B);
  CHECK(logged("opera: folded a duplicate member entry into one"));
  become(B);
  CHECK(mn::g_peer_count == 2);
  const Frame f = heartbeat_to(B, A);
  CHECK(counter_of(f) == (1ULL << 40) + 1);
  become(A);
  const uint32_t received = mn::g_messages_received;
  deliver(A, B.mac, f);
  become(A);
  CHECK(mn::g_messages_received == received + 1);
  std::printf("PASS a_fold_on_the_first_boot_keeps_the_floor\n");
}

void test_a_reservation_that_cannot_be_stored_refuses_the_frame() {
  // A counter is never signed before its reservation is durable: when NVS
  // refuses the write, the frame is not sent, the counter is not spent,
  // and the log says why the member stops hearing this device, once per
  // 5 minutes while the refusals last.
  const char* const kRefused = "opera: send-counter reservation refused by NVS";
  fresh_opera({&A, &B, &C});
  B.espnow.sent.clear();
  g_health.clear();
  host_sim::nvs_writes_fail = true;
  CHECK(!frame_to(B, A));                          // the first needs a reservation
  CHECK(!frame_to(B, C));                          // and so does C's
  CHECK(times_logged(kRefused) == 1);
  for (int i = 0; i < 10; ++i) CHECK(!frame_to(B, A));
  CHECK(times_logged(kRefused) == 1);              // not once per frame
  host_sim::now_ms += 300000;
  CHECK(!frame_to(B, A));
  CHECK(times_logged(kRefused) == 2);              // again after 5 minutes
  host_sim::nvs_writes_fail = false;
  CHECK(sent_to(B, A.mac).empty() && sent_to(B, C.mac).empty());
  CHECK(frame_to(B, A));
  CHECK(counter_of(sent_to(B, A.mac).back()) == 1);
  for (uint64_t i = 2; i <= kBlock; ++i) CHECK(frame_to(B, A));
  host_sim::nvs_writes_fail = true;
  CHECK(!frame_to(B, A));                          // 1025 crosses the reservation
  CHECK(times_logged(kRefused) == 3);              // a new run of refusals is logged
  CHECK(frame_to(B, C));                           // C's first was reserved with A's 1
  host_sim::nvs_writes_fail = false;
  CHECK(counter_of(sent_to(B, C.mac).back()) == 1);
  CHECK(counter_of(sent_to(B, A.mac).back()) == kBlock);
  CHECK(frame_to(B, A));
  CHECK(counter_of(sent_to(B, A.mac).back()) == kBlock + 1);
  boot(B);
  CHECK(frame_to(B, A));
  CHECK(counter_of(sent_to(B, A.mac).back()) == 2 * kBlock + 1);
  std::printf("PASS a_reservation_that_cannot_be_stored_refuses_the_frame\n");
}

void test_a_damaged_reservation_record_is_logged_and_replaced() {
  // A record that is not whole entries is not trusted for any member. The
  // boot says so and resumes every member above 2^48, above anything this
  // firmware signed since the 2^40 floor of the first boot after the
  // update; it used to restart them at 1, and every member that had heard
  // the device dropped its frames. The next reservation writes a whole
  // record again.
  fresh_opera({&A, &B, &C});
  for (int i = 0; i < 5; ++i) deliver(A, B.mac, heartbeat_to(B, A));
  CHECK(nvs_value(B, kTxKey).size() == 2 * (mn::FINGERPRINT_SIZE + 8));
  B.nvs[kTxKey].pop_back();
  g_health.clear();
  boot(B);
  CHECK(logged("opera: send-counter reservations unreadable"));
  B.espnow.sent.clear();
  const Frame f = heartbeat_to(B, A);
  CHECK(counter_of(f) == (1ULL << 48) + 1);
  become(A);
  const uint32_t received = mn::g_messages_received;
  deliver(A, B.mac, f);
  become(A);
  CHECK(mn::g_messages_received == received + 1);
  CHECK(nvs_value(B, kTxKey).size() == 2 * (mn::FINGERPRINT_SIZE + 8));
  std::printf("PASS a_damaged_reservation_record_is_logged_and_replaced\n");
}

void test_a_flood_spends_no_counter_while_the_storm_gate_holds() {
  // The storm gate (100 frames in a second, then 30 s of silence) ran
  // after send_to_peer had reserved and spent the counter, so a flood
  // spent one counter per call and wrote a reservation per 1024 calls,
  // whatever reached the air. send_to_peer now asks the gate first.
  fresh_opera({&A, &B, &C});
  host_sim::nvs_writes.clear();
  become(B);
  B.espnow.sent.clear();
  mn::OperaPeer* p = entry(B, A);
  const mn::HeartbeatPayload hb = {};
  const auto flood = [&](uint32_t ms) {            // 50 sends every 10 ms: 5000 a second
    for (uint32_t t = 0; t < ms; t += 10) {
      host_sim::now_ms += 10;
      for (int i = 0; i < 50; ++i) {
        mn::send_to_peer(p, mn::MSG_HEARTBEAT, reinterpret_cast<const uint8_t*>(&hb), sizeof hb);
      }
    }
  };
  flood(1000);
  CHECK(sent_to(B, A.mac).size() == 100);
  CHECK(p->msg_counter_tx == 102);                 // 100 sent, 1 refused by the gate it tripped
  CHECK(host_sim::nvs_writes[kTxKey] == 1);
  flood(599000);                                   // ten minutes in all
  const uint64_t spent = p->msg_counter_tx - 1;
  CHECK(spent == sent_to(B, A.mac).size() + mn::g_storm_trigger_count);
  CHECK(host_sim::nvs_writes[kTxKey] == 1 + spent / kBlock);
  CHECK(host_sim::nvs_writes[kTxKey] <= 3);        // about one per 5 minutes
  host_sim::now_ms += 31000;                       // let the gate settle for later tests
  std::printf("PASS a_flood_spends_no_counter_while_the_storm_gate_holds\n");
}

// ── F73: a pairing add_peer refuses fails, and says so ──────────────────
//
// handle_pair_confirm (initiator) and handle_pair_complete (joiner)
// ignored add_peer's result: whatever it refused, they persisted, went
// MESH_ACTIVE and reported success. The initiator also sealed the
// opera_secret to the partner it then did not hold. Now the partner is
// added first; a refusal ends the pairing before anything is sent or
// stored, logs it, and the callback reports the failure. (The other side
// cannot know: an initiator that refuses sends no COMPLETE, so its joiner
// times out; a joiner that refuses has already let its initiator add it.)

void check_initiator_refused(Device& ini, Device& joi, uint8_t peers_before) {
  become(ini);
  CHECK(pair_frames(ini, joi.mac, mn::MSG_PAIR_COMPLETE) == 0);
  CHECK(mn::g_mesh_state != mn::MESH_ACTIVE);
  CHECK(!mn::is_pairing());
  CHECK(mn::g_peer_count == peers_before);
  CHECK(nvs_peer_count(ini) == peers_before);
  const PairEvent* ev = last_event_of(ini);
  CHECK(ev != nullptr && ev->role == mn::PAIR_ROLE_INITIATOR && !ev->success);
  CHECK(logged("opera: pairing failed"));
  become(ini);
  static const uint8_t zero[32] = {};
  CHECK(memcmp(mn::g_pairing.session_key, zero, sizeof zero) == 0);   // nothing left
}

void test_an_initiator_with_a_full_opera_refuses_the_pairing() {
  for (int joiner_first = 0; joiner_first < 2; ++joiner_first) {
    fresh_opera({&A, &B, &C});
    fill_opera(A);                                  // 16 members
    fresh_device(J);
    pair_to_codes(A, J);
    g_pair_events.clear();
    g_health.clear();
    if (joiner_first) {
      // The F75 path: the joiner's CONFIRM is kept until A's owner confirms.
      become(J);
      CHECK(mn::confirm_pairing());
      deliver(A, J.mac, last_pair(J, A.mac, mn::MSG_PAIR_CONFIRM));
      become(A);
      A.espnow.sent.clear();
      CHECK(mn::confirm_pairing());
      mn::update();                                 // the refusal comes on A's loop
    } else {
      confirm_initiator_first(A, J);
    }
    check_initiator_refused(A, J, mn::MAX_OPERA_SIZE);
    become(A);
    CHECK(mn::g_mesh_state == mn::MESH_CONNECTING);
    become(J);
    CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);   // J waits, then times out
    mn::cancel_pairing();
  }
  std::printf("PASS an_initiator_with_a_full_opera_refuses_the_pairing\n");
}

void test_an_initiator_refuses_a_partner_removed_during_the_pairing() {
  // A re-pairs with B; while the codes are shown, A's owner removes B, so
  // B is deny-listed (spec §5.6). The old handler sent B the opera_secret
  // when B's ESP-NOW registration let it, and reported success.
  fresh_opera({&A, &B, &C});
  pair_to_codes(A, B);
  g_pair_events.clear();
  g_health.clear();
  become(A);
  uint8_t fp[mn::FINGERPRINT_SIZE];
  mn::compute_fingerprint(B.pub, fp);
  CHECK(mn::remove_peer(fp));
  confirm_initiator_first(A, B);
  check_initiator_refused(A, B, 1);
  CHECK(entry(A, B) == nullptr);
  std::printf("PASS an_initiator_refuses_a_partner_removed_during_the_pairing\n");
}

void test_an_initiator_refuses_a_re_pair_onto_another_members_address() {
  // B, which A holds, re-pairs from the address A holds for C (a copied
  // address). add_peer refuses it (one address, one member); the old
  // handler still sealed the opera_secret to that address and reported
  // success.
  fresh_opera({&A, &B, &C});
  uint8_t b_mac[6];
  memcpy(b_mac, B.mac, 6);
  memcpy(B.mac, C.mac, 6);
  pair_to_codes(A, B);
  g_pair_events.clear();
  g_health.clear();
  confirm_initiator_first(A, B);
  check_initiator_refused(A, B, 2);
  CHECK(same_mac(entry(A, B)->mac_addr, b_mac));
  CHECK(same_mac(entry(A, C)->mac_addr, C.mac));
  memcpy(B.mac, b_mac, 6);
  std::printf("PASS an_initiator_refuses_a_re_pair_onto_another_members_address\n");
}

void test_an_initiator_refuses_a_new_member_at_another_members_address() {
  // Sweep F98: J, a device A does not hold, pairs with A from the address A
  // holds for C (a copied address). add_peer appended J there, so J and C
  // shared one ESP-NOW registration and the pairing reported success; now
  // it is refused like the re-pair above (one address, one member).
  fresh_opera({&A, &B, &C});
  fresh_device(J);
  uint8_t j_mac[6];
  memcpy(j_mac, J.mac, 6);
  memcpy(J.mac, C.mac, 6);
  pair_to_codes(A, J);
  g_pair_events.clear();
  g_health.clear();
  confirm_initiator_first(A, J);
  check_initiator_refused(A, J, 2);
  CHECK(entry(A, J) == nullptr);
  CHECK(same_mac(entry(A, C)->mac_addr, C.mac));
  become(A);
  A.espnow.sent.clear();
  mn::send_heartbeat();
  CHECK(sent_to(A, C.mac).size() == 1);           // C is still sent its frames
  memcpy(J.mac, j_mac, 6);
  std::printf("PASS an_initiator_refuses_a_new_member_at_another_members_address\n");
}

void test_a_joiner_refuses_a_new_initiator_at_another_members_address() {
  // F98, the joiner's side: B, which holds A and C, joins J's new opera,
  // and J pairs from C's address. B appended J there and took J's opera;
  // now B refuses J and keeps the opera it had.
  fresh_opera({&A, &B, &C});
  fresh_device(J);
  uint8_t j_mac[6];
  memcpy(j_mac, J.mac, 6);
  memcpy(J.mac, C.mac, 6);
  become(B);
  uint8_t opera_id[mn::OPERA_ID_SIZE];
  memcpy(opera_id, mn::g_opera_config.opera_id, sizeof opera_id);
  pair_to_codes(J, B);
  g_pair_events.clear();
  g_health.clear();
  confirm_initiator_first(J, B);
  CHECK(pair_frames(J, B.mac, mn::MSG_PAIR_COMPLETE) == 1);   // J completed its side
  become(B);
  CHECK(memcmp(mn::g_opera_config.opera_id, opera_id, sizeof opera_id) == 0);
  CHECK(mn::g_peer_count == 2);
  CHECK(entry(B, J) == nullptr);
  CHECK(same_mac(entry(B, C)->mac_addr, C.mac) && B.espnow.has(C.mac));
  const PairEvent* ev = last_event_of(B);
  CHECK(ev != nullptr && ev->role == mn::PAIR_ROLE_JOINER && !ev->success);
  CHECK(logged("opera: pairing failed"));
  memcpy(J.mac, j_mac, 6);
  std::printf("PASS a_joiner_refuses_a_new_initiator_at_another_members_address\n");
}

void test_a_joiner_with_a_full_opera_keeps_its_own() {
  // B holds 16 members and joins J's new opera. Its add_peer refuses J, so
  // B keeps the opera it had, in RAM and in NVS. The old handler replaced
  // B's opera with J's, saved it, and reported success.
  fresh_opera({&A, &B, &C});
  fill_opera(B);
  boot(B);
  become(B);
  uint8_t opera_id[mn::OPERA_ID_SIZE];
  memcpy(opera_id, mn::g_opera_config.opera_id, sizeof opera_id);
  const std::vector<uint8_t> nvs_id = nvs_value(B, "mesh/opera_id");
  CHECK(nvs_id.size() == mn::OPERA_ID_SIZE);
  fresh_device(J);
  pair_to_codes(J, B);
  g_pair_events.clear();
  g_health.clear();
  confirm_initiator_first(J, B);
  CHECK(pair_frames(J, B.mac, mn::MSG_PAIR_COMPLETE) == 1);   // J completed its side
  become(B);
  CHECK(memcmp(mn::g_opera_config.opera_id, opera_id, sizeof opera_id) == 0);
  CHECK(nvs_value(B, "mesh/opera_id") == nvs_id);
  CHECK(mn::g_peer_count == mn::MAX_OPERA_SIZE);
  CHECK(nvs_peer_count(B) == mn::MAX_OPERA_SIZE);
  CHECK(entry(B, J) == nullptr);
  become(B);
  CHECK(mn::g_mesh_state == mn::MESH_CONNECTING);
  const PairEvent* ev = last_event_of(B);
  CHECK(ev != nullptr && ev->role == mn::PAIR_ROLE_JOINER && !ev->success);
  CHECK(logged("opera: pairing failed"));
  std::printf("PASS a_joiner_with_a_full_opera_keeps_its_own\n");
}

void test_a_joiner_refuses_an_initiator_removed_during_the_pairing() {
  // B re-pairs with A; once B's owner has confirmed, B's owner removes A.
  // The COMPLETE that follows names a deny-listed key.
  fresh_opera({&A, &B, &C});
  pair_to_codes(A, B);
  g_pair_events.clear();
  g_health.clear();
  become(A);
  CHECK(mn::confirm_pairing());
  become(B);
  CHECK(mn::confirm_pairing());
  const Frame confirm_b = last_pair(B, A.mac, mn::MSG_PAIR_CONFIRM);
  uint8_t fp[mn::FINGERPRINT_SIZE];
  mn::compute_fingerprint(A.pub, fp);
  become(B);
  CHECK(mn::remove_peer(fp));
  deliver(A, B.mac, confirm_b);
  const Frame complete = last_pair(A, B.mac, mn::MSG_PAIR_COMPLETE);
  deliver(B, A.mac, complete);
  become(B);
  CHECK(mn::g_mesh_state != mn::MESH_ACTIVE);
  CHECK(!mn::is_pairing());
  CHECK(mn::g_peer_count == 1);
  CHECK(nvs_peer_count(B) == 1);
  CHECK(entry(B, A) == nullptr);
  const PairEvent* ev = last_event_of(B);
  CHECK(ev != nullptr && ev->role == mn::PAIR_ROLE_JOINER && !ev->success);
  CHECK(logged("opera: pairing failed"));
  std::printf("PASS a_joiner_refuses_an_initiator_removed_during_the_pairing\n");
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
  deliver(A, J.mac, last_pair(J, A.mac, mn::MSG_PAIR_CONFIRM));
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);   // waits for its owner
  CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_COMPLETE) == 0);
  A.espnow.sent.clear();
  CHECK(mn::confirm_pairing());
  become(A);
  mn::update();                                     // A's loop sends the COMPLETE
  // One pairing frame: the COMPLETE, and no CONFIRM before it (two frames
  // back to back could meet the joiner's one-frame receive buffer, and the
  // COMPLETE would be the one dropped).
  CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_CONFIRM) == 0);
  CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_COMPLETE) == 1);
  CHECK(is_pair_frame(sent_to(A, J.mac).front(), mn::MSG_PAIR_COMPLETE));
  deliver(J, A.mac, last_pair(A, J.mac, mn::MSG_PAIR_COMPLETE));
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
  deliver(A, J.mac, last_pair(J, A.mac, mn::MSG_PAIR_CONFIRM));
  CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_COMPLETE) == 1);
  deliver(J, A.mac, last_pair(A, J.mac, mn::MSG_PAIR_COMPLETE));
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
    const Frame confirm_j = last_pair(J, A.mac, mn::MSG_PAIR_CONFIRM);
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
    deliver(J, A.mac, last_pair(A, J.mac, mn::MSG_PAIR_COMPLETE));
    CHECK(completed(A, J) && completed(J, A));
  }
  std::printf("PASS a_confirm_from_another_address_does_not_count\n");
}

void test_a_confirm_before_the_code_is_shown_does_not_count() {
  // F75 made a CONFIRM stick (peer_confirmed) until the initiator's owner
  // confirms, so when one counts matters. Before the ACCEPT the initiator's
  // session key is all zero and the code 0: a CONFIRM over those is one
  // anyone can compute. Sent from the address the OFFER went to while the
  // initiator is still in MESH_PAIRING_INIT, it must not count, or the
  // owner's confirm would send COMPLETE without the joiner's CONFIRM.
  fresh_device(A);
  fresh_device(J);
  g_pair_events.clear();
  become(A);
  CHECK(mn::start_pairing_initiator(nullptr));
  become(J);
  CHECK(mn::start_pairing_joiner());
  A.espnow.sent.clear();
  J.espnow.sent.clear();
  host_sim::now_ms += 2001;
  become(J);
  mn::update();
  deliver(A, J.mac, sent_to(J, BROADCAST).back());   // A offers to J
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_INIT);
  mn::PairConfirmPayload forged = {};
  uint8_t zero[mn::SESSION_KEY_SIZE + 4] = {};
  mn::sha256_domain(mn::DOMAIN_PAIR_CONFIRM, zero, sizeof zero, forged.confirmation_hash);
  Frame f(1 + sizeof forged);
  f[0] = mn::MSG_PAIR_CONFIRM;
  memcpy(f.data() + 1, &forged, sizeof forged);
  deliver(A, J.mac, f);
  become(A);
  CHECK(!mn::g_pairing.peer_confirmed);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_INIT);
  deliver(J, A.mac, sent_to(A, J.mac).back());       // the OFFER
  deliver(A, J.mac, sent_to(J, A.mac).back());       // the ACCEPT
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_PAIRING_CONFIRM);
  A.espnow.sent.clear();
  CHECK(mn::confirm_pairing());                      // A's owner; J's has not confirmed
  mn::update();
  CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_COMPLETE) == 0);
  CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_CONFIRM) == 1);
  become(J);
  CHECK(mn::confirm_pairing());
  deliver(A, J.mac, last_pair(J, A.mac, mn::MSG_PAIR_CONFIRM));
  CHECK(pair_frames(A, J.mac, mn::MSG_PAIR_COMPLETE) == 1);
  deliver(J, A.mac, last_pair(A, J.mac, mn::MSG_PAIR_COMPLETE));
  CHECK(completed(A, J) && completed(J, A));
  std::printf("PASS a_confirm_before_the_code_is_shown_does_not_count\n");
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
  deliver(A, J.mac, last_pair(J, A.mac, mn::MSG_PAIR_CONFIRM));
  deliver(J, A.mac, last_pair(A, J.mac, mn::MSG_PAIR_COMPLETE));
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

// ── F76: an opera with members it has not heard announces itself ───────
//
// update() sent a heartbeat only in MESH_ACTIVE, and broadcast_message
// skips members below PEER_CONNECTED. So an opera that had heard nobody
// sent nothing: after a fresh pairing each side holds the other at
// PEER_UNKNOWN, and after every member reboots each one is
// MESH_CONNECTING. The heartbeat now goes out in MESH_CONNECTING too, to
// every member whatever its state, at the same 30 s cadence.

void test_a_fresh_pairing_is_heard_both_ways() {
  // A pairing adds each side to the other at PEER_UNKNOWN. (The initiator's
  // first heartbeat can follow its COMPLETE in the same update() pass, so
  // the joiner may have heard it already; the joiner has sent nothing
  // signed yet.)
  fresh_device(A);
  fresh_device(J);
  pair_to_codes(A, J);
  confirm_initiator_first(A, J);
  CHECK(completed(A, J) && completed(J, A));
  CHECK(entry(A, J)->state == mn::PEER_UNKNOWN);
  const Traffic t = run({&A, &J}, 2 * mn::HEARTBEAT_INTERVAL_MS);
  CHECK(t.of(A, J) >= 1 && t.of(J, A) >= 1);
  CHECK(entry(A, J)->state == mn::PEER_CONNECTED);
  CHECK(entry(J, A)->state == mn::PEER_CONNECTED);
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_ACTIVE);
  become(J);
  CHECK(mn::g_mesh_state == mn::MESH_ACTIVE);
  std::printf("PASS a_fresh_pairing_is_heard_both_ways\n");
}

void test_an_opera_whose_members_all_rebooted_comes_back() {
  fresh_opera({&A, &B, &C});
  Device* all[3] = {&A, &B, &C};
  for (Device* d : all) {
    become(*d);
    CHECK(mn::g_mesh_state == mn::MESH_CONNECTING);
  }
  run({&A, &B, &C}, 2 * mn::HEARTBEAT_INTERVAL_MS);
  for (Device* d : all) {
    become(*d);
    CHECK(mn::g_mesh_state == mn::MESH_ACTIVE);
    for (Device* o : all) {
      if (o != d) CHECK(entry(*d, *o)->state == mn::PEER_CONNECTED);
    }
  }
  std::printf("PASS an_opera_whose_members_all_rebooted_comes_back\n");
}

void test_two_active_members_that_never_heard_each_other_do() {
  // B and C are each MESH_ACTIVE (they hear A) and hold each other at
  // PEER_UNKNOWN: what a B-to-C pairing leaves in an opera A already runs.
  // Heartbeats skipped PEER_UNKNOWN members, so B and C never heard each
  // other, whatever the opera did.
  fresh_opera({&A, &B, &C});
  for (Device* d : {&A, &B, &C}) {
    become(*d);
    mn::g_mesh_state = mn::MESH_ACTIVE;
    for (Device* o : {&A, &B, &C}) {
      if (o == d) continue;
      mn::OperaPeer* p = entry(*d, *o);
      const bool b_and_c = (d != &A && o != &A);
      p->state = b_and_c ? mn::PEER_UNKNOWN : mn::PEER_CONNECTED;
      p->last_seen_ms = host_sim::now_ms;
    }
  }
  const Traffic t = run({&A, &B, &C}, 2 * mn::HEARTBEAT_INTERVAL_MS);
  CHECK(t.of(B, C) >= 1 && t.of(C, B) >= 1);
  CHECK(entry(B, C)->state == mn::PEER_CONNECTED);
  CHECK(entry(C, B)->state == mn::PEER_CONNECTED);
  std::printf("PASS two_active_members_that_never_heard_each_other_do\n");
}

// An opera configured with no members, as a boot leaves one (init(): a
// configured opera is MESH_CONNECTING): nobody to tell.
void opera_of_one(Device& d) {
  fresh_device(d);
  become(d);
  host_sim::fill_random(mn::g_opera_config.opera_secret, mn::OPERA_SECRET_SIZE);
  mn::compute_opera_id(mn::g_opera_config.opera_secret, mn::g_opera_config.opera_id);
  mn::g_opera_config.configured = true;
  mn::g_opera_config.enabled = true;
  strcpy(mn::g_opera_config.opera_name, "alone");
  CHECK(mn::persist_opera_config());
  boot(d);
  CHECK(mn::g_mesh_state == mn::MESH_CONNECTING && mn::g_peer_count == 0);
}

void test_the_announce_keeps_the_heartbeat_cadence() {
  // Bounded: one frame per member per HEARTBEAT_INTERVAL_MS, whether the
  // opera is MESH_CONNECTING or MESH_ACTIVE, and whether the member has
  // been heard or not. 600 s is 20 intervals; the first goes at once.
  fresh_opera({&A, &B, &C});
  opera_of_one(K);
  const size_t k_sent = K.espnow.sent.size();
  const Traffic t = run({&A, &B, &C, &K}, 600000, 1000);
  for (Device* d : {&A, &B, &C}) {
    for (Device* o : {&A, &B, &C}) {
      if (o == d) continue;
      CHECK(t.of(*d, *o) >= 20 && t.of(*d, *o) <= 21);
    }
  }
  CHECK(K.espnow.sent.size() == k_sent);
  become(K);
  CHECK(mn::g_mesh_state == mn::MESH_CONNECTING);   // it ran the announce, to no one
  std::printf("PASS the_announce_keeps_the_heartbeat_cadence\n");
}

// Signed frames `from` sent to `to` (pairing frames are shorter).
size_t signed_frames(const Device& from, const Device& to) {
  size_t n = 0;
  for (const Frame& f : sent_to(from, to.mac)) n += f.size() >= 102 ? 1 : 0;
  return n;
}

void test_an_opera_nobody_answers_keeps_the_heartbeat_cadence() {
  // The announce while MESH_CONNECTING is the same 30 s heartbeat, however
  // long nobody answers: A runs alone for 10 minutes (B and C are off),
  // stays MESH_CONNECTING, and sends each of them 20 or 21 frames.
  fresh_opera({&A, &B, &C});
  A.espnow.sent.clear();
  for (uint32_t t = 0; t < 600000; t += 1000) {
    host_sim::now_ms += 1000;
    become(A);
    mn::update();
  }
  become(A);
  CHECK(mn::g_mesh_state == mn::MESH_CONNECTING);
  for (const Device* o : {&B, &C}) {
    const size_t n = signed_frames(A, *o);
    CHECK(n >= 20 && n <= 21);
  }
  std::printf("PASS an_opera_nobody_answers_keeps_the_heartbeat_cadence\n");
}

// ── F99: a device re-paired after a removal hears its remover at once ───
//
// A removal is one-sided: the removed device keeps its last-seen counter
// for the remover, and a re-pair re-binds the remover there, counters and
// all. The remover added it back as a new member, whose send counter
// started at 1 (F71's record holds current members only), so the re-paired
// device dropped the remover's frames until that counter climbed back
// (host-probed with the #1761 harness: B dropped A's frames 1..5 and heard
// 6). A new member now starts one past the highest reservation the device
// stored for anyone, a removed member's included, as a boot resumes every
// member.

// `self` takes `from`'s next heartbeat (a verified, fresh frame).
bool hears_next_heartbeat(Device& self, Device& from) {
  const Frame f = heartbeat_to(from, self);
  become(self);
  const uint32_t received = mn::g_messages_received;
  deliver(self, from.mac, f);
  become(self);
  return mn::g_messages_received == received + 1;
}

void remove_member(Device& self, const Device& gone) {
  uint8_t fp[mn::FINGERPRINT_SIZE];
  mn::compute_fingerprint(gone.pub, fp);
  become(self);
  CHECK(mn::remove_peer(fp));
}

// The deny-list's grace runs out on `self` (spec §5.6: 7 days), so it pairs
// with the device it removed again.
void past_the_grace(Device& self) {
  host_sim::now_ms += mesh_revocation::REVOCATION_GRACE_MS + 1000;
  become(self);
  mn::update();
}

void re_pair(Device& ini, Device& joi) {
  pair_to_codes(ini, joi);
  confirm_initiator_first(ini, joi);
  CHECK(completed(ini, joi) && completed(joi, ini));
}

void test_a_re_paired_removed_member_hears_its_remover_at_once() {
  fresh_opera({&A, &B, &C});
  for (int i = 0; i < 5; ++i) deliver(B, A.mac, heartbeat_to(A, B));
  CHECK(entry(B, A)->msg_counter_rx == 5);
  remove_member(A, B);
  past_the_grace(A);
  re_pair(A, B);
  CHECK(entry(B, A)->msg_counter_rx >= 5);         // B kept what it had heard
  CHECK(entry(A, B)->msg_counter_tx > 5);          // was 1
  CHECK(hears_next_heartbeat(B, A));
  std::printf("PASS a_re_paired_removed_member_hears_its_remover_at_once\n");
}

void test_a_removed_members_reservation_outlives_it_and_a_reboot() {
  // The removed member held the highest reservation (A sent B 3072
  // frames, and C none: C's reservation is the first block's), and A
  // reboots before the re-pair. The removal's save dropped B's entry from
  // the record, so the boot resumed C one past the first block, and the
  // re-pair started B a block above that (C's first heartbeats reserved
  // one), below what B had heard. A removal now holds every survivor to
  // the highest reservation.
  fresh_opera({&A, &B, &C});
  for (uint64_t i = 0; i < 3 * kBlock; ++i) CHECK(frame_to(A, B));
  deliver(B, A.mac, sent_to(A, B.mac).back());
  CHECK(entry(B, A)->msg_counter_rx == 3 * kBlock);
  CHECK(entry(A, C)->msg_counter_tx_reserved == kBlock);   // C's never moved
  remove_member(A, B);
  boot(A);
  past_the_grace(A);
  re_pair(A, B);
  CHECK(entry(A, B)->msg_counter_tx > 3 * kBlock);
  CHECK(hears_next_heartbeat(B, A));
  std::printf("PASS a_removed_members_reservation_outlives_it_and_a_reboot\n");
}

void test_a_device_re_paired_after_its_partner_held_no_one_hears_it() {
  // Two ways A ends up holding no member while B keeps A: A removes its
  // last member (the record is not rewritten with no member; a boot reads
  // it all the same), or A leaves (B is not told: canary-wap acts on no
  // LEAVE_OPERA).
  for (int leave = 0; leave < 2; ++leave) {
    fresh_opera({&A, &B});
    for (int i = 0; i < 5; ++i) deliver(B, A.mac, heartbeat_to(A, B));
    if (leave) {
      become(A);
      CHECK(mn::leave_opera());
    } else {
      remove_member(A, B);
      boot(A);
      become(A);
      CHECK(mn::g_peer_count == 0);
      past_the_grace(A);
    }
    re_pair(A, B);
    CHECK(entry(A, B)->msg_counter_tx > 5);
    CHECK(hears_next_heartbeat(B, A));
  }
  std::printf("PASS a_device_re_paired_after_its_partner_held_no_one_hears_it\n");
}

void test_a_device_whose_opera_was_not_loaded_re_pairs_above_its_counters() {
  // Flash encryption off: a boot loads no opera and no member (spec §5.5),
  // while a member that keeps A keeps what it last heard. init() reads the
  // send-counter record anyway (it is not flash-encryption gated), so the
  // re-pair starts above it. (The switch is one per image here: B's own
  // saves are refused too during the re-pair, and it pairs in RAM.)
  fresh_opera({&A, &B});
  for (int i = 0; i < 5; ++i) deliver(B, A.mac, heartbeat_to(A, B));
  host_sim::flash_encrypted = false;
  boot(A);
  become(A);
  CHECK(!mn::g_opera_config.configured && mn::g_peer_count == 0);
  re_pair(A, B);
  CHECK(entry(A, B)->msg_counter_tx > 5);
  CHECK(hears_next_heartbeat(B, A));
  host_sim::flash_encrypted = true;
  std::printf("PASS a_device_whose_opera_was_not_loaded_re_pairs_above_its_counters\n");
}

// ── F95: a removal's rotation reaches no member; counters across it ────
//
// remove_peer rotates the opera_secret and sends MSG_OPERA_REKEY, sealed
// under a session key, to each member holding a session. Nothing opens a
// session (no code sends MSG_AUTH_CHALLENGE), so the rotation reaches no
// one: the remover commits at once and every survivor stays on the old
// opera_id, split from it until it re-pairs. Starting the AUTH exchange
// does not fix that as the exchange stands, which the first test pins (the
// fix is F48's, a wire and crypto change). What changes here is the
// counters across a rotation: they carry on, as on the PlatformIO tree.

// The opera_id `d` holds now.
std::vector<uint8_t> opera_id_of(Device& d) {
  become(d);
  return std::vector<uint8_t>(mn::g_opera_config.opera_id,
                              mn::g_opera_config.opera_id + mn::OPERA_ID_SIZE);
}

void test_no_session_opens_and_a_removal_splits_the_opera() {
  // Evidence for F95, pinned so it fails the day it stops being true:
  // this is the state F48 has to change, not a requirement.
  fresh_opera({&A, &B, &C});
  run({&A, &B, &C}, 600000, 1000);
  int sessions = 0;
  for (Device* d : {&A, &B, &C}) {
    for (Device* o : {&A, &B, &C}) {
      if (o != d) sessions += entry(*d, *o)->session_established ? 1 : 0;
    }
  }
  CHECK(sessions == 0);                            // nothing opens one
  // Started by hand, the exchange cannot finish. The AUTH_RESPONSE is a
  // 160 B payload: 38 B header + 160 + 64 B signature is 262 B, over the
  // 250 B an ESP-NOW frame carries, and send_to_peer refuses it. So the
  // responder holds a session the challenger never gets.
  CHECK(mn::signed_frame_bytes(sizeof(mn::AuthResponsePayload)) > mn::MAX_MESSAGE_SIZE);
  mn::AuthChallengePayload ch;
  host_sim::fill_random(ch.nonce, sizeof ch.nonce);
  memcpy(ch.pubkey, A.pub, 32);
  host_sim::now_ms += 20;
  CHECK(mn::send_to_peer(entry(A, B), mn::MSG_AUTH_CHALLENGE, reinterpret_cast<const uint8_t*>(&ch),
                         sizeof ch));
  B.espnow.sent.clear();
  deliver(B, A.mac, sent_to(A, B.mac).back());
  for (const Frame& f : sent_to(B, A.mac)) CHECK(f.size() < 2 || f[1] != mn::MSG_AUTH_RESPONSE);
  CHECK(entry(B, A)->session_established);
  CHECK(!entry(A, B)->session_established);
  // Nor would the keys agree if it were sent: the exchange runs X25519
  // over the long-term Ed25519 keys, so each side computes an unrelated
  // number (here OpenSSL's X25519, which clamps the scalar; rweather's on
  // the device does not, and they differ there too).
  uint8_t k_ab[32], k_ba[32];
  become(A);
  CHECK(mn::derive_session_key(A.priv, B.pub, k_ab));
  CHECK(mn::derive_session_key(B.priv, A.pub, k_ba));
  CHECK(memcmp(k_ab, k_ba, sizeof k_ab) != 0);
  // So a removal splits the opera: the REKEY goes to no member (A holds
  // no session), A commits at once, B and C stay on the old opera_id.
  const std::vector<uint8_t> old_id = opera_id_of(A);
  remove_member(A, C);
  become(A);
  CHECK(mn::g_rekey.pending_acks == 0);
  run({&A, &B}, 120000, 1000);
  CHECK(opera_id_of(A) != old_id);
  CHECK(opera_id_of(B) == old_id);
  std::printf("PASS no_session_opens_and_a_removal_splits_the_opera\n");
}

void test_a_survivor_re_paired_after_a_rotation_hears_the_remover_at_once() {
  // The way back from that split is a re-pair with the remover, which
  // re-binds each side at the other with its counters. The rotation had
  // set the remover's counters for every member back to 1, so the
  // survivor dropped the remover's frames until they climbed back past
  // the last one it heard. A rotation now leaves the counters alone.
  fresh_opera({&A, &B, &C});
  for (int i = 0; i < 5; ++i) deliver(B, A.mac, heartbeat_to(A, B));
  for (int i = 0; i < 5; ++i) deliver(A, B.mac, heartbeat_to(B, A));
  remove_member(A, C);
  become(A);
  mn::update();                                    // commits: it reached no one
  CHECK(entry(A, B)->msg_counter_tx > 5);          // was set back to 1
  CHECK(entry(A, B)->msg_counter_rx >= 5);         // was set back to 0
  re_pair(A, B);
  CHECK(opera_id_of(A) == opera_id_of(B));
  CHECK(hears_next_heartbeat(B, A));
  CHECK(hears_next_heartbeat(A, B));
  std::printf("PASS a_survivor_re_paired_after_a_rotation_hears_the_remover_at_once\n");
}

void test_a_rotation_that_reaches_a_member_keeps_every_counter() {
  // What a rotation does once it can reach a member, which needs a working
  // AUTH exchange (F48). The test stands in for one by giving A and B one
  // session key. Both sides reset their counters for each other: in RAM
  // only for the last-seen ("replay_ctrs" kept the old value until the
  // sketch's next 5-minute save), so a member that rebooted in between
  // restored it and dropped the other's restarted frames until they climbed
  // back. Now every counter carries on, and RAM and the stored copy agree.
  fresh_opera({&A, &B, &C});
  for (int i = 0; i < 5; ++i) deliver(B, A.mac, heartbeat_to(A, B));
  for (int i = 0; i < 5; ++i) deliver(A, B.mac, heartbeat_to(B, A));
  become(A);
  CHECK(mn::save_replay_counters());               // the sketch's 5-minute saves
  become(B);
  CHECK(mn::save_replay_counters());
  uint8_t key[mn::SESSION_KEY_SIZE];
  host_sim::fill_random(key, sizeof key);
  for (const auto& side : {std::make_pair(&A, &B), std::make_pair(&B, &A)}) {
    mn::OperaPeer* p = entry(*side.first, *side.second);
    memcpy(p->session_key, key, sizeof key);
    p->session_established = true;
  }
  const std::vector<uint8_t> old_id = opera_id_of(A);
  remove_member(A, C);
  const Frame rekey = sent_to(A, B.mac).back();
  CHECK(rekey.size() >= 102 && rekey[1] == mn::MSG_OPERA_REKEY);
  B.espnow.sent.clear();
  deliver(B, A.mac, rekey);                        // B ACKs, then switches
  Frame ack;
  for (const Frame& f : sent_to(B, A.mac)) {
    if (f.size() >= 102 && f[1] == mn::MSG_OPERA_REKEY_ACK) ack = f;
  }
  CHECK(!ack.empty());
  deliver(A, B.mac, ack);                          // every ACK in: A commits
  CHECK(opera_id_of(A) != old_id);
  CHECK(opera_id_of(B) == opera_id_of(A));
  CHECK(entry(A, B)->msg_counter_rx >= 5);         // were set back to 0
  CHECK(entry(B, A)->msg_counter_rx >= 5);
  CHECK(hears_next_heartbeat(A, B));               // B's were set back to 1
  boot(B);                                         // before the next save
  CHECK(opera_id_of(B) == opera_id_of(A));
  CHECK(hears_next_heartbeat(B, A));               // A's were set back to 1
  CHECK(hears_next_heartbeat(A, B));
  std::printf("PASS a_rotation_that_reaches_a_member_keeps_every_counter\n");
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
    {"a_boot_aligns_every_members_counter", test_a_boot_aligns_every_members_counter},
    {"a_fold_on_the_first_boot_keeps_the_floor", test_a_fold_on_the_first_boot_keeps_the_floor},
    {"the_first_boot_after_the_update_is_heard_at_once",
     test_the_first_boot_after_the_update_is_heard_at_once},
    {"a_flood_spends_no_counter_while_the_storm_gate_holds",
     test_a_flood_spends_no_counter_while_the_storm_gate_holds},
    {"an_initiator_with_a_full_opera_refuses_the_pairing",
     test_an_initiator_with_a_full_opera_refuses_the_pairing},
    {"an_initiator_refuses_a_partner_removed_during_the_pairing",
     test_an_initiator_refuses_a_partner_removed_during_the_pairing},
    {"an_initiator_refuses_a_re_pair_onto_another_members_address",
     test_an_initiator_refuses_a_re_pair_onto_another_members_address},
    {"an_initiator_refuses_a_new_member_at_another_members_address",
     test_an_initiator_refuses_a_new_member_at_another_members_address},
    {"a_joiner_refuses_a_new_initiator_at_another_members_address",
     test_a_joiner_refuses_a_new_initiator_at_another_members_address},
    {"a_joiner_with_a_full_opera_keeps_its_own", test_a_joiner_with_a_full_opera_keeps_its_own},
    {"a_joiner_refuses_an_initiator_removed_during_the_pairing",
     test_a_joiner_refuses_an_initiator_removed_during_the_pairing},
    {"a_discover_goes_out_with_no_broadcast_peer_registered",
     test_a_discover_goes_out_with_no_broadcast_peer_registered},
    {"a_channel_change_re_adds_the_broadcast_peer", test_a_channel_change_re_adds_the_broadcast_peer},
    {"the_joiners_owner_may_confirm_first", test_the_joiners_owner_may_confirm_first},
    {"the_initiator_still_waits_for_the_joiners_confirm",
     test_the_initiator_still_waits_for_the_joiners_confirm},
    {"a_confirm_from_another_address_does_not_count", test_a_confirm_from_another_address_does_not_count},
    {"a_bad_confirm_from_another_address_does_not_end_the_pairing",
     test_a_bad_confirm_from_another_address_does_not_end_the_pairing},
    {"a_confirm_before_the_code_is_shown_does_not_count",
     test_a_confirm_before_the_code_is_shown_does_not_count},
    {"a_fresh_pairing_is_heard_both_ways", test_a_fresh_pairing_is_heard_both_ways},
    {"an_opera_whose_members_all_rebooted_comes_back", test_an_opera_whose_members_all_rebooted_comes_back},
    {"two_active_members_that_never_heard_each_other_do", test_two_active_members_that_never_heard_each_other_do},
    {"the_announce_keeps_the_heartbeat_cadence", test_the_announce_keeps_the_heartbeat_cadence},
    {"an_opera_nobody_answers_keeps_the_heartbeat_cadence",
     test_an_opera_nobody_answers_keeps_the_heartbeat_cadence},
    {"a_re_paired_removed_member_hears_its_remover_at_once",
     test_a_re_paired_removed_member_hears_its_remover_at_once},
    {"a_removed_members_reservation_outlives_it_and_a_reboot",
     test_a_removed_members_reservation_outlives_it_and_a_reboot},
    {"a_device_re_paired_after_its_partner_held_no_one_hears_it",
     test_a_device_re_paired_after_its_partner_held_no_one_hears_it},
    {"a_device_whose_opera_was_not_loaded_re_pairs_above_its_counters",
     test_a_device_whose_opera_was_not_loaded_re_pairs_above_its_counters},
    {"no_session_opens_and_a_removal_splits_the_opera", test_no_session_opens_and_a_removal_splits_the_opera},
    {"a_survivor_re_paired_after_a_rotation_hears_the_remover_at_once",
     test_a_survivor_re_paired_after_a_rotation_hears_the_remover_at_once},
    {"a_rotation_that_reaches_a_member_keeps_every_counter",
     test_a_rotation_that_reaches_a_member_keeps_every_counter},
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
