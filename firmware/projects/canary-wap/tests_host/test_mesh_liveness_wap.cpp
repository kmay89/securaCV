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
// Sweep item F74 was a way the opera went
// quiet, or a pairing went wrong, with nothing reporting it.
//   F74  a joiner's DISCOVER went nowhere once the ESP-NOW broadcast peer
//        was gone (mesh_network relied on other modules to register it,
//        and its own channel-change listener deleted it);
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

struct Test {
  const char* name;
  void (*fn)();
};
const Test kTests[] = {
    {"a_discover_goes_out_with_no_broadcast_peer_registered",
     test_a_discover_goes_out_with_no_broadcast_peer_registered},
    {"a_channel_change_re_adds_the_broadcast_peer", test_a_channel_change_re_adds_the_broadcast_peer},
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
