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
 *   gps         — optional NMEA module: detected / has-fix / absent
 *   sd          — mounted + free space
 *   power       — optional battery: SoC + charge state, or USB-only ABSENT
 *   microphone  — PDM mic (FEATURE_ACOUSTIC_EVENTS): listening / muted
 *                 by user / bring-up failed; ABSENT when compiled out
 *   buzzer      — audible-chirp subsystem up (FEATURE_AUDIBLE_CHIRP)
 *   tamper      — tamper input armed iff FEATURE_TAMPER_GPIO, else ABSENT
 *   gpio        — boot button readable + not stuck (sanity)
 *
 * Optional peripherals (gps/power/buzzer/tamper/microphone) only ever
 * report PASS/SKIP/ABSENT — never FAIL — so a missing optional part can
 * never gate setup. "all_passed" counts FAIL only.
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
#include "selftest_logic.h"
// Optional-peripheral probes. All three are header-only and self-guard on
// their FEATURE_* flags (power_monitor is always compiled; audible_chirp
// ships a no-op stub namespace when FEATURE_AUDIBLE_CHIRP is 0), so it is
// safe to include them here unconditionally.
#include "power_monitor.h"
#include "audible_chirp.h"
#include "build_config.h"
#if FEATURE_BLUETOOTH
// After build_config.h so FEATURE_BLUETOOTH is defined; gives the
// bluetooth probe the pairing-channel status accessors on DEV/FULL builds.
#include "bluetooth_channel.h"
#endif
#if FEATURE_ACOUSTIC_EVENTS
#include "securacv_audio.h"
#endif

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

// Set by setup() once BLE/Bluetooth init has been attempted, so the
// bluetooth probe can tell "not up yet / skipped in safe mode" (SKIP)
// from "init ran and the stack failed" (FAIL). Owned by the .ino.
extern volatile bool g_ble_init_attempted;

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

static const size_t MAX_PROBES = 10;

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

  const bool radio_off = (m == WIFI_OFF);
  const selftest_logic::WifiKind kind =
      selftest_logic::wifi_kind(sta_up, ap_up, radio_off);
  r->status = (Status)selftest_logic::wifi_status(kind);

  switch (kind) {
    case selftest_logic::WifiKind::JOINED: {
      const int32_t rssi = WiFi.RSSI();
      metric["rssi_dbm"] = rssi;
      metric["ssid"]     = WiFi.SSID();
      metric["ip"]       = WiFi.localIP().toString();
      metric["channel"]  = WiFi.channel();
      r->code = rssi;
      set_detail(r, "Joined %s · %lddBm", WiFi.SSID().c_str(), (long)rssi);
      break;
    }
    case selftest_logic::WifiKind::HOTSPOT:
      // Wizard steps 1-3 run before the home join — SKIP, not failure.
      metric["ap_clients"] = (int)WiFi.softAPgetStationNum();
      r->code = 0;
      set_detail(r, "Hotspot active · waiting for home Wi-Fi");
      break;
    case selftest_logic::WifiKind::LINK_DOWN:
      // The radio is ON — the home link merely dropped. Saying "radio
      // off" here (the old text) sent users chasing the wrong fault.
      r->code = -1;
      set_detail(r, "Home Wi-Fi link down");
      break;
    case selftest_logic::WifiKind::RADIO_OFF:
    default:
      r->code = -1;
      set_detail(r, "Wi-Fi radio off");
      break;
  }
}

