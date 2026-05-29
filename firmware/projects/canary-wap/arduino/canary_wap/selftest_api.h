/*
 * SecuraCV Canary — Hardware self-test (Apple-pit-stop wizard step)
 *
 * One-shot probe of every subsystem the wizard needs to confirm before
 * handing the device back to the user. The wizard polls /api/selftest
 * once per setup run; the dashboard's Diagnostics card can re-poll on
 * demand.
 *
 * Probes (in run order):
 *   wifi        — STA connection state, RSSI, SSID
 *   camera      — sensor present + initialized + last init err
 *   bluetooth   — NimBLE stack up + at least one sub-feature active
 *   sd          — mounted + free space
 *   microphone  — present iff PDM mic compiled in (currently absent on
 *                 canary-wap; reports ABSENT cleanly so the UI greys it)
 *   gpio        — boot button readable + not stuck (sanity)
 *
 * Header-only on purpose, matching the *_api.h pattern already in this
 * directory (bluetooth_api.h, chirp_api.h, audible_chirp_api.h, …).
 * Include from canary_wap.ino AFTER hardware_state.h, ble_manager.h,
 * and sd_storage.h are pulled in. The HTTP handler is registered
 * alongside the wifi handlers because it needs to be reachable on the
 * captive-portal AP (no auth gate) — the AP itself is the boundary.
 *
 * Wizard contract:
 *   GET /api/selftest  →  { ok:true, all_passed, total_ms, summary, probes:[…] }
 *
 * Each probe entry carries a stable `name`, a `status` ("pass" |
 * "fail" | "skip" | "absent" | "unknown"), a one-line `detail` for
 * the user, and a `metric` blob with structured numbers a power user
 * can reveal under a disclosure. Detail strings stay short (≤ 96 B)
 * so the whole response fits comfortably in one TCP segment on the
 * AP.
 */

#ifndef SECURACV_SELFTEST_API_H
#define SECURACV_SELFTEST_API_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_http_server.h>

#include "ble_config.h"
#include "ble_manager.h"
#include "hardware_state.h"
#include "sd_storage.h"

// Camera state lives in the .ino (g_camera_initialized,
// g_peek_sensor_pid, g_peek_last_init_err, g_peek_last_init_label,
// g_peek_init_count). g_hw.camera_available mirrors the first one and
// IS visible here via hardware_state.h's extern, so we read that
// instead of pulling more .ino-private globals.

// Boot button GPIO is declared in the .ino too (BOOT_BUTTON_GPIO=0
// on every supported board today). We probe pin 0 directly; if a
// future board moves the boot button this probe just needs the pin
// updated alongside the .ino constant.
#ifndef SELFTEST_BOOT_BUTTON_GPIO
#define SELFTEST_BOOT_BUTTON_GPIO 0
#endif

namespace selftest {

enum class Status : uint8_t {
  UNKNOWN = 0,
  PASS    = 1,
  FAIL    = 2,
  SKIP    = 3,   // present + healthy, but the feature is intentionally off
  ABSENT  = 4    // hardware/feature isn't built into this firmware variant
};

inline const char* status_name(Status s) {
  switch (s) {
    case Status::PASS:    return "pass";
    case Status::FAIL:    return "fail";
    case Status::SKIP:    return "skip";
    case Status::ABSENT:  return "absent";
    case Status::UNKNOWN:
    default:              return "unknown";
  }
}

struct ProbeResult {
  const char* name;          // stable id: "wifi", "camera", …
  const char* label;         // user-facing: "Wi-Fi", "Camera", …
  Status      status;
  int32_t     code;          // subsystem-specific error/info code
  char        detail[96];    // human-readable one-liner
  // metric is emitted as a JSON object so the client can render
  // arbitrary key/value rows under the "show details" disclosure
  // without us hand-rolling another struct here. The probe just
  // populates fields directly on the JsonObject we hand it.
};

static const size_t MAX_PROBES = 6;

struct Report {
  ProbeResult probes[MAX_PROBES];
  uint8_t     count;
  uint8_t     pass_count;
  uint8_t     fail_count;
  uint8_t     absent_count;
  uint32_t    total_ms;
};

// ─────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────

inline void set_detail(ProbeResult* r, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r->detail, sizeof(r->detail), fmt, ap);
  va_end(ap);
}

