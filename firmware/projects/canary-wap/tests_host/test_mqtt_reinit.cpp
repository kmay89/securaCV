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
// And the loop task never stops a client (the F106 review): stop takes the
// client's API lock, which the esp_mqtt task holds across a connect attempt
// (network_timeout_ms, 10 s by default), so a stop on the loop task could
// outlast its 8 s panic watchdog. A re-init detaches the client on the loop
// task and a one-shot worker ("mqtt_retire") stops and destroys it; a later
// loop pass opens the new one. The fake's stop can be made to take as long
// as that lock wait (fake::stop_blocks_ms), and every loop pass is timed.
//
// The test is one thread, so a task is a role: fake::task names the task
// the test is playing, delay() (stubs/mqtt/Arduino.h) is where a waiting
// handler gives another task its turn, and a created task
// (stubs/mqtt/freertos/task.h) runs when the test gives it its turn
// (run_tasks). Host-tested only: the Arduino compile of the sketch is CI's,
// and nothing here runs esp_mqtt.
//
// And the loop task's publishes are bounded (sweep F112). While connected,
// esp_mqtt_client_publish() writes the socket on the publishing task, and
// the esp_mqtt task holds the client's API lock across its own socket
// operations; each gives up after the client's network.timeout_ms, which
// csi_mqtt.cpp used to leave at esp_mqtt's 10 s default, past the loop's
// 8 s watchdog. The fake follows esp-mqtt's publish path as its source reads
// at the commit ESP-IDF 5.5.4 pins (6af4446): a socket that takes nothing
// for the timeout fails the write, the connection is aborted (DISCONNECTED,
// dispatched on the task that hit it), and a publish to a client that is
// not connected returns -1 once it has the lock. The esp_mqtt task holds
// the API lock across every event it dispatches, CONNECTED included, whose
// handler sends the reconnect burst under it; the bridge announces the link
// only once that burst is sent (the F112 review). A model, not esp_mqtt.
//
// Run: ./test_mqtt_reinit [name]

#include "csi_mqtt.h"

#include <Arduino.h>       // stubs/mqtt: the clock and delay() hook
#include <Preferences.h>   // stubs/mqtt: the NVS

#include <freertos/task.h>  // stubs/mqtt: created tasks wait for their turn

#include "csi_event_egress.h"
#include "csi_integration.h"
#include "device_signature.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
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
  int network_timeout_ms = 0;  // as configured; 0 or less is esp_mqtt's 10 s
  bool connected = false;      // esp_mqtt's own view: CONNECTED until aborted
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
std::vector<int> published_to;             // the client of each publish
int publishes_on_dead = 0;
int destroyed_under_publish = 0;
std::function<void()> during_publish;      // runs inside the next publish
std::function<void()> during_client_init;  // runs inside the next esp_mqtt_client_init
// How long esp_mqtt_client_stop() holds its caller: the rest of a connect
// attempt the esp_mqtt task holds the API lock across (0: returns at once).
uint32_t stop_blocks_ms = 0;
// A stalled link (sweep F112): the socket takes no bytes for this long
// (0: it takes them at once). A write gives up after the client's network
// timeout; a stall shorter than that ends, and the write goes through.
uint32_t socket_stall_ms = 0;
// The esp_mqtt task is inside one of its own socket operations (a keepalive
// ping, a resend) when the next publish comes, holding the API lock; with
// the socket stalled it gives up after the timeout and aborts the connection.
bool mqtt_task_in_socket_op = false;
// A slow link, not a stalled one (the F112 review): every write the socket
// takes costs this long (0: at once). No write times out, since each makes
// progress, and that restarts esp_mqtt's network timeout.
uint32_t write_ms = 0;
// The client's API lock, as the esp_mqtt task holds it: across every event
// it dispatches (mqtt_client.c's esp_mqtt_task takes it at the top of each
// iteration and dispatches CONNECTED under it). A publish or subscribe from
// another task then waits for the dispatch to end. The fake cannot block,
// so it records each such wait and, when the dispatch ends, how long it was.
std::string lock_holder;
std::vector<std::pair<std::string, uint32_t>> lock_waits_open;   // task, since
std::vector<uint32_t> lock_waited_ms;

// esp_mqtt's own rule (mqtt_client.c at the IDF 5.5.4 pin): a timeout of 0
// or less is MQTT_NETWORK_TIMEOUT_MS, 10 s.
constexpr uint32_t kEspMqttDefaultTimeoutMs = 10000;
uint32_t timeout_of(const esp_mqtt_client* c) {
  return c->network_timeout_ms > 0 ? (uint32_t)c->network_timeout_ms : kEspMqttDefaultTimeoutMs;
}

