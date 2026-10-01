// A few canary-wap devices on one host, running the REAL mesh_network.cpp.
//
// mesh_network.cpp keeps its state in file statics (one device per image),
// so this header is included in the same translation unit as the .cpp
// (#include MESH_NETWORK_CPP first) and swaps those statics per device:
// become(d) saves the current device's statics and loads d's, together
// with d's ESP-NOW peer list, sent-frame log and NVS store (the stubs in
// stubs/mesh_net). Frames move between devices as bytes: a device's sends
// land in its EspNow::sent log, and deliver() hands bytes to a receiver
// through its registered ESP-NOW receive callback and one update() pass,
// the path a frame takes on a device. Nothing here re-implements a wire
// format, a signature or a gate.

#ifndef MESH_NET_SIM_H
#define MESH_NET_SIM_H

#include <cstdio>
#include <cstring>
#include <vector>

namespace mesh_net_sim {

namespace mn = mesh_network;

struct Device {
  const char* name = "";
  uint8_t priv[32] = {};
  uint8_t pub[32] = {};
  uint8_t mac[6] = {};
  // mesh_network.cpp's statics, as last saved.
  bool initialized = false;
  uint8_t fingerprint[mn::FINGERPRINT_SIZE] = {};
  char device_name[mn::MAX_PEER_NAME_LEN + 1] = {};
  mn::OperaConfig cfg = {};
  mn::OperaPeer peers[mn::MAX_OPERA_SIZE] = {};
  uint8_t peer_count = 0;
  mesh_revocation::List revoked = {};
  bool revoked_stored = false;
  mn::MeshState state = mn::MESH_DISABLED;
  bool espnow_initialized = false;
  uint32_t messages_sent = 0, messages_received = 0, message_errors = 0;
  uint32_t auth_failures = 0, start_time_ms = 0;
  uint32_t last_heartbeat_ms = 0, last_peer_check_ms = 0;
  uint32_t storm_window_start_ms = 0, storm_window_count = 0;
  uint32_t storm_pause_until_ms = 0, storm_trigger_count = 0;
  mn::RekeyState rekey = {};
  mn::PairingSession pairing = {};
  // The device's radio and flash.
  host_sim::EspNow espnow;
  host_sim::NvsStore nvs;
};

inline Device* g_cur = nullptr;

inline void save(Device& d) {
  d.initialized = mn::g_initialized;
  memcpy(d.fingerprint, mn::g_device_fingerprint, sizeof d.fingerprint);
  memcpy(d.device_name, mn::g_device_name, sizeof d.device_name);
  d.cfg = mn::g_opera_config;
  memcpy(d.peers, mn::g_peers, sizeof d.peers);
  d.peer_count = mn::g_peer_count;
  d.revoked = mn::g_revoked;
  d.revoked_stored = mn::g_revoked_stored;
  d.state = mn::g_mesh_state;
  d.espnow_initialized = mn::g_espnow_initialized;
  d.messages_sent = mn::g_messages_sent;
  d.messages_received = mn::g_messages_received;
  d.message_errors = mn::g_message_errors;
  d.auth_failures = mn::g_auth_failures;
  d.start_time_ms = mn::g_start_time_ms;
  d.last_heartbeat_ms = mn::g_last_heartbeat_ms;
  d.last_peer_check_ms = mn::g_last_peer_check_ms;
  d.storm_window_start_ms = mn::g_storm_window_start_ms;
  d.storm_window_count = mn::g_storm_window_count;
  d.storm_pause_until_ms = mn::g_storm_pause_until_ms;
  d.storm_trigger_count = mn::g_storm_trigger_count;
  d.rekey = mn::g_rekey;
  d.pairing = mn::g_pairing;
}

inline void load(Device& d) {
  mn::g_initialized = d.initialized;
  mn::g_device_privkey = d.priv;
  mn::g_device_pubkey = d.pub;
  memcpy(mn::g_device_fingerprint, d.fingerprint, sizeof d.fingerprint);
  memcpy(mn::g_device_name, d.device_name, sizeof d.device_name);
  mn::g_opera_config = d.cfg;
  memcpy(mn::g_peers, d.peers, sizeof d.peers);
  mn::g_peer_count = d.peer_count;
  mn::g_revoked = d.revoked;
  mn::g_revoked_stored = d.revoked_stored;
  mn::g_mesh_state = d.state;
  mn::g_espnow_initialized = d.espnow_initialized;
  mn::g_messages_sent = d.messages_sent;
  mn::g_messages_received = d.messages_received;
  mn::g_message_errors = d.message_errors;
  mn::g_auth_failures = d.auth_failures;
  mn::g_start_time_ms = d.start_time_ms;
  mn::g_last_heartbeat_ms = d.last_heartbeat_ms;
  mn::g_last_peer_check_ms = d.last_peer_check_ms;
  mn::g_storm_window_start_ms = d.storm_window_start_ms;
  mn::g_storm_window_count = d.storm_window_count;
  mn::g_storm_pause_until_ms = d.storm_pause_until_ms;
  mn::g_storm_trigger_count = d.storm_trigger_count;
  mn::g_rekey = d.rekey;
  mn::g_pairing = d.pairing;
  mn::g_rx_pending = false;
  host_sim::espnow = &d.espnow;
  host_sim::nvs = &d.nvs;
}

inline void become(Device& d) {
  if (g_cur == &d) return;
  if (g_cur != nullptr) save(*g_cur);
  load(d);
  g_cur = &d;
}

// A device powering up: RAM gone, flash and identity kept. Runs the real
// mesh_network::init() (NVS opera config, peers, deny-list) and the
// sketch's load_replay_counters() right after it, as canary_wap.ino does.
// The broadcast address is registered here because on a device something
// else registers it: csi_probe::init (the CSI active probe the WAP brings up
// whenever csi_hal runs), and chirp_channel's or beacon_channel's broadcast
// sends. mesh_network.cpp never registers it itself, and its channel-change
// listener deletes it; of those three only chirp and beacon add it back, so
// after a channel change a pairing DISCOVER relies on one of them.
inline void boot(Device& d) {
  if (g_cur != nullptr && g_cur != &d) save(*g_cur);
  Device fresh;
  fresh.name = d.name;
  memcpy(fresh.priv, d.priv, sizeof fresh.priv);
  memcpy(fresh.pub, d.pub, sizeof fresh.pub);
  memcpy(fresh.mac, d.mac, sizeof fresh.mac);
  fresh.nvs = d.nvs;
  d = fresh;
  load(d);
  g_cur = &d;
  mn::init(d.priv, d.pub, d.name);
  mn::load_replay_counters();
  // The channel policy's first poll reports a channel change, and the
  // listener init() registered drops the broadcast registration. Settle it
  // here (the policy is one per image, and every simulated device shares
  // the channel), then register broadcast (see above), so no update()
  // later drops it from under a test.
  mesh_channel_policy::poll_radio();
  esp_now_peer_info_t bc = {};
  memset(bc.peer_addr, 0xFF, 6);
  if (!esp_now_is_peer_exist(bc.peer_addr)) esp_now_add_peer(&bc);
}

inline void make_device(Device& d, const char* name, uint8_t mac_last) {
  d.name = name;
  host_sim::fill_random(d.priv, 32);
  Ed25519::derivePublicKey(d.pub, d.priv);
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, mac_last};
  memcpy(d.mac, mac, 6);
}