// ─────────────────────────────────────────────────────────────────────
// Probes — each one must run in <50 ms so the wizard can present a
// snappy aggregate. None of them mutate user-visible state.
// ─────────────────────────────────────────────────────────────────────

inline void probe_wifi(ProbeResult* r, JsonObject metric) {
  r->name  = "wifi";
  r->label = "Wi-Fi";

  const bool sta_up   = WiFi.isConnected();
  const wifi_mode_t m = WiFi.getMode();
  const bool ap_up    = (m == WIFI_AP || m == WIFI_AP_STA);

  metric["ap_active"]    = ap_up;
  metric["sta_connected"] = sta_up;
  metric["mode"]         = (int)m;

  if (sta_up) {
    const int32_t rssi = WiFi.RSSI();
    metric["rssi_dbm"] = rssi;
    metric["ssid"]     = WiFi.SSID();
    metric["ip"]       = WiFi.localIP().toString();
    metric["channel"]  = WiFi.channel();
    r->status = Status::PASS;
    r->code   = rssi;
    set_detail(r, "Joined %s · %lddBm",
               WiFi.SSID().c_str(), (long)rssi);
  } else if (ap_up) {
    // During wizard step 1-3 we expect AP-only; that's a SKIP, not a
    // failure. The wizard will run the self-test AFTER WiFi joins, but
    // we want this endpoint to be honest about state at any moment.
    metric["ap_clients"] = (int)WiFi.softAPgetStationNum();
    r->status = Status::SKIP;
    r->code   = 0;
    set_detail(r, "Hotspot active · waiting for home Wi-Fi");
  } else {
    r->status = Status::FAIL;
    r->code   = -1;
    set_detail(r, "Wi-Fi radio off");
  }
}

inline void probe_camera(ProbeResult* r, JsonObject metric) {
  r->name  = "camera";
  r->label = "Camera";

  const bool ever = g_hw.camera_ever_init;
  const bool now  = g_hw.camera_available;

  metric["ever_initialized"] = ever;
  metric["initialized"]      = now;

  if (now) {
    r->status = Status::PASS;
    r->code   = 0;
    set_detail(r, "Sensor online");
  } else if (ever) {
    r->status = Status::FAIL;
    r->code   = -2;
    set_detail(r, "Sensor offline (was working — try /api/peek/init)");
  } else {
    // Either the board has no camera (e.g., XIAO C3 without Sense
    // expansion) or init never succeeded. We can't tell those apart
    // here without yanking the .ino's init-error globals; the dashboard
    // surfaces those via /api/peek/status, so we keep this terse.
    r->status = Status::ABSENT;
    r->code   = -3;
    set_detail(r, "No camera detected");
  }
}

inline void probe_bluetooth(ProbeResult* r, JsonObject metric) {
  r->name  = "bluetooth";
  r->label = "Bluetooth";

#if FEATURE_BLE
  const bool avail  = ble_manager::isAvailable();
  const bool opera  = ble_manager::isOperaActive();
  const bool chirp  = ble_manager::isChirpActive();
  const bool nearby = ble_manager::isNearbyActive();

  metric["available"]    = avail;
  metric["opera"]        = opera;
  metric["chirp"]        = chirp;
  metric["nearby"]       = nearby;

  if (!avail) {
    r->status = Status::FAIL;
    r->code   = -1;
    set_detail(r, "NimBLE init failed");
    return;
  }
  // At least one subsystem must be live for "Bluetooth works".
  if (opera || chirp || nearby) {
    r->status = Status::PASS;
    r->code   = 0;
    set_detail(r, "Radio up · %s%s%s",
               opera  ? "advertising "  : "",
               nearby ? "scanning "     : "",
               chirp  ? "chirping"      : "");
  } else {
    // Stack is up but every feature is gated off. Honest SKIP.
    r->status = Status::SKIP;
    r->code   = 0;
    set_detail(r, "Radio up · all features disabled");
  }
#else
  metric["compiled_in"] = false;
  r->status = Status::ABSENT;
  r->code   = 0;
  set_detail(r, "Not built into this firmware");
#endif
}