inline void probe_camera(ProbeResult* r, JsonObject metric) {
  r->name  = "camera";
  r->label = "Camera";

  const bool ever = g_hw.camera_ever_init;
  const bool now  = g_hw.camera_available;

  metric["ever_initialized"] = ever;
  metric["initialized"]      = now;

  metric["standby"] = g_hw.camera_standby;

  if (now) {
    r->status = Status::PASS;
    r->code   = 0;
    set_detail(r, "Sensor online");
  } else if (g_hw.camera_standby) {
    // Parked by the power manager — healthy, wakes on use. Reporting
    // this as FAIL would page the user about a feature working as
    // designed.
    r->status = Status::PASS;
    r->code   = 0;
    set_detail(r, "Asleep to save power — wakes when used");
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

  // "Compiled in" means the radio is in this build — discovery OR the
  // pairing channel. Keying on FEATURE_BLE alone made a DEV build (pairing
  // channel up, discovery off) claim Bluetooth was "not built in".
#if FEATURE_BLE || FEATURE_BLUETOOTH
  const bool compiled_in = true;
  const bool safe_mode   = g_hw.safe_mode;
  const bool init_done   = g_ble_init_attempted;

  bool avail = false;
  bool any_active = false;
#if FEATURE_BLE
  const bool opera  = ble_manager::isOperaActive();
  const bool chirp  = ble_manager::isChirpActive();
  const bool nearby = ble_manager::isNearbyActive();
  avail |= ble_manager::isAvailable();
  any_active = any_active || opera || chirp || nearby;
  metric["discovery_available"] = ble_manager::isAvailable();
  metric["opera"]  = opera;
  metric["chirp"]  = chirp;
  metric["nearby"] = nearby;
#endif
#if FEATURE_BLUETOOTH
  avail |= bluetooth_channel::is_initialized();
  any_active = any_active || bluetooth_channel::is_advertising();
  metric["channel_up"]         = bluetooth_channel::is_initialized();
  metric["channel_advertising"] = bluetooth_channel::is_advertising();
  if (bluetooth_channel::init_fail_reason()[0]) {
    metric["init_fail_reason"] = bluetooth_channel::init_fail_reason();
  }
#endif
  metric["available"]      = avail;
  metric["init_attempted"] = init_done;
  metric["safe_mode"]      = safe_mode;
  // Crash evidence (persisted in NVS by safe_mode_check): lets the wizard
  // say WHY the device is in recovery mode instead of just that it is.
  if (g_hw.last_crash_stage[0])  metric["last_crash_stage"]  = (const char*)g_hw.last_crash_stage;
  if (g_hw.last_crash_reason[0]) metric["last_crash_reason"] = (const char*)g_hw.last_crash_reason;

  r->status = (Status)selftest_logic::bluetooth_status(
      compiled_in, init_done, avail, safe_mode, any_active);
  r->code = 0;
  switch (r->status) {
    case Status::PASS:
      set_detail(r, "Radio up · paired-ready");
      break;
    case Status::FAIL:
      r->code = -1;
      // Prefer the channel's recorded cause ("internal RAM too fragmented…",
      // "NimBLE stack init failed…") over the old catch-all label — the
      // field report "NimBLE init failed" hid a heap-guard refusal.
#if FEATURE_BLUETOOTH
      if (bluetooth_channel::init_fail_reason()[0]) {
        set_detail(r, "%s", bluetooth_channel::init_fail_reason());
        break;
      }
#endif
      set_detail(r, "NimBLE init failed");
      break;
    case Status::SKIP:
      if (safe_mode)        set_detail(r, "Skipped in safe mode");
      else if (!init_done)  set_detail(r, "Starting up…");
      else                  set_detail(r, "Radio up · all features idle");
      break;
    default:
      set_detail(r, "Bluetooth state unknown");
      break;
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

#if FEATURE_ACOUSTIC_EVENTS
  metric["compiled_in"] = true;
  metric["running"]     = audio_is_running();
  metric["muted"]       = audio_is_muted();

  uint16_t rms = 0;
  uint32_t age_ms = UINT32_MAX;
  audio_get_live_level(&rms, &age_ms);
  metric["last_rms"] = rms;
  if (age_ms != UINT32_MAX) metric["age_ms"] = age_ms;

  if (audio_is_running()) {
    r->status = Status::PASS;
    r->code   = 0;
    set_detail(r, "PDM mic armed · T3/T4 cadence detector running");
  } else if (audio_is_muted()) {
    // Muted-by-user is a deliberate state, not a hardware failure —
    // the wizard greys the row rather than failing the pre-flight.
    r->status = Status::SKIP;
    r->code   = 0;
    set_detail(r, "Muted by user");
  } else {
    // Bring-up failure: real fault worth surfacing, but the mic is an
    // optional peripheral — per the contract above it must never FAIL
    // (FAIL flips all_passed and would gate setup on a unit that can
    // still witness, record, and alert). SKIP keeps the row visible
    // with the diagnosis; code -1 distinguishes it from muted-by-user.
    r->status = Status::SKIP;
    r->code   = -1;
    set_detail(r, "Mic did not start — acoustic detection unavailable");
  }
#else
  // No mic in this build profile. Audible chirp uses a piezo OUTPUT,
  // not a mic input. Report ABSENT cleanly so the wizard greys the
  // row instead of failing the whole pre-flight.
  metric["compiled_in"] = false;
  r->status = Status::ABSENT;
  r->code   = 0;
  set_detail(r, "Not used by this firmware");
#endif
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
// Optional peripherals — these are honest about absence: a board with no
// GPS / no battery / no buzzer reports ABSENT (greyed, non-blocking), and
// only ever PASS/SKIP/ABSENT — never FAIL — so a missing optional part can
// never gate setup. The user-facing "what to look into" guidance lives in
// the companion UI's hint matrix, keyed by these probe names + statuses.
// ─────────────────────────────────────────────────────────────────────

inline void probe_gps(ProbeResult* r, JsonObject metric) {
  r->name  = "gps";
  r->label = "GPS";

  metric["state"]        = gps_state_name(g_hw.gps_state);
  metric["available"]    = g_hw.gps_available;
  metric["ever_detected"] = g_hw.gps_ever_detected;

  if (g_hw.gps_available && g_hw.gps_state == GPS_HAS_FIX) {
    r->status = Status::PASS;
    r->code   = 0;
    set_detail(r, "Fix acquired");
  } else if (g_hw.gps_state == GPS_DETECTED ||
             g_hw.gps_state == GPS_LOST_FIX ||
             g_hw.gps_ever_detected) {
    // Module is wired but hasn't locked on yet. Not a failure — GPS needs
    // sky view and a minute or two. SKIP keeps it greyed-but-noted.
    r->status = Status::SKIP;
    r->code   = 0;
    set_detail(r, "Module detected · waiting for fix");
  } else {
    r->status = Status::ABSENT;
    r->code   = 0;
    set_detail(r, "No GPS module attached");
  }
}

inline void probe_power(ProbeResult* r, JsonObject metric) {
  r->name  = "power";
  r->label = "Battery";

  PowerState ps{};
  const bool ok = power_monitor::get_state(&ps);

  metric["initialized"] = ok;
  if (ok) {
    metric["monitor_mode"]     = power_monitor::mode_name(ps.monitor_mode);
    metric["charge_state"]     = power_monitor::charge_state_name(ps.charge_state);
    metric["soc_pct"]          = ps.soc_pct;
    metric["voltage_mv"]       = ps.voltage_mv;
    metric["battery_present"]  = ps.battery_present;
    metric["divider_detected"] = ps.divider_detected;
  }

  if (!ok) {
    // Monitor hasn't sampled yet (very early boot) — report ABSENT rather
    // than FAIL; battery state simply isn't known.
    r->status = Status::ABSENT;
    r->code   = 0;
    set_detail(r, "Battery state not available yet");
  } else if (!ps.battery_present || ps.charge_state == CHARGE_STATE_NO_BATTERY) {
    r->status = Status::ABSENT;
    r->code   = 0;
    set_detail(r, "On USB power · no battery");
  } else {
    // Battery is present. A low/critical level is real-world information,
    // not a pre-flight failure (the device runs fine on USB while it
    // charges), so we PASS and let the detail + UI hint carry the nuance.
    r->status = Status::PASS;
    r->code   = (int32_t)ps.soc_pct;
    set_detail(r, "Battery %u%% · %s",
               (unsigned)ps.soc_pct,
               power_monitor::charge_state_name(ps.charge_state));
  }
}

inline void probe_buzzer(ProbeResult* r, JsonObject metric) {
  r->name  = "buzzer";
  r->label = "Buzzer";

#if FEATURE_AUDIBLE_CHIRP
  const bool avail = audible_chirp::is_available();
  metric["available"]   = avail;
  metric["visual_only"] = audible_chirp::is_visual_only();
  metric["gpio"]        = audible_chirp::get_gpio();

  if (avail) {
    // The alert subsystem (LEDC tone + LED fallback) is up. We can't sense
    // whether a *physical* passive buzzer is wired — there's no feedback
    // line — so we PASS the subsystem and let the UI hint nudge the user to
    // play a test tone to confirm they actually hear it.
    r->status = Status::PASS;
    r->code   = 0;
    set_detail(r, "Alert tones ready");
  } else {
    r->status = Status::SKIP;
    r->code   = 0;
    set_detail(r, "Alert tones not started");
  }
#else
  metric["compiled_in"] = false;
  r->status = Status::ABSENT;
  r->code   = 0;
  set_detail(r, "Not built into this firmware");
#endif
}

inline void probe_tamper(ProbeResult* r, JsonObject metric) {
  r->name  = "tamper";
  r->label = "Tamper";

#if FEATURE_TAMPER_GPIO
  // Tamper monitoring is compiled in. There is no standalone pin-read driver
  // exposed yet, so we report it as armed rather than claiming a specific
  // line state we can't read here.
  metric["enabled"] = true;
  r->status = Status::SKIP;
  r->code   = 0;
  set_detail(r, "Tamper monitoring armed");
#else
  metric["enabled"] = false;
  r->status = Status::ABSENT;
  r->code   = 0;
  set_detail(r, "Tamper monitoring not enabled");
#endif
}

// ─────────────────────────────────────────────────────────────────────
// Aggregator
// ─────────────────────────────────────────────────────────────────────

inline bool run_to_json(JsonDocument& doc) {
  const uint32_t t0 = millis();

  // Ten rows in stable order so the wizard UI doesn't reshuffle row
  // positions between polls. Order matches the user's mental model:
  // network → vision → radios → storage → power → audio → security → pins.
  const char* const ORDER[MAX_PROBES] = {
    "wifi", "camera", "bluetooth", "gps", "sd",
    "power", "microphone", "buzzer", "tamper", "gpio"
  };
  (void)ORDER;  // documentation-only; the calls below are the truth

  JsonArray probes = doc["probes"].to<JsonArray>();
  ProbeResult tmp{};

  uint8_t pass_n = 0, fail_n = 0, absent_n = 0, skip_n = 0;
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
    else if (tmp.status == Status::SKIP)   skip_n++;
  };

  push(probe_wifi);
  push(probe_camera);
  push(probe_bluetooth);
  push(probe_gps);
  push(probe_sd);
  push(probe_power);
  push(probe_microphone);
  push(probe_buzzer);
  push(probe_tamper);
  push(probe_gpio);

  const uint32_t total_ms = millis() - t0;

  doc["ok"]           = true;
  doc["total_ms"]     = total_ms;
  doc["pass_count"]   = pass_n;
  doc["fail_count"]   = fail_n;
  doc["absent_count"] = absent_n;
  doc["skip_count"]   = skip_n;
  // Safe mode (rapid-reboot recovery) skips every peripheral init, so on a
  // healthy unit it produces a wall of SKIP/ABSENT rows that looks like
  // mass failure. Surface it so the UI can explain the grey rows instead
  // of letting the user read a recovering device as a broken one.
  doc["safe_mode"]    = g_hw.safe_mode;
  // "all_passed" excludes ABSENT/SKIP — those aren't problems, they're
  // just rows the user should see to know the device's full picture.
  doc["all_passed"]   = selftest_logic::all_passed(fail_n);

  // Single short summary line the wizard uses for its aria-live
  // announcement so screen-reader users hear a verdict, not a wall of
  // metrics. Mirrors the existing announce() pattern in companion_pwa.h.
  char summary[96];
  if (fail_n == 0) {
    // Count SKIP with ABSENT so the arithmetic covers all ten rows — the
    // old "%u of %u" quietly dropped SKIP and read as "5 of 5" on a
    // ten-probe run.
    snprintf(summary, sizeof(summary),
             "All checks passed (%u active, %u not in use)",
             (unsigned)pass_n, (unsigned)(absent_n + skip_n));
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
