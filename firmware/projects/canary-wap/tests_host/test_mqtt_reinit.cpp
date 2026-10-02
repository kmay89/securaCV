// Host test: the canary-wap's MQTT bridge re-inits on the loop task (sweep
// F106). The REAL csi_mqtt.cpp, compiled over stubs/mqtt: an in-memory NVS,
// an esp_http_server whose requests and responses the test holds, and a
// fake esp_mqtt client (defined below) that records which task made each
// call and notices a client destroyed under a publish.
//
// Before: a config POST or POST /api/mqtt/test ran csi_mqtt::init() on the
// httpd task. init() calls teardown_client() (esp_mqtt_client_stop,
// esp_mqtt_client_destroy, s_client = nullptr) while the loop task's egress,
// or any other publish in csi_mqtt.cpp, can be inside publish_raw() with
// the old handle, which it only null-checks. A QR provisioning ran init()
// on the scanner's task, and the OTA settings handler published the
// auto-update switch from the httpd task.
//
// Now the handlers call request_reinit() and wait (bounded) for loop() to
// serve it, the QR path only requests, and set_update_auto_state() leaves
// the publish to loop().
//
// The test is one thread, so a task is a role: fake::task names the task
// the test is playing, and delay() (stubs/mqtt/Arduino.h) is where a
// waiting handler gives another task its turn. Host-tested only: the
// Arduino compile of the sketch is CI's, and nothing here runs esp_mqtt.
//
// Run: ./test_mqtt_reinit [name]

#include "csi_mqtt.h"

#include <Arduino.h>       // stubs/mqtt: the clock and delay() hook
#include <Preferences.h>   // stubs/mqtt: the NVS

#include "csi_event_egress.h"
#include "csi_integration.h"
#include "device_signature.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "mqtt_client.h"
}

// ── The fake esp_mqtt client ────────────────────────────────────────────

struct esp_mqtt_client {
  int id = 0;
  bool alive = true;          // destroyed clients stay allocated, marked dead
  bool started = false;
  int in_publish = 0;         // a publish into this client is under way
  esp_event_handler_t handler = nullptr;
  void* handler_args = nullptr;
  std::string uri;
  std::string will_topic;
};

namespace fake {

struct Call {
  std::string what;
  std::string task;
  int client;
};

std::string task = "loop";                 // the task the test is playing
std::vector<esp_mqtt_client*> clients;     // every client ever made, never freed
std::vector<Call> calls;
std::vector<std::pair<std::string, std::string>> published;   // topic, payload
std::vector<std::string> published_on;     // the task of each publish
int publishes_on_dead = 0;
int destroyed_under_publish = 0;
std::function<void()> during_publish;      // runs inside the next publish

void reset() {
  task = "loop";
  clients.clear();
  calls.clear();
  published.clear();
  published_on.clear();
  publishes_on_dead = 0;
  destroyed_under_publish = 0;
  during_publish = nullptr;
}

esp_mqtt_client* last_client() { return clients.empty() ? nullptr : clients.back(); }

int count(const std::string& what, const std::string& on_task = "") {
  int n = 0;
  for (const Call& c : calls) {
    if (c.what == what && (on_task.empty() || c.task == on_task)) ++n;
  }
  return n;
}

// The esp_mqtt task delivers an event to the client's handler.
void deliver(esp_mqtt_client* c, esp_mqtt_event_id_t id) {
  const std::string was = task;
  task = "mqtt";
  esp_mqtt_event_t e = {};
  e.event_id = id;
  e.client = c;
  c->handler(c->handler_args, "MQTT_EVENTS", (int32_t)id, &e);
  task = was;
}

}  // namespace fake

extern "C" {

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t* config) {
  esp_mqtt_client* c = new esp_mqtt_client();
  c->id = (int)fake::clients.size() + 1;
  c->uri = config->broker.address.uri ? config->broker.address.uri : "";
  c->will_topic = config->session.last_will.topic ? config->session.last_will.topic : "";
  fake::clients.push_back(c);
  fake::calls.push_back({"init", fake::task, c->id});
  return c;
}

esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, esp_mqtt_event_id_t,
                                         esp_event_handler_t handler, void* args) {
  c->handler = handler;
  c->handler_args = args;
  return ESP_OK;
}

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c) {
  c->started = true;
  fake::calls.push_back({"start", fake::task, c->id});
  return ESP_OK;
}

esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c) {
  c->started = false;
  fake::calls.push_back({"stop", fake::task, c->id});
  return ESP_OK;
}

esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t c) {
  if (c->in_publish > 0) ++fake::destroyed_under_publish;
  c->alive = false;
  fake::calls.push_back({"destroy", fake::task, c->id});
  return ESP_OK;
}

int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char* topic, const char* data,
                            int len, int, int) {
  if (c == nullptr || !c->alive) {
    ++fake::publishes_on_dead;
    return -1;
  }
  ++c->in_publish;
  if (fake::during_publish) {
    std::function<void()> f = std::move(fake::during_publish);
    fake::during_publish = nullptr;
    f();
  }
  --c->in_publish;
  fake::published.emplace_back(topic, std::string(data, data + len));
  fake::published_on.push_back(fake::task);
  return 1;
}

int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char*, int) {
  return (c != nullptr && c->alive) ? 1 : -1;
}

}  // extern "C"

// ── What the sketch links in on a device ────────────────────────────────

namespace csi_integration {
bool session_validate_cookie(httpd_req_t*) { return true; }
void add_outbound_bytes(uint32_t) {}
void hex_encode(const uint8_t* in, size_t len, char* out) {
  static const char k[] = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out[2 * i] = k[in[i] >> 4];
    out[2 * i + 1] = k[in[i] & 15];
  }
  out[2 * len] = '\0';
}
}  // namespace csi_integration

namespace device_signature {
bool sign_chain(uint32_t, const uint8_t[32], char*, size_t) { return false; }
bool sign_event(uint32_t, const char*, const char*, const char*, int, int, int, char*, size_t) {
  return false;
}
bool sign_counts(uint32_t, char*, size_t) { return false; }
const char* fingerprint_hex() { return "0011223344556677"; }
}  // namespace device_signature

namespace csi_event_egress {
int g_pumps = 0;
void pump() { ++g_pumps; }
}  // namespace csi_event_egress

// ── The test ────────────────────────────────────────────────────────────

namespace reinit {

int g_checks = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)

const char* const kDeviceId = "canary-wap-7f3a";

// A device whose bridge is enabled for `host`, booted: setup() gives the
// bridge its identity and start_http_server() runs the boot init(), both
// on the loop task; then the esp_mqtt task connects.
void boot_with_broker(const char* host, bool connect = true) {
  fake::reset();
  stub_nvs().clear();
  stub_mqtt::on_delay = nullptr;
  csi_mqtt::Config c;
  csi_mqtt::config_load(&c);
  c.enabled = true;
  strcpy(c.host, host);
  CHECK(csi_mqtt::config_save(c));
  csi_mqtt::set_identity(kDeviceId, "2.4.15-wap", "ab");
  CHECK(csi_mqtt::init(kDeviceId, "2.4.15-wap", "ab"));
  CHECK(fake::clients.size() == 1);
  if (connect) {
    fake::deliver(fake::last_client(), MQTT_EVENT_CONNECTED);
    CHECK(csi_mqtt::connected());
  }
  fake::calls.clear();
  fake::published.clear();
  fake::published_on.clear();
}

// The loop task's turn: one pass of csi_mqtt::loop().
void loop_pass() {
  const std::string was = fake::task;
  fake::task = "loop";
  csi_mqtt::loop();
  fake::task = was;
}

// An httpd handler's run. On the handler's `turn_at`-th delay the loop
// task gets one pass (never when 0); `on_delay` runs on every delay after
// that, for the esp_mqtt task's events.
struct Http {
  httpd_req_t req;
  unsigned delays = 0;
  uint32_t took_ms = 0;
};
Http http(esp_err_t (*handler)(httpd_req_t*), const std::string& body, unsigned turn_at,
          std::function<void(unsigned)> on_delay = nullptr) {
  Http h;
  h.req.body = body;
  const uint32_t start = stub_mqtt::now_ms;
  stub_mqtt::on_delay = [&](uint32_t ms) {
    stub_mqtt::now_ms += ms;
    ++h.delays;
    if (turn_at != 0 && h.delays == turn_at) loop_pass();
    if (on_delay) on_delay(h.delays);
  };
  const std::string was = fake::task;
  fake::task = "httpd";
  handler(&h.req);
  fake::task = was;
  stub_mqtt::on_delay = nullptr;
  h.took_ms = stub_mqtt::now_ms - start;
  return h;
}

bool has_key(const std::string& json, const char* key) {
  return json.find(std::string("\"") + key + "\":") != std::string::npos;
}

// ── The config POST ─────────────────────────────────────────────────────

