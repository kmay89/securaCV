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
// Being the harness that compiles csi_mqtt.cpp, it also holds the health
// publish's event-id space warning (sweep F82): `event_id_space_low` comes
// up once the allocator reaches kHoldLimit, and stays up after a wrap.
//
// Run: ./test_mqtt_reinit [name]

#include "csi_mqtt.h"

#include <Arduino.h>       // stubs/mqtt: the clock and delay() hook
#include <Preferences.h>   // stubs/mqtt: the NVS

#include <freertos/task.h>  // stubs/mqtt: created tasks wait for their turn

#include "csi_event_egress.h"
#include "csi_integration.h"
#include "device_signature.h"
#include "json_strict.h"     // GET /api/mqtt/config parses under JSON.parse's rules (sweep F227)
#include "mqtt_transport_logic.h"  // ca_pem_looks_valid(): the shape the CA test cuts past

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
  std::string certificate;     // broker.verification.certificate, as configured
  std::string username;        // credentials.username, as configured (sweep F289)
  std::string password;        // credentials.authentication.password, as configured
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
std::vector<bool> published_retained;      // each publish's retain flag
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
  published_retained.clear();
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
  c->certificate = config->broker.verification.certificate ? config->broker.verification.certificate : "";
  c->username = config->credentials.username ? config->credentials.username : "";
  c->password = config->credentials.authentication.password
                    ? config->credentials.authentication.password : "";
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
                            int len, int, int retain) {
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
  fake::published_retained.push_back(retain != 0);
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

// The request's dashboard session, as the device's session store would
// answer for it (cv_session_store.h, sweep F289): `any` is a live session of
// either origin, `owner` one a BOOT tap or a setup-door claim minted. A
// landing-page session is any && !owner. Every test starts as the owner
// (boot_with_broker()), which is what "a session" could do before F289.
namespace session {
bool any = true;
bool owner = true;
}  // namespace session

namespace csi_integration {
bool session_validate_cookie(httpd_req_t*) { return session::any; }
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

// api_auth.h's weak hook for an owner's session, which csi_integration.cpp
// defines on a device.
bool cv_session_validate_owner(httpd_req_t*) { return session::any && session::owner; }

namespace device_signature {
bool sign_chain(uint32_t, const uint8_t[32], char*, size_t) { return false; }
bool sign_event(uint32_t, const char*, const char*, const char*, int, int, int, char*, size_t) {
  return false;
}
bool sign_counts(uint32_t, char*, size_t) { return false; }
const char* fingerprint_hex() { return "0011223344556677"; }
}  // namespace device_signature

// The chokepoint's allocator: the health publish reads its next id for the
// event-id space warning (sweep F82).
uint32_t g_next_event_id = 0xC0000000u;
extern "C" uint32_t csi_event_get_next_event_id(void) { return g_next_event_id; }

namespace csi_event_egress {
int g_pumps = 0;
uint32_t g_epoch_at_pump = 0;   // destination_epoch() as the last pump saw it
void pump() {
  ++g_pumps;
  g_epoch_at_pump = csi_mqtt::destination_epoch();
}
// The egress's counters, as its stats() returns them on the loop task; the
// egress topic's publish reads them (sweep F149).
Stats g_stats = {};
std::vector<std::string> g_stats_read_on;   // the task of each stats() read
Stats stats() {
  g_stats_read_on.push_back(fake::task);
  return g_stats;
}
}  // namespace csi_event_egress

// ── GET /api/mqtt/config's hook and heap count (sweep F227 review) ──────

// wap_json_writer.h's hook in csi_mqtt.cpp's build (mqtt_test_hooks.h, the
// Makefile's -include): each string the writing pass has measured.
namespace mqtt_test {
void (*after_measure)(const char* s) = nullptr;
}  // namespace mqtt_test

// The blocks csi_mqtt.cpp takes with malloc() and gives back with free(),
// counted while `counting` is set. On Linux the Makefile links this suite
// with GNU ld's --wrap=malloc and --wrap=free, so every malloc() and free()
// this binary's own objects call (csi_mqtt.cpp's among them; libstdc++'s
// are its own and are not counted) comes through here. Elsewhere nothing is
// counted, and the test that reads the count says it did not run.
namespace heap {
bool counting = false;
size_t taken = 0;              // blocks malloc'd while counting
void* held[64] = {};           // ... and not yet freed
size_t n_held = 0;
bool overflowed = false;       // more than `held` holds were outstanding
}  // namespace heap

#ifdef MQTT_TEST_HEAP_WRAP
extern "C" void* __real_malloc(size_t n);
extern "C" void __real_free(void* p);
extern "C" void* __wrap_malloc(size_t n) {
  void* p = __real_malloc(n);
  if (heap::counting && p != nullptr) {
    ++heap::taken;
    if (heap::n_held < sizeof heap::held / sizeof heap::held[0]) heap::held[heap::n_held++] = p;
    else heap::overflowed = true;
  }
  return p;
}
extern "C" void __wrap_free(void* p) {
  if (heap::counting && p != nullptr) {
    for (size_t i = 0; i < heap::n_held; ++i) {
      if (heap::held[i] == p) {
        heap::held[i] = heap::held[--heap::n_held];
        break;
      }
    }
  }
  __real_free(p);
}
constexpr bool kHeapCounted = true;
#else
constexpr bool kHeapCounted = false;
#endif

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
void boot_with_broker(const char* host, bool connect = true, const char* user = "",
                      const char* pass = "") {
  power_cycle();
  fake::reset();
  session::any = true;      // an owner's session, and no bearer token set
  session::owner = true;
  csi_mqtt::set_api_token_provider(nullptr);
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
  strcpy(c.user, user);
  strcpy(c.pass, pass);
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
  fake::published_retained.clear();
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
// How the next request's body reaches the handler (sweep F266), taken by
// the next http() and cleared: the socket's reads (stubs/mqtt's recv_plan:
// a cap per read, or an error code) and a Content-Length that is not the
// body's size (a peer that closes early, or one that claims too much).
struct Wire {
  std::vector<int> plan;
  bool set_len = false;
  size_t content_len = 0;
};
Wire g_wire;
// The request headers the next http() call sends (taken, then cleared).
std::map<std::string, std::string> g_next_req_hdrs;

Http http(esp_err_t (*handler)(httpd_req_t*), const std::string& body, unsigned turn_at,
          std::function<void(unsigned)> on_delay = nullptr, bool workers = true) {
  Http h;
  h.req.body = body;
  h.req.content_len = g_wire.set_len ? g_wire.content_len : body.size();   // IDF: Content-Length
  h.req.recv_plan = g_wire.plan;
  g_wire = Wire{};
  h.req.req_hdrs = std::move(g_next_req_hdrs);
  g_next_req_hdrs.clear();
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

// ── The health publish (sweep F82) ──────────────────────────────────────

// The health topic says when the event-id space is running out: from the
// moment the allocator's next id reaches csi_event_id_floor::kHoldLimit,
// and after it wraps (ids from 1 again). The canary PIO tree's health
// carries the same flag. Home Assistant refuses a wrapped device's events;
// nothing said so before.
std::string last_health() {
  for (size_t i = fake::published.size(); i-- > 0;) {
    const std::string& topic = fake::published[i].first;
    if (topic.size() >= 7 && topic.compare(topic.size() - 7, 7, "/health") == 0) {
      return fake::published[i].second;
    }
  }
  return "";
}

void test_the_health_publish_warns_before_the_id_space_runs_out() {
  boot_with_broker("10.0.0.1");
  g_next_event_id = 0xC0000000u + 1234;
  csi_mqtt::publish_health(200000, 312);
  CHECK(last_health().find("\"event_id_space_low\":false") != std::string::npos);
  g_next_event_id = 0xF0000000u - 1;
  csi_mqtt::publish_health(200000, 312);
  CHECK(last_health().find("\"event_id_space_low\":false") != std::string::npos);
  g_next_event_id = 0xF0000000u;   // kHoldLimit
  csi_mqtt::publish_health(200000, 312);
  CHECK(last_health().find("\"event_id_space_low\":true") != std::string::npos);
  g_next_event_id = 7;             // wrapped
  const csi_mqtt::MqttTamperLevels levels = {1, 0};
  csi_mqtt::MqttBatteryInfo batt = {80, 95, 3900, "discharging"};
  csi_mqtt::publish_health(200000, 312, &batt, &levels);
  const std::string body = last_health();
  CHECK(body.find("\"event_id_space_low\":true") != std::string::npos);
  CHECK(body.find("\"sd_mounted\":true") != std::string::npos && body.back() == '}');
  g_next_event_id = 0xC0000000u;
  std::printf("PASS the_health_publish_warns_before_the_id_space_runs_out\n");
}

// ── The egress counters' topic (sweep F149) ────────────────────────────

// The canary-wap's egress counters reach Home Assistant on a retained topic
// of their own, right after health: the object (up to 319 bytes) does not
// fit the health body's 34 spare bytes. The body names the health it
// follows (its firmware version and uptime), so Home Assistant can tell a
// retained copy from an earlier boot, or one a firmware with no egress topic
// left behind, from this boot's; the counters ride under the canary health's
// key, `csi_event_egress`, name for name.
std::string last_on(const std::string& suffix, bool* retained = nullptr) {
  for (size_t i = fake::published.size(); i-- > 0;) {
    const std::string& topic = fake::published[i].first;
    if (topic.size() >= suffix.size() &&
        topic.compare(topic.size() - suffix.size(), suffix.size(), suffix) == 0) {
      if (retained != nullptr) *retained = fake::published_retained[i];
      return fake::published[i].second;
    }
  }
  return "";
}

void test_the_egress_counters_ride_a_retained_topic_of_their_own() {
  boot_with_broker("10.0.0.1");
  csi_event_egress::Stats& st = csi_event_egress::g_stats;
  st.dropped = 1; st.held_dropped = 2; st.ambient_dropped = 3; st.unsent_dropped = 4;
  st.planner.live = 5; st.planner.held = 6; st.planner.queued = 7; st.planner.replayed = 8;
  st.planner.skipped = 9; st.planner.untrusted = 10; st.planner.unsendable = 11;
  st.planner.truncated_unsent = 12; st.planner.read_giveups = 13;
  // Before this boot's first health body there is no health to name: the
  // retained copy on the broker stays as it is.
  csi_mqtt::publish_egress();
  CHECK(fake::published.empty());
  csi_mqtt::publish_health(200000, 312);
  CHECK(fake::published.size() == 1);
  csi_event_egress::g_stats_read_on.clear();
  csi_mqtt::publish_egress();
  CHECK(fake::published.size() == 2);
  CHECK(fake::published[1].first == std::string("securacv/") + kDeviceId + "/egress");
  CHECK(fake::published_retained[1]);   // a hub that starts later still reads them
  CHECK(fake::published[1].second ==
        "{\"firmware_version\":\"2.4.15-wap\",\"uptime\":312,\"csi_event_egress\":"
        "{\"dropped\":1,\"held_dropped\":2,\"ambient_dropped\":3,\"unsent_dropped\":4,"
        "\"planner\":{\"live\":5,\"held\":6,\"queued\":7,\"replayed\":8,\"skipped\":9,"
        "\"untrusted\":10,\"unsendable\":11,\"truncated_unsent\":12,\"read_giveups\":13}}}");
  CHECK(csi_event_egress::g_stats_read_on.size() == 1 &&
        csi_event_egress::g_stats_read_on[0] == "loop");
  // The next health's uptime is the one the next egress body names.
  csi_mqtt::publish_health(200000, 372);
  csi_mqtt::publish_egress();
  CHECK(last_on("/egress").find("\"uptime\":372,") != std::string::npos);
  // Every counter at its widest, the longest version the identity keeps
  // (23 characters) and the widest uptime: the whole body goes out.
  memset(&st, 0xFF, sizeof(st));
  csi_mqtt::set_identity(kDeviceId, "2.4.15-wap-rc1+abcdef12", "ab");
  csi_mqtt::publish_health(200000, 4294967295u);
  csi_mqtt::publish_egress();
  bool retained = false;
  const std::string widest = last_on("/egress", &retained);
  CHECK(widest.size() == 405 && retained && widest.back() == '}');
  CHECK(widest.size() < csi_mqtt::kEgressBodyMax);
  CHECK(widest.find("\"firmware_version\":\"2.4.15-wap-rc1+abcdef12\",\"uptime\":4294967295,") !=
        std::string::npos);
  // The health body is not where they went: it keeps its own keys.
  CHECK(last_on("/health").find("dropped") == std::string::npos);
  csi_mqtt::set_identity(kDeviceId, "2.4.15-wap", "ab");
  // With the link down nothing is published (the loop's 60 s block only
  // runs while connected; the bridge's gate refuses anyway).
  fake::deliver(fake::last_client(), MQTT_EVENT_DISCONNECTED);
  const size_t before = fake::published.size();
  csi_mqtt::publish_egress();
  CHECK(fake::published.size() == before);
  st = csi_event_egress::Stats{};
  std::printf("PASS the_egress_counters_ride_a_retained_topic_of_their_own\n");
}

// Sweep F150: a read-only open of a "csi" namespace nothing has created
// logged "nvs_open failed: NOT_FOUND" at error level (Arduino-ESP32's
// Preferences::begin()). The bridge's boot read of its settings goes through
// csi_module_settings_nvs.h's begin_read_only(), which asks IDF's nvs_open()
// first: on such NVS it opens nothing, logs nothing, and loads the defaults
// it loaded before (disabled, port 1883, the default prefix, discovery on).
// A namespace that is there is read as before.
void test_the_settings_read_on_a_fresh_nvs_logs_nothing() {
  stub_nvs().clear();
  stub_nvs_created().clear();
  const int logs = stub_nvs_error_logs();
  csi_mqtt::Config cfg;
  CHECK(csi_mqtt::config_load(&cfg));
  CHECK(stub_nvs_error_logs() == logs);
  CHECK(!stub_nvs_has_namespace("csi"));          // a read creates nothing
  CHECK(!cfg.enabled && cfg.port == 1883 && cfg.discovery && !cfg.ca_set);
  CHECK(std::string(cfg.prefix) == "securacv" && cfg.host[0] == '\0');

  // Stored, then read back with no error line.
  cfg.enabled = true;
  std::snprintf(cfg.host, sizeof cfg.host, "%s", "10.0.0.9");
  cfg.discovery = false;
  CHECK(csi_mqtt::config_save(cfg));
  csi_mqtt::Config back;
  CHECK(csi_mqtt::config_load(&back));
  CHECK(stub_nvs_error_logs() == logs);
  CHECK(back.enabled && std::string(back.host) == "10.0.0.9" && !back.discovery);
  stub_nvs().clear();
  stub_nvs_created().clear();
  std::printf("PASS the_settings_read_on_a_fresh_nvs_logs_nothing\n");
}

// Sweep F267: once anything has created the shared "csi" namespace (the
// egress writes its delivery ceiling there at boot, csi_event_egress.cpp),
// the read-only open succeeds, and config_load() read the host, user,
// password and prefix with getString() whether or not they were there.
// Arduino-ESP32 3.3.8's getString() logs "nvs_get_str len fail: <key>
// NOT_FOUND" at error level for a missing key (the stub counts each), so a
// debug build whose bridge was never configured printed four error lines at
// every boot, re-init and config GET or POST. Each is now read behind
// isKey(), as the time zone and the CA are: no line, and the defaults the
// refused reads left (no host, user or password, the default prefix,
// disabled, port 1883, discovery on). A namespace holding only some of the
// strings reads those and logs nothing for the rest.
void test_an_unconfigured_bridge_reads_its_settings_without_an_error_line() {
  stub_nvs().clear();
  stub_nvs_created().clear();
  {
    Preferences p;   // the egress's ceiling record, as it writes it
    CHECK(p.begin("csi", false));
    CHECK(p.putULong(csi_mqtt::NVS_KEY_DELIVERED, 0xC0000040u) > 0);
    p.end();
  }
  CHECK(stub_nvs_has_namespace("csi"));
  const int logs = stub_nvs_error_logs();
  csi_mqtt::Config cfg;
  std::memset(&cfg, 0x5A, sizeof cfg);   // config_load() owns every field
  CHECK(csi_mqtt::config_load(&cfg));
  if (stub_nvs_error_logs() != logs) {
    std::printf("FAIL config_load() on an unconfigured namespace logged %d error line(s)\n",
                stub_nvs_error_logs() - logs);
  }
  CHECK(stub_nvs_error_logs() == logs);
  CHECK(!cfg.enabled && cfg.port == 1883 && cfg.discovery && !cfg.ca_set);
  CHECK(cfg.tls_mode == 0 && !cfg.tls);
  CHECK(cfg.host[0] == '\0' && cfg.user[0] == '\0' && cfg.pass[0] == '\0');
  CHECK(std::string(cfg.prefix) == "securacv");

  // The config GET reads the same way: no line either.
  const int before_get = stub_nvs_error_logs();
  const Http get = http(csi_mqtt::handle_config_get, "", /*turn_at=*/0);
  CHECK(get.req.status == "200 OK");
  CHECK(stub_nvs_error_logs() == before_get);

  // Some of the strings stored: those come back, the rest stay at their
  // defaults, and nothing is logged.
  {
    Preferences p;
    CHECK(p.begin("csi", false));
    CHECK(p.putString(csi_mqtt::NVS_KEY_HOST, "10.0.0.7") > 0);
    CHECK(p.putString(csi_mqtt::NVS_KEY_PREFIX, "attic") > 0);
    p.end();
  }
  const int before_some = stub_nvs_error_logs();
  csi_mqtt::Config some;
  CHECK(csi_mqtt::config_load(&some));
  CHECK(stub_nvs_error_logs() == before_some);
  CHECK(std::string(some.host) == "10.0.0.7" && std::string(some.prefix) == "attic");
  CHECK(some.user[0] == '\0' && some.pass[0] == '\0');

  // The user without the password (config_save() writes all four, so a
  // first save cut by a power loss between those two writes leaves this),
  // and the password without the user: each read behind its own key.
  for (int which = 0; which < 2; ++which) {
    stub_nvs().clear();
    stub_nvs_created().clear();
    {
      Preferences p;
      CHECK(p.begin("csi", false));
      CHECK(p.putString(which == 0 ? csi_mqtt::NVS_KEY_USER : csi_mqtt::NVS_KEY_PASS,
                        which == 0 ? "ada" : "s3cret") > 0);
      p.end();
    }
    const int before_one = stub_nvs_error_logs();
    csi_mqtt::Config one;
    CHECK(csi_mqtt::config_load(&one));
    CHECK(stub_nvs_error_logs() == before_one);
    if (which == 0) {
      CHECK(std::string(one.user) == "ada" && one.pass[0] == '\0');
    } else {
      CHECK(std::string(one.pass) == "s3cret" && one.user[0] == '\0');
    }
    CHECK(one.host[0] == '\0' && std::string(one.prefix) == "securacv");
  }
  stub_nvs().clear();
  stub_nvs_created().clear();
  std::printf("PASS an_unconfigured_bridge_reads_its_settings_without_an_error_line\n");
}

// ── GET /api/mqtt/config (sweep F227) ──────────────────────────────────

// The old format of GET /api/mqtt/config's answer (an unescaped %s per
// string), filled from the config as stored and the bridge as it stands: an
// ordinary config's answer must still be these bytes.
std::string old_config_answer() {
  csi_mqtt::Config c;
  csi_mqtt::config_load(&c);
  char old[800];
  snprintf(old, sizeof(old),
    "{"
      "\"enabled\":%s,"
      "\"host\":\"%s\","
      "\"port\":%u,"
      "\"user\":\"%s\","
      "\"prefix\":\"%s\","
      "\"tls\":%s,"
      "\"tls_mode\":%u,"
      "\"ca_set\":%s,"
      "\"transport\":\"%s\","
      "\"last_error\":\"%s\","
      "\"discovery\":%s,"
      "\"password_set\":%s,"
      "\"connected\":%s"
    "}",
    c.enabled ? "true" : "false", c.host, (unsigned)c.port, c.user, c.prefix,
    c.tls ? "true" : "false", (unsigned)c.tls_mode, c.ca_set ? "true" : "false",
    csi_mqtt::transport_name(), csi_mqtt::last_error(), c.discovery ? "true" : "false",
    c.pass[0] ? "true" : "false", csi_mqtt::connected() ? "true" : "false");
  return old;
}

// One GET /api/mqtt/config, with the blocks it took from the heap and did
// not give back counted (Linux: the Makefile's --wrap; see `heap` above).
struct ConfigGet {
  Http h;
  size_t taken = 0;
  size_t kept = 0;
};
ConfigGet get_config() {
  ConfigGet g;
  heap::taken = 0;
  heap::n_held = 0;
  heap::overflowed = false;
  heap::counting = true;
  g.h = http(csi_mqtt::handle_config_get, "", /*turn_at=*/0);
  heap::counting = false;
  CHECK(!heap::overflowed);
  g.taken = heap::taken;
  g.kept = heap::n_held;
  return g;
}

// The settings page's strings come back as JSON whatever they hold. The
// POST's json_extract_string() decodes \" \\ and the control escapes and
// passes raw bytes, so a host or user holding a quote, a backslash or a
// control byte was stored as such, and the GET wrote it with an unescaped
// %s: the answer stopped parsing and the settings page could not show the
// broker it had saved. (The prefix cannot hold one: the POST keeps it to
// [a-zA-Z0-9_/-].) Now the answer is written by wap_json_writer.h and gives
// every string back as stored; an ordinary config answers the bytes the old
// format did, field for field.
void test_the_config_answer_escapes_what_the_page_sent() {
  boot_with_broker("10.0.0.1");
  // An ordinary config with no field at its default: a TLS port, the CA
  // mode (no CA saved, so the bridge refuses to open and says why in
  // last_error), a prefix and a user of its own. The old format's bytes,
  // verbatim, and each field read back.
  {
    const Http p = http(csi_mqtt::handle_config_post,
                        "{\"port\":8883,\"tls_mode\":1,\"prefix\":\"home/wap-2\",\"user\":\"ops\","
                        "\"discovery\":false}",
                        /*turn_at=*/1);
    CHECK(p.req.status == "200 OK");
    CHECK(csi_mqtt::last_error()[0] != '\0');          // the refusal, in words
    CHECK(std::strcmp(csi_mqtt::transport_name(), "refused") == 0);
    const ConfigGet g = get_config();
    CHECK(g.h.req.status == "200 OK");
    CHECK(g.h.req.resp == old_config_answer());
    json_strict::Value v;
    CHECK(json_strict::parse(g.h.req.resp, &v));
    const json_strict::Value* f;
    CHECK((f = v.get("enabled")) != nullptr && f->kind == json_strict::Value::Bool && f->b);
    CHECK((f = v.get("host")) != nullptr && f->text == "10.0.0.1");
    CHECK((f = v.get("port")) != nullptr && f->kind == json_strict::Value::Number && f->text == "8883");
    CHECK((f = v.get("user")) != nullptr && f->text == "ops");
    CHECK((f = v.get("prefix")) != nullptr && f->text == "home/wap-2");
    CHECK((f = v.get("tls")) != nullptr && f->kind == json_strict::Value::Bool && f->b);
    CHECK((f = v.get("tls_mode")) != nullptr && f->kind == json_strict::Value::Number && f->text == "1");
    CHECK((f = v.get("ca_set")) != nullptr && f->kind == json_strict::Value::Bool && !f->b);
    CHECK((f = v.get("transport")) != nullptr && f->text == "refused");
    CHECK((f = v.get("last_error")) != nullptr && f->text == csi_mqtt::last_error());
    CHECK((f = v.get("discovery")) != nullptr && f->kind == json_strict::Value::Bool && !f->b);
    CHECK((f = v.get("password_set")) != nullptr && f->kind == json_strict::Value::Bool && !f->b);
    CHECK((f = v.get("connected")) != nullptr && f->kind == json_strict::Value::Bool && !f->b);
    CHECK(v.keys.size() == 13);                         // nothing else, the password least of all
  }
  // A host and a user holding a quote, a backslash, decoded control escapes
  // and a raw control byte, saved through the real POST.
  std::string body = "{\"tls_mode\":0,\"port\":1883,\"host\":\"br\\\"ok\\\\er";
  body.push_back('\x01');
  body += ".lan\",\"user\":\"u\\nse\\tr";
  body.push_back('\x1f');
  body += "\",\"password\":\"pw\"}";
  const Http h = http(csi_mqtt::handle_config_post, body, /*turn_at=*/1);
  CHECK(h.req.status == "200 OK");
  csi_mqtt::Config c;
  csi_mqtt::config_load(&c);
  const std::string host = std::string("br\"ok\\er") + '\x01' + ".lan";
  const std::string user = std::string("u\nse\tr") + '\x1f';
  CHECK(host == c.host);                         // stored as decoded
  CHECK(user == c.user);
  const ConfigGet g = get_config();
  CHECK(g.h.req.status == "200 OK");
  json_strict::Value v;
  CHECK(json_strict::parse(g.h.req.resp, &v));
  const json_strict::Value* vh = v.get("host");
  const json_strict::Value* vu = v.get("user");
  const json_strict::Value* vp = v.get("password_set");
  CHECK(vh != nullptr && vh->text == host);
  CHECK(vu != nullptr && vu->text == user);
  CHECK(vp != nullptr && vp->kind == json_strict::Value::Bool && vp->b);
  CHECK(!has_key(g.h.req.resp, "pass"));         // the password stays write-only
  // A long host and user, every byte escaping to six: an answer past 800
  // bytes, answered whole. The old format could not reach 800 (its longest,
  // every field at its limit, is 622 bytes), so its fixed 800-byte buffer
  // never cut one; an escaped answer can pass it, so the answer's buffer is
  // its measured length. (Kept under the fields' limits: what config_load()
  // reads back at the limit is F241's.)
  const std::string long_host(120, '\x02'), long_user(60, '\x03');
  (void)http(csi_mqtt::handle_config_post, "{\"host\":\"" + long_host + "\",\"user\":\"" + long_user + "\"}",
             /*turn_at=*/1);
  const ConfigGet gl = get_config();
  CHECK(gl.h.req.resp.size() > 800);
  json_strict::Value lv;
  CHECK(json_strict::parse(gl.h.req.resp, &lv));
  const json_strict::Value* lh = lv.get("host");
  const json_strict::Value* lu = lv.get("user");
  CHECK(lh != nullptr && lh->text == long_host);
  CHECK(lu != nullptr && lu->text == long_user);
  std::printf("PASS the_config_answer_escapes_what_the_page_sent\n");
}

// The answer's heap buffer is given back on every path (the F227 review):
// the answer sent, and the answer refused because a string grew between the
// measuring pass and the writing pass. Nothing in csi_mqtt.cpp grows one
// (both passes read the handler's own copies), so the refusal is reached
// through wap_json_writer.h's test hook, which grows the copy of the host
// inside the writing pass: the handler answers 500 "answer changed", sends
// none of the half-written text, and frees the buffer.
void test_the_config_answer_gives_its_buffer_back() {
  if (!kHeapCounted) {
    std::printf("SKIP the_config_answer_gives_its_buffer_back (the heap count needs GNU ld's --wrap: Linux)\n");
    return;
  }
  boot_with_broker("broker.lan");
  // Sent: one block taken (the answer), none kept.
  const ConfigGet sent = get_config();
  CHECK(sent.h.req.status == "200 OK");
  CHECK(sent.taken >= 1);                        // the count is live: the answer's buffer
  CHECK(sent.kept == 0);
  // Refused: the host grows by a byte after the writing pass measured it.
  static int grown = 0;
  grown = 0;
  mqtt_test::after_measure = [](const char* s) {
    if (grown == 0 && s != nullptr && std::strcmp(s, "broker.lan") == 0) {
      char* w = const_cast<char*>(s);            // the handler's own Config copy, 129 bytes
      std::strcpy(w, "broker.lanx");
      ++grown;
    }
  };
  const ConfigGet refused = get_config();
  mqtt_test::after_measure = nullptr;
  CHECK(grown == 1);
  CHECK(refused.h.req.status == "500 Internal Server Error");
  CHECK(refused.h.req.resp == "{\"ok\":false,\"reason\":\"answer changed\"}");
  CHECK(refused.taken >= 1);
  CHECK(refused.kept == 0);
  // And the stored host is what it was: the copy grew, not the config.
  csi_mqtt::Config c;
  csi_mqtt::config_load(&c);
  CHECK(std::strcmp(c.host, "broker.lan") == 0);
  std::printf("PASS the_config_answer_gives_its_buffer_back\n");
}

// ── The settings' lengths (sweep F241) ──────────────────────────────────
//
// POST /api/mqtt/config stored up to MAX_*_LEN characters of each string
// setting (cutting a longer user or prefix to it without a word), and
// config_load() read each back with Preferences::getString(key, buf,
// MAX_*_LEN). arduino-esp32 3.3.8's getString refuses a value whose length
// with its NUL exceeds the size it is given (stubs/mqtt/Preferences.h quotes
// it), so a setting of exactly MAX_*_LEN characters was stored and read back
// as nothing: a 32-character prefix as "securacv", a 128-character host as no
// host. config_load() now reads each with its member's whole size, and the
// POST refuses a value its member cannot hold instead of cutting it.

// Each string setting: its key in the POST's body, its NVS key, the most
// characters its Config member holds, the member's size, the member.
struct Setting {
  const char* field;
  const char* key;
  size_t max;
  size_t size;
  char* (*member)(csi_mqtt::Config&);
  char fill;
};
#define F241_SETTING(field, key, max, name, fill) \
  {field, key, max, sizeof(csi_mqtt::Config::name), \
   [](csi_mqtt::Config& c) -> char* { return c.name; }, fill}
const Setting kSettings[] = {
    F241_SETTING("host", csi_mqtt::NVS_KEY_HOST, csi_mqtt::MAX_HOST_LEN, host, 'h'),
    F241_SETTING("user", csi_mqtt::NVS_KEY_USER, csi_mqtt::MAX_USER_LEN, user, 'u'),
    F241_SETTING("password", csi_mqtt::NVS_KEY_PASS, csi_mqtt::MAX_PASS_LEN, pass, 'w'),
    F241_SETTING("prefix", csi_mqtt::NVS_KEY_PREFIX, csi_mqtt::MAX_PREFIX_LEN, prefix, 'p'),
};
#undef F241_SETTING

// A check that reports and lets the test go on, so one run names every
// setting that fails; each test ends on CHECK(g_soft_failures == 0).
int g_soft_failures = 0;
#define SOFT(what, c)                                                        \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(c)) {                                                              \
      std::printf("FAIL%s %s:%d: %s\n", (what).c_str(), __FILE__, __LINE__, #c); \
      ++g_soft_failures;                                                     \
    }                                                                        \
  } while (0)

std::string post_one(const Setting& s, const std::string& value) {
  return std::string("{\"") + s.field + "\":\"" + value + "\"}";
}

std::string too_long(const char* field, size_t max) {
  return std::string("{\"ok\":false,\"reason\":\"") + field + " must be at most " + std::to_string(max) +
         " bytes\"}";
}

// A setting of its member's whole length is stored and read back whole, with
// no error log, through config_save() and config_load(). One byte more
// can only come from outside this firmware (config_save() writes the member,
// the POST refuses it): the core refuses it whole, as it refused a value at
// the limit before, so that field reads empty, the prefix its default.
void test_each_setting_reads_back_at_its_limit() {
  g_soft_failures = 0;
  for (const Setting& s : kSettings) {
    const std::string what = std::string(" [") + s.field + "]";
    CHECK(s.size == s.max + 1);
    stub_nvs().clear();
    stub_nvs_created().clear();
    csi_mqtt::Config c;
    CHECK(csi_mqtt::config_load(&c));
    c.enabled = true;
    std::snprintf(c.host, sizeof c.host, "%s", "10.0.0.9");
    const std::string at(s.max, s.fill);
    std::memcpy(s.member(c), at.c_str(), at.size() + 1);
    CHECK(csi_mqtt::config_save(c));
    const int logs = stub_nvs_error_logs();
    csi_mqtt::Config back;
    CHECK(csi_mqtt::config_load(&back));
    if (std::string(s.member(back)) != at) {
      std::printf("FAIL%s: %zu bytes stored, %zu read back\n", what.c_str(), at.size(),
                  std::strlen(s.member(back)));
      ++g_soft_failures;
    }
    SOFT(what, stub_nvs_error_logs() == logs);

    Preferences p;
    CHECK(p.begin("csi", false));
    p.putString(s.key, std::string(s.max + 1, s.fill).c_str());
    p.end();
    const int before_past = stub_nvs_error_logs();
    csi_mqtt::Config past;
    CHECK(csi_mqtt::config_load(&past));
    const std::string expect = std::strcmp(s.field, "prefix") == 0 ? "securacv" : "";
    SOFT(what, std::string(s.member(past)) == expect);
    SOFT(what, stub_nvs_error_logs() == before_past + 1);
  }
  stub_nvs().clear();
  stub_nvs_created().clear();
  CHECK(g_soft_failures == 0);
  std::printf("PASS each_setting_reads_back_at_its_limit\n");
}

// Through the handler: POST /api/mqtt/config takes each setting at its
// member's whole length, which config_load() and GET /api/mqtt/config then
// give back whole, and refuses one byte more with the limit in the reason,
// storing nothing: not the field cut to its member (the one-past value is
// another letter, so a cut would differ from what is stored), and not the
// other fields of the same body. An escape the extract decodes is one byte.
void test_the_config_post_takes_each_setting_at_its_limit_and_refuses_one_past() {
  g_soft_failures = 0;
  boot_with_broker("10.0.0.1");
  for (const Setting& s : kSettings) {
    const std::string what = std::string(" [") + s.field + "]";
    const std::string at(s.max, s.fill);
    const Http ok = http(csi_mqtt::handle_config_post, post_one(s, at), /*turn_at=*/1);
    SOFT(what, ok.req.status == "200 OK" && ok.req.resp == "{\"ok\":true}");
    csi_mqtt::Config c;
    CHECK(csi_mqtt::config_load(&c));
    if (std::string(s.member(c)) != at) {
      std::printf("FAIL%s: %zu bytes posted, %zu read back\n", what.c_str(), at.size(),
                  std::strlen(s.member(c)));
      ++g_soft_failures;
    }

    // One past, in another letter (upper case: the prefix's set takes it).
    const char other = (char)(s.fill - 'a' + 'A');
    const std::string past(s.max + 1, other);
    const Http refused = http(csi_mqtt::handle_config_post, post_one(s, past), /*turn_at=*/1);
    SOFT(what, refused.req.status == "400 Bad Request");
    if (refused.req.resp != too_long(s.field, s.max)) {
      std::printf("FAIL%s: one past the limit answered %s %s\n", what.c_str(),
                  refused.req.status.c_str(), refused.req.resp.c_str());
      ++g_soft_failures;
    }
    csi_mqtt::Config after;
    CHECK(csi_mqtt::config_load(&after));
    SOFT(what, std::string(s.member(after)) == at);   // nothing stored, nothing cut

    // The same refusal in a body that changes three other settings: none of
    // them is stored either.
    const std::string body = std::string("{\"enabled\":false,\"port\":1999,\"discovery\":false,\"") +
                             s.field + "\":\"" + past + "\"}";
    const Http mixed = http(csi_mqtt::handle_config_post, body, /*turn_at=*/1);
    SOFT(what, mixed.req.status == "400 Bad Request" && mixed.req.resp == too_long(s.field, s.max));
    csi_mqtt::Config kept;
    CHECK(csi_mqtt::config_load(&kept));
    SOFT(what, kept.enabled && kept.port == after.port && kept.port != 1999 && kept.discovery);
    SOFT(what, std::string(s.member(kept)) == at);
  }

  // 127 bytes and an escaped quote: 128 bytes stored, quote and all.
  const std::string pw = std::string(csi_mqtt::MAX_PASS_LEN - 1, 'w');
  const Http esc = http(csi_mqtt::handle_config_post, "{\"password\":\"" + pw + "\\\"\"}", /*turn_at=*/1);
  CHECK(esc.req.status == "200 OK");
  csi_mqtt::Config c;
  CHECK(csi_mqtt::config_load(&c));
  CHECK(std::string(c.pass) == pw + "\"");
  const Http esc2 = http(csi_mqtt::handle_config_post, "{\"password\":\"" + pw + "\\\"x\"}", /*turn_at=*/1);
  CHECK(esc2.req.status == "400 Bad Request");

  // GET answers the longest host, user and prefix whole (the password is
  // write-only), in a body that still fits its buffer.
  const Http get = http(csi_mqtt::handle_config_get, "", /*turn_at=*/0);
  CHECK(get.req.resp.find("\"host\":\"" + std::string(csi_mqtt::MAX_HOST_LEN, 'h') + "\",") !=
        std::string::npos);
  CHECK(get.req.resp.find("\"user\":\"" + std::string(csi_mqtt::MAX_USER_LEN, 'u') + "\",") !=
        std::string::npos);
  CHECK(get.req.resp.find("\"prefix\":\"" + std::string(csi_mqtt::MAX_PREFIX_LEN, 'p') + "\",") !=
        std::string::npos);
  CHECK(get.req.resp.find("\"password_set\":true,") != std::string::npos);
  CHECK(!get.req.resp.empty() && get.req.resp.back() == '}');
  // The bridge took the longest host into its broker URI whole.
  CHECK(fake::last_client()->uri ==
        "mqtt://" + std::string(csi_mqtt::MAX_HOST_LEN, 'h') + ":1883");
  power_cycle();
  CHECK(g_soft_failures == 0);
  std::printf("PASS the_config_post_takes_each_setting_at_its_limit_and_refuses_one_past\n");
}

// The limits are bytes, as the member holds them: a user of 32 two-byte
// letters (64 bytes) is taken whole, 33 of them (66 bytes, 33 characters,
// under the page's maxlength of 64) are refused with the limit in bytes. A
// prefix outside its character set is refused for the set, whatever its
// length.
void test_the_config_post_limits_are_bytes() {
  boot_with_broker("10.0.0.1");
  std::string e32, e33;
  for (int i = 0; i < 32; ++i) e32 += "\xC3\xA9";   // U+00E9
  e33 = e32 + "\xC3\xA9";
  const Http ok = http(csi_mqtt::handle_config_post, "{\"user\":\"" + e32 + "\"}", /*turn_at=*/1);
  CHECK(ok.req.status == "200 OK");
  csi_mqtt::Config c;
  CHECK(csi_mqtt::config_load(&c));
  CHECK(std::string(c.user) == e32 && std::strlen(c.user) == csi_mqtt::MAX_USER_LEN);
  const Http refused = http(csi_mqtt::handle_config_post, "{\"user\":\"" + e33 + "\"}", /*turn_at=*/1);
  CHECK(refused.req.status == "400 Bad Request");
  CHECK(refused.req.resp == too_long("user", csi_mqtt::MAX_USER_LEN));
  CHECK(csi_mqtt::config_load(&c) && std::string(c.user) == e32);

  std::string e20;
  for (int i = 0; i < 20; ++i) e20 += "\xC3\xA9";   // 40 bytes, 20 letters
  const Http set = http(csi_mqtt::handle_config_post, "{\"prefix\":\"" + e20 + "\"}", /*turn_at=*/1);
  CHECK(set.req.status == "400 Bad Request");
  CHECK(set.req.resp == "{\"ok\":false,\"reason\":\"prefix must be [a-zA-Z0-9_/-]+\"}");
  power_cycle();
  std::printf("PASS the_config_post_limits_are_bytes\n");
}

// A PEM of exactly `n` bytes, one certificate (the shape check wants a BEGIN
// and an END line; the bytes between are `fill`).
std::string pem_of(size_t n, char fill) {
  const std::string head = "-----BEGIN CERTIFICATE-----\n";
  const std::string tail = "\n-----END CERTIFICATE-----";
  CHECK(n >= head.size() + tail.size());
  return head + std::string(n - head.size() - tail.size(), fill) + tail;
}

// The PEM as a JSON string's inside: its newlines escaped.
std::string json_inside(const std::string& v) {
  std::string out;
  for (char ch : v) {
    if (ch == '\n') out += "\\n";
    else if (ch == '\r') out += "\\r";
    else out += ch;
  }
  return out;
}

std::string stored_ca() {
  const auto it = stub_nvs().find(std::string("csi/") + csi_mqtt::NVS_KEY_CA);
  return it == stub_nvs().end() ? std::string() : std::string(it->second.begin(), it->second.end());
}

// The TLS CA (sweep F241, the review): the handler cut a longer CA to
// MAX_CA_LEN bytes and stored it if the cut still held a BEGIN and an END
// line, so a two-certificate chain was saved ending mid-certificate. A CA of
// MAX_CA_LEN bytes is taken (the /mqtt page sends it with a newline after, one
// past: whitespace past the limit is dropped), reaches the client whole
// through ca_load(), and one byte of certificate more is refused with the
// limit, storing nothing of that body.
void test_the_config_post_takes_a_ca_at_its_limit_and_refuses_one_past() {
  boot_with_broker("10.0.0.1");
  const size_t max = csi_mqtt::MAX_CA_LEN;
  const std::string at = pem_of(max, 'A');
  const Http ok = http(csi_mqtt::handle_config_post,
                       "{\"tls_mode\":1,\"ca\":\"" + json_inside(at + "\n") + "\"}", /*turn_at=*/1);
  CHECK(ok.req.status == "200 OK" && ok.req.resp == "{\"ok\":true}");
  CHECK(stored_ca() == at);
  CHECK(fake::last_client()->certificate == at);   // ca_load() read it whole
  CHECK(fake::last_client()->uri.rfind("mqtts://10.0.0.1:", 0) == 0);

  // One shorter, the newline fits and is kept, as before.
  const std::string shorter = pem_of(max - 1, 'C');
  const Http ok2 = http(csi_mqtt::handle_config_post,
                        "{\"ca\":\"" + json_inside(shorter + "\n") + "\"}", /*turn_at=*/1);
  CHECK(ok2.req.status == "200 OK");
  CHECK(stored_ca() == shorter + "\n");
  // Whitespace of any kind past the limit is not the certificate.
  const Http ok3 = http(csi_mqtt::handle_config_post,
                        "{\"ca\":\"" + json_inside(at + "\r\n\r\n") + " \\t\"}", /*turn_at=*/1);
  CHECK(ok3.req.status == "200 OK");
  CHECK(stored_ca() == at);

  const std::string refusal = too_long("ca", max);
  // One byte of certificate past the limit, with other settings in the body.
  const Http past = http(csi_mqtt::handle_config_post,
                         "{\"port\":1999,\"host\":\"10.9.9.9\",\"ca\":\"" + json_inside(pem_of(max + 1, 'B') + "\n") +
                             "\"}",
                         /*turn_at=*/1);
  CHECK(past.req.status == "400 Bad Request" && past.req.resp == refusal);
  CHECK(stored_ca() == at);
  csi_mqtt::Config c;
  CHECK(csi_mqtt::config_load(&c));
  CHECK(std::string(c.host) == "10.0.0.1" && c.port != 1999);
  // A letter after whitespace past the limit is certificate too.
  const Http past2 = http(csi_mqtt::handle_config_post,
                          "{\"ca\":\"" + json_inside(at + "\n") + "x\"}", /*turn_at=*/1);
  CHECK(past2.req.status == "400 Bad Request" && past2.req.resp == refusal);
  CHECK(stored_ca() == at);
  // A two-certificate chain whose cut at MAX_CA_LEN still holds the first
  // whole: the old handler stored that cut (BEGIN and END are both in it).
  const std::string chain = pem_of(2000, 'D') + "\n" + pem_of(1510, 'E');
  CHECK(chain.size() > max);
  const std::string cut = chain.substr(0, max);
  CHECK(canary::net::mqtt_tls::ca_pem_looks_valid(cut.c_str()));
  const Http two = http(csi_mqtt::handle_config_post,
                        "{\"ca\":\"" + json_inside(chain + "\n") + "\"}", /*turn_at=*/1);
  CHECK(two.req.status == "400 Bad Request" && two.req.resp == refusal);
  CHECK(stored_ca() == at);
  power_cycle();
  std::printf("PASS the_config_post_takes_a_ca_at_its_limit_and_refuses_one_past\n");
}

// Sweep F266: POST /api/mqtt/config parsed what ONE httpd_req_recv()
// returned, and IDF's recv returns what the socket holds, so a body that
// came in more than one TCP segment was parsed cut. Here the body carries a
// 2.4 KB CA, the way the /mqtt page sends a pasted PEM, with the host, port,
// user and discovery after it, and the socket hands it over in three reads,
// the CA split across all three. The handler now reads until Content-Length
// bytes are in (http_body::read_all(), as the Beacon routes do), asks for no
// more than is still owed, and sits out up to three socket timeouts; a peer
// that closes early, or a fourth timeout, is refused with nothing stored.
void test_the_config_post_reads_a_body_that_arrives_in_pieces() {
  boot_with_broker("10.0.0.1");
  const std::string pem = pem_of(2400, 'Q');
  const std::string body = "{\"tls_mode\":1,\"ca\":\"" + json_inside(pem + "\n") +
                           "\",\"host\":\"10.0.0.5\",\"port\":8884,\"user\":\"bob\",\"discovery\":false}";
  const size_t ca_at = body.find("BEGIN");
  const size_t ca_end = body.find("END CERTIFICATE");
  CHECK(ca_at < 700 && ca_end > 700 + 900);   // each of the three reads holds some of the CA
  g_wire.plan = {700, 900};                    // then the rest, in one read
  const Http h = http(csi_mqtt::handle_config_post, body, /*turn_at=*/1);
  if (h.req.status != "200 OK") {
    std::printf("FAIL a body in three reads answered %s %s\n", h.req.status.c_str(), h.req.resp.c_str());
  }
  CHECK(h.req.status == "200 OK" && h.req.resp == "{\"ok\":true}");
  CHECK(h.req.body_off == body.size() && h.req.recv_calls == 3);
  CHECK(h.req.recv_asked_max <= body.size());   // never more than is owed
  CHECK(stored_ca() == pem + "\n");
  csi_mqtt::Config c;
  CHECK(csi_mqtt::config_load(&c));
  CHECK(std::string(c.host) == "10.0.0.5" && c.port == 8884 && std::string(c.user) == "bob");
  CHECK(!c.discovery && c.tls_mode == 1);
  CHECK(fake::last_client()->certificate == pem + "\n");
  CHECK(fake::last_client()->uri == "mqtts://10.0.0.5:8884");

  // Three socket timeouts between the pieces are sat out.
  const std::string body2 = "{\"user\":\"carol\",\"ca\":\"" + json_inside(pem_of(1800, 'R') + "\n") + "\"}";
  g_wire.plan = {600, HTTPD_SOCK_ERR_TIMEOUT, HTTPD_SOCK_ERR_TIMEOUT, 500, HTTPD_SOCK_ERR_TIMEOUT};
  const Http h2 = http(csi_mqtt::handle_config_post, body2, /*turn_at=*/1);
  CHECK(h2.req.status == "200 OK" && h2.req.body_off == body2.size());
  CHECK(csi_mqtt::config_load(&c) && std::string(c.user) == "carol");
  CHECK(stored_ca() == pem_of(1800, 'R') + "\n");

  // A fourth is not, and nothing of that body is stored.
  const std::string read_failed = "{\"ok\":false,\"reason\":\"body read failed\"}";
  const std::string body3 = "{\"user\":\"dave\",\"ca\":\"" + json_inside(pem_of(1700, 'S') + "\n") + "\"}";
  g_wire.plan = {600, HTTPD_SOCK_ERR_TIMEOUT, HTTPD_SOCK_ERR_TIMEOUT, HTTPD_SOCK_ERR_TIMEOUT,
                 HTTPD_SOCK_ERR_TIMEOUT};
  const Http h3 = http(csi_mqtt::handle_config_post, body3, /*turn_at=*/1);
  CHECK(h3.req.status == "400 Bad Request" && h3.req.resp == read_failed);
  CHECK(csi_mqtt::config_load(&c) && std::string(c.user) == "carol");
  CHECK(stored_ca() == pem_of(1800, 'R') + "\n");
  // A socket error, the same.
  g_wire.plan = {600, HTTPD_SOCK_ERR_FAIL};
  const Http h4 = http(csi_mqtt::handle_config_post, body3, /*turn_at=*/1);
  CHECK(h4.req.status == "400 Bad Request" && h4.req.resp == read_failed);
  CHECK(csi_mqtt::config_load(&c) && std::string(c.user) == "carol");

  // A peer that closes before its Content-Length is in: the old handler
  // parsed what had come.
  g_wire.set_len = true;
  g_wire.content_len = std::string("{\"user\":\"erin\"}").size() + 10;
  const Http h5 = http(csi_mqtt::handle_config_post, "{\"user\":\"erin\"}", /*turn_at=*/1);
  CHECK(h5.req.status == "400 Bad Request" && h5.req.resp == read_failed);
  CHECK(csi_mqtt::config_load(&c) && std::string(c.user) == "carol");
  power_cycle();
  std::printf("PASS the_config_post_reads_a_body_that_arrives_in_pieces\n");
}

// Sweep F266: a body past the handler's cap was cut at it and the cut
// parsed, though the handler's comment said it was refused. Now a
// Content-Length past csi_mqtt::MAX_CONFIG_BODY_LEN is refused with the
// limit in the reason (400, as the handler's other refusals; the /mqtt page
// and the setup wizard show it) before anything is read, and a body of
// exactly the cap is read and taken. No Content-Length is "empty body".
void test_the_config_post_refuses_a_body_past_its_cap() {
  boot_with_broker("10.0.0.1");
  const size_t max = csi_mqtt::MAX_CONFIG_BODY_LEN;
  CHECK(max == 640 + 2 * csi_mqtt::MAX_CA_LEN + 63);
  // A body of `n` bytes that sets the host first, padded with a key the
  // handler does not read.
  auto padded = [](const std::string& host, size_t n) {
    const std::string head = "{\"host\":\"" + host + "\",\"pad\":\"";
    const std::string tail = "\"}";
    CHECK(n > head.size() + tail.size());
    return head + std::string(n - head.size() - tail.size(), 'x') + tail;
  };
  const Http at = http(csi_mqtt::handle_config_post, padded("10.0.0.6", max), /*turn_at=*/1);
  CHECK(at.req.status == "200 OK" && at.req.resp == "{\"ok\":true}");
  csi_mqtt::Config c;
  CHECK(csi_mqtt::config_load(&c) && std::string(c.host) == "10.0.0.6");

  const Http past = http(csi_mqtt::handle_config_post, padded("10.0.0.7", max + 1), /*turn_at=*/1);
  if (past.req.status != "400 Bad Request") {
    std::printf("FAIL a body one past the cap answered %s %s\n", past.req.status.c_str(),
                past.req.resp.c_str());
  }
  CHECK(past.req.status == "400 Bad Request" && past.req.resp == too_long("body", max));
  CHECK(past.req.recv_calls == 0);   // refused before any of it was read
  CHECK(csi_mqtt::config_load(&c) && std::string(c.host) == "10.0.0.6");

  // A Content-Length far past the cap, whatever the socket then holds.
  g_wire.set_len = true;
  g_wire.content_len = 1u << 20;
  const Http huge = http(csi_mqtt::handle_config_post, "{\"host\":\"10.0.0.8\"}", /*turn_at=*/1);
  CHECK(huge.req.status == "400 Bad Request" && huge.req.resp == too_long("body", max));
  CHECK(huge.req.recv_calls == 0);
  CHECK(csi_mqtt::config_load(&c) && std::string(c.host) == "10.0.0.6");

  // No Content-Length (or 0): nothing to read.
  g_wire.set_len = true;
  g_wire.content_len = 0;
  const Http none = http(csi_mqtt::handle_config_post, "{\"host\":\"10.0.0.9\"}", /*turn_at=*/1);
  CHECK(none.req.status == "400 Bad Request" &&
        none.req.resp == "{\"ok\":false,\"reason\":\"empty body\"}");
  CHECK(csi_mqtt::config_load(&c) && std::string(c.host) == "10.0.0.6");
  power_cycle();
  std::printf("PASS the_config_post_refuses_a_body_past_its_cap\n");
}

// The /mqtt page's inputs stop at the lengths the device takes.
void test_the_settings_page_limits_each_field() {
  g_soft_failures = 0;
  const Http page = http(csi_mqtt::handle_ui, "", /*turn_at=*/0);
  CHECK(page.req.status == "200 OK");
  for (const Setting& s : kSettings) {
    const std::string id = std::string("id=\"") + s.field + "\"";
    const size_t at = page.req.resp.find(id);
    CHECK(at != std::string::npos);
    if (at == std::string::npos) continue;
    const size_t open = page.req.resp.rfind("<input", at);
    const size_t close = page.req.resp.find('>', at);
    CHECK(open != std::string::npos && close != std::string::npos);
    if (open == std::string::npos || close == std::string::npos) continue;
    const std::string tag = page.req.resp.substr(open, close - open + 1);
    const std::string limit = "maxlength=\"" + std::to_string(s.max) + "\"";
    if (tag.find(limit) == std::string::npos) {
      std::printf("FAIL [%s]: the page's input has no %s: %s\n", s.field, limit.c_str(), tag.c_str());
      ++g_soft_failures;
    }
  }
  CHECK(g_soft_failures == 0);
  std::printf("PASS the_settings_page_limits_each_field\n");
}

// ── Where the stored broker password goes (sweep F289) ──────────────────
//
// The landing page at / mints a dashboard session for any caller that
// reaches the WAP, on the home network too, and authorized() takes any
// session. Before F289's review a POST that moved the broker and sent no
// "password" kept the stored one, so such a session could point the bridge
// at a listener of its own (host "evil.example", TLS off) and the re-init
// handed that listener the owner's broker user and password in the clear.
// Now a save that keeps the stored password and moves it (host, port, user,
// TLS mode or CA) takes the bearer header or an owner's session (a BOOT tap
// or a setup-door claim minted it); a landing session is refused before
// anything is saved and no client is opened. Typing the password again
// still saves, since the caller then sends its own.

constexpr char kOwnerBroker[] = "mqtt://10.0.0.1:1883";
constexpr char kBrokerUser[] = "homeuser";
constexpr char kBrokerPass[] = "s3cret-broker-pw";
constexpr char kApiToken[] = "a3f1c09e5b7d42e8a3f1c09e5b7d42e8";
const char* api_token() { return kApiToken; }
const char* empty_token() { return ""; }

void boot_with_password() {
  boot_with_broker("10.0.0.1", true, kBrokerUser, kBrokerPass);
  CHECK(fake::last_client()->uri == kOwnerBroker);
  CHECK(fake::last_client()->password == kBrokerPass);   // the harness sees it
  fake::calls.clear();
}

void as_landing_session() {
  session::any = true;
  session::owner = false;
}

// Any client, ever opened, that was handed the stored password for a broker
// other than the owner's.
bool stored_password_left_home() {
  for (const esp_mqtt_client* c : fake::clients) {
    if (c->password == kBrokerPass && c->uri != kOwnerBroker) return true;
  }
  return false;
}

// The stored settings are the owner's, untouched.
bool stored_settings_are_the_owners() {
  csi_mqtt::Config c;
  csi_mqtt::config_load(&c);
  return std::strcmp(c.host, "10.0.0.1") == 0 && c.port == 1883 &&
         std::strcmp(c.user, kBrokerUser) == 0 && std::strcmp(c.pass, kBrokerPass) == 0 &&
         c.tls_mode == 0 && stored_ca().empty();
}

bool refused_for_the_password(const Http& h) {
  return h.req.status == "403 Forbidden" &&
         h.req.resp.find("\"error\":\"password_needed\"") != std::string::npos &&
         h.req.resp.find("\"ok\":false") != std::string::npos;
}

void test_a_landing_session_cannot_send_the_kept_password_elsewhere() {
  const std::string ca = pem_of(400, 'A');
  const std::string moves[] = {
      "{\"host\":\"evil.example\",\"tls_mode\":0}",   // the review's probe
      "{\"host\":\"evil.example\"}",
      "{\"port\":1884}",
      "{\"user\":\"someone-else\"}",
      "{\"tls_mode\":1}",
      "{\"tls\":true}",
      "{\"ca\":\"" + json_inside(ca + "\n") + "\"}",
      "{\"ca\":\"\"}",
      // The /mqtt page's save shape, with only the host changed.
      "{\"enabled\":true,\"host\":\"evil.example\",\"port\":1883,\"user\":\"homeuser\","
      "\"prefix\":\"securacv\",\"tls_mode\":0,\"discovery\":true}",
  };
  for (const std::string& body : moves) {
    boot_with_password();
    as_landing_session();
    const Http h = http(csi_mqtt::handle_config_post, body, /*turn_at=*/1);
    if (!refused_for_the_password(h)) {
      std::printf("FAIL [%s]: a landing session moved the kept password: %s %s\n",
                  body.c_str(), h.req.status.c_str(), h.req.resp.c_str());
    }
    CHECK(refused_for_the_password(h));
    CHECK(stored_settings_are_the_owners());
    CHECK(fake::count("init") == 0);          // no client was opened
    CHECK(!stored_password_left_home());
    CHECK(fake::last_client()->uri == kOwnerBroker);
  }
  std::printf("PASS a_landing_session_cannot_send_the_kept_password_elsewhere\n");
}

void test_a_landing_session_still_saves_what_keeps_the_password_home() {
  // The /mqtt page's ordinary save: the same broker, user and encryption,
  // a new prefix and discovery off. Nothing moves the password.
  boot_with_password();
  as_landing_session();
  const Http same = http(csi_mqtt::handle_config_post,
                         "{\"enabled\":true,\"host\":\"10.0.0.1\",\"port\":1883,\"user\":\"homeuser\","
                         "\"prefix\":\"house\",\"tls_mode\":0,\"discovery\":false}",
                         /*turn_at=*/1);
  CHECK(same.req.status == "200 OK" && same.req.resp == "{\"ok\":true}");
  csi_mqtt::Config c;
  csi_mqtt::config_load(&c);
  CHECK(std::strcmp(c.prefix, "house") == 0 && !c.discovery);
  CHECK(std::strcmp(c.pass, kBrokerPass) == 0);
  CHECK(fake::last_client()->uri == kOwnerBroker && fake::last_client()->password == kBrokerPass);

  // A new broker with a password the caller typed: the stored one is
  // replaced, never sent.
  const Http typed = http(csi_mqtt::handle_config_post,
                          "{\"host\":\"evil.example\",\"password\":\"typed-here\"}", /*turn_at=*/1);
  CHECK(typed.req.status == "200 OK");
  CHECK(fake::last_client()->uri == "mqtt://evil.example:1883");
  CHECK(fake::last_client()->password == "typed-here");
  CHECK(!stored_password_left_home());

  // An empty one clears it: the new broker gets no password at all.
  boot_with_password();
  as_landing_session();
  const Http cleared = http(csi_mqtt::handle_config_post,
                            "{\"host\":\"evil.example\",\"password\":\"\"}", /*turn_at=*/1);
  CHECK(cleared.req.status == "200 OK");
  CHECK(fake::last_client()->uri == "mqtt://evil.example:1883");
  CHECK(fake::last_client()->password.empty());
  CHECK(!stored_password_left_home());

  // With no password stored there is nothing to keep: a landing session
  // moves the broker as before.
  boot_with_broker("10.0.0.1");
  as_landing_session();
  const Http none = http(csi_mqtt::handle_config_post, "{\"host\":\"10.0.0.9\"}", /*turn_at=*/1);
  CHECK(none.req.status == "200 OK");
  CHECK(fake::last_client()->uri == "mqtt://10.0.0.9:1883");
  std::printf("PASS a_landing_session_still_saves_what_keeps_the_password_home\n");
}

void test_the_owner_and_the_bearer_move_the_kept_password() {
  // An owner's session (a BOOT tap or a claim minted it) moves the broker
  // and keeps the password, as any session could before.
  boot_with_password();
  const Http owner = http(csi_mqtt::handle_config_post, "{\"host\":\"10.0.0.2\"}", /*turn_at=*/1);
  CHECK(owner.req.status == "200 OK" && owner.req.resp == "{\"ok\":true}");
  CHECK(fake::last_client()->uri == "mqtt://10.0.0.2:1883");
  CHECK(fake::last_client()->username == kBrokerUser);
  CHECK(fake::last_client()->password == kBrokerPass);

  // The bearer header does too, beside a landing session ...
  boot_with_password();
  as_landing_session();
  csi_mqtt::set_api_token_provider(api_token);
  g_next_req_hdrs["Authorization"] = std::string("Bearer ") + kApiToken;
  const Http bearer = http(csi_mqtt::handle_config_post, "{\"host\":\"10.0.0.3\"}", /*turn_at=*/1);
  CHECK(bearer.req.status == "200 OK");
  CHECK(fake::last_client()->uri == "mqtt://10.0.0.3:1883");
  CHECK(fake::last_client()->password == kBrokerPass);

  // ... and with no session at all.
  boot_with_password();
  session::any = false;
  session::owner = false;
  csi_mqtt::set_api_token_provider(api_token);
  g_next_req_hdrs["Authorization"] = std::string("Bearer ") + kApiToken;
  const Http alone = http(csi_mqtt::handle_config_post, "{\"port\":1884}", /*turn_at=*/1);
  CHECK(alone.req.status == "200 OK");
  CHECK(fake::last_client()->uri == "mqtt://10.0.0.1:1884");

  // A wrong bearer beside a landing session is the landing session: refused.
  boot_with_password();
  as_landing_session();
  csi_mqtt::set_api_token_provider(api_token);
  std::string wrong = kApiToken;
  wrong[31] = wrong[31] == '0' ? '1' : '0';
  g_next_req_hdrs["Authorization"] = "Bearer " + wrong;
  const Http bad = http(csi_mqtt::handle_config_post, "{\"host\":\"evil.example\"}", /*turn_at=*/1);
  CHECK(refused_for_the_password(bad));
  CHECK(stored_settings_are_the_owners() && !stored_password_left_home());

  // An empty token is no token, whatever the header says.
  boot_with_password();
  as_landing_session();
  csi_mqtt::set_api_token_provider(empty_token);
  g_next_req_hdrs["Authorization"] = "Bearer ";
  const Http empty = http(csi_mqtt::handle_config_post, "{\"host\":\"evil.example\"}", /*turn_at=*/1);
  CHECK(refused_for_the_password(empty));
  CHECK(stored_settings_are_the_owners() && !stored_password_left_home());
  csi_mqtt::set_api_token_provider(nullptr);
  std::printf("PASS the_owner_and_the_bearer_move_the_kept_password\n");
}

struct Test {
  const char* name;
  void (*fn)();
};
const Test kTests[] = {
    {"a_config_post_reinits_on_the_loop_task", test_a_config_post_reinits_on_the_loop_task},
    {"the_settings_read_on_a_fresh_nvs_logs_nothing", test_the_settings_read_on_a_fresh_nvs_logs_nothing},
    {"an_unconfigured_bridge_reads_its_settings_without_an_error_line",
     test_an_unconfigured_bridge_reads_its_settings_without_an_error_line},
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
    {"the_health_publish_warns_before_the_id_space_runs_out", test_the_health_publish_warns_before_the_id_space_runs_out},
    {"the_egress_counters_ride_a_retained_topic_of_their_own", test_the_egress_counters_ride_a_retained_topic_of_their_own},
    {"the_config_answer_escapes_what_the_page_sent", test_the_config_answer_escapes_what_the_page_sent},
    {"the_config_answer_gives_its_buffer_back", test_the_config_answer_gives_its_buffer_back},
    {"each_setting_reads_back_at_its_limit", test_each_setting_reads_back_at_its_limit},
    {"the_config_post_takes_each_setting_at_its_limit_and_refuses_one_past",
     test_the_config_post_takes_each_setting_at_its_limit_and_refuses_one_past},
    {"the_settings_page_limits_each_field", test_the_settings_page_limits_each_field},
    {"the_config_post_limits_are_bytes", test_the_config_post_limits_are_bytes},
    {"the_config_post_takes_a_ca_at_its_limit_and_refuses_one_past",
     test_the_config_post_takes_a_ca_at_its_limit_and_refuses_one_past},
    {"the_config_post_reads_a_body_that_arrives_in_pieces",
     test_the_config_post_reads_a_body_that_arrives_in_pieces},
    {"the_config_post_refuses_a_body_past_its_cap", test_the_config_post_refuses_a_body_past_its_cap},
    {"a_landing_session_cannot_send_the_kept_password_elsewhere",
     test_a_landing_session_cannot_send_the_kept_password_elsewhere},
    {"a_landing_session_still_saves_what_keeps_the_password_home",
     test_a_landing_session_still_saves_what_keeps_the_password_home},
    {"the_owner_and_the_bearer_move_the_kept_password",
     test_the_owner_and_the_bearer_move_the_kept_password},
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