void reset() {
  task = "loop";
  clients.clear();
  calls.clear();
  published.clear();
  published_on.clear();
  published_to.clear();
  publishes_on_dead = 0;
  destroyed_under_publish = 0;
  during_publish = nullptr;
  during_client_init = nullptr;
  stop_blocks_ms = 0;
  socket_stall_ms = 0;
  mqtt_task_in_socket_op = false;
  write_ms = 0;
  lock_holder.clear();
  lock_waits_open.clear();
  lock_waited_ms.clear();
}

// The task the test is playing, as the handle xTaskGetCurrentTaskHandle()
// gives (stubs/mqtt/freertos/task.h): one per role, never nullptr.
TaskHandle_t task_handle() {
  static std::map<std::string, uintptr_t> ids;
  auto it = ids.find(task);
  if (it == ids.end()) it = ids.emplace(task, ids.size() + 1).first;
  return reinterpret_cast<TaskHandle_t>(it->second);
}

// A call that takes the client's API lock: from a task other than the one
// holding it, it waits (recorded).
void take_lock() {
  if (!lock_holder.empty() && lock_holder != task) lock_waits_open.emplace_back(task, stub_mqtt::now_ms);
}

esp_mqtt_client* last_client() { return clients.empty() ? nullptr : clients.back(); }

int count(const std::string& what, const std::string& on_task = "") {
  int n = 0;
  for (const Call& c : calls) {
    if (c.what == what && (on_task.empty() || c.task == on_task)) ++n;
  }
  return n;
}

// The client's handler gets an event on the task the test is playing.
void dispatch(esp_mqtt_client* c, esp_mqtt_event_id_t id) {
  if (id == MQTT_EVENT_CONNECTED) c->connected = true;
  if (id == MQTT_EVENT_DISCONNECTED) c->connected = false;
  esp_mqtt_event_t e = {};
  e.event_id = id;
  e.client = c;
  c->handler(c->handler_args, "MQTT_EVENTS", (int32_t)id, &e);
}

// The esp_mqtt task delivers an event to the client's handler, holding the
// client's API lock across the dispatch.
void deliver(esp_mqtt_client* c, esp_mqtt_event_id_t id) {
  const std::string was = task;
  const std::string held = lock_holder;
  task = "mqtt";
  lock_holder = "mqtt";
  dispatch(c, id);
  lock_holder = held;
  for (const auto& w : lock_waits_open) lock_waited_ms.push_back(stub_mqtt::now_ms - w.second);
  lock_waits_open.clear();
  task = was;
}

// esp_mqtt_abort_connection(): the socket closed, the state WAIT_RECONNECT,
// DISCONNECTED dispatched on the calling task (esp_event_loop_run inline).
void abort_connection(esp_mqtt_client* c) { dispatch(c, MQTT_EVENT_DISCONNECTED); }

}  // namespace fake

extern "C" {

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t* config) {
  esp_mqtt_client* c = new esp_mqtt_client();
  c->id = (int)fake::clients.size() + 1;
  c->uri = config->broker.address.uri ? config->broker.address.uri : "";
  c->will_topic = config->session.last_will.topic ? config->session.last_will.topic : "";
  c->network_timeout_ms = config->network.timeout_ms;
  fake::clients.push_back(c);
  fake::calls.push_back({"init", fake::task, c->id});
  if (fake::during_client_init) {
    std::function<void()> f = std::move(fake::during_client_init);
    fake::during_client_init = nullptr;
    f();
  }
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
  stub_mqtt::now_ms += fake::stop_blocks_ms;   // the API lock, held across a connect
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
  fake::take_lock();
  // The API lock: the esp_mqtt task's own socket operation goes first, and
  // on a stalled socket it gives up after the timeout and aborts.
  if (fake::mqtt_task_in_socket_op) {
    fake::mqtt_task_in_socket_op = false;
    if (fake::socket_stall_ms > 0 && c->connected) {
      const uint32_t t = fake::timeout_of(c);
      if (fake::socket_stall_ms >= t) {
        stub_mqtt::now_ms += t;
        fake::socket_stall_ms -= t;
        const std::string was = fake::task;
        fake::task = "mqtt";
        fake::abort_connection(c);
        fake::task = was;
      } else {
        stub_mqtt::now_ms += fake::socket_stall_ms;
        fake::socket_stall_ms = 0;
      }
    }
  }
  // Not connected (any more): QoS 0 is not kept, -1.
  if (!c->connected) return -1;
  // The write: a stalled socket fails it after the timeout and aborts the
  // connection on this task; a shorter stall ends and the write goes through.
  if (fake::socket_stall_ms > 0) {
    const uint32_t t = fake::timeout_of(c);
    if (fake::socket_stall_ms >= t) {
      stub_mqtt::now_ms += t;
      fake::socket_stall_ms -= t;
      fake::abort_connection(c);
      return -1;
    }
    stub_mqtt::now_ms += fake::socket_stall_ms;
    fake::socket_stall_ms = 0;
  }
  stub_mqtt::now_ms += fake::write_ms;
  ++c->in_publish;
  if (fake::during_publish) {
    std::function<void()> f = std::move(fake::during_publish);
    fake::during_publish = nullptr;
    f();
  }
  --c->in_publish;
  fake::published.emplace_back(topic, std::string(data, data + len));
  fake::published_on.push_back(fake::task);
  fake::published_to.push_back(c->id);
  return 1;
}