// A new broker saved over POST /api/mqtt/config: the old client is stopped
// and destroyed, and the new one made, by the loop task, not the handler;
// the handler still answers {"ok":true} once the new client is up.
void test_a_config_post_reinits_on_the_loop_task() {
  boot_with_broker("10.0.0.1");
  const Http h = http(csi_mqtt::handle_config_post, "{\"host\":\"10.0.0.2\"}", /*turn_at=*/1);
  CHECK(h.req.status == "200 OK");
  CHECK(h.req.resp == "{\"ok\":true}");
  CHECK(fake::count("stop") == 1 && fake::count("stop", "loop") == 1);
  CHECK(fake::count("destroy") == 1 && fake::count("destroy", "loop") == 1);
  CHECK(fake::count("init") == 1 && fake::count("init", "loop") == 1);
  CHECK(fake::clients.size() == 2);
  CHECK(fake::last_client()->uri == "mqtt://10.0.0.2:1883");
  CHECK(!fake::clients[0]->alive && fake::clients[1]->alive);
  std::printf("PASS a_config_post_reinits_on_the_loop_task\n");
}

// The loop task is inside a publish (its egress, or a health or status
// publish) when the config POST arrives: the client it is publishing to
// stays alive under it. The handler waits its 2 s, answers, and the next
// loop() pass re-inits.
void test_a_publish_in_flight_keeps_its_client() {
  boot_with_broker("10.0.0.1");
  Http h;
  fake::during_publish = [&] {
    h = http(csi_mqtt::handle_config_post, "{\"host\":\"10.0.0.2\"}", /*turn_at=*/0);
  };
  csi_mqtt::publish_counts(7);                  // the loop task's publish
  CHECK(fake::destroyed_under_publish == 0);
  CHECK(fake::publishes_on_dead == 0);
  CHECK(h.req.resp == "{\"ok\":true}");         // the save stands
  CHECK(h.took_ms >= 2000 && h.took_ms < 2100);  // bounded
  CHECK(fake::clients.size() == 1 && fake::clients[0]->alive);
  // The loop's next pass serves the request, and publishes go to the new client.
  loop_pass();
  CHECK(fake::clients.size() == 2 && !fake::clients[0]->alive);
  CHECK(fake::count("destroy", "loop") == 1 && fake::count("destroy", "httpd") == 0);
  fake::deliver(fake::last_client(), MQTT_EVENT_CONNECTED);
  fake::published.clear();
  csi_mqtt::publish_counts(8);
  CHECK(fake::publishes_on_dead == 0);
  CHECK(fake::published.size() == 1);
  std::printf("PASS a_publish_in_flight_keeps_its_client\n");
}

// ── POST /api/mqtt/test ─────────────────────────────────────────────────

// The test handler's fresh connect: the loop task re-inits, the new client
// connects, and the answer keeps its shape and says so.
void test_the_test_handler_reports_the_new_clients_connect() {
  boot_with_broker("10.0.0.1");
  const Http h = http(csi_mqtt::handle_test, "", /*turn_at=*/1, [](unsigned n) {
    if (n == 3) fake::deliver(fake::last_client(), MQTT_EVENT_CONNECTED);   // the esp_mqtt task
  });
  CHECK(fake::count("destroy", "loop") == 1 && fake::count("init", "loop") == 1);
  CHECK(fake::count("destroy", "httpd") == 0 && fake::count("init", "httpd") == 0);
  const std::string& j = h.req.resp;
  CHECK(j.find("\"ok\":true") != std::string::npos);
  CHECK(j.find("\"connected\":true") != std::string::npos);
  for (const char* k : {"ok", "connected", "waited_ms", "transport", "last_error"}) CHECK(has_key(j, k));
  CHECK(j.find("\"transport\":\"plain\"") != std::string::npos);
  std::printf("PASS the_test_handler_reports_the_new_clients_connect\n");
}

// The loop task never gets to it: the handler answers within its 4 s, says
// it did not reach the broker (the old client's connection does not count),
// and the re-init still runs on the loop's next pass.
void test_the_test_handler_is_bounded() {
  boot_with_broker("10.0.0.1");                 // connected
  const Http h = http(csi_mqtt::handle_test, "", /*turn_at=*/0);
  CHECK(h.took_ms >= 4000 && h.took_ms < 4100);
  CHECK(h.req.resp.find("\"ok\":false") != std::string::npos);
  CHECK(h.req.resp.find("\"connected\":false") != std::string::npos);
  CHECK(fake::count("destroy") == 0);
  loop_pass();
  CHECK(fake::count("destroy", "loop") == 1 && fake::count("init", "loop") == 1);
  std::printf("PASS the_test_handler_is_bounded\n");
}

// ── Requests ────────────────────────────────────────────────────────────

