// Host tests for firmware/common/network/node_status.h — the GET /api/status
// body every node serves: exact bytes for the minimal and the full body, the
// omit-when-unknown rule, escaping, and the worst case fitting the cap.

// Arduino's Print.h defines these; a local named HEX breaks every device
// build and no host test would notice (the fleet_selfreport lesson).
#define HEX 16
#define DEC 10

#include "../common/network/node_status.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d: ", __func__, __LINE__);         \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

static NodeStatus blank() {
  NodeStatus s{};
  s.setup_ap = -1;
  s.ble_door = -1;
  s.saved_networks = -1;
  s.peers_seen = -1;
  s.wall_polled_s_ago = -1;
  s.ota_pending_verify = -1;
  return s;
}

static void the_minimal_body_says_only_what_it_knows() {
  NodeStatus s = blank();
  s.node = "canary-sense";
  s.fw = "1.2.3";
  s.uptime_s = 42;
  char out[NODE_STATUS_BODY_CAP];
  const size_t n = node_status_build(out, sizeof(out), &s);
  CHECK(n == std::strlen(out), "returns the length");
  CHECK(!std::strcmp(out, "{\"node\":\"canary-sense\",\"fw\":\"1.2.3\",\"uptime_s\":42}"), "got %s", out);
}

static void the_full_body_in_order() {
  NodeStatus s = blank();
  s.node = "canary-display"; s.product = "securacv-canary-display-nightstand-s3"; s.fw = "2.4.16";
  s.hardware = "waveshare-esp32s3-lcd147"; s.name = "Bedside";
  s.uptime_s = 3600; s.ip = "192.168.1.42"; s.rssi = -67; s.onboarding = "provisioned";
  s.setup_ap = 0; s.ble_door = -1; s.saved_networks = 2;
  s.hub = NS_HUB_OK; s.peers_seen = 3; s.wall = "direct"; s.wall_polled_s_ago = 8;
  s.peer = "on"; s.coord = "hub";
  s.ota_state = "idle"; s.ota_target = "2.4.17"; s.ota_pending_verify = 0;
  s.creds_at_rest = "plaintext-nvs"; s.clock = "ntp"; s.tz = "Europe/Berlin";
  char out[NODE_STATUS_BODY_CAP];
  node_status_build(out, sizeof(out), &s);
  const char* want =
      "{\"node\":\"canary-display\",\"fw\":\"2.4.16\",\"uptime_s\":3600"
      ",\"product\":\"securacv-canary-display-nightstand-s3\",\"hw\":\"waveshare-esp32s3-lcd147\",\"name\":\"Bedside\""
      ",\"ip\":\"192.168.1.42\",\"rssi\":-67,\"onboarding\":\"provisioned\",\"setup_ap\":false,\"saved_networks\":2"
      ",\"hub\":\"ok\",\"peers_seen\":3,\"wall\":\"direct\",\"wall_polled_s_ago\":8,\"peer\":\"on\",\"coord\":\"hub\""
      ",\"ota\":{\"state\":\"idle\",\"target\":\"2.4.17\",\"pending_verify\":false}"
      ",\"creds_at_rest\":\"plaintext-nvs\",\"clock\":\"ntp\",\"tz\":\"Europe/Berlin\"}";
  CHECK(!std::strcmp(out, want), "got %s", out);
}

static void the_hub_words_and_the_booleans() {
  NodeStatus s = blank();
  s.node = "canary-wap"; s.fw = "x"; s.hub = NS_HUB_NONE; s.setup_ap = 1; s.ble_door = 1; s.ota_state = "downloading"; s.ota_pending_verify = 1;
  char out[NODE_STATUS_BODY_CAP];
  node_status_build(out, sizeof(out), &s);
  CHECK(std::strstr(out, "\"hub\":\"none\"") != nullptr, "none: %s", out);
  CHECK(std::strstr(out, "\"setup_ap\":true,\"ble_door\":true") != nullptr, "booleans: %s", out);
  CHECK(std::strstr(out, "\"ota\":{\"state\":\"downloading\",\"pending_verify\":true}") != nullptr, "ota without target: %s", out);
  s.hub = NS_HUB_DOWN;
  node_status_build(out, sizeof(out), &s);
  CHECK(std::strstr(out, "\"hub\":\"down\"") != nullptr, "down");
  s.hub = 7;
  node_status_build(out, sizeof(out), &s);
  CHECK(std::strstr(out, "\"hub\":\"unknown\"") != nullptr, "a value this firmware never sent reads unknown");
}

