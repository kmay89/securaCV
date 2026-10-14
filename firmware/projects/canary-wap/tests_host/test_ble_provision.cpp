// The BLE provisioning service (ble_provision.cpp), compiled for real over
// the Bluetooth harness's NimBLE and ArduinoJson stand-ins (stubs/bt) and a
// WiFi stand-in (stubs/ble_prov), for the F262 review's two findings on it.
//
// The scan list. A phone triggers a scan and reads the list from the
// SCAN_RESULTS characteristic, which holds MAX_RESULTS_PAYLOAD (480) bytes of
// it at most. F262 writes each SSID's control bytes as U+FFFD (three bytes
// each), and the old trim, taken when 20 rows did not fit, kept 8 rows
// without measuring them again: 639 bytes for 8 names of 32 characters, 1151
// for 8 names of 32 control bytes. The list now drops rows from the end, the
// weakest, until it fits, each row whole; here every list a scan of 0 to 25
// networks of every shape makes fits, parses as a browser reads it, keeps
// the strongest rows in order, and keeps as many as fit (one more does not).
//
// The way back. The phone shows the list and posts the text it showed back
// in CREDS, U+FFFD and all, which no access point broadcasts; the device
// joins the listed network that text names by the network's own bytes
// (wap_json::listed_by_shown() over the rows the phone was sent). Here the
// phone is modeled whole: the SCAN_RESULTS bytes decoded as a browser's
// TextDecoder decodes them (browser_utf8.h), JSON.parse (json_strict.h), the
// row's SSID JSON.stringify'd into the CREDS write. A name holding 0x01, a
// Latin-1 name, the 32-byte name of 0x01 (96 bytes of text) and a valid
// UTF-8 name each reach the join by their own bytes; a name the list did not
// show is joined as posted; a row the budget dropped is not mapped.
//
// Against ble_provision.cpp before the review the list check fails (the 8
// kept rows are 639 or 1151 bytes) and every CREDS case whose post differs
// from the name fails (the join gets the U+FFFD text, or the 96-byte text is
// refused as too long).
#ifndef BLE_PROVISION_CPP
#error "BLE_PROVISION_CPP (absolute path to ble_provision.cpp) must be defined"
#endif

// Arduino.h's global names first, as in the sketch's translation unit.
#include "arduino_globals.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Arduino.h"
#include "NimBLEDevice.h"
#include "WiFi.h"
#include "log_level.h"

// The sketch's side of the module (canary_wap.ino): the join it asks for,
// recorded, with the sketch's own bounds (1 to 32 bytes, a password of 64 at
// most), and the health log, which nothing here reads.
static std::vector<std::string> g_joins;
bool ble_request_wifi_provisioning(const char* ssid, const char* password) {
  if (!ssid || !password) return false;
  const size_t ssid_len = strlen(ssid);
  if (ssid_len == 0 || ssid_len > 32 || strlen(password) > 64) return false;
  g_joins.push_back(ssid);
  return true;
}
void log_health(LogLevel, LogCategory, const char*, const char*) {}

#include BLE_PROVISION_CPP

#include "browser_utf8.h"
#include "json_strict.h"

namespace {

int g_checks = 0;
int g_failures = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      ++g_failures;                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);            \
    }                                                                     \
  } while (0)

NimBLEServer g_server;

// A scan the phone triggered, finished with `nets` (strongest first, as the
// core sorts them), and the list the module published for it.
std::string scan(const std::vector<HostNetwork>& nets) {
  WiFi.networks = nets;
  WiFi.scan_done = false;
  ble_provision::g_scan_trigger->host_write("1");
  WiFi.scan_done = true;
  ble_provision::tick();
  return ble_provision::g_scan_results->getValue();
}

// The list as the phone's page reads it: decoded the WHATWG way, then
// JSON.parse. False when it does not parse.
bool phone_reads(const std::string& list, json_strict::Value* out) {
  return json_strict::parse(browser_utf8(list), out);
}

// JSON.stringify of a string the page holds (as UTF-8): `"` `\` and the
// control characters escaped (\b \f \n \r \t by name, the rest \u00XX), every
// other character as it is.
std::string stringify(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char e[8];
          std::snprintf(e, sizeof e, "\\u%04x", c);
          out += e;
        } else {
          out += (char)c;
        }
    }
  }
  return out + "\"";
}

