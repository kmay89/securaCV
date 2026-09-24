// canary-local/emulator/src/emu_radio.cpp — the radio, and the neighborhood
// it hears.
//
// Implements shim/WiFi.h and shim/WiFiUdp.h for the one firmware TU that
// drives them directly: net/provision.cpp, the first-boot SoftAP + captive
// portal, compiled verbatim. The line this file holds is the silicon's: it
// plays the ESP32 radio (a SoftAP with one station slot, an async
// 13-channel sweep, a STA association that takes a moment) and the routers
// around it (which SSIDs are on the air, at what strength, behind which
// key). Everything the portal DOES with that — dedupe and sort the list,
// escape it into JSON, classify a failure, persist only on success, linger
// for the phone's last poll — is the firmware's own code.
//
// The staged LAN is page-owned (emu_set_lan). Its "home" router follows the
// page's Wi-Fi switch, so "unplug the router" takes its SSID off the air for
// the portal exactly as it drops the provisioned supervisor's link. A join
// resolves against it after JOIN_MS of virtual time:
//   SSID not on the air             -> WL_NO_SSID_AVAIL   (firmware: "Network not found")
//   secured and the key is wrong    -> WL_CONNECT_FAILED  (firmware: "Wrong password")
//   otherwise                       -> WL_CONNECTED
// An open network takes any key here; the real driver's auth-mode threshold
// detail is below what the portal can observe.
//
// No packet leaves the machine (Invariant IV): the phone is the page, the
// routers are a list, and the DNS socket only ever hears emu_dns_query().
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <emscripten.h>
#include <string.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "emu_bus.h"

