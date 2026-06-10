#include "canary/net/wifi_mgr.h"
#include "canary/log.h"

#include <WiFi.h>

#include "canary/runtime_config.h"  // NVS-backed credentials (OTA-safe)

namespace canary::net {

void wifi_init_or_reboot() {
  const auto& cfg = canary::cfg::get();
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);

  log_header("WIFI");
  canary::dbg_serial().printf("Connecting SSID=\"%s\" ...\n", cfg.wifi_ssid);

  const uint32_t start = canary::ms_now();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    canary::dbg_serial().print(".");

    if ((canary::ms_now() - start) > 30000UL) {
      canary::dbg_serial().println();
      log_line("WIFI", "Timeout. Rebooting...");
      delay(200);
      ESP.restart();
    }
  }

  canary::dbg_serial().println();
  log_header("WIFI");
  canary::dbg_serial().printf(
    "Connected IP=%s RSSI=%ddBm\n",
    WiFi.localIP().toString().c_str(),
    WiFi.RSSI()
  );
}

} // namespace canary::net