// The phone's CREDS write of `ssid` (as the page holds it) and a password,
// an hour after the last (past the 5 s cooldown and the hourly cap). The join
// it led to, or "\x01none" when the module refused it.
std::string creds(const std::string& ssid, const std::string& password = "hunter22") {
  host_sim::now_ms += 61u * 60u * 1000u;
  g_joins.clear();
  ble_provision::g_creds->host_write("{\"ssid\":" + stringify(ssid) + ",\"password\":" + stringify(password) + "}");
  return g_joins.size() == 1 ? g_joins[0] : std::string("\x01none");
}

std::string fill(size_t n, char c) { return std::string(n, c); }

// ── The list fits its budget ──────────────────────────────────────────────

// Every list a scan of 0 to 25 networks makes, for names of every shape: at
// most MAX_RESULTS_PAYLOAD bytes; parses as the phone reads it; its rows are
// the scan's first networks, in order, each whole (the name as replaced_text()
// writes it, the RSSI and the security); `truncated` exactly when rows were
// dropped for the budget; and it keeps as many rows as fit: one more is over.
void test_the_list_fits_its_budget() {
  const std::vector<std::string> shapes = {
      fill(32, '\x01'),                          // 96 bytes of text each
      fill(32, 'W'),                             // the old trim's 639 bytes
      "kit\x01" "chen",                          // short, one replaced byte
      fill(32, '"'),                             // two bytes each in JSON
      "Caf\xE9",                                 // not UTF-8: as it is
  };
  for (size_t shape = 0; shape < shapes.size(); ++shape) {
    for (int n = 0; n <= 25; ++n) {
      std::vector<HostNetwork> nets;
      for (int i = 0; i < n; ++i) {
        std::string name = shapes[shape];
        if (shape == 2 || shape == 4) name += std::to_string(i);
        nets.push_back({name, -30 - i, i % 2 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN});
      }
      const std::string list = scan(nets);
      CHECK(list.size() <= ble_provision::MAX_RESULTS_PAYLOAD);
      json_strict::Value v;
      CHECK(phone_reads(list, &v));
      const json_strict::Value* aps = v.get("aps");
      const json_strict::Value* count = v.get("count");
      CHECK(aps != nullptr && aps->kind == json_strict::Value::Array);
      CHECK(count != nullptr && count->text == std::to_string(n));
      if (aps == nullptr) continue;
      const int rows = (int)aps->items.size();
      CHECK(rows <= n && rows <= 20);
      const bool dropped = rows < (n < 20 ? n : 20);
      const json_strict::Value* truncated = v.get("truncated");
      CHECK((truncated != nullptr && truncated->kind == json_strict::Value::Bool && truncated->b) == dropped);
      for (int i = 0; i < rows; ++i) {
        const json_strict::Value* ssid = aps->items[i].get("ssid");
        const json_strict::Value* rssi = aps->items[i].get("rssi");
        const json_strict::Value* sec = aps->items[i].get("sec");
        char want[wap_json::replaced_cap(32)];
        CHECK(wap_json::replaced_text(want, sizeof(want), nets[i].ssid.c_str()));
        CHECK(ssid != nullptr && ssid->text == browser_utf8(want));
        CHECK(rssi != nullptr && rssi->text == std::to_string(nets[i].rssi));
        CHECK(sec != nullptr && sec->text == (i % 2 ? "wpa2" : "open"));
      }
      // One more row would not fit.
      if (dropped) {
        static char names[20][33];
        for (int i = 0; i <= rows; ++i) {
          std::strncpy(names[i], nets[i].ssid.c_str(), 32);
          names[i][32] = '\0';
        }
        CHECK(ble_provision::scan_list_json(n, 0, names, rows + 1, true).length() > ble_provision::MAX_RESULTS_PAYLOAD);
      }
      // The rows the phone was sent are the ones CREDS reads back.
      CHECK(ble_provision::g_listed_count == rows);
    }
  }
  // Names of 32 control bytes: the old trim kept 8 rows, 1151 bytes; now
  // three rows (466 bytes; four would be 603).
  std::vector<HostNetwork> eight;
  for (int i = 0; i < 25; ++i) eight.push_back({fill(32, (char)(1 + i % 8)), -40 - i, WIFI_AUTH_WPA2_PSK});
  json_strict::Value v;
  CHECK(phone_reads(scan(eight), &v) && v.get("aps") != nullptr && v.get("aps")->items.size() == 3);
}

