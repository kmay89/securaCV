// Host tests for firmware/common/network/mdns_txt.h — the one _securacv._tcp
// TXT schema: canonical order, omit rules, caps words, host labels.

#include "../common/network/mdns_txt.h"

#include <cstdio>
#include <cstring>

using namespace canary::net::mdns_txt;

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

static const char* value_of(const Kv* kv, size_t n, const char* key) {
  for (size_t i = 0; i < n; i++) if (!std::strcmp(kv[i].key, key)) return kv[i].value;
  return nullptr;
}

static void the_canonical_order_and_the_omits() {
  Fields f;
  f.device_id = "canary_sense_001"; f.host = "canary-sense-001-a1b2c3"; f.fw = "2.4.16";
  f.model = "XIAO ESP32C6"; f.dt = "canary-sense"; f.role = ROLE_WITNESS;
  f.caps = CAP_STATUS | CAP_FLEET | CAP_OTA | CAP_MQTT; f.coord = "self";
  Kv kv[MAX_KEYS]; Scratch s;
  const size_t n = enumerate(f, kv, MAX_KEYS, s);
  CHECK(n == 11, "11 keys without hw/tls: %zu", n);
  const char* order[] = {"device_id", "name", "host", "fw", "model", "dt", "role", "caps", "coord", "broker", "bport"};
  for (size_t i = 0; i < n; i++) CHECK(!std::strcmp(kv[i].key, order[i]), "key %zu is %s", i, kv[i].key);
  CHECK(!std::strcmp(value_of(kv, n, "name"), "canary_sense_001"), "the id doubles as the name");
  CHECK(!std::strcmp(value_of(kv, n, "caps"), "status,fleet,ota,mqtt"), "caps: %s", value_of(kv, n, "caps"));
  CHECK(!std::strcmp(value_of(kv, n, "broker"), "") && !std::strcmp(value_of(kv, n, "bport"), ""), "tombstones");
  CHECK(value_of(kv, n, "hw") == nullptr, "no hw key when the build has none to name");
  CHECK(value_of(kv, n, "tls") == nullptr, "no tls keys without a TLS story");

  f.hw = "xiao-esp32c6-mr60"; f.name = "Hallway"; f.broker = "192.168.1.5"; f.bport = 1883; f.tls = 1; f.secure_port = 8443;
  const size_t m = enumerate(f, kv, MAX_KEYS, s);
  CHECK(m == 14, "every key: %zu", m);
  CHECK(!std::strcmp(kv[7].key, "hw") && !std::strcmp(kv[7].value, "xiao-esp32c6-mr60"), "hw sits after role");
  CHECK(!std::strcmp(value_of(kv, m, "name"), "Hallway"), "a name wins over the id");
  CHECK(!std::strcmp(value_of(kv, m, "bport"), "1883"), "bport rendered");
  CHECK(!std::strcmp(value_of(kv, m, "tls"), "1") && !std::strcmp(value_of(kv, m, "secure_port"), "8443"), "tls pair");
  f.tls = 0;
  const size_t k = enumerate(f, kv, MAX_KEYS, s);
  CHECK(k == 13 && !std::strcmp(value_of(kv, k, "tls"), "0") && value_of(kv, k, "secure_port") == nullptr, "tls off: no port");
  Kv few[4];
  CHECK(enumerate(f, few, 4, s) == 0, "a cap too small yields no partial advert");
}

static void a_broker_without_a_port_is_no_broker() {
  Fields f; f.broker = "192.168.1.5"; f.bport = 0;
  Kv kv[MAX_KEYS]; Scratch s;
  const size_t n = enumerate(f, kv, MAX_KEYS, s);
  CHECK(!std::strcmp(value_of(kv, n, "broker"), ""), "no port, no referral");
  CHECK(!std::strcmp(value_of(kv, n, "device_id"), "canary") && !std::strcmp(value_of(kv, n, "role"), "witness"), "defaults");
}

static void caps_round_trip() {
  char buf[CAPS_CAP];
  CHECK(caps_string(0, buf, sizeof(buf)) == 0 && buf[0] == '\0', "no caps, empty word list");
  CHECK(caps_string(CAP_STATUS | CAP_FLEET | CAP_OTA | CAP_BLE_DOOR | CAP_BEACON | CAP_PEER | CAP_MQTT | CAP_WEB, buf, sizeof(buf)) > 0, "all");
  CHECK(!std::strcmp(buf, "status,fleet,ota,ble,beacon,peer,mqtt,web"), "all words: %s", buf);
  CHECK(parse_caps(buf) == (CAP_STATUS | CAP_FLEET | CAP_OTA | CAP_BLE_DOOR | CAP_BEACON | CAP_PEER | CAP_MQTT | CAP_WEB), "round trip");
  CHECK(parse_caps("fleet,nonsense,peer") == (CAP_FLEET | CAP_PEER), "unknown words are skipped");
  CHECK(parse_caps("") == 0 && parse_caps(nullptr) == 0, "empty");
  char tiny[8];
  CHECK(caps_string(CAP_STATUS | CAP_FLEET, tiny, sizeof(tiny)) == 0 && tiny[0] == '\0', "no half list");
}

static void host_labels() {
  CHECK(valid_host_label("canary-sense-001-a1b2c3"), "a unit host");
  CHECK(valid_host_label("canary"), "bare");
  CHECK(!valid_host_label("-canary"), "leading dash");
  CHECK(!valid_host_label("canary-"), "trailing dash");
  CHECK(!valid_host_label("canary.local"), "a dot is not a label");
  CHECK(!valid_host_label("canary sense"), "a space");
  CHECK(!valid_host_label(""), "empty");
  CHECK(!valid_host_label(nullptr), "null");
  char long_label[70]; std::memset(long_label, 'a', 64); long_label[64] = '\0';
  CHECK(!valid_host_label(long_label), "64 chars is too long");
  long_label[63] = '\0';
  CHECK(valid_host_label(long_label), "63 chars is the limit");
}

static void the_constants() {
  CHECK(!std::strcmp(SERVICE, "securacv") && !std::strcmp(PROTO, "tcp"), "_securacv._tcp");
  CHECK(PORT_HTTP == 80, "the port the node answers on");
  CHECK(!std::strcmp(COORD_HUB, "hub") && !std::strcmp(COORD_SELF, "self"), "coord words");
}

int main() {
  the_canonical_order_and_the_omits();
  a_broker_without_a_port_is_no_broker();
  caps_round_trip();
  host_labels();
  the_constants();
  if (g_failures) { std::printf("test_mdns_txt: %d failure(s)\n", g_failures); return 1; }
  std::printf("test_mdns_txt: all passed\n");
  return 0;
}