// Requests made before one loop() pass coalesce into one re-init, which
// serves them all; a later request gets its own.
void test_requests_coalesce_into_one_reinit() {
  boot_with_broker("10.0.0.1");
  const uint32_t a = csi_mqtt::request_reinit();
  const uint32_t b = csi_mqtt::request_reinit();
  const uint32_t c = csi_mqtt::request_reinit();
  CHECK(!csi_mqtt::reinit_done(a) && !csi_mqtt::reinit_done(c));
  CHECK(fake::count("init") == 0);              // nothing until the loop's turn
  loop_pass();
  CHECK(fake::count("init") == 1 && fake::count("destroy") == 1);
  CHECK(csi_mqtt::reinit_done(a) && csi_mqtt::reinit_done(b) && csi_mqtt::reinit_done(c));
  loop_pass();
  CHECK(fake::count("init") == 1);              // served: no second re-init
  const uint32_t d = csi_mqtt::request_reinit();
  CHECK(!csi_mqtt::reinit_done(d));
  loop_pass();
  CHECK(csi_mqtt::reinit_done(d) && fake::count("init") == 2);
  std::printf("PASS requests_coalesce_into_one_reinit\n");
}

// A QR provisioning saves the hub and only requests (the scanner's task):
// the loop's re-init opens the client with the identity setup() gave the
// bridge, even when no boot init() ran (the AP, and the HTTP server with
// it, did not start).
void test_a_qr_request_keeps_the_identity_setup_gave() {
  fake::reset();
  stub_nvs().clear();
  csi_mqtt::set_identity(kDeviceId, "2.4.15-wap", "ab");   // setup(), loop task
  csi_mqtt::Config c;
  csi_mqtt::config_load(&c);
  c.enabled = true;
  strcpy(c.host, "10.0.0.9");
  CHECK(csi_mqtt::config_save(c));
  fake::task = "qr_scan";
  (void)csi_mqtt::request_reinit();
  CHECK(fake::clients.empty());
  loop_pass();
  CHECK(fake::clients.size() == 1 && fake::count("init", "loop") == 1);
  CHECK(fake::last_client()->uri == "mqtt://10.0.0.9:1883");
  CHECK(fake::last_client()->will_topic == std::string("securacv/") + kDeviceId + "/status");
  fake::task = "loop";
  std::printf("PASS a_qr_request_keeps_the_identity_setup_gave\n");
}

// The OTA settings handler sets the auto-update switch from the httpd task:
// nothing is published there; loop() publishes it, retained, once.
void test_the_auto_update_state_is_published_by_the_loop() {
  boot_with_broker("10.0.0.1");
  fake::task = "httpd";
  csi_mqtt::set_update_auto_state(true);
  fake::task = "loop";
  CHECK(fake::published.empty());
  loop_pass();
  CHECK(fake::published.size() == 1);
  CHECK(fake::published[0].first == std::string("securacv/") + kDeviceId + "/update/auto");
  CHECK(fake::published[0].second == "ON");
  CHECK(fake::published_on[0] == "loop");
  loop_pass();
  CHECK(fake::published.size() == 1);           // once
  std::printf("PASS the_auto_update_state_is_published_by_the_loop\n");
}

struct Test {
  const char* name;
  void (*fn)();
};
const Test kTests[] = {
    {"a_config_post_reinits_on_the_loop_task", test_a_config_post_reinits_on_the_loop_task},
    {"a_publish_in_flight_keeps_its_client", test_a_publish_in_flight_keeps_its_client},
    {"the_test_handler_reports_the_new_clients_connect", test_the_test_handler_reports_the_new_clients_connect},
    {"the_test_handler_is_bounded", test_the_test_handler_is_bounded},
    {"requests_coalesce_into_one_reinit", test_requests_coalesce_into_one_reinit},
    {"a_qr_request_keeps_the_identity_setup_gave", test_a_qr_request_keeps_the_identity_setup_gave},
    {"the_auto_update_state_is_published_by_the_loop", test_the_auto_update_state_is_published_by_the_loop},
};

}  // namespace reinit

int main(int argc, char** argv) {
  using namespace reinit;
  const char* only = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (const Test& t : kTests) {
    if (only != nullptr && std::strstr(t.name, only) == nullptr) continue;
    t.fn();
    ++ran;
  }
  CHECK(ran > 0);
  if (only != nullptr) {
    std::printf("%d of %zu tests run (%d checks), filter \"%s\"\n", ran,
                sizeof kTests / sizeof kTests[0], g_checks, only);
    return 0;
  }
  std::printf("ALL %d MQTT re-init checks PASSED (%d tests)\n", g_checks, ran);
  return 0;
}