namespace {

EM_JS(void, js_radio_event, (const char* kind, const char* detail), {
  if (Module.onNetEvent) Module.onNetEvent(UTF8ToString(kind), UTF8ToString(detail));
});
EM_JS(void, js_udp_send,
      (int src_port, const char* dst_ip, int dst_port, const char* hex), {
        if (Module.onUdpSend)
          Module.onUdpSend(src_port, UTF8ToString(dst_ip), dst_port,
                           UTF8ToString(hex));
      });

// One WPA2 association + DHCP lease on a real ESP32: a couple of seconds.
constexpr uint32_t JOIN_MS = 1800;
// The 2.4 GHz band the S3's radio sweeps (channels 1-13).
constexpr uint32_t SCAN_CHANNELS = 13;

struct Net {
  std::string ssid;
  int32_t rssi;
  bool secure;
  bool home;  // the page's router: on the air only while the Wi-Fi switch is up
  std::string pass;
};
std::vector<Net> g_lan;

bool on_air(const Net& n) { return !n.home || emu_bus_wifi_up(); }

wifi_mode_t g_mode = WIFI_STA;

// Scan: 0 none, 1 running, 2 results held.
int g_scan_state = 0;
uint32_t g_scan_done_at = 0;
std::vector<Net> g_scan_rows;

// SoftAP.
bool g_ap_up = false;
std::string g_ap_ssid, g_ap_pass;
int g_ap_channel = 1;
int g_ap_max = 4;
int g_stations = 0;

// STA association started by begin(). g_sta_joined is sticky until
// disconnect(): a link the portal made follows the Wi-Fi switch afterwards.
bool g_join_active = false;
bool g_sta_touched = false;  // any begin() since the last disconnect()
uint32_t g_join_at = 0;
std::string g_join_ssid, g_join_pass;
wl_status_t g_sta_status = WL_DISCONNECTED;
bool g_sta_joined = false;

void scan_settle() {
  if (g_scan_state == 1 && (int32_t)(millis() - g_scan_done_at) >= 0) {
    g_scan_rows.clear();
    for (const Net& n : g_lan) {
      if (on_air(n)) g_scan_rows.push_back(n);
    }
    g_scan_state = 2;
  }
}

wl_status_t join_verdict() {
  for (const Net& n : g_lan) {
    if (n.ssid != g_join_ssid || !on_air(n)) continue;
    if (n.secure && n.pass != g_join_pass) return WL_CONNECT_FAILED;
    return WL_CONNECTED;
  }
  return WL_NO_SSID_AVAIL;
}

void sta_settle() {
  if (!g_join_active || (int32_t)(millis() - g_join_at) < (int32_t)JOIN_MS) return;
  g_join_active = false;
  g_sta_status = join_verdict();
  g_sta_joined = g_sta_status == WL_CONNECTED;
  js_radio_event("sta-join",
                 g_sta_joined ? "associated"
                 : g_sta_status == WL_CONNECT_FAILED ? "refused: wrong key"
                                                     : "refused: no such network on the air");
}

const char* hex_of(const std::string& s) {
  static std::string out;
  static const char H[] = "0123456789abcdef";
  out.clear();
  for (unsigned char c : s) {
    out.push_back(H[c >> 4]);
    out.push_back(H[c & 0xF]);
  }
  return out.c_str();
}

bool unhex(const char* in, size_t len, std::string& out) {
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  out.clear();
  if (len % 2) return false;
  for (size_t i = 0; i < len; i += 2) {
    const int hi = nib(in[i]), lo = nib(in[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back((char)((hi << 4) | lo));
  }
  return true;
}

// ── UDP: one inbound queue per bound port ──────────────────────────────
struct Datagram {
  std::vector<uint8_t> bytes;
  uint16_t src_port;
};
std::map<uint16_t, std::deque<Datagram>>& udp_ports() {
  static std::map<uint16_t, std::deque<Datagram>> m;
  return m;
}
// The page's phone on the setup network: the address the AP's DHCP hands
// its first station.
const IPAddress PHONE_IP(192, 168, 4, 2);
uint16_t g_next_phone_port = 50000;

}  // namespace

// ── WiFiClass ────────────────────────────────────────────────────────────
bool WiFiClass::mode(wifi_mode_t m) {
  g_mode = m;
  return true;
}
wifi_mode_t WiFiClass::getMode() { return g_mode; }

wl_status_t WiFiClass::begin(const char* ssid, const char* passphrase) {
  g_join_ssid = ssid ? ssid : "";
  g_join_pass = passphrase ? passphrase : "";
  g_join_active = true;
  g_sta_touched = true;
  g_sta_joined = false;
  g_sta_status = WL_DISCONNECTED;
  g_join_at = millis();
  js_radio_event("sta-join", "associating");
  return g_sta_status;
}

wl_status_t WiFiClass::status() {
  if (!g_sta_touched) {
    // No portal join in play: the emulated supervisor's link IS the page's
    // switch (emu_net.cpp's wifi_mgr contract).
    return emu_bus_wifi_up() ? WL_CONNECTED : WL_DISCONNECTED;
  }
  sta_settle();
  if (g_join_active) return WL_DISCONNECTED;  // association in flight
  if (g_sta_joined) return emu_bus_wifi_up() ? WL_CONNECTED : WL_CONNECTION_LOST;
  return g_sta_status;
}

int32_t WiFiClass::RSSI() { return emu_bus_wifi_rssi(); }

bool WiFiClass::disconnect(bool, bool) {
  if (g_join_active || g_sta_joined) js_radio_event("sta-join", "disconnected");
  g_join_active = false;
  g_sta_touched = false;
  g_sta_joined = false;
  g_sta_status = WL_DISCONNECTED;
  return true;
}

int16_t WiFiClass::scanNetworks(bool async, bool, bool, uint32_t max_ms_per_chan,
                                uint8_t channel) {
  const uint32_t per = max_ms_per_chan ? max_ms_per_chan : 300;
  const uint32_t span = channel ? per : per * SCAN_CHANNELS;
  g_scan_state = 1;
  g_scan_done_at = millis() + span;
  js_radio_event("scan", async ? "sweep started (async)" : "sweep started");
  if (async) return WIFI_SCAN_RUNNING;
  // A blocking sweep: the virtual clock waits it out, as the radio would.
  while (g_scan_state == 1) {
    delay(25);
    scan_settle();
  }
  return (int16_t)g_scan_rows.size();
}

int16_t WiFiClass::scanComplete() {
  scan_settle();
  if (g_scan_state == 1) return WIFI_SCAN_RUNNING;
  if (g_scan_state == 2) return (int16_t)g_scan_rows.size();
  return WIFI_SCAN_FAILED;
}

void WiFiClass::scanDelete() {
  g_scan_rows.clear();
  g_scan_state = 0;
}

String WiFiClass::SSID(uint8_t i) {
  return i < g_scan_rows.size() ? String(g_scan_rows[i].ssid.c_str()) : String("");
}
int32_t WiFiClass::RSSI(uint8_t i) {
  return i < g_scan_rows.size() ? g_scan_rows[i].rssi : 0;
}
wifi_auth_mode_t WiFiClass::encryptionType(uint8_t i) {
  return (i < g_scan_rows.size() && g_scan_rows[i].secure) ? WIFI_AUTH_WPA2_PSK
                                                            : WIFI_AUTH_OPEN;
}

bool WiFiClass::softAP(const char* ssid, const char* passphrase, int channel,
                       int, int max_connection) {
  // The driver's own refusals: no name, or a WPA2 key outside 8..63.
  if (!ssid || !ssid[0] || strlen(ssid) > 32) return false;
  const size_t plen = passphrase ? strlen(passphrase) : 0;
  if (plen && (plen < 8 || plen > 63)) return false;
  g_ap_ssid = ssid;
  g_ap_pass = passphrase ? passphrase : "";
  g_ap_channel = channel;
  g_ap_max = max_connection > 0 ? max_connection : 4;
  g_ap_up = true;
  g_stations = 0;
  char detail[64];
  snprintf(detail, sizeof(detail), "\"%s\" ch %d, %d station max", ssid, channel,
           g_ap_max);
  js_radio_event("softap-up", detail);
  return true;
}

bool WiFiClass::softAPdisconnect(bool) {
  const bool was = g_ap_up;
  g_ap_up = false;
  g_stations = 0;
  if (was) js_radio_event("softap-down", g_ap_ssid.c_str());
  return true;
}

uint8_t WiFiClass::softAPgetStationNum() { return g_ap_up ? (uint8_t)g_stations : 0; }

WiFiClass WiFi;

// ── WiFiUDP ──────────────────────────────────────────────────────────────
uint8_t WiFiUDP::begin(uint16_t port) {
  stop();
  port_ = port;
  udp_ports()[port];  // bound, empty
  return 1;
}

void WiFiUDP::stop() {
  if (port_) udp_ports().erase(port_);
  port_ = 0;
  rx_.clear();
  rx_off_ = 0;
  tx_.clear();
  tx_open_ = false;
}

int WiFiUDP::parsePacket() {
  if (!port_) return 0;
  auto it = udp_ports().find(port_);
  if (it == udp_ports().end() || it->second.empty()) return 0;
  rx_ = std::move(it->second.front().bytes);
  rx_port_ = it->second.front().src_port;
  rx_ip_ = PHONE_IP;
  rx_off_ = 0;
  it->second.pop_front();
  return (int)rx_.size();
}

int WiFiUDP::read(uint8_t* buf, size_t len) {
  const size_t left = rx_.size() - rx_off_;
  const size_t n = len < left ? len : left;
  if (n) memcpy(buf, rx_.data() + rx_off_, n);
  rx_off_ += n;
  return (int)n;
}

int WiFiUDP::beginPacket(IPAddress ip, uint16_t port) {
  tx_.clear();
  tx_ip_ = ip;
  tx_port_ = port;
  tx_open_ = true;
  return 1;
}

size_t WiFiUDP::write(const uint8_t* buf, size_t size) {
  if (!tx_open_) return 0;
  tx_.insert(tx_.end(), buf, buf + size);
  return size;
}

int WiFiUDP::endPacket() {
  if (!tx_open_) return 0;
  tx_open_ = false;
  const std::string bytes(tx_.begin(), tx_.end());
  js_udp_send(port_, tx_ip_.toString().c_str(), tx_port_, hex_of(bytes));
  tx_.clear();
  return 1;
}

// ── The page's side ─────────────────────────────────────────────────────
extern "C" {

// Stage the neighborhood: one network per line,
//   <hex ssid> <rssi dBm> <secure 0|1> <home 0|1> <hex key>
// (hex, so any byte an SSID may legally hold survives the JS boundary).
// Replaces the whole list; an empty spec is an empty band.
EMSCRIPTEN_KEEPALIVE void emu_set_lan(const char* spec) {
  g_lan.clear();
  const char* p = spec ? spec : "";
  while (*p) {
    const char* eol = strchr(p, '\n');
    const std::string line(p, eol ? (size_t)(eol - p) : strlen(p));
    p = eol ? eol + 1 : p + strlen(p);
    char ssid_hex[80] = {0}, key_hex[140] = {0};
    int rssi = 0, secure = 0, home = 0;
    const int got = sscanf(line.c_str(), "%79s %d %d %d %139s", ssid_hex, &rssi,
                           &secure, &home, key_hex);
    if (got < 4) continue;
    Net n;
    if (!unhex(ssid_hex, strlen(ssid_hex), n.ssid) || n.ssid.empty() ||
        n.ssid.size() > 32)
      continue;
    if (got == 5 && strcmp(key_hex, "-") != 0 &&
        !unhex(key_hex, strlen(key_hex), n.pass))
      continue;
    n.rssi = rssi;
    n.secure = secure != 0;
    n.home = home != 0;
    g_lan.push_back(n);
  }
}

// What the glass's join QR carries (WIFI:S:<ssid>;T:WPA;P:<key>;;) — the
// SoftAP the firmware raised, read back from the radio it configured. JSON
// with hex fields; `up` 0 when no setup network is on the air.
EMSCRIPTEN_KEEPALIVE const char* emu_softap_info(void) {
  static std::string out;
  out = "{\"up\":";
  out += g_ap_up ? "1" : "0";
  out += ",\"ssid_hex\":\"";
  out += hex_of(g_ap_ssid);
  out += "\",\"pass_hex\":\"";
  out += hex_of(g_ap_pass);
  out += "\",\"channel\":";
  out += std::to_string(g_ap_channel);
  out += ",\"max\":";
  out += std::to_string(g_ap_max);
  out += ",\"stations\":";
  out += std::to_string(g_ap_up ? g_stations : 0);
  out += "}";
  return out.c_str();
}

// The phone asks the AP to associate. The AP's answer, as a phone sees it:
//   1 joined · 0 no such network on the air · -1 wrong key · -2 AP full.
EMSCRIPTEN_KEEPALIVE int emu_phone_join(const char* ssid, const char* pass) {
  if (!g_ap_up || !ssid || g_ap_ssid != ssid) return 0;
  if (!g_ap_pass.empty() && g_ap_pass != (pass ? pass : "")) {
    js_radio_event("softap-station", "refused: wrong key");
    return -1;
  }
  if (g_stations >= g_ap_max) {
    js_radio_event("softap-station", "refused: station limit");
    return -2;
  }
  g_stations++;
  js_radio_event("softap-station", "phone joined (192.168.4.2)");
  return 1;
}

EMSCRIPTEN_KEEPALIVE void emu_phone_leave(void) {
  if (g_ap_up && g_stations > 0) {
    g_stations--;
    js_radio_event("softap-station", "phone left");
  }
}

// One datagram from the phone to <AP>:<port>. Returns the phone's source
// port (the reply's destination, which Module.onUdpSend reports), or 0 when
// nothing is bound there — the query is lost, as it would be on the air.
EMSCRIPTEN_KEEPALIVE int emu_udp_send_hex(int port, const char* hexbytes) {
  auto it = udp_ports().find((uint16_t)port);
  if (it == udp_ports().end()) return 0;
  std::string bytes;
  if (!hexbytes || !unhex(hexbytes, strlen(hexbytes), bytes)) return 0;
  const uint16_t src = g_next_phone_port++;
  if (g_next_phone_port < 50000) g_next_phone_port = 50000;
  it->second.push_back(Datagram{std::vector<uint8_t>(bytes.begin(), bytes.end()), src});
  return src;
}

}  // extern "C"
