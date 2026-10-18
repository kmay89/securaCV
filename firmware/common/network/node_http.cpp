// common/network/node_http.cpp — the headless Canaries' small HTTP surface.
//
// See node_http.h. Compiled directly by each adopting board's build
// (build_src_filter, beside setup_portal.cpp), so it must stay core-2.x AND
// core-3.x clean. The server exists only while the STA link is up and the
// setup portal (which owns port 80) is down; it is torn down on either
// change and rebuilt on the next, so a board that bounces between the two
// never has two listeners on one port.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <string.h>

#include "network/node_http.h"

namespace canary {
namespace net {
namespace node_http {

namespace {

Hooks s_hooks{};
WebServer* s_server = nullptr;
uint32_t s_last_fleet_poll_ms = 0;
bool s_hooked = false;

void send_cors() {
  s_server->sendHeader("Access-Control-Allow-Origin", "*");
  s_server->sendHeader("Cache-Control", "no-store");
}

void handle_status() {
  // Static: the body belongs in BSS, not on the handler's stack, and the
  // cap is the header's worst case so truncation is impossible.
  static char body[NODE_STATUS_BODY_CAP];
  NodeStatus st{};
  st.setup_ap = -1;
  st.ble_door = -1;
  st.saved_networks = -1;
  st.peers_seen = -1;
  st.wall_polled_s_ago = -1;
  st.ota_pending_verify = -1;
  if (s_hooks.fill_status) s_hooks.fill_status(st);
  node_status_build(body, sizeof(body), &st);
  send_cors();
  s_server->send(200, "application/json", body);
}

void handle_fleet() {
  s_last_fleet_poll_ms = millis();
  if (s_last_fleet_poll_ms == 0) s_last_fleet_poll_ms = 1;  // 0 = never
  static char body[FLEET_SELFREPORT_BODY_CAP(48, 40)];
  FleetSelfDevice self{};
  self.chain_height = -1;
  if (s_hooks.fill_fleet) s_hooks.fill_fleet(self);
  const char* kernel = s_hooks.kernel_name ? s_hooks.kernel_name() : nullptr;
  size_t o = fleet_selfreport_open(body, sizeof(body), kernel ? kernel : (self.name ? self.name : "Canary"));
  o = fleet_selfreport_append_device(body, sizeof(body), o, &self);
  o = fleet_selfreport_close(body, sizeof(body), o);
  (void)o;
  send_cors();
  s_server->send(200, "application/json", body);
}

void handle_fleet_options() {
  s_server->sendHeader("Access-Control-Allow-Origin", "*");
  s_server->sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  s_server->send(204, "text/plain", "");
}

void start() {
  if (s_server != nullptr) return;
  s_server = new WebServer(80);
  s_server->on("/api/status", HTTP_GET, handle_status);
  s_server->on("/api/fleet", HTTP_GET, handle_fleet);
  s_server->on("/api/fleet", HTTP_OPTIONS, handle_fleet_options);
  s_server->onNotFound([]() { s_server->send(404, "text/plain", "not here"); });
  s_server->begin();
  Serial.println("[HTTP] /api/status and /api/fleet serving on :80");
}

void stop() {
  if (s_server == nullptr) return;
  s_server->stop();
  delete s_server;
  s_server = nullptr;
  Serial.println("[HTTP] Node surface down (link lost or setup portal up)");
}

}  // namespace

void begin(const Hooks& hooks) {
  s_hooks = hooks;
  s_hooked = true;
}

void loop(uint32_t now_ms, bool sta_online, bool portal_active) {
  (void)now_ms;
  if (!s_hooked) return;
  const bool want = sta_online && !portal_active;
  if (want && s_server == nullptr) start();
  if (!want && s_server != nullptr) stop();
  if (s_server != nullptr) s_server->handleClient();
}

bool serving() { return s_server != nullptr; }

uint32_t last_fleet_poll_ms() { return s_last_fleet_poll_ms; }

int fleet_polled_s_ago(uint32_t now_ms) {
  if (s_last_fleet_poll_ms == 0) return -1;
  const int32_t d = (int32_t)(now_ms - s_last_fleet_poll_ms);
  return d < 0 ? 0 : d / 1000;
}

bool wall_polling(uint32_t now_ms) {
  if (s_last_fleet_poll_ms == 0) return false;
  return (int32_t)(now_ms - s_last_fleet_poll_ms) < (int32_t)WALL_DIRECT_WINDOW_MS;
}

}  // namespace node_http
}  // namespace net
}  // namespace canary
