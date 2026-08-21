#include "ota_mgr.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <cstring>

#include "config.h"
#include "version.h"
#include "log.h"
#include "mqtt_mgr.h"
#include "ota_web.h"

#include "securacv_ota.h"     // shared engine (firmware/common/ota, via lib_extra_dirs)
#include "ota_release_key.h"  // shared Ed25519 release public key

namespace canary::net {

namespace {

Topics g_topics{};
/* Set by the OTA progress callback (runs on the OTA task, must stay
 * non-blocking); the main loop turns it into an MQTT publish. */
volatile bool g_publish_pending = false;
uint32_t g_next_check_ms = 0;
uint32_t g_last_pub_ms = 0;

void on_progress(securacv_ota_state_t state, uint8_t percent,
                 securacv_ota_error_t error, void* user_data) {
  (void)state; (void)percent; (void)error; (void)user_data;
  g_publish_pending = true;
}

void schedule_next(uint32_t delay_ms, uint32_t jitter_ms) {
  uint32_t jitter = (jitter_ms > 0) ? (esp_random() % jitter_ms) : 0;
  g_next_check_ms = millis() + delay_ms + jitter;
}

/* Copy src into dst, replacing JSON-breaking characters — the release
 * summary comes from CHANGELOG text and is embedded in a snprintf-built
 * payload (vision house style; no ArduinoJson dependency in this TU). */
void copy_json_safe(char* dst, size_t cap, const char* src) {
  if (dst == nullptr || cap == 0) return;
  if (src == nullptr) { dst[0] = '\0'; return; }
  size_t o = 0;
  for (size_t i = 0; src[i] != '\0' && o < cap - 1; i++) {
    char c = src[i];
    dst[o++] = (c == '"' || c == '\\' || (uint8_t)c < 0x20) ? ' ' : c;
  }
  dst[o] = '\0';
}

/* State payload for the HA MQTT update entity (installed_version /
 * latest_version / in_progress / update_percentage / release_summary /
 * release_url). Retained + reconnect-republished by mqtt_mgr. */
void publish_update_state_now() {
  const securacv_ota_manifest_t* m = securacv_ota_get_manifest();
  const bool avail = (m != nullptr) && securacv_ota_update_available();
  const char* latest = avail ? m->version : CANARY_FW_VERSION;

  char summary_kv[224] = "";
  char url_kv[160] = "";
  if (avail && m->release_notes[0] != '\0') {
    char summary[160];
    copy_json_safe(summary, sizeof(summary), m->release_notes);
    snprintf(summary_kv, sizeof(summary_kv), ",\"release_summary\":\"%s\"", summary);
  }
  if (avail && m->release_url[0] != '\0') {
    snprintf(url_kv, sizeof(url_kv), ",\"release_url\":\"%s\"", m->release_url);
  }

  const securacv_ota_state_t st = securacv_ota_get_state();
  const bool in_progress = (st == SECURACV_OTA_DOWNLOADING ||
                            st == SECURACV_OTA_VERIFYING ||
                            st == SECURACV_OTA_FLASHING ||
                            st == SECURACV_OTA_REBOOTING);
  char pct[16];
  if (in_progress) snprintf(pct, sizeof(pct), "%u", (unsigned)securacv_ota_get_progress());
  else snprintf(pct, sizeof(pct), "null");  // resets HA's progress bar

  char msg[640];
  snprintf(msg, sizeof(msg),
           "{"
           "\"installed_version\":\"%s\","
           "\"latest_version\":\"%s\","
           "\"in_progress\":%s,"
           "\"update_percentage\":%s"
           "%s%s"
           "}",
           CANARY_FW_VERSION, latest,
           in_progress ? "true" : "false",
           pct, summary_kv, url_kv);

  publish_update_state_retained(g_topics, msg);
}

}  // namespace

void ota_boot_validate() {
  // Confirm — or roll back — a freshly applied OTA image. Called right
  // after WiFi comes up and BEFORE the blocking MQTT connect: confirming
  // must not depend on the broker being reachable, or a broker outage
  // during a device's first post-update boot would let any reboot revert
  // a perfectly good image (the engine owns rollback confirmation via
  // verifyRollbackLater — unconfirmed images revert on the next start).
  // The required probe pins the connectivity this device exists to
  // provide. A failed required probe reboots into the previous firmware
  // (does not return).
  static const securacv_selftest_t k_selftests[] = {
    { "wifi connectivity", [](const char*) -> bool {
        return WiFi.status() == WL_CONNECTED;
      }, true },
  };
  securacv_ota_register_selftest(&k_selftests[0]);
  securacv_ota_boot_self_test();
}

void ota_init(const Topics& topics) {
  g_topics = topics;

  securacv_ota_config_t cfg = SECURACV_OTA_CONFIG_DEFAULT;
  cfg.product          = OTA_PRODUCT;
  cfg.current_version  = CANARY_FW_VERSION;
  cfg.manifest_url     = SECURACV_OTA_MANIFEST_URL;
  cfg.release_pubkey   = SECURACV_OTA_RELEASE_PUBKEY;
  cfg.on_progress      = on_progress;
  if (securacv_ota_init(&cfg) == ESP_OK) {
    log_line("OTA", "Pull-OTA engine ready.");
  } else {
    log_line("OTA", "Pull-OTA engine init FAILED.");
  }

  // Log the outcome of an install reboot. The engine recorded the install
  // target the moment the boot partition flipped; running the old version
  // again means rollback.
  {
    char target[SECURACV_OTA_VERSION_MAX];
    if (securacv_ota_take_pending_version(target, sizeof(target))) {
      if (strcmp(target, CANARY_FW_VERSION) == 0) {
        log_line("OTA", "Firmware update applied.");
      } else {
        log_line("OTA", "Firmware update rolled back — previous software restored.");
      }
    }
  }

  publish_update_auto_retained(g_topics, securacv_ota_get_auto_update());
  publish_update_channel_retained(
      g_topics, securacv_ota_get_channel() == SECURACV_OTA_CHANNEL_DEV);
  publish_update_state_now();

  // First check a couple of minutes after boot (jittered) so a fresh
  // install learns about updates right away; then daily.
  schedule_next(120000UL, 60000UL);
}

void ota_loop(uint32_t now_ms) {
  // HA pressed Install / toggled the auto switch (latched by the MQTT
  // callback, acted on here so flash decisions stay on the main loop).
  if (take_pending_install()) {
    log_line("OTA", "Install requested from Home Assistant.");
    securacv_ota_check_and_install();
  }
  const int auto_cmd = take_pending_auto();
  if (auto_cmd >= 0) {
    const bool enable = (auto_cmd == 1);
    // Skip the NVS write when nothing changed — HA may republish the same
    // command, and flash wear is the budget that matters here.
    if (securacv_ota_get_auto_update() != enable) {
      securacv_ota_set_auto_update(enable);
      log_line("OTA", enable ? "Auto-update turned on." : "Auto-update turned off.");
    }
    publish_update_auto_retained(g_topics, enable);
  }
  const int channel_cmd = take_pending_channel();
  if (channel_cmd >= 0) {
    const securacv_ota_channel_t ch = (channel_cmd == 1)
        ? SECURACV_OTA_CHANNEL_DEV : SECURACV_OTA_CHANNEL_RELEASE;
    // Same flash-wear discipline as the auto switch; on a real change,
    // check the new channel promptly so the switch has a visible effect
    // instead of waiting out the daily timer.
    if (securacv_ota_get_channel() != ch) {
      securacv_ota_set_channel(ch);
      log_line("OTA", ch == SECURACV_OTA_CHANNEL_DEV
          ? "Update channel: dev (rolling prerelease)."
          : "Update channel: release (stable).");
      schedule_next(5000UL, 2000UL);
    }
    publish_update_channel_retained(g_topics, ch == SECURACV_OTA_CHANNEL_DEV);
  }

  // Daily jittered check — the jitter spreads a fleet's checks over an
  // hour so a release never sees a thundering herd.
  if (g_next_check_ms != 0 && (int32_t)(now_ms - g_next_check_ms) >= 0) {
    if (securacv_ota_get_state() != SECURACV_OTA_IDLE ||
        WiFi.status() != WL_CONNECTED) {
      schedule_next(15UL * 60 * 1000, 60000UL);
    } else {
      schedule_next(24UL * 60 * 60 * 1000, 3600000UL);
      if (securacv_ota_get_auto_update()) {
        securacv_ota_check_and_install();
      } else {
        securacv_ota_check();
      }
    }
  }

  // Push entity changes promptly (progress %, transitions) and refresh
  // the retained snapshot every 30 s.
  const bool periodic = (int32_t)(now_ms - g_last_pub_ms) >= 30000;
  if (mqtt_connected() && (g_publish_pending || periodic)) {
    g_publish_pending = false;
    g_last_pub_ms = now_ms;
    publish_update_state_now();
  }
}

// ── On-glass settings facade ─────────────────────────────────────────────

OtaStatus ota_status() {
  OtaStatus s{};
  s.installed = CANARY_FW_VERSION;
  const securacv_ota_manifest_t* m = securacv_ota_get_manifest();
  s.update_available = securacv_ota_update_available();
  s.latest = (m != nullptr && s.update_available) ? m->version : CANARY_FW_VERSION;
  const securacv_ota_state_t st = securacv_ota_get_state();
  s.busy = (st != SECURACV_OTA_IDLE);
  s.progress = securacv_ota_get_progress();
  s.auto_update = securacv_ota_get_auto_update();
  s.dev_channel = (securacv_ota_get_channel() == SECURACV_OTA_CHANNEL_DEV);
  if (s.busy)                  s.state_text = securacv_ota_friendly_state(st);
  else if (s.update_available) s.state_text = "update ready";
  else                         s.state_text = "up to date";
  return s;
}

void ota_request_check() { securacv_ota_check(); }

void ota_request_install() { securacv_ota_check_and_install(); }

void ota_set_auto_update(bool on) {
  if (securacv_ota_get_auto_update() != on) {
    securacv_ota_set_auto_update(on);
    log_line("OTA", on ? "Auto-update turned on (settings)."
                       : "Auto-update turned off (settings).");
  }
  publish_update_auto_retained(g_topics, on);
  g_publish_pending = true;
}

// ── /api/ota web facade (ota_web.h) ──────────────────────────────────────
// glass_web's routes land here so the HTTP trigger and HA's MQTT Install
// button converge on the same engine calls — one code path, one "busy"
// arbiter. The engine's progress callback (on_progress above) fires on the
// state transitions these kicks cause, so the HA update entity narrates a
// web-started run exactly like an MQTT-started one.

namespace {
OtaKick kick_result(esp_err_t err) {
  if (err == ESP_OK) return OtaKick::Started;
  if (err == ESP_ERR_INVALID_STATE) return OtaKick::Busy;  // already running
  return OtaKick::Failed;
}
}  // namespace

OtaWebStatus ota_web_status() {
  OtaWebStatus s{};
  s.installed = CANARY_FW_VERSION;
  const securacv_ota_manifest_t* m = securacv_ota_get_manifest();
  s.latest = (m != nullptr) ? m->version : nullptr;
  const securacv_ota_state_t st = securacv_ota_get_state();
  const securacv_ota_error_t err = securacv_ota_get_last_error();
  s.state = securacv_ota_state_str(st);
  s.state_text = securacv_ota_friendly_state(st);
  s.error = securacv_ota_error_str(err);
  s.error_text = securacv_ota_friendly_error(err);
  s.update_available = securacv_ota_update_available();
  s.auto_update = securacv_ota_get_auto_update();
  s.progress = securacv_ota_get_progress();
  return s;
}

OtaKick ota_web_check() {
  return kick_result(securacv_ota_check());
}

OtaKick ota_web_install() {
  log_line("OTA", "Install requested from the web page.");
  return kick_result(securacv_ota_check_and_install());
}

} // namespace canary::net
