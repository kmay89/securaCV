// src/net/wx_direct.cpp — the standalone-weather fetcher. See the header.
//
// Not compiled into the emulator (no sockets in the wasm shim) and compiled
// to nothing unless the flavor carries FEATURE_STANDALONE_WEATHER — today
// the 7" bedside/wall glass, whose faces already render the hub blob this
// fetcher impersonates.
#include <config.h>
#if defined(FEATURE_STANDALONE_WEATHER) && FEATURE_STANDALONE_WEATHER && \
    defined(FEATURE_HUB_WEATHER) && FEATURE_HUB_WEATHER &&               \
    !defined(EMU_BUILD_FLAVOR)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <time.h>

#include "canary/net/wx_direct.h"
#include "canary/net/wx_core.h"
#include "canary/net/wifi_mgr.h"
#include "canary/net/mqtt_mgr.h"
#include "canary/care/bedside.h"
#include "canary/glass_settings.h"
#include "canary/log.h"

namespace canary::net {

namespace {

// ISRG Root X1 — the root open-meteo.com chains to (Let's Encrypt). Pinned
// rather than setInsecure(): a weather line is visual-only, but "no
// downgrade paths" is a rule, not a per-feature judgment call. If the
// service ever moves CAs the fetch fails VISIBLY (status 4), never quietly
// accepts an unverified peer.
constexpr const char kIsrgRootX1[] = R"(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)";

constexpr uint32_t WX_FIRST_MS = 30000;      // let WiFi settle after boot
constexpr uint32_t WX_FETCH_MS = 45UL * 60UL * 1000UL;
constexpr uint32_t WX_RETRY_MS = 5UL * 60UL * 1000UL;

uint32_t s_next_ms = WX_FIRST_MS;
uint32_t s_last_ok_ms = 0;
bool s_have_ok = false;
bool s_last_failed = false;

// Read a daily array slot as x10 int; absent -> fallback.
int day10(JsonVariantConst arr, int idx, int fallback) {
  JsonVariantConst v = arr[idx];
  if (v.isNull()) return fallback;
  return (int)lroundf(v.as<float>() * 10.0f);
}

bool fetch_once() {
  const auto& gs = canary::glass::settings();
  char path[320];
  wx_url_path(gs.wx_lat10, gs.wx_lon10, path, sizeof(path));

  WiFiClientSecure client;
  client.setCACert(kIsrgRootX1);
  // Tight deadlines: this runs on the loop task at a 45-minute cadence, so
  // the worst a dead network can cost the glass is a few seconds, once.
  client.setTimeout(4000);
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  if (!http.begin(client, WX_HOST, 443, path, /*https=*/true)) return false;
  const int code = http.GET();
  if (code != 200) {
    http.end();
    canary::log_line("WX", "Forecast fetch refused - will retry later.");
    return false;
  }
  JsonDocument doc;
  const auto err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    canary::log_line("WX", "Forecast parse failed - will retry later.");
    return false;
  }

  const int t_now = doc["current"]["temperature_2m"].isNull()
                        ? -10000
                        : (int)lroundf(doc["current"]["temperature_2m"].as<float>() * 10.0f);
  const int code_now = doc["current"]["weather_code"] | -1;
  JsonVariantConst daily = doc["daily"];
  const int hi = day10(daily["temperature_2m_max"], 0, -10000);
  const int lo = day10(daily["temperature_2m_min"], 0, -10000);
  const int hi2 = day10(daily["temperature_2m_max"], 1, -10000);
  const int lo2 = day10(daily["temperature_2m_min"], 1, -10000);
  const int rain = daily["precipitation_probability_max"][0] | -1;
  const int rain2 = daily["precipitation_probability_max"][1] | -1;
  const int code2 = daily["weather_code"][1] | -1;

  char blob[256];
  const int n = wx_blob_build(blob, sizeof(blob), t_now, hi, lo, rain,
                              wx_code_word(code_now), hi2, lo2, rain2,
                              wx_code_word(code2), (uint32_t)time(nullptr));
  canary::care::bedside_on_weather(blob, (unsigned)n);
  return true;
}

}  // namespace

void wx_direct_loop(uint32_t now_ms) {
  const auto& gs = canary::glass::settings();
  if (!gs.wx_direct || !gs.wx_loc_set) return;
  // A home with a hub keeps the hub as the single egress point — this gate
  // is the privacy invariant, not an optimization. Never a fallback.
  if (!mqtt_broker_is_placeholder()) return;
  if (!wifi_connected()) return;
  if (time(nullptr) < 1700000000) return;  // the blob's freshness stamp
                                           // needs a real wall clock
  if ((int32_t)(now_ms - s_next_ms) < 0) return;

  const bool ok = fetch_once();
  s_last_failed = !ok;
  if (ok) {
    s_have_ok = true;
    s_last_ok_ms = now_ms;
    canary::log_line("WX", "Forecast fetched (standalone, coarse location).");
  }
  s_next_ms = now_ms + (ok ? WX_FETCH_MS : WX_RETRY_MS);
}

uint8_t wx_direct_status() {
  const auto& gs = canary::glass::settings();
  if (!mqtt_broker_is_placeholder()) return 2;
  if (!gs.wx_direct) return 0;
  if (!gs.wx_loc_set) return 1;
  return s_last_failed ? 4 : 3;
}

uint16_t wx_direct_age_min(uint32_t now_ms) {
  if (!s_have_ok) return 0xFFFF;
  return (uint16_t)((now_ms - s_last_ok_ms) / 60000UL);
}

}  // namespace canary::net

#endif  // FEATURE_STANDALONE_WEATHER && FEATURE_HUB_WEATHER && !EMU
