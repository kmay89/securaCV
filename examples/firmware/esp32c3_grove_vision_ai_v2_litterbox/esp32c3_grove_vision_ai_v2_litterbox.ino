/*
  SecuraCV Litter Box Witness (ESP32C3 + Grove Vision AI V2)

  Conformance goals:
  - Emit ONLY event-contract fields: event_type, time_bucket, zone_id, confidence.
  - No device IDs, timestamps, session IDs, or raw frames.
  - One JSON object per line for grove_vision2_ingest.

  Wiring:
  - ESP32C3 UART -> Grove Vision AI V2 UART (adjust pins as needed).

  NOTE: The AT command names and response format depend on the Grove Vision AI V2
  firmware from the Seeed repo. Adjust `vision_get_cat_presence` to match your
  firmware output. The default parser expects a line like: "cat:0.82".
*/

#include <WiFi.h>
#include <time.h>

// ====== USER CONFIG ======
static const char* WIFI_SSID = "YOUR_WIFI";
static const char* WIFI_PASS = "YOUR_PASS";

static const char* ZONE_ID = "zone:litterbox";
static const char* EVENT_TYPE = "boundary_crossing_object_small";
static const uint32_t BUCKET_SIZE_S = 600;  // 10 minutes (conformance baseline)

// ====== UART to Grove Vision AI V2 ======
static HardwareSerial VisionSerial(1);
static const int VISION_RX = 6;   // ESP32C3 RX (from Vision TX)
static const int VISION_TX = 7;   // ESP32C3 TX (to Vision RX)
static const uint32_t VISION_BAUD = 115200;

// ====== State ======
static bool last_cat_present = false;
static uint32_t last_poll_ms = 0;
static const uint32_t poll_interval_ms = 800;

// ====== Helpers ======
static void wifi_connect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
  }
}

static void time_sync() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = 0;
  while (now < 100000) {
    delay(250);
    time(&now);
  }
}

static uint64_t time_bucket_start(time_t now_s, uint32_t bucket_s) {
  return static_cast<uint64_t>(now_s - (now_s % bucket_s));
}

static String vision_send_cmd_readline(const String& cmd, uint32_t timeout_ms = 2000) {
  VisionSerial.print(cmd);
  VisionSerial.print("\r\n");

  uint32_t start = millis();
  String line;
  while ((millis() - start) < timeout_ms) {
    if (VisionSerial.available()) {
      char c = (char)VisionSerial.read();
      if (c == '\n') break;
      if (c != '\r') line += c;
    }
    delay(1);
  }
  return line;
}

// Example placeholder parser: expects "cat:0.82" or "cat=0.82".
static bool vision_get_cat_presence(float* score_out) {
  String resp = vision_send_cmd_readline("AT+INFER?");
  int idx = resp.indexOf("cat");
  if (idx < 0) {
    *score_out = 0.0f;
    return false;
  }
  int sep = resp.indexOf(":", idx);
  if (sep < 0) sep = resp.indexOf("=", idx);
  if (sep < 0) {
    *score_out = 0.0f;
    return false;
  }
  float s = resp.substring(sep + 1).toFloat();
  *score_out = s;
  return (s >= 0.70f);
}

static void emit_event(float confidence) {
  time_t now_s = 0;
  time(&now_s);
  uint64_t bucket_start = time_bucket_start(now_s, BUCKET_SIZE_S);

  // Conforming payload: only event_type, time_bucket, zone_id, confidence.
  String json = "{";
  json += "\"event_type\":\"" + String(EVENT_TYPE) + "\",";
  json += "\"time_bucket\":{\"start_epoch_s\":" + String((uint64_t)bucket_start) +
          ",\"size_s\":" + String(BUCKET_SIZE_S) + "},";
  json += "\"zone_id\":\"" + String(ZONE_ID) + "\",";
  json += "\"confidence\":" + String(confidence, 3);
  json += "}";

  Serial.println(json);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  wifi_connect();
  time_sync();

  VisionSerial.begin(VISION_BAUD, SERIAL_8N1, VISION_RX, VISION_TX);
  last_poll_ms = millis();
}

void loop() {
  if ((millis() - last_poll_ms) < poll_interval_ms) {
    delay(10);
    return;
  }
  last_poll_ms = millis();

  float score = 0.0f;
  bool cat_present = vision_get_cat_presence(&score);

  // Emit an event on transition only (no enter/exit metadata in payload).
  if (cat_present != last_cat_present) {
    emit_event(score);
  }

  last_cat_present = cat_present;
}
