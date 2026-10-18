// The REAL chirp_channel.cpp on the host, for the Chirp suites that test the
// code itself rather than a copy of it (sweep F271): test_chirp_security.cpp
// and test_chirp_protocol_invariants.cpp include this once, before their
// cases. It compiles chirp_channel.cpp over stubs/mesh_net (an ESP-NOW peer
// list and send log, an in-memory NVS, OpenSSL's Ed25519 and SHA-256, a clock
// the test sets), with the test's own wall clock, and gives the cases other
// devices in range: session keys, signed frames, and the receiver's boot.
//
// Those two files used to re-implement the canonical builders, the sets, the
// priority insert and the Bloom filter in plain C++ and test the copies,
// saying drift from the firmware was caught at code review; a copy kept an
// 8-key suppress set the firmware no longer had (F233). Everything here goes
// through chirp_channel.cpp's own functions, reached the way a frame or the
// loop task reaches them.
//
// Host-tested only: the stubs stand in for the radio and the clock; the
// Arduino compile is CI's.

#ifndef CHIRP_HOST_HARNESS_H
#define CHIRP_HOST_HARNESS_H

#ifndef CHIRP_CHANNEL_CPP
#error "CHIRP_CHANNEL_CPP (absolute path to chirp_channel.cpp) must be defined"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Arduino.h"
#include "Preferences.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The wall clock chirp_channel.cpp reads: set (past MIN_UNIX_TIME), UTC,
// daytime, so a witness is fresh and night mode is off.
namespace host_sim {
inline time_t wall_now = 1760000000;   // 2025-10-09 08:53:20 UTC
inline struct tm wall_tm;
}  // namespace host_sim
inline time_t host_sim_time(time_t* out) {
  if (out != nullptr) *out = host_sim::wall_now;
  return host_sim::wall_now;
}
inline struct tm* host_sim_localtime(const time_t* t) {
  return gmtime_r(t, &host_sim::wall_tm);
}

// nvs_store.h's two chirp-namespace calls, over the stub NVS.
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
// The firmware file is held to the device build's warnings (see
// test_chirp_commands_wap.cpp); the suites' own code keeps all of them.
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

// The sketch provides this on a device (canary_wap.ino). Defined here, not
// inline: chirp_channel.cpp calls it above, and each suite that includes this
// header is one translation unit.
inline std::vector<std::string> g_health;
void health_log(LogLevel, LogCategory, const char* message) { g_health.push_back(message); }

namespace chirp_host {

inline int g_checks = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++::chirp_host::g_checks;                                             \
    if (!(c)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)

namespace cc = chirp_channel;

// Another chirp device in range, with its own session key.
struct Neighbor {
  uint8_t priv[32];
  uint8_t pub[32];
  uint8_t sid[cc::SESSION_ID_SIZE];
  uint8_t mac[6];
};
inline Neighbor neighbor_of(uint8_t seed) {
  Neighbor n;
  memset(n.priv, seed, sizeof n.priv);
  n.priv[0] ^= 0x5A;
  n.priv[31] ^= 0xC3;
  Ed25519::derivePublicKey(n.pub, n.priv);
  cc::session_id_from_pubkey(n.pub, n.sid);
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x02, seed};
  memcpy(n.mac, mac, sizeof mac);
  return n;
}

