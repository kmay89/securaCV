#include "canary/runtime_config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#include "canary/config.h"
#include "canary/log.h"

// Prefer local dev secrets if present; otherwise use the CI stub. Two
// spellings because the secrets dir may reach the compiler either as an
// include ROOT (-I<project>/secrets -> "secrets.h") or as a subdir of an
// include root ("secrets/secrets.h").
#if __has_include("secrets.h")
  #include "secrets.h"
#elif __has_include("secrets/secrets.h")
  #include "secrets/secrets.h"
#else
  #include "secrets.ci.h"
#endif

namespace canary::cfg {

namespace {

RuntimeConfig g_cfg{};
bool g_loaded = false;

void copy_str(char* dst, size_t cap, const char* src) {
  if (dst == nullptr || cap == 0) return;
  if (src == nullptr) { dst[0] = '\0'; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

/* Placeholder credentials must never overwrite a unit's real NVS values.
 * These match include/secrets.ci.h and the generic header the
 * firmware-release workflow generates for OTA release builds. */
bool is_placeholder(const char* v) {
  return v == nullptr || v[0] == '\0' ||
         strcmp(v, "ci") == 0 ||
         strcmp(v, "ci-placeholder") == 0 ||
         strcmp(v, "127.0.0.1") == 0;
}

/* Credentials: a real compiled value wins and is persisted (a fresh
 * secrets build over USB reconfigures the unit); a placeholder defers
 * to whatever NVS already holds. A key that EXISTS in NVS is a real
 * answer even when its value is empty: the flashers seed an empty
 * wifi_pass for an open network, and substituting the compiled
 * placeholder here made the join fail with a password no router has
 * ever seen. Only a key that is genuinely absent falls back. */
void load_credential(Preferences& prefs, const char* key,
                     char* dst, size_t cap, const char* compiled) {
  if (!is_placeholder(compiled)) {
    copy_str(dst, cap, compiled);
    String stored = prefs.getString(key, "");
    if (stored != dst) prefs.putString(key, dst);
    return;
  }
  if (prefs.isKey(key)) {
    String stored = prefs.getString(key, "");
    copy_str(dst, cap, stored.c_str());
    if (dst[0] == '\0' && cap > 0) {
      // Blob-typed key (getBytesLength is 0 for string entries): a stale
      // flasher frontend seeded the wap/canary BLOB scheme under this key,
      // and getString on a blob reads "". Same human intent — honor it.
      // Blobs carry no NUL: copy and terminate.
      const size_t n = prefs.getBytesLength(key);
      if (n > 0 && n < cap) {
        prefs.getBytes(key, dst, n);
        dst[n] = '\0';
      }
    }
  } else {
    copy_str(dst, cap, compiled);
  }
}

}  // namespace

const RuntimeConfig& get() {
  if (g_loaded) return g_cfg;

  Preferences prefs;
  if (!prefs.begin("securacv", /*readOnly=*/false)) {
    // NVS unavailable — fall back to compiled values for this boot.
    copy_str(g_cfg.device_id, sizeof(g_cfg.device_id), DEVICE_ID);
    copy_str(g_cfg.wifi_ssid, sizeof(g_cfg.wifi_ssid), WIFI_SSID);
    copy_str(g_cfg.wifi_pass, sizeof(g_cfg.wifi_pass), WIFI_PASS);
    copy_str(g_cfg.mqtt_host, sizeof(g_cfg.mqtt_host), MQTT_HOST);
    g_cfg.mqtt_port = (uint16_t)MQTT_PORT;
    copy_str(g_cfg.mqtt_user, sizeof(g_cfg.mqtt_user), MQTT_USER);
    copy_str(g_cfg.mqtt_pass, sizeof(g_cfg.mqtt_pass), MQTT_PASS);
    g_loaded = true;
    log_line("CFG", "NVS unavailable — using compiled configuration.");
    return g_cfg;
  }

  // Identity is sticky: NVS always wins; compiled DEVICE_ID seeds first boot.
  {
    String stored = prefs.getString("dev_id", "");
    if (stored.length() > 0) {
      copy_str(g_cfg.device_id, sizeof(g_cfg.device_id), stored.c_str());
    } else {
      copy_str(g_cfg.device_id, sizeof(g_cfg.device_id), DEVICE_ID);
      prefs.putString("dev_id", g_cfg.device_id);
    }
  }

  load_credential(prefs, "wifi_ssid", g_cfg.wifi_ssid, sizeof(g_cfg.wifi_ssid), WIFI_SSID);
  load_credential(prefs, "wifi_pass", g_cfg.wifi_pass, sizeof(g_cfg.wifi_pass), WIFI_PASS);
  load_credential(prefs, "mqtt_host", g_cfg.mqtt_host, sizeof(g_cfg.mqtt_host), MQTT_HOST);
  load_credential(prefs, "mqtt_user", g_cfg.mqtt_user, sizeof(g_cfg.mqtt_user), MQTT_USER);
  load_credential(prefs, "mqtt_pass", g_cfg.mqtt_pass, sizeof(g_cfg.mqtt_pass), MQTT_PASS);

  {
    uint16_t port = prefs.getUShort("mqtt_port", 0);
    const uint16_t compiled = (uint16_t)MQTT_PORT;
    if (compiled != 0 && !is_placeholder(MQTT_HOST)) {
      // Port travels with a real compiled broker config.
      g_cfg.mqtt_port = compiled;
      if (port != compiled) prefs.putUShort("mqtt_port", compiled);
    } else {
      g_cfg.mqtt_port = (port != 0) ? port : compiled;
    }
  }

  prefs.end();
  g_loaded = true;
  return g_cfg;
}

bool wifi_credentials_configured() {
  const RuntimeConfig& cfg = get();
  return !is_placeholder(cfg.wifi_ssid);
}

bool set_wifi_credentials(const char* ssid, const char* pass) {
  if (ssid == nullptr || ssid[0] == '\0') return false;
  get();  // ensure the cache exists before we patch it
  copy_str(g_cfg.wifi_ssid, sizeof(g_cfg.wifi_ssid), ssid);
  copy_str(g_cfg.wifi_pass, sizeof(g_cfg.wifi_pass), pass ? pass : "");
  Preferences prefs;
  if (!prefs.begin("securacv", /*readOnly=*/false)) {
    // NVS down: the session still works (cache is patched); the portal will
    // simply run again next boot. Honest failure, not a dead end.
    log_line("CFG", "NVS unavailable - credentials held for this boot only.");
    return false;
  }
  prefs.putString("wifi_ssid", g_cfg.wifi_ssid);
  prefs.putString("wifi_pass", g_cfg.wifi_pass);
  // Trust the readback, not the calls: a full or refusing store fails the
  // same way for the user as a write that never happened — the next boot
  // reloads whatever is actually in flash. (putString's return can't tell
  // an empty password apart from a failed write, so readback is the one
  // honest answer; same rule as the display's forget_wifi_credentials.)
  const bool durable = prefs.getString("wifi_ssid", "") == g_cfg.wifi_ssid &&
                       prefs.getString("wifi_pass", "") == g_cfg.wifi_pass;
  prefs.end();
  if (durable) {
    log_line("CFG", "WiFi credentials provisioned to NVS.");
  } else {
    log_line("CFG", "NVS would not hold the credentials - this boot only.");
  }
  return durable;
}

bool mqtt_credentials_configured() {
  const RuntimeConfig& cfg = get();
  return !is_placeholder(cfg.mqtt_host) && cfg.mqtt_port != 0;
}

} // namespace canary::cfg