// Deliver bytes to `to` as if they arrived on its radio from `src`: the
// ESP-NOW receive callback (which queues them), then one update() pass
// (which processes them), as on a device.
inline void deliver(Device& to, const uint8_t src[6], const std::vector<uint8_t>& bytes) {
  become(to);
  wifi_pkt_rx_ctrl_t ctrl = {-50};
  uint8_t s[6], dst[6];
  memcpy(s, src, 6);
  memcpy(dst, to.mac, 6);
  esp_now_recv_info_t info = {s, dst, &ctrl};
  if (to.espnow.recv_cb != nullptr) to.espnow.recv_cb(&info, bytes.data(), (int)bytes.size());
  mn::update();
}

// The frames `from` sent to `to_mac`, oldest first.
inline std::vector<std::vector<uint8_t>> sent_to(const Device& from, const uint8_t to_mac[6]) {
  std::vector<std::vector<uint8_t>> out;
  for (const host_sim::Sent& s : from.espnow.sent) {
    if (memcmp(s.to.data(), to_mac, 6) == 0) out.push_back(s.bytes);
  }
  return out;
}

// The signed header's counter (bytes 26..33, little-endian; spec §4.5).
inline uint64_t counter_of(const std::vector<uint8_t>& f) {
  uint64_t c = 0;
  for (int i = 0; i < 8; ++i) c |= (uint64_t)f[26 + i] << (8 * i);
  return c;
}

// The peer entry `self` holds for `other` (by fingerprint), or nullptr.
inline mn::OperaPeer* entry(Device& self, const Device& other) {
  become(self);
  uint8_t fp[mn::FINGERPRINT_SIZE];
  mn::compute_fingerprint(other.pub, fp);
  return mn::find_peer_by_fingerprint(fp);
}

inline bool same_mac(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 6) == 0; }

}  // namespace mesh_net_sim

#endif  // MESH_NET_SIM_H
