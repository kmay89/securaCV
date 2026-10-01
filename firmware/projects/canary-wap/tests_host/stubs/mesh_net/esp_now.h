/* Host stand-in for ESP-IDF's <esp_now.h> (canary-wap mesh_network host
 * harness). The peer list behaves as the driver's does where the receive
 * path depends on it: add refuses a duplicate (ESP_ERR_ESPNOW_EXIST) and a
 * full list (20 entries), del refuses an address it does not hold, and a
 * unicast send to an address that is not registered fails
 * (ESP_ERR_ESPNOW_NOT_FOUND) — so a test sees a member stranded when its
 * registration is gone. Every accepted send is recorded. One instance per
 * simulated device; host_sim::espnow points at the current one. */
#ifndef STUB_MESH_NET_ESP_NOW_H
#define STUB_MESH_NET_ESP_NOW_H

#include <array>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vector>

#include "Arduino.h"   // host_sim::note_side_effect

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#define ESP_FAIL -1
#endif
#define ESP_ERR_ESPNOW_BASE      0x3066
#define ESP_ERR_ESPNOW_NOT_INIT  (ESP_ERR_ESPNOW_BASE + 1)
#define ESP_ERR_ESPNOW_ARG       (ESP_ERR_ESPNOW_BASE + 2)
#define ESP_ERR_ESPNOW_FULL      (ESP_ERR_ESPNOW_BASE + 4)
#define ESP_ERR_ESPNOW_NOT_FOUND (ESP_ERR_ESPNOW_BASE + 5)
#define ESP_ERR_ESPNOW_EXIST     (ESP_ERR_ESPNOW_BASE + 7)
#define ESP_NOW_ETH_ALEN 6
#define ESP_NOW_MAX_TOTAL_PEER_NUM 20

typedef enum { ESP_NOW_SEND_SUCCESS = 0, ESP_NOW_SEND_FAIL } esp_now_send_status_t;
typedef enum { WIFI_IF_STA = 0, WIFI_IF_AP } wifi_interface_t;
typedef struct { int8_t rssi; } wifi_pkt_rx_ctrl_t;
typedef struct {
  uint8_t* src_addr;
  uint8_t* des_addr;
  wifi_pkt_rx_ctrl_t* rx_ctrl;
} esp_now_recv_info_t;
typedef struct {
  const uint8_t* des_addr;
  const uint8_t* src_addr;
} wifi_tx_info_t;
typedef struct {
  uint8_t peer_addr[ESP_NOW_ETH_ALEN];
  uint8_t lmk[16];
  uint8_t channel;
  wifi_interface_t ifidx;
  bool encrypt;
  void* priv;
} esp_now_peer_info_t;
typedef void (*esp_now_recv_cb_t)(const esp_now_recv_info_t*, const uint8_t*, int);
typedef void (*esp_now_send_cb_t)(const wifi_tx_info_t*, esp_now_send_status_t);

namespace host_sim {
using Mac = std::array<uint8_t, 6>;
inline Mac mac_of(const uint8_t* m) { Mac a; memcpy(a.data(), m, 6); return a; }
struct Sent { Mac to; std::vector<uint8_t> bytes; };
struct EspNow {
  bool inited = false;
  std::vector<Mac> peers;
  std::vector<Sent> sent;
  esp_now_recv_cb_t recv_cb = nullptr;
  esp_now_send_cb_t send_cb = nullptr;
  bool has(const uint8_t* m) const {
    for (const Mac& p : peers) if (memcmp(p.data(), m, 6) == 0) return true;
    return false;
  }
};
inline EspNow default_espnow;
inline EspNow* espnow = &default_espnow;
}  // namespace host_sim

inline esp_err_t esp_now_init() { host_sim::espnow->inited = true; return ESP_OK; }
inline esp_err_t esp_now_deinit() { host_sim::espnow->inited = false; return ESP_OK; }
inline esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb) { host_sim::espnow->recv_cb = cb; return ESP_OK; }
inline esp_err_t esp_now_register_send_cb(esp_now_send_cb_t cb) { host_sim::espnow->send_cb = cb; return ESP_OK; }
inline esp_err_t esp_now_unregister_recv_cb() { host_sim::espnow->recv_cb = nullptr; return ESP_OK; }
inline esp_err_t esp_now_unregister_send_cb() { host_sim::espnow->send_cb = nullptr; return ESP_OK; }
inline bool esp_now_is_peer_exist(const uint8_t* mac) { return host_sim::espnow->has(mac); }
inline esp_err_t esp_now_add_peer(const esp_now_peer_info_t* p) {
  host_sim::note_side_effect();
  if (p == nullptr) return ESP_ERR_ESPNOW_ARG;
  if (host_sim::espnow->has(p->peer_addr)) return ESP_ERR_ESPNOW_EXIST;
  if (host_sim::espnow->peers.size() >= ESP_NOW_MAX_TOTAL_PEER_NUM) return ESP_ERR_ESPNOW_FULL;
  host_sim::espnow->peers.push_back(host_sim::mac_of(p->peer_addr));
  return ESP_OK;
}
inline esp_err_t esp_now_del_peer(const uint8_t* mac) {
  host_sim::note_side_effect();
  auto& v = host_sim::espnow->peers;
  for (size_t i = 0; i < v.size(); ++i) {
    if (memcmp(v[i].data(), mac, 6) == 0) { v.erase(v.begin() + (long)i); return ESP_OK; }
  }
  return ESP_ERR_ESPNOW_NOT_FOUND;
}
inline esp_err_t esp_now_send(const uint8_t* mac, const uint8_t* data, size_t len) {
  host_sim::note_side_effect();
  if (!host_sim::espnow->inited) return ESP_ERR_ESPNOW_NOT_INIT;
  if (mac == nullptr || !host_sim::espnow->has(mac)) return ESP_ERR_ESPNOW_NOT_FOUND;
  host_sim::Sent s;
  s.to = host_sim::mac_of(mac);
  s.bytes.assign(data, data + len);
  host_sim::espnow->sent.push_back(s);
  return ESP_OK;
}

#endif