int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char*, int) {
  fake::take_lock();
  if (c != nullptr && c->connected) stub_mqtt::now_ms += fake::write_ms;
  fake::calls.push_back({"subscribe", fake::task, c != nullptr ? c->id : 0});
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
uint32_t g_epoch_at_pump = 0;   // destination_epoch() as the last pump saw it
void pump() {
  ++g_pumps;
  g_epoch_at_pump = csi_mqtt::destination_epoch();
}
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

// The loop task is fed to an 8 s panic watchdog; the project's rule is that
// nothing that can block for more than 1 s runs on it at all
// (firmware/LESSONS_LEARNED.md). Every loop pass is held to the rule.
constexpr uint32_t kLoopPassBudgetMs = 1000;
uint32_t g_longest_loop_pass_ms = 0;
unsigned g_loop_passes = 0;

void loop_pass();
void run_tasks();

// What a reboot leaves: no client. The bridge's state outlives a test (it
// is csi_mqtt.cpp's), so whatever client the last test left is retired
// through the bridge's own path, with the bridge switched off.
void power_cycle() {
  stub_nvs().clear();                                 // disabled
  stub_mqtt::on_delay = nullptr;
  stub_mqtt::fail_task_create = 0;
  fake::stop_blocks_ms = 0;
  fake::during_client_init = nullptr;
  fake::during_publish = nullptr;
  (void)csi_mqtt::request_reinit();
  loop_pass();
  run_tasks();
  loop_pass();
  CHECK(!csi_mqtt::connected() && stub_mqtt::tasks.empty());
}

// A device whose bridge is enabled for `host`, booted: setup() gives the
// bridge its identity and start_http_server() runs the boot init(), both
// on the loop task; then the esp_mqtt task connects.
void boot_with_broker(const char* host, bool connect = true) {
  power_cycle();
  fake::reset();
  stub_nvs().clear();
  stub_mqtt::on_delay = nullptr;
  stub_mqtt::tasks.clear();
  stub_mqtt::tasks_created = 0;
  stub_mqtt::tasks_deleted = 0;
  stub_mqtt::fail_task_create = 0;
  g_longest_loop_pass_ms = 0;
  g_loop_passes = 0;
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
  fake::published_to.clear();
}

// The loop task's turn: one pass of csi_mqtt::loop(), timed.
void loop_pass() {
  const std::string was = fake::task;
  fake::task = "loop";
  const uint32_t start = stub_mqtt::now_ms;
  csi_mqtt::loop();
  const uint32_t took = stub_mqtt::now_ms - start;
  if (took > g_longest_loop_pass_ms) g_longest_loop_pass_ms = took;
  ++g_loop_passes;
  fake::task = was;
}

// The created tasks' turn: each runs to its end, as the task it is.
void run_tasks() {
  while (!stub_mqtt::tasks.empty()) {
    const stub_mqtt::Task t = stub_mqtt::tasks.front();
    stub_mqtt::tasks.erase(stub_mqtt::tasks.begin());
    const std::string was = fake::task;
    fake::task = t.name;
    t.fn(t.arg);
    fake::task = was;
  }
}

// What a re-init takes when nothing stalls it: the pass that detaches the
// open client, its worker, the pass that opens the new one.
void reinit_turns() {
  loop_pass();
  run_tasks();
  loop_pass();
}

// An httpd handler's run. From the handler's `turn_at`-th delay on (never
// when 0) the loop task gets a pass on every delay, and then the created
// tasks theirs (unless `workers` is false: a worker that has not finished);
// `on_delay` runs on every delay, for the esp_mqtt task's events.
struct Http {
  httpd_req_t req;
  unsigned delays = 0;
  uint32_t took_ms = 0;
};
Http http(esp_err_t (*handler)(httpd_req_t*), const std::string& body, unsigned turn_at,
          std::function<void(unsigned)> on_delay = nullptr, bool workers = true) {
  Http h;
  h.req.body = body;
  const uint32_t start = stub_mqtt::now_ms;
  stub_mqtt::on_delay = [&](uint32_t ms) {
    stub_mqtt::now_ms += ms;
    ++h.delays;
    if (turn_at != 0 && h.delays >= turn_at) {
      loop_pass();
      if (workers) run_tasks();
    }
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

// A new broker saved over POST /api/mqtt/config: the new client is made by
// the loop task and the old one stopped and destroyed by its worker, never
// by the handler; the handler still answers {"ok":true} once the new client
// is up.
void test_a_config_post_reinits_on_the_loop_task() {
  boot_with_broker("10.0.0.1");
  const Http h = http(csi_mqtt::handle_config_post, "{\"host\":\"10.0.0.2\"}", /*turn_at=*/1);
  CHECK(h.req.status == "200 OK");
  CHECK(h.req.resp == "{\"ok\":true}");
  CHECK(fake::count("stop") == 1 && fake::count("stop", "mqtt_retire") == 1);
  CHECK(fake::count("destroy") == 1 && fake::count("destroy", "mqtt_retire") == 1);
  CHECK(fake::count("init") == 1 && fake::count("init", "loop") == 1);
  CHECK(stub_mqtt::tasks_created == 1 && stub_mqtt::tasks_deleted == 1);   // the worker ended itself
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
  // The loop's next passes serve the request (the old client's worker
  // between them), and publishes go to the new client.
  reinit_turns();
  CHECK(fake::clients.size() == 2 && !fake::clients[0]->alive);
  CHECK(fake::count("destroy", "mqtt_retire") == 1 && fake::count("destroy", "httpd") == 0);
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
  CHECK(fake::count("destroy", "mqtt_retire") == 1 && fake::count("init", "loop") == 1);
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
  reinit_turns();
  CHECK(fake::count("destroy", "mqtt_retire") == 1 && fake::count("init", "loop") == 1);
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
  reinit_turns();
  CHECK(fake::count("init") == 1 && fake::count("destroy") == 1);
  CHECK(csi_mqtt::reinit_done(a) && csi_mqtt::reinit_done(b) && csi_mqtt::reinit_done(c));
  reinit_turns();
  CHECK(fake::count("init") == 1);              // served: no second re-init
  CHECK(stub_mqtt::tasks_created == 1);
  const uint32_t d = csi_mqtt::request_reinit();
  CHECK(!csi_mqtt::reinit_done(d));
  reinit_turns();
  CHECK(csi_mqtt::reinit_done(d) && fake::count("init") == 2);
  std::printf("PASS requests_coalesce_into_one_reinit\n");
}

// A QR provisioning saves the hub and only requests (the scanner's task):
// the loop's re-init opens the client with the identity setup() gave the
// bridge, even when no boot init() ran (the AP, and the HTTP server with
// it, did not start).
void test_a_qr_request_keeps_the_identity_setup_gave() {
  power_cycle();
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


// ── The stop that can wait out a connect (the F106 review) ──────────────

// The companion's "Test & save" with an unreachable broker: the config POST,
// then POST /api/mqtt/test at once, while the esp_mqtt task is inside a
// connect attempt and holds the API lock for the rest of it (9940 ms here).
// No loop pass may wait for that: the worker does, and the loop keeps
// turning (the watchdog is fed) while both handlers wait out their budgets.
// The new client opens only once the old one is gone, and one open serves
// both requests.
void test_a_stop_that_blocks_never_holds_the_loop() {
  boot_with_broker("10.0.0.1", /*connect=*/false);   // connecting, never connects
  fake::stop_blocks_ms = 9940;
  const Http save = http(csi_mqtt::handle_config_post, "{\"host\":\"10.0.0.2\"}", /*turn_at=*/1,
                         nullptr, /*workers=*/false);
  CHECK(g_longest_loop_pass_ms < kLoopPassBudgetMs);        // no pass waited for the stop
  CHECK(save.req.resp == "{\"ok\":true}");                  // the save stands
  CHECK(save.took_ms >= 2000 && save.took_ms < 2100);       // its 2 s, no more
  const Http test = http(csi_mqtt::handle_test, "", /*turn_at=*/1, nullptr, /*workers=*/false);
  CHECK(g_longest_loop_pass_ms < kLoopPassBudgetMs);
  CHECK(test.took_ms >= 4000 && test.took_ms < 4100);       // its 4 s, no more
  CHECK(test.req.resp.find("\"ok\":false") != std::string::npos);
  CHECK(g_loop_passes > 100);                               // the loop kept turning
  CHECK(fake::count("stop") == 0);                          // the worker has not had its turn
  CHECK(fake::clients.size() == 1);                         // and nothing opened before it
  CHECK(fake::count("init") == 0);
  CHECK(stub_mqtt::tasks_created == 1);                     // one worker, made once
  // The worker's turn: its stop waits out the connect, on the worker.
  run_tasks();
  CHECK(fake::count("stop") == 1 && fake::count("stop", "mqtt_retire") == 1);
  CHECK(fake::count("destroy", "mqtt_retire") == 1 && !fake::clients[0]->alive);
  loop_pass();
  CHECK(fake::clients.size() == 2 && fake::count("init", "loop") == 1);
  CHECK(fake::last_client()->uri == "mqtt://10.0.0.2:1883");
  loop_pass();
  CHECK(fake::clients.size() == 2);                         // both requests served by one open
  CHECK(fake::count("stop", "loop") == 0 && fake::count("destroy", "loop") == 0);
  CHECK(g_longest_loop_pass_ms < kLoopPassBudgetMs);
  std::printf("PASS a_stop_that_blocks_never_holds_the_loop\n");
}

// A detached client runs on until its worker's stop returns: nothing on the
// loop task publishes to it, and its events (a connect landing late, a drop,
// an error) are not the bridge's: they leave connected() false, subscribe
// nothing and publish nothing. The new client's are.
void test_a_detached_clients_events_are_ignored() {
  boot_with_broker("10.0.0.1");                       // connected
  (void)csi_mqtt::request_reinit();
  loop_pass();                                        // detached; the worker waits its turn
  CHECK(!csi_mqtt::connected());
  csi_mqtt::publish_counts(1);
  CHECK(fake::published.empty() && fake::publishes_on_dead == 0);
  esp_mqtt_client* old = fake::clients[0];
  fake::deliver(old, MQTT_EVENT_CONNECTED);           // its reconnect lands late
  CHECK(!csi_mqtt::connected());
  CHECK(fake::published.empty() && fake::count("subscribe") == 0);
  fake::deliver(old, MQTT_EVENT_DISCONNECTED);
  CHECK(!csi_mqtt::connected());
  run_tasks();
  loop_pass();
  CHECK(fake::clients.size() == 2 && !csi_mqtt::connected());
  fake::deliver(old, MQTT_EVENT_CONNECTED);           // (a stale event, even now)
  CHECK(!csi_mqtt::connected());
  fake::deliver(fake::last_client(), MQTT_EVENT_CONNECTED);
  CHECK(csi_mqtt::connected());
  CHECK(!fake::published.empty());
  for (int to : fake::published_to) CHECK(to == 2);   // "online" and discovery, on the new client
  CHECK(fake::count("subscribe") > 0);
  for (const fake::Call& c : fake::calls) {
    if (c.what == "subscribe") CHECK(c.client == 2 && c.task == "mqtt");
  }
  std::printf("PASS a_detached_clients_events_are_ignored\n");
}

// The worker could not be created (no memory): the old client stays
// detached and is not stopped on the loop task; a later pass makes the
// worker, and the re-init completes.
void test_a_worker_that_cannot_start_is_retried() {
  boot_with_broker("10.0.0.1");
  const uint32_t r = csi_mqtt::request_reinit();
  stub_mqtt::fail_task_create = 1;
  loop_pass();
  CHECK(stub_mqtt::tasks_created == 0 && stub_mqtt::tasks.empty());
  CHECK(fake::count("stop") == 0 && !csi_mqtt::reinit_done(r));
  loop_pass();                                        // made this time
  CHECK(stub_mqtt::tasks_created == 1);
  CHECK(fake::count("stop") == 0);
  run_tasks();
  loop_pass();
  CHECK(csi_mqtt::reinit_done(r) && fake::clients.size() == 2);
  CHECK(fake::count("stop", "mqtt_retire") == 1 && fake::count("stop", "loop") == 0);
  std::printf("PASS a_worker_that_cannot_start_is_retried\n");
}

// A second init() with a client open (the boot's is the only one the sketch
// makes) does not stop it there; it asks loop() for the re-init.
void test_a_second_init_asks_the_loop() {
  boot_with_broker("10.0.0.1");
  CHECK(csi_mqtt::init(kDeviceId, "2.4.15-wap", "ab"));
  CHECK(fake::count("stop") == 0 && fake::clients.size() == 1);
  reinit_turns();
  CHECK(fake::clients.size() == 2 && fake::count("stop", "mqtt_retire") == 1);
  std::printf("PASS a_second_init_asks_the_loop\n");
}

// ── Which requests one open serves ──────────────────────────────────────

void save_host(const char* host) {
  csi_mqtt::Config c;
  csi_mqtt::config_load(&c);
  strcpy(c.host, host);
  CHECK(csi_mqtt::config_save(c));
}

// A request is served by an open whose NVS read began after it was made. A
// Save that lands while an open runs (here: inside esp_mqtt_client_init,
// after the settings were read) is not served by it; the next re-init reads
// its settings. Answering it from the first would report {"ok":true} with
// the old broker still live.
void test_a_request_made_during_the_open_waits_for_the_next() {
  boot_with_broker("10.0.0.1");
  save_host("10.0.0.2");
  const uint32_t first = csi_mqtt::request_reinit();
  uint32_t late = 0;
  loop_pass();
  run_tasks();
  fake::during_client_init = [&] {
    save_host("10.0.0.3");
    late = csi_mqtt::request_reinit();
  };
  loop_pass();                                        // the open, with the Save landing inside it
  CHECK(late != 0);
  CHECK(fake::last_client()->uri == "mqtt://10.0.0.2:1883");
  CHECK(csi_mqtt::reinit_done(first));
  CHECK(!csi_mqtt::reinit_done(late));
  reinit_turns();
  CHECK(csi_mqtt::reinit_done(late));
  CHECK(fake::count("init") == 2);
  CHECK(fake::last_client()->uri == "mqtt://10.0.0.3:1883");
  std::printf("PASS a_request_made_during_the_open_waits_for_the_next\n");
}

// A request made while the old client is still being stopped is served by
// the open that follows: its read of NVS has not begun.
void test_a_request_made_during_the_stop_is_served_by_the_open() {
  boot_with_broker("10.0.0.1");
  save_host("10.0.0.2");
  const uint32_t first = csi_mqtt::request_reinit();
  loop_pass();                                        // detached; worker pending
  save_host("10.0.0.3");
  const uint32_t second = csi_mqtt::request_reinit();
  run_tasks();
  loop_pass();
  CHECK(csi_mqtt::reinit_done(first) && csi_mqtt::reinit_done(second));
  CHECK(fake::count("init") == 1);
  CHECK(fake::last_client()->uri == "mqtt://10.0.0.3:1883");
  std::printf("PASS a_request_made_during_the_stop_is_served_by_the_open\n");
}

// The re-init runs before the egress pump in loop(): the pass that opens a
// client for a new destination is the pass the pump sees its epoch, so
// nothing that waited for the old broker goes to the new one first.
void test_the_pump_sees_a_new_destination_the_pass_it_opens() {
  boot_with_broker("10.0.0.1");
  const uint32_t before = csi_mqtt::destination_epoch();
  save_host("10.0.0.2");
  (void)csi_mqtt::request_reinit();
  loop_pass();
  run_tasks();
  loop_pass();                                        // opens the new client, then pumps
  CHECK(fake::clients.size() == 2);
  CHECK(csi_mqtt::destination_epoch() == before + 1);
  CHECK(csi_event_egress::g_epoch_at_pump == before + 1);
  std::printf("PASS the_pump_sees_a_new_destination_the_pass_it_opens\n");
}

// ── The loop task's publishes are bounded (sweep F112) ──────────────────

// canary_wap.ino's WATCHDOG_TIMEOUT_SEC: the panic watchdog the loop task is
// subscribed to. check_wap_loop_commands.py holds the sketch's static_assert
// of csi_mqtt.h's budget against it.
constexpr uint32_t kLoopWatchdogMs = 8000;

// One busy loop pass's publishes, in canary_wap.ino's order: loop() (the
// re-init and the event egress pump), the retained status, health, counts
// and chain, and the chirp state. Returns how long the pass took.
uint32_t busy_publish_pass() {
  const std::string was = fake::task;
  fake::task = "loop";
  const uint32_t start = stub_mqtt::now_ms;
  csi_mqtt::loop();
  csi_mqtt::publish_status(true, true, -55);
  csi_mqtt::publish_health(150000, 3600);
  csi_mqtt::publish_counts(42);
  const uint8_t head[32] = {0};
  csi_mqtt::publish_chain(42, head);
  csi_mqtt::publish_chirp_state("active");
  fake::task = was;
  return stub_mqtt::now_ms - start;
}

// Every client the bridge opens, at boot and on a re-init, carries the
// network timeout, and the budget it sets fits under the loop's watchdog.
void test_every_client_bounds_its_network_operations() {
  CHECK(csi_mqtt::kNetworkTimeoutMs > 0);
  CHECK(csi_mqtt::kNetworkOpsBudget * csi_mqtt::kNetworkTimeoutMs < kLoopWatchdogMs);
  boot_with_broker("10.0.0.1");
  CHECK(fake::last_client()->network_timeout_ms == (int)csi_mqtt::kNetworkTimeoutMs);
  CHECK(fake::timeout_of(fake::last_client()) < kLoopWatchdogMs);
  save_host("10.0.0.2");
  (void)csi_mqtt::request_reinit();
  reinit_turns();
  CHECK(fake::clients.size() == 2);
  CHECK(fake::last_client()->network_timeout_ms == (int)csi_mqtt::kNetworkTimeoutMs);
  std::printf("PASS every_client_bounds_its_network_operations\n");
}

// The link dies with the client connected (Wi-Fi gone, the broker hung, the
// TCP send buffer full): the loop's first publish waits out one network
// timeout, fails, and aborts the connection; every publish after it in the
// pass returns at once. The pass stays under the watchdog. With esp_mqtt's
// 10 s default (the code before F112), the first publish alone held the
// loop task 10 s.
void test_a_stalled_link_cannot_hold_the_loop_past_its_watchdog() {
  boot_with_broker("10.0.0.1");
  fake::socket_stall_ms = 60000;
  const uint32_t took = busy_publish_pass();
  CHECK(took < kLoopWatchdogMs);
  CHECK(took == csi_mqtt::kNetworkTimeoutMs);        // one timeout, not one per publish
  CHECK(!csi_mqtt::connected());                     // the abort reached the bridge
  CHECK(fake::published.empty());                    // nothing claimed sent
  // The next pass does not touch the socket: publish_raw sees no link.
  const uint32_t again = busy_publish_pass();
  CHECK(again == 0);
  std::printf("PASS a_stalled_link_cannot_hold_the_loop_past_its_watchdog\n");
}

// The esp_mqtt task is inside its own socket operation (a keepalive ping)
// holding the API lock when the loop publishes, and the socket is stalled:
// it gives up after the timeout and aborts, and the loop's publish, which
// waited for the lock, returns -1 without writing. One timeout, not the
// esp_mqtt default.
void test_a_publish_behind_the_esp_mqtt_tasks_stalled_write_is_bounded() {
  boot_with_broker("10.0.0.1");
  fake::socket_stall_ms = 60000;
  fake::mqtt_task_in_socket_op = true;
  const uint32_t took = busy_publish_pass();
  CHECK(took < kLoopWatchdogMs);
  CHECK(took == csi_mqtt::kNetworkTimeoutMs);
  CHECK(!csi_mqtt::connected());
  CHECK(fake::published.empty());
  std::printf("PASS a_publish_behind_the_esp_mqtt_tasks_stalled_write_is_bounded\n");
}

// A link that is slow, not dead: a stall shorter than the timeout ends and
// the publish goes through. The timeout drops a stalled connection, not a
// slow one (and the loop pays the stall).
void test_a_stall_under_the_timeout_keeps_the_connection() {
  boot_with_broker("10.0.0.1");
  fake::socket_stall_ms = csi_mqtt::kNetworkTimeoutMs / 2;
  const uint32_t took = busy_publish_pass();
  CHECK(took == csi_mqtt::kNetworkTimeoutMs / 2);
  CHECK(csi_mqtt::connected());
  CHECK(fake::published.size() == 5);                // status, health, counts, chain, chirp
  std::printf("PASS a_stall_under_the_timeout_keeps_the_connection\n");
}

// The connect burst (the F112 review): esp_mqtt dispatches CONNECTED with
// the client's API lock held, and the handler sends the retained status, the
// discovery set, the cached states and the subscribes under it. Over a slow
// link (every write waits a little on the socket, none long enough to time
// out) that alone outlasts the loop's watchdog, and every write restarts
// the network timeout, so the timeout does not bound it. A loop pass in the
// middle of it must not wait for that lock: the bridge says the link is up
// only once the burst is sent, so the pass's publishes return at the gate
// (the egress keeps its rows for the next pass). Before, the handler said
// so first, and the pass's first publish waited out the rest of the burst.
void test_a_loop_pass_never_waits_behind_the_connect_burst() {
  boot_with_broker("10.0.0.1", /*connect=*/false);
  fake::write_ms = 250;
  size_t pass_published = 99;
  bool up_during_burst = true;
  fake::during_publish = [&] {          // inside the burst's first write (the status)
    CHECK(fake::task == "mqtt");
    up_during_burst = csi_mqtt::connected();
    const size_t before = fake::published.size();
    (void)busy_publish_pass();
    pass_published = fake::published.size() - before;
  };
  const uint32_t t0 = stub_mqtt::now_ms;
  fake::deliver(fake::last_client(), MQTT_EVENT_CONNECTED);
  const uint32_t burst_ms = stub_mqtt::now_ms - t0;
  CHECK(burst_ms > kLoopWatchdogMs);                 // the burst alone, at 250 ms a write
  uint32_t longest_wait = 0;
  for (uint32_t w : fake::lock_waited_ms) longest_wait = w > longest_wait ? w : longest_wait;
  if (longest_wait > 0) {
    std::fprintf(stderr, "the loop waited %u ms for the burst's lock\n", (unsigned)longest_wait);
  }
  CHECK(fake::lock_waited_ms.empty());               // the pass took no lock the burst held
  CHECK(!up_during_burst && pass_published == 0);
  CHECK(csi_mqtt::connected());                      // announced once the burst was sent
  CHECK(!fake::published.empty() && fake::published.front().first == "securacv/canary-wap-7f3a/status");
  for (const std::string& t : fake::published_on) CHECK(t == "mqtt");
  // The next pass publishes as before.
  fake::write_ms = 0;
  const size_t before = fake::published.size();
  CHECK(busy_publish_pass() == 0);
  CHECK(fake::published.size() - before == 5);       // status, health, counts, chain, chirp
  std::printf("PASS a_loop_pass_never_waits_behind_the_connect_burst\n");
}

// The burst's own write fails (the link stalls as it connects): esp_mqtt
// aborts the connection and dispatches DISCONNECTED on the esp_mqtt task,
// inside the CONNECTED dispatch. The burst stops at the gate after one
// timeout, and the link is never announced.
void test_a_connect_burst_cut_short_leaves_the_link_down() {
  boot_with_broker("10.0.0.1", /*connect=*/false);
  fake::socket_stall_ms = 60000;
  const uint32_t t0 = stub_mqtt::now_ms;
  fake::deliver(fake::last_client(), MQTT_EVENT_CONNECTED);
  CHECK(stub_mqtt::now_ms - t0 == csi_mqtt::kNetworkTimeoutMs);
  CHECK(!csi_mqtt::connected());
  CHECK(fake::published.empty());
  CHECK(busy_publish_pass() == 0);                   // the loop does not touch the socket
  CHECK(fake::published.empty());
  std::printf("PASS a_connect_burst_cut_short_leaves_the_link_down\n");
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
    {"a_stop_that_blocks_never_holds_the_loop", test_a_stop_that_blocks_never_holds_the_loop},
    {"a_detached_clients_events_are_ignored", test_a_detached_clients_events_are_ignored},
    {"a_worker_that_cannot_start_is_retried", test_a_worker_that_cannot_start_is_retried},
    {"a_second_init_asks_the_loop", test_a_second_init_asks_the_loop},
    {"a_request_made_during_the_open_waits_for_the_next", test_a_request_made_during_the_open_waits_for_the_next},
    {"a_request_made_during_the_stop_is_served_by_the_open", test_a_request_made_during_the_stop_is_served_by_the_open},
    {"the_pump_sees_a_new_destination_the_pass_it_opens", test_the_pump_sees_a_new_destination_the_pass_it_opens},
    {"every_client_bounds_its_network_operations", test_every_client_bounds_its_network_operations},
    {"a_stalled_link_cannot_hold_the_loop_past_its_watchdog", test_a_stalled_link_cannot_hold_the_loop_past_its_watchdog},
    {"a_publish_behind_the_esp_mqtt_tasks_stalled_write_is_bounded", test_a_publish_behind_the_esp_mqtt_tasks_stalled_write_is_bounded},
    {"a_stall_under_the_timeout_keeps_the_connection", test_a_stall_under_the_timeout_keeps_the_connection},
    {"a_loop_pass_never_waits_behind_the_connect_burst", test_a_loop_pass_never_waits_behind_the_connect_burst},
    {"a_connect_burst_cut_short_leaves_the_link_down", test_a_connect_burst_cut_short_leaves_the_link_down},
};

}  // namespace reinit

int main(int argc, char** argv) {
  using namespace reinit;
  stub_mqtt::current_task = fake::task_handle;
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