static void strings_are_escaped() {
  NodeStatus s = blank();
  s.node = "canary"; s.fw = ""; s.name = "Bob's \"Den\"\n\x01";
  char out[NODE_STATUS_BODY_CAP];
  node_status_build(out, sizeof(out), &s);
  CHECK(std::strstr(out, "\"name\":\"Bob's \\\"Den\\\"\\n\\u0001\"") != nullptr, "escaped: %s", out);
}

static void the_worst_case_fits_the_cap() {
  // Every string at its cap, every byte a control char that escapes to six.
  const std::string name(NODE_STATUS_NAME_MAX, '\x01'), product(NODE_STATUS_PRODUCT_MAX, '\x01'),
      fw(NODE_STATUS_FW_MAX, '\x01'), hw(NODE_STATUS_HW_MAX, '\x01'), word(NODE_STATUS_WORD_MAX, '\x01'),
      ip(NODE_STATUS_IP_MAX, '\x01'), tz(NODE_STATUS_TZ_MAX, '\x01'), ver(NODE_STATUS_OTA_VER_MAX, '\x01');
  NodeStatus s = blank();
  s.node = word.c_str(); s.product = product.c_str(); s.fw = fw.c_str(); s.hardware = hw.c_str(); s.name = name.c_str();
  s.uptime_s = 0xFFFFFFFFu; s.ip = ip.c_str(); s.rssi = -2147483647; s.onboarding = word.c_str();
  s.setup_ap = 1; s.ble_door = 1; s.saved_networks = 2147483647; s.hub = NS_HUB_DOWN; s.peers_seen = 2147483647;
  s.wall = word.c_str(); s.wall_polled_s_ago = 2147483647; s.peer = word.c_str(); s.coord = word.c_str();
  s.ota_state = word.c_str(); s.ota_target = ver.c_str();
  s.ota_pending_verify = 1; s.creds_at_rest = word.c_str(); s.clock = word.c_str(); s.tz = tz.c_str();
  char out[NODE_STATUS_BODY_CAP];
  const size_t n = node_status_build(out, sizeof(out), &s);
  CHECK(n < sizeof(out) - 1, "worst case %zu fits cap %zu with room", n, sizeof(out));
  CHECK(out[n - 1] == '}', "closed");
}

static void a_small_buffer_never_overflows() {
  NodeStatus s = blank();
  s.node = "canary-sense"; s.fw = "1.2.3"; s.name = "A long enough name to clip";
  char out[24];
  std::memset(out, 'Z', sizeof(out));
  const size_t n = node_status_build(out, sizeof(out), &s);
  CHECK(n <= sizeof(out) - 1, "clamped");
  CHECK(out[n] == '\0', "NUL in bounds");
  CHECK(node_status_build(nullptr, 0, &s) == 0, "null out");
  char one[1];
  CHECK(node_status_build(one, 1, &s) == 0 && one[0] == '\0', "cap 1");
  CHECK(node_status_build(out, sizeof(out), nullptr) == 0 && out[0] == '\0', "null status");
}

int main() {
  the_minimal_body_says_only_what_it_knows();
  the_full_body_in_order();
  the_hub_words_and_the_booleans();
  strings_are_escaped();
  the_worst_case_fits_the_cap();
  a_small_buffer_never_overflows();
  if (g_failures) {
    std::printf("test_node_status: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_node_status: all passed\n");
  return 0;
}