inline void probe_sd(ProbeResult* r, JsonObject metric) {
  r->name  = "sd";
  r->label = "SD card";

  const bool mounted = g_hw.sd_available && (g_hw.sd_state == SD_MOUNTED);
  metric["state"]   = sd_state_name(g_hw.sd_state);
  metric["mounted"] = mounted;

  if (mounted) {
    metric["total_bytes"] = g_hw.sd_total_bytes;
    metric["free_bytes"]  = g_hw.sd_free_bytes;
    // Render a compact size for the one-liner. uint64 → MiB rounded.
    const uint64_t total_mib = g_hw.sd_total_bytes >> 20;
    const uint64_t free_mib  = g_hw.sd_free_bytes  >> 20;
    r->status = Status::PASS;
    r->code   = 0;
    set_detail(r, "Mounted · %llu MiB free of %llu MiB",
               (unsigned long long)free_mib,
               (unsigned long long)total_mib);
  } else if (g_hw.sd_state == SD_ERROR) {
    r->status = Status::FAIL;
    r->code   = -1;
    set_detail(r, "Card present but errored — try a different card");
  } else {
    r->status = Status::ABSENT;
    r->code   = 0;
    set_detail(r, "No card inserted");
  }
}

inline void probe_microphone(ProbeResult* r, JsonObject metric) {
  r->name  = "microphone";
  r->label = "Microphone";

  // The canary-wap firmware does not currently bring up the XIAO
  // ESP32-S3 Sense's onboard PDM mic. Audible chirp uses a piezo
  // OUTPUT, not a mic input. Until a mic-driven feature ships
  // (occupancy-by-ambient-noise, anti-spoof speech detector, …),
  // we report ABSENT cleanly so the wizard greys the row instead
  // of failing the whole pre-flight.
  metric["compiled_in"] = false;
  r->status = Status::ABSENT;
  r->code   = 0;
  set_detail(r, "Not used by this firmware");
}

inline void probe_gpio(ProbeResult* r, JsonObject metric) {
  r->name  = "gpio";
  r->label = "GPIO";

  // Two cheap sanity checks:
  //   1. The boot button pin is configured INPUT_PULLUP at setup() and
  //      should read HIGH when not pressed. If it reads LOW for two
  //      consecutive samples 5 ms apart, either someone is holding the
  //      button (legit) or the line is shorted (failure). We can't
  //      distinguish those without user interaction, so we just report
  //      the observed level — the wizard treats "stuck low" as
  //      informational, not a hard fail.
  //   2. millis() has advanced since boot (catches a wedged kernel
  //      timer that would silently break every other probe).
  pinMode(SELFTEST_BOOT_BUTTON_GPIO, INPUT_PULLUP);
  const int s1 = digitalRead(SELFTEST_BOOT_BUTTON_GPIO);
  delay(5);
  const int s2 = digitalRead(SELFTEST_BOOT_BUTTON_GPIO);
  const bool stuck_low = (s1 == LOW && s2 == LOW);

  metric["boot_btn_pin"]   = SELFTEST_BOOT_BUTTON_GPIO;
  metric["boot_btn_level"] = s2;
  metric["uptime_ms"]      = (uint32_t)millis();

  // millis()==0 only happens in the first millisecond after boot; if
  // we see it on a self-test request we have a kernel-timer issue.
  if (millis() == 0) {
    r->status = Status::FAIL;
    r->code   = -2;
    set_detail(r, "System tick stuck at 0");
    return;
  }

  if (stuck_low) {
    // Almost certainly user pressing BOOT during the test. We surface
    // it so the wizard can tell them to release the button, but it is
    // not a hard fail (the rest of the system is fine).
    r->status = Status::SKIP;
    r->code   = 0;
    set_detail(r, "BOOT button held — release and re-run");
  } else {
    r->status = Status::PASS;
    r->code   = 0;
    set_detail(r, "Pins responsive");
  }
}

