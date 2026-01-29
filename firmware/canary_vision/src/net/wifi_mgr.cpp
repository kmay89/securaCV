#include "canary/net/wifi_mgr.h"
#include "canary/log.h"

#include <WiFi.h>

#if __has_include("secrets/secrets.h")
  #include "secrets/secrets.h"
#else
  #include "secrets/secrets.ci.h"
#endif


namespace canary::net {

void wifi_init_or_reboot() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  log_header("WIFI");
  Serial.printf("Connecting SSID=\"%s\" ...\n", WIFI_SSID);

  const uint32_t start = ms_now();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (ms_now() - start > 30'000) {
      Serial.println();
      log_line("WIFI", "Timeout. Rebooting...");
      delay(200);
      ESP.restart();
    }
  }

  Serial.println();
  log_header("WIFI");
  Serial.printf("Connected IP=%s RSSI=%ddBm\n",
                WiFi.localIP().toString().c_str(),
                WiFi.RSSI());
}

} // namespace
