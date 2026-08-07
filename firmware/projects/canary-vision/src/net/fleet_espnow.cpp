// src/net/fleet_espnow.cpp — fleet-link presence beacon, ESP-NOW carrier.
//
// See canary/net/fleet_espnow.h for what this band is for, and
// firmware/common/fleet_link/fleet_beacon_espnow.h for the wire contract and
// channel policy. This file is transmission only: the bytes come from
// fleet_beacon_payload.cpp, identical to the ones the BLE advert and the LAN
// datagram carry. canary-display's espnow_peer.cpp is the receive half and
// has parsed exactly these frames since it was written — this file is the
// transmitter that had never existed.

#include "canary/net/fleet_espnow.h"

#include "canary/config.h"

#if defined(FEATURE_FLEET_ESPNOW) && FEATURE_FLEET_ESPNOW

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#include <fleet_beacon_espnow.h>  // cadence / channel contract (common/fleet_link)

#include "canary/net/fleet_beacon_payload.h"
#include "canary/net/wifi_mgr.h"
#include "canary/log.h"

namespace canary::net {

namespace {

bool     s_ready     = false;
uint32_t s_next_ms   = 0;
uint32_t s_last_gen  = 0;
uint32_t s_sent      = 0;
bool     s_announced = false;

const uint8_t kBcast[6] = FLEET_BEACON_ESPNOW_BCAST_ADDR;

// Park a never-provisioned radio on the fallback channel so a boxed pair
// meets with zero configuration. Only when there are no credentials at all:
// while the reconnect machinery owns the radio (configured, temporarily
// down) or the STA is associated, retuning under it would be the exact
// channel fight mesh_channel_policy.h exists to prevent.
void park_channel_if_idle() {
  if (wifi_configured()) return;
  uint8_t ch = 0;
  wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&ch, &sec) != ESP_OK) return;
  if (ch == FLEET_BEACON_ESPNOW_FALLBACK_CHANNEL) return;
  esp_wifi_set_channel(FLEET_BEACON_ESPNOW_FALLBACK_CHANNEL,
                       WIFI_SECOND_CHAN_NONE);
}

void send_now(uint32_t now) {
  uint8_t mfg[FLEET_BEACON_MFG_V2_LEN];
  const size_t n = fleet_beacon_payload_build(mfg);

  // Whether this pass is an edge republish (generation moved) or a routine
  // refresh — read BEFORE the send so the log below can say which.
  const uint32_t gen = fleet_beacon_payload_generation();
  const bool edge = (gen != s_last_gen);

  if (esp_now_send(kBcast, mfg, n) != ESP_OK) {
    // The driver refused (radio mid-reassociation, queue full). Nothing to
    // unwind — the next tick simply tries again; a presence beacon owes no
    // delivery guarantee and must never be the reason a rejoin stalls.
    return;
  }

  s_sent++;
  s_last_gen = gen;
  s_next_ms = now + FLEET_BEACON_ESPNOW_REFRESH_MS;

  if (edge) {
    // The trigger-timing number the bench story runs on: FSM edge to air.
    // Everything before it (NPU invoke time) is logged by vision_tick; a
    // receiving display measures its own radio-to-glass time. No shared
    // clock is claimed anywhere — each side reports only what it measured.
    canary::log_header("PAIR");
    canary::dbg_serial().printf(
        "detection edge on air (espnow) %lu ms after the FSM flipped\n",
        (unsigned long)(now - fleet_beacon_payload_edge_ms()));
  }

  if (!s_announced) {
    s_announced = true;
    canary::log_line("BEACON",
                     "Fleet-link presence beacon on ESP-NOW (direct radio, router-free).");
  }
}

}  // namespace

void fleet_espnow_begin(uint32_t /*now*/) {
  if (s_ready) return;
  // ESP-NOW rides the already-started WiFi driver (wifi_init_or_reboot puts
  // the MAC in STA mode even with no credentials). Failure degrades to a
  // silent no-op: the other carriers keep speaking.
  if (esp_now_init() != ESP_OK) {
    log_line("ESPNOW", "init failed - BLE/LAN remain the beacon carriers");
    return;
  }
  // Broadcast peer, channel 0 = "whatever the radio is on" — the channel
  // policy lives in park_channel_if_idle()/the STA, never in the peer entry
  // (the mesh_channel_policy lesson).
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, kBcast, sizeof(kBcast));
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    esp_now_deinit();
    log_line("ESPNOW", "peer add failed - BLE/LAN remain the beacon carriers");
    return;
  }
  s_ready = true;
}

void fleet_espnow_tick(uint32_t now) {
  if (!s_ready) return;

  park_channel_if_idle();

  // A detection edge goes out immediately; otherwise ride the refresh.
  if (fleet_beacon_payload_generation() != s_last_gen) {
    send_now(now);
    return;
  }
  if ((int32_t)(now - s_next_ms) < 0) return;
  send_now(now);
}

uint32_t fleet_espnow_sent() { return s_sent; }

}  // namespace canary::net

#else  // FEATURE_FLEET_ESPNOW off — no-op stubs (per-board size-guard off switch)

namespace canary::net {
void fleet_espnow_begin(uint32_t) {}
void fleet_espnow_tick(uint32_t) {}
uint32_t fleet_espnow_sent() { return 0; }
}  // namespace canary::net

#endif  // FEATURE_FLEET_ESPNOW