// ─────────────────────────────────────────────────────────────────────
// Aggregator
// ─────────────────────────────────────────────────────────────────────

inline bool run_to_json(JsonDocument& doc) {
  const uint32_t t0 = millis();

  // Six rows in stable order so the wizard UI doesn't reshuffle row
  // positions between polls. Order matches the user's mental model:
  // network → vision → radio → storage → audio → low-level pins.
  const char* const ORDER[MAX_PROBES] = {
    "wifi", "camera", "bluetooth", "sd", "microphone", "gpio"
  };
  (void)ORDER;  // documentation-only; the calls below are the truth

  JsonArray probes = doc["probes"].to<JsonArray>();
  ProbeResult tmp{};

  uint8_t pass_n = 0, fail_n = 0, absent_n = 0;
  auto push = [&](void (*fn)(ProbeResult*, JsonObject)) {
    JsonObject row    = probes.add<JsonObject>();
    JsonObject metric = row["metric"].to<JsonObject>();
    memset(&tmp, 0, sizeof(tmp));
    fn(&tmp, metric);
    row["name"]   = tmp.name;
    row["label"]  = tmp.label;
    row["status"] = status_name(tmp.status);
    row["code"]   = tmp.code;
    row["detail"] = tmp.detail;
    if (tmp.status == Status::PASS)        pass_n++;
    else if (tmp.status == Status::FAIL)   fail_n++;
    else if (tmp.status == Status::ABSENT) absent_n++;
  };

  push(probe_wifi);
  push(probe_camera);
  push(probe_bluetooth);
  push(probe_sd);
  push(probe_microphone);
  push(probe_gpio);

  const uint32_t total_ms = millis() - t0;

  doc["ok"]           = true;
  doc["total_ms"]     = total_ms;
  doc["pass_count"]   = pass_n;
  doc["fail_count"]   = fail_n;
  doc["absent_count"] = absent_n;
  // "all_passed" excludes ABSENT/SKIP — those aren't problems, they're
  // just rows the user should see to know the device's full picture.
  doc["all_passed"]   = (fail_n == 0);

  // Single short summary line the wizard uses for its aria-live
  // announcement so screen-reader users hear a verdict, not a wall of
  // metrics. Mirrors the existing announce() pattern in companion_pwa.h.
  char summary[96];
  if (fail_n == 0) {
    snprintf(summary, sizeof(summary),
             "All checks passed (%u of %u, %u not present)",
             (unsigned)pass_n, (unsigned)(pass_n + fail_n),
             (unsigned)absent_n);
  } else {
    // Verb has to agree with the count: "1 check needs" vs "2 checks need".
    snprintf(summary, sizeof(summary),
             "%u check%s need%s attention",
             (unsigned)fail_n,
             fail_n == 1 ? ""  : "s",
             fail_n == 1 ? "s" : "");
  }
  doc["summary"] = summary;

  return fail_n == 0;
}

// ─────────────────────────────────────────────────────────────────────
// HTTP handler — registered alongside the wifi handlers because it
// MUST be reachable on the captive-portal AP (no auth) so the wizard
// can call it before any post-pair token is set up. The AP itself is
// the security boundary, identical to /api/wifi/scan.
// ─────────────────────────────────────────────────────────────────────

inline esp_err_t handle_selftest(httpd_req_t* req) {
  JsonDocument doc;
  run_to_json(doc);
  String response;
  serializeJson(doc, response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_sendstr(req, response.c_str());
}

}  // namespace selftest

#endif  // SECURACV_SELFTEST_API_H
