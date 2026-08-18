#include "canary/runtime_config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstdio>   // snprintf — personalized_device_id
#include <cstring>

#include "canary/config.h"
#include "canary/log.h"
#include "identity/device_pseudonym.h"  // the per-unit suffix (MAC-free, Invariant III)

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

/* THIS UNIT'S OWN NAME, not its model's.
 *
 * DEVICE_ID is a compile-time constant, one per config — so every Canary
 * Nightstand ever flashed seeds the identical "canary_nightstand_001", and
 * two of them on one network are two rows with one name, one mDNS TXT
 * `device_id`, and one set of MQTT topics. The owner cannot tell which is
 * which and neither can anything else.
 *
 * The fix is the suffix the mDNS HOSTNAME has always carried (make_hostname
 * in net/discovery.cpp): six characters of the salted device pseudonym, which
 * is stable, per-unit, and derived from a random NVS salt rather than from
 * the MAC — so this buys uniqueness without touching a trackable identifier
 * (Invariant III). The hostname was already unique; it was the identity every
 * surface actually SHOWS that collided.
 *
 * Returns false if the pseudonym is unavailable (a broken NVS on the very
 * first boot), in which case the caller keeps the bare compiled id — a
 * colliding name is a far smaller problem than no name.
 */
bool personalized_device_id(char* out, size_t cap) {
  if (out == nullptr || cap == 0) return false;
  char devid_hex[device_pseudonym::HEX_LEN + 1] = {0};
  if (!device_pseudonym::device_id_hex(devid_hex, sizeof(devid_hex))) return false;
  if (devid_hex[0] == '\0') return false;
  const int n = snprintf(out, cap, "%s-%.6s", DEVICE_ID, devid_hex);
  return n > 0 && (size_t)n < cap;
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
 * ever seen. Only a key that is genuinely absent falls back.
 *
 * BOTH seed schemes are honored. The flashers write these keys as NVS
 * strings for this firmware, but a stale flasher frontend (cached page
 * from before the per-product scheme plumbing) writes the wap/canary
 * BLOB scheme under the same key names. isKey() is type-blind and
 * getString() on a blob returns "" — which read as "seeded empty",
 * so a display flashed with perfectly good credentials still raised
 * its setup wizard. A blob under the key is the same human intent;
 * read it rather than shrugging into onboarding. */
enum class CredSource { Compiled, NvsString, NvsBlob, Fallback };

CredSource load_credential(Preferences& prefs, const char* key,
                           char* dst, size_t cap, const char* compiled) {
  if (!is_placeholder(compiled)) {
    copy_str(dst, cap, compiled);
    String stored = prefs.getString(key, "");
    if (stored != dst) prefs.putString(key, dst);
    return CredSource::Compiled;
  }
  if (prefs.isKey(key)) {
    String stored = prefs.getString(key, "");
    copy_str(dst, cap, stored.c_str());
    if (dst[0] != '\0') return CredSource::NvsString;
    if (cap > 0) {
      // Blob-typed key (getBytesLength is 0 for string entries): a
      // blob-scheme seed. Blobs carry no NUL — copy and terminate.
      const size_t n = prefs.getBytesLength(key);
      if (n > 0 && n < cap) {
        prefs.getBytes(key, dst, n);
        dst[n] = '\0';
        return CredSource::NvsBlob;
      }
    }
    return CredSource::NvsString;  // present-but-empty: a real (open) answer
  }
  copy_str(dst, cap, compiled);
  return CredSource::Fallback;
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

  // Identity is sticky: NVS always wins; the compiled DEVICE_ID seeds first
  // boot — personalized, so two units of the same model are two names.
  {
    String stored = prefs.getString("dev_id", "");
    char personal[sizeof(g_cfg.device_id)] = {0};
    const bool have_personal =
        personalized_device_id(personal, sizeof(personal));

    if (stored.length() == 0) {
      // First boot: seed the personalized id, never the bare model name.
      copy_str(g_cfg.device_id, sizeof(g_cfg.device_id),
               have_personal ? personal : DEVICE_ID);
      prefs.putString("dev_id", g_cfg.device_id);
    } else if (have_personal && strcmp(stored.c_str(), DEVICE_ID) == 0) {
      // ONE-TIME MIGRATION, for units seeded before this existed.
      //
      // Safe precisely because the stored value is byte-equal to the compiled
      // default: nothing in this firmware writes dev_id except the seed above,
      // so an id that still equals DEVICE_ID is one nobody ever chose. A unit
      // whose id differs — because it was renamed, or seeded from a different
      // config — is left exactly alone, which is why this compares rather than
      // detecting a missing suffix.
      //
      // It does change what this display publishes and which MQTT topics it
      // owns, once. That is the point: it is the change from a name shared
      // with every other unit of its model to a name that is its own.
      copy_str(g_cfg.device_id, sizeof(g_cfg.device_id), personal);
      prefs.putString("dev_id", g_cfg.device_id);
      log_line("CFG", "device id personalized (was the shared model default).");
    } else {
      copy_str(g_cfg.device_id, sizeof(g_cfg.device_id), stored.c_str());
    }
  }

  // One value-free breadcrumb for the question every bad setup session
  // starts with: did this boot see seeded Wi-Fi, and via which scheme?
  // (The 2.4.4 field debugging ran blind here — a device in its wizard
  // could not say whether the flasher's seed was absent or unreadable.)
  {
    const CredSource src = load_credential(prefs, "wifi_ssid", g_cfg.wifi_ssid,
                                           sizeof(g_cfg.wifi_ssid), WIFI_SSID);
    switch (src) {
      case CredSource::Compiled:
        log_line("CFG", "WiFi: using compiled credentials.");
        break;
      case CredSource::NvsString:
        if (g_cfg.wifi_ssid[0] != '\0') {
          log_line("CFG", "WiFi: seeded credentials found (string scheme).");
        } else {
          log_line("CFG", "WiFi: no credentials - setup will open.");
        }
        break;
      case CredSource::NvsBlob:
        log_line("CFG", "WiFi: seeded credentials found (legacy blob scheme).");
        break;
      case CredSource::Fallback:
        log_line("CFG", "WiFi: no credentials - setup will open.");
        break;
    }
  }
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

bool wifi_is_placeholder() {
  return is_placeholder(get().wifi_ssid);
}

bool forget_wifi_credentials() {
  Preferences prefs;
  if (!prefs.begin("securacv", /*readOnly=*/false)) {
    log_line("CFG", "NVS unavailable - nothing to forget.");
    return false;
  }
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_pass");
  // Trust the readback, not the calls: remove() also returns false for a
  // key that never existed, and a remove that silently failed is exactly
  // the case where rebooting into "setup will open" would be a lie — the
  // old SSID would load right back (review catch).
  const bool gone = !prefs.isKey("wifi_ssid") && !prefs.isKey("wifi_pass");
  prefs.end();
  if (!gone) {
    log_line("CFG", "NVS would not release the credentials - still joined.");
    return false;
  }
  // Patch the cache too, so anything that reads before the reboot sees the
  // truth (and the emulator, whose restart returns, lands in the wizard).
  get();
  g_cfg.wifi_ssid[0] = '\0';
  g_cfg.wifi_pass[0] = '\0';
  log_line("CFG", "WiFi credentials forgotten - setup opens on next boot.");
  return true;
}

void set_wifi_credentials(const char* ssid, const char* pass) {
  if (ssid == nullptr || ssid[0] == '\0') return;
  get();  // ensure the cache exists before we patch it
  copy_str(g_cfg.wifi_ssid, sizeof(g_cfg.wifi_ssid), ssid);
  copy_str(g_cfg.wifi_pass, sizeof(g_cfg.wifi_pass), pass ? pass : "");
  Preferences prefs;
  if (prefs.begin("securacv", /*readOnly=*/false)) {
    prefs.putString("wifi_ssid", g_cfg.wifi_ssid);
    prefs.putString("wifi_pass", g_cfg.wifi_pass);
    prefs.end();
    log_line("CFG", "WiFi credentials provisioned to NVS.");
  } else {
    // NVS down: the session still works (cache is patched); the wizard will
    // simply run again next boot. Honest failure, not a dead end.
    log_line("CFG", "NVS unavailable - credentials held for this boot only.");
  }
}

} // namespace canary::cfg