inline cc::ChirpHeader header_of(const Neighbor& n, cc::ChirpMsgType type, uint8_t nonce_byte) {
  cc::ChirpHeader h;
  memset(&h, 0, sizeof h);
  h.magic = cc::CHIRP_MAGIC;
  h.version = cc::PROTOCOL_VERSION;
  h.msg_type = type;
  memcpy(h.session_id, n.sid, sizeof h.session_id);
  h.timestamp = (uint32_t)host_sim::wall_now;
  memset(h.nonce, nonce_byte, sizeof h.nonce);
  h.nonce[0] = n.mac[5];
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
// loop task.
inline void deliver(const Neighbor& n, const std::vector<uint8_t>& f) {
  cc::dispatch_espnow_message(n.mac, f.data(), (int)f.size(), -60);
}
inline std::vector<uint8_t> presence_of(const Neighbor& n) {
  cc::ChirpPresencePayload p;
  memset(&p, 0, sizeof p);
  p.listening = 1;
  p.last_chirp_age_min = 255;
  return frame_of(header_of(n, cc::CHIRP_MSG_PRESENCE, 0x70), p);
}
// `n`'s signed witness (hop 0), signed as send_chirp() signs one.
inline std::vector<uint8_t> witness_of(const Neighbor& n, uint8_t nonce_byte,
                                       cc::ChirpTemplate tpl = cc::TPL_INFRA_POWER_OUT,
                                       cc::ChirpUrgency urg = cc::CHIRP_URG_CAUTION) {
  const cc::ChirpHeader h = header_of(n, cc::CHIRP_MSG_WITNESS, nonce_byte);
  cc::ChirpWitnessPayload p;
  memset(&p, 0, sizeof p);
  p.template_id = (uint8_t)tpl;
  p.detail_slot = (uint8_t)cc::DETAIL_NONE;
  p.urgency = (uint8_t)urg;
  p.ttl_minutes = 15;
  memcpy(p.session_pubkey, n.pub, sizeof p.session_pubkey);
  uint8_t canonical[256];
  const size_t cl = cc::build_witness_canonical(&h, &p, n.pub, canonical, sizeof canonical);
  CHECK(cl > 0);
  Ed25519::sign(p.signature, n.priv, n.pub, canonical, cl);
  return frame_of(h, p);
}
// `n`'s signed confirmation (CHIRP_MSG_ACK, confirmed) of the chirp `nonce`
// names; `frame_byte` varies the header's nonce, which the signature does not
// cover, so a repeat is a new frame.
inline std::vector<uint8_t> confirm_of(const Neighbor& n, const uint8_t nonce[8],
                                       uint8_t frame_byte = 0x71) {
  const cc::ChirpHeader h = header_of(n, cc::CHIRP_MSG_ACK, frame_byte);
  cc::ChirpAckPayload p;
  memset(&p, 0, sizeof p);
  memcpy(p.original_nonce, nonce, 8);
  p.ack_type = cc::CHIRP_ACK_CONFIRMED;
  memcpy(p.confirmer_session_pubkey, n.pub, sizeof p.confirmer_session_pubkey);
  uint8_t canonical[128];
  const size_t cl =
      cc::build_ack_canonical(nonce, cc::CHIRP_ACK_CONFIRMED, n.pub, canonical, sizeof canonical);
  CHECK(cl > 0);
  Ed25519::sign(p.signature, n.priv, n.pub, canonical, cl);
  return frame_of(h, p);
}
inline void nonce_of(const std::vector<uint8_t>& frame, uint8_t out[8]) {
  memcpy(out, frame.data() + offsetof(cc::ChirpHeader, nonce), 8);
}

// The recent table's row for the chirp `nonce` names, or nullptr.
inline cc::ReceivedChirp* row_of(const uint8_t nonce[8]) {
  for (size_t i = 0; i < cc::g_recent_chirp_count; ++i) {
    if (memcmp(cc::g_recent_chirps[i].nonce, nonce, 8) == 0) return &cc::g_recent_chirps[i];
  }
  return nullptr;
}

// The frames this device sent since `from` (the stub's send log).
inline std::vector<std::vector<uint8_t>> sent_since(size_t from) {
  std::vector<std::vector<uint8_t>> out;
  const auto& sent = host_sim::espnow->sent;
  for (size_t i = from; i < sent.size(); ++i) out.push_back(sent[i].bytes);
  return out;
}
inline std::vector<std::vector<uint8_t>> sent_of_type(size_t from, cc::ChirpMsgType type) {
  std::vector<std::vector<uint8_t>> out;
  for (const std::vector<uint8_t>& f : sent_since(from)) {
    CHECK(f.size() >= sizeof(cc::ChirpHeader));
    if (f[offsetof(cc::ChirpHeader, msg_type)] == type) out.push_back(f);
  }
  return out;
}

// The receiver: a device powering up with its channel on (the owner's
// enable), an hour after the last scene, its wall clock set and its NVS and
// radio empty. With `presence_met`, it has also been on for the presence
// requirement, so it may originate (send, confirm, vote). Returns the
// scene's start.
inline uint32_t boot_enabled(bool presence_met = false) {
  host_sim::nvs->clear();
  *host_sim::espnow = host_sim::EspNow();
  host_sim::wall_now = 1760000000;
  host_sim::millis_step_ms = 0;
  host_sim::now_ms += 3600000u;
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
  cc::g_relays_this_minute = 0;
  cc::g_relay_minute_start = 0;
  cc::g_status_view = decltype(cc::g_status_view)();
  cc::g_nearby_view = decltype(cc::g_nearby_view)();
  cc::g_recent_view = decltype(cc::g_recent_view)();
  cc::g_tables_changed = true;
  CHECK(cc::init());
  CHECK(cc::enable());
  CHECK(cc::g_state == cc::CHIRP_ACTIVE);
  cc::update();
  if (presence_met) {
    host_sim::now_ms += cc::PRESENCE_REQUIRED_MS;
    host_sim::wall_now += cc::PRESENCE_REQUIRED_MS / 1000;
    CHECK(cc::has_presence_requirement());
  }
  return host_sim::now_ms;
}

struct Test {
  const char* name;
  void (*fn)();
};

// Runs every case, or those whose name holds argv[1]; prints the suite's
// pass line (the Makefile's run_marked reads "ALL ... PASSED").
inline int run_tests(const Test* tests, size_t count, const char* suite, int argc, char** argv) {
  const char* only = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (size_t i = 0; i < count; ++i) {
    if (only != nullptr && std::strstr(tests[i].name, only) == nullptr) continue;
    tests[i].fn();
    std::printf("PASS %s\n", tests[i].name);
    ++ran;
  }
  CHECK(ran > 0);
  if (only != nullptr) {
    std::printf("%d of %zu tests run (%d checks), filter \"%s\"\n", ran, count, g_checks, only);
    return 0;
  }
  std::printf("ALL %d %s checks PASSED (%d tests)\n", g_checks, suite, ran);
  return 0;
}

}  // namespace chirp_host

#endif  // CHIRP_HOST_HARNESS_H