// ── CREDS joins the listed network by its own bytes ───────────────────────

// The SSID of row `i` as the phone's page holds it.
std::string shown_row(const std::string& list, size_t i) {
  json_strict::Value v;
  if (!phone_reads(list, &v)) return "\x01unparsed";
  const json_strict::Value* aps = v.get("aps");
  if (aps == nullptr || i >= aps->items.size()) return "\x01norow";
  const json_strict::Value* ssid = aps->items[i].get("ssid");
  return ssid != nullptr ? ssid->text : std::string("\x01nossid");
}

void test_creds_join_the_listed_network_by_its_own_bytes() {
  const std::vector<HostNetwork> nets = {
      {"kit\x01" "chen", -40, WIFI_AUTH_WPA2_PSK},
      {"Caf\xE9", -50, WIFI_AUTH_WPA2_PSK},
      {fill(32, '\x01'), -55, WIFI_AUTH_OPEN},
      {"Caf\xC3\xA9 \"2\"", -60, WIFI_AUTH_WPA2_PSK},
      {"a\tb", -65, WIFI_AUTH_WPA2_PSK},
  };
  const std::string list = scan(nets);
  CHECK(ble_provision::g_listed_count == 5);
  for (size_t i = 0; i < nets.size(); ++i) {
    const std::string shown = shown_row(list, i);
    CHECK(shown[0] != '\x01');
    // The page shows the replaced name; the join is the network's own bytes.
    CHECK(creds(shown) == nets[i].ssid);
  }
  CHECK(shown_row(list, 2).size() == 96);
  // A name the list did not show is joined as posted, its own bytes.
  CHECK(creds("Neighbor") == "Neighbor");
  CHECK(creds("kit\xEF\xBF\xBD" "che") == "kit\xEF\xBF\xBD" "che");
  // A post longer than 32 bytes that names no listed network is refused.
  CHECK(creds(fill(33, 'x')) == "\x01none");
  // A network that holds U+FFFD itself is its own text, ahead of one that
  // only shows as it.
  const std::string fffd = "kit\xEF\xBF\xBD" "chen";
  scan({{"kit\x02" "chen", -40, WIFI_AUTH_WPA2_PSK}, {fffd, -45, WIFI_AUTH_WPA2_PSK}});
  CHECK(creds(fffd) == fffd);
  // A row the budget dropped was never shown, so its text names nothing: the
  // 94-byte text of "E" and 31 bytes of 0x01, past the third row, is refused.
  // (Names that differ only in their control bytes show as one text, and
  // that text names the first of them.)
  std::vector<HostNetwork> many;
  for (int i = 0; i < 6; ++i) many.push_back({std::string(1, (char)('A' + i)) + fill(31, '\x01'), -40 - i, WIFI_AUTH_WPA2_PSK});
  const std::string trimmed = scan(many);
  CHECK(ble_provision::g_listed_count == 3);
  CHECK(creds(shown_row(trimmed, 0)) == many[0].ssid);
  char dropped[wap_json::replaced_cap(32)];
  CHECK(wap_json::replaced_text(dropped, sizeof(dropped), many[4].ssid.c_str()));
  CHECK(creds(dropped) == "\x01none");
  // A later scan replaces the rows: the old list's text names nothing now.
  scan({{"Shed", -40, WIFI_AUTH_OPEN}});
  CHECK(creds(shown_row(list, 0)) == "kit\xEF\xBF\xBD" "chen");
  CHECK(ble_provision::g_listed_count == 1);
}

}  // namespace

int main() {
  CHECK(ble_provision::init(&g_server));
  test_the_list_fits_its_budget();
  test_creds_join_the_listed_network_by_its_own_bytes();
  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL %d ble_provision checks PASSED\n", g_checks);
  return 0;
}
