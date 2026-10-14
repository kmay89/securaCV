// The canary-wap's device name holds no control byte (sweep F226), through
// the REAL device_name_rule.h and setup_wizard.h's init(), set_device_name()
// and get_device_name(), cut verbatim (firmware/tests_host/cut_functions.awk)
// over the first-boot suite's fake NVS (stubs/first_boot), and canary_wap.ino's
// sanitize_mdns_label(), cut verbatim, to hold the routes' label test to the
// sanitizer it replaced.
//
// Both rename routes, POST /api/wifi/connect's `device_name` and POST
// /api/device-name, store the name through setup_wizard::set_device_name(),
// which bounded its length and nothing else, and two answers carry the name
// through ArduinoJson 7.4.1: POST /api/device-name's own answer and GET
// /api/fleet/scan's self row. ArduinoJson writes `"` `\` \b \f \n \r \t as
// two-byte escapes and every other byte raw (TextFormatter::writeChar over
// EscapeSequence's table), so a name holding 0x01 made both answers fail a
// browser's JSON.parse. Now:
//
//  - each route decides with device_name_rule::usable() and stores with
//    set_device_name(), which refuses a name holding a control byte itself
//    (check_wap_json_answers.py rule J8 holds the routes, the setter and the
//    loader to these forms; each route's step is spelled below as the sketch
//    spells it);
//  - a name stored before the rule is read back by init() with each control
//    byte as a space, so its mDNS label is the one it had;
//  - so every name the device can hold writes both answers (spelled below
//    with ArduinoJson's escaping, which a scratch harness over the library
//    matched byte for byte for every byte value) as text json_strict.h
//    parses under JSON.parse's rules.
//
// With the setter's old length-only test put back 279 checks fail, with
// init()'s scrub removed 230, and with usable() taking control bytes 187.
//
// And POST /api/device-name's refusal (sweep F263): its words are
// device_name_rule::kRefusal, written beside usable(), whose bound a
// static_assert holds to kMaxLen; the words name each clause the rule
// applies, and the old ones ("letters/digits/hyphen") named one it never did.

#include "arduino_globals.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Arduino.h"
#include "Preferences.h"
#include "csi_module_settings_nvs.h"
#include "device_name_rule.h"
#include "json_strict.h"

static uint32_t millis() { return 4242; }

// ── setup_wizard.h's name: its statics and three functions, cut verbatim ──
namespace setup_wizard {
static constexpr size_t DEVICE_NAME_MAX = 32;
static bool s_first_boot = false;
static bool s_active = false;
static uint32_t s_started_ms = 0;
static char s_device_name[DEVICE_NAME_MAX + 1] = {0};
#include "test_device_name_wizard.inc"
}  // namespace setup_wizard

// ── canary_wap.ino's mDNS-label sanitizer, cut verbatim ──
#include "test_device_name_label.inc"

namespace {

int g_checks = 0;
int g_failures = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      ++g_failures;                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);            \
    }                                                                     \
  } while (0)

// ── NVS and a boot ──
void store_name(const char* v, size_t n) {
  HostFirstBootNvs::Row& r = host_nvs().rows["securacv/dev_name"];
  r.type = 's';
  r.bytes.assign(v, v + n);
}

std::string stored_name() {
  auto it = host_nvs().rows.find("securacv/dev_name");
  if (it == host_nvs().rows.end()) return "<none>";
  return std::string(it->second.bytes.begin(), it->second.bytes.end());
}

void boot() {
  setup_wizard::s_first_boot = false;
  setup_wizard::s_active = false;
  setup_wizard::s_started_ms = 0;
  std::memset(setup_wizard::s_device_name, 0, sizeof(setup_wizard::s_device_name));
  setup_wizard::init();
}

std::string name_now() {
  const char* n = setup_wizard::get_device_name();
  return n ? n : "";
}

// ── The two routes' rename steps, as canary_wap.ino spells them (rule J8) ──

// handle_wifi_connect(): an unusable name is ignored, the Wi-Fi save goes on.
bool wifi_connect_rename(const char* device_name) {
  return device_name_rule::usable(device_name) && setup_wizard::set_device_name(device_name);
}

// handle_device_name(): the answer's `ok`, and its error when refused.
const char* device_name_route(const char* name, bool* ok) {
  *ok = false;
  if (!device_name_rule::usable(name)) return device_name_rule::kRefusal;
  if (!setup_wizard::set_device_name(name)) return "Could not save name";
  *ok = true;
  return nullptr;
}

// ── The two ArduinoJson answers that carry the name ──

// ArduinoJson 7.4.1's string: TextFormatter::writeChar() over
// EscapeSequence's serializing table ("\"\"\\\\b\bf\fn\nr\rt\t"); every
// other byte, a control byte included, is written as it is.
std::string aj_string(const char* s) {
  std::string out = "\"";
  for (const char* p = s ? s : ""; *p; ++p) {
    switch (*p) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += *p;
    }
  }
  return out + "\"";
}

// POST /api/device-name's answer after a rename: ok, device_name, mdns_host.
std::string device_name_answer() {
  return std::string("{\"ok\":true,\"device_name\":") + aj_string(setup_wizard::get_device_name()) +
         ",\"mdns_host\":\"canary-kitchen\"}";
}

// GET /api/fleet/scan's answer around its self row.
std::string fleet_scan_answer() {
  const char* nm = setup_wizard::get_device_name();
  return std::string("{\"ok\":true,\"scanning\":false,\"age_ms\":0,\"self\":{\"device_id\":\"canary-s3-4dC2\",\"name\":") +
         aj_string(nm ? nm : "") +
         ",\"mdns_host\":\"canary-kitchen\",\"fw\":\"2.4.15\",\"ip\":\"192.168.4.1\"},\"canaries\":[]}";
}

// Both answers parse under JSON.parse's rules and give the name back as held.
bool answers_parse() {
  json_strict::Value a, b;
  const std::string name = name_now();
  if (!json_strict::parse(device_name_answer(), &a) || !json_strict::parse(fleet_scan_answer(), &b)) return false;
  const json_strict::Value* an = a.get("device_name");
  const json_strict::Value* self = b.get("self");
  const json_strict::Value* bn = self ? self->get("name") : nullptr;
  return an && bn && an->text == name && bn->text == name;
}

std::string with_byte(const char* head, int b, const char* tail) {
  std::string s = head;
  s.push_back((char)b);
  return s + tail;
}

// ── Tests ──

// The rule itself: every byte below 0x20 refused wherever it stands; every
// other name the routes took before is still storable.
void test_the_rule_refuses_every_control_byte() {
  for (int b = 1; b < 0x20; ++b) {
    for (const std::string& n : {with_byte("", b, "kitchen"), with_byte("kit", b, "chen"),
                                 with_byte("kitchen", b, "")}) {
      CHECK(device_name_rule::is_control(n[n.find((char)b)]));
      CHECK(!device_name_rule::storable(n.c_str()));
      CHECK(!device_name_rule::usable(n.c_str()));
    }
  }
  const char* const ok[] = {"kitchen", "Front Door 2", "kitchen \"north\" \\ door", "K\xc3\xbc" "che",
                            "a\x7f" "b", "x", "0123456789abcdef0123456789abcdef"};
  for (const char* n : ok) {
    CHECK(device_name_rule::storable(n));
    CHECK(device_name_rule::usable(n));
  }
  CHECK(!device_name_rule::storable(nullptr));
  CHECK(!device_name_rule::storable(""));
  CHECK(!device_name_rule::storable("0123456789abcdef0123456789abcdefX"));   // 33 bytes
  CHECK(device_name_rule::storable("---"));
  CHECK(!device_name_rule::usable("---"));                                    // an empty label
  CHECK(!device_name_rule::usable(" \"\\ "));
}

// usable() is the routes' old test (1-32 bytes and a label the sanitizer
// keeps) with control bytes refused, and nothing else changed: for every
// one- and two-byte name, and names around them, the old test and the new
// one agree except where a control byte is in the name.
void test_usable_is_the_old_route_test_without_control_bytes() {
  auto old_test = [](const std::string& n) {
    if (n.empty() || n.size() > setup_wizard::DEVICE_NAME_MAX) return false;
    char probe[setup_wizard::DEVICE_NAME_MAX + 1] = {0};
    sanitize_mdns_label(n.c_str(), probe, sizeof(probe));
    return probe[0] != '\0';
  };
  auto has_control = [](const std::string& n) {
    for (char c : n)
      if ((unsigned char)c < 0x20) return true;
    return false;
  };
  int compared = 0;
  for (int a = 1; a < 256; ++a) {
    for (int b = 0; b < 256; ++b) {
      std::string n(1, (char)a);
      if (b) n.push_back((char)b);
      for (const std::string& m : {n, "-" + n + "-", n + std::string(30, 'x'), n + std::string(31, 'x')}) {
        const bool expect = old_test(m) && !has_control(m);
        if (device_name_rule::usable(m.c_str()) != expect) {
          CHECK(device_name_rule::usable(m.c_str()) == expect);
          return;
        }
        ++compared;
      }
    }
  }
  CHECK(compared == 255 * 256 * 4);
}

// The setter both routes call: a name holding a control byte is refused,
// and the name in RAM and in NVS stays the one before.
void test_the_setter_refuses_every_control_byte() {
  host_nvs().clear();
  store_name("Porch", 5);
  boot();
  CHECK(name_now() == "Porch");
  for (int b = 1; b < 0x20; ++b) {
    for (const std::string& n : {with_byte("", b, "kitchen"), with_byte("kit", b, "chen"),
                                 with_byte("kitchen", b, "")}) {
      CHECK(!setup_wizard::set_device_name(n.c_str()));
      CHECK(name_now() == "Porch");
      CHECK(stored_name() == "Porch");
    }
  }
  CHECK(setup_wizard::set_device_name("kitchen \"north\""));
  CHECK(name_now() == "kitchen \"north\"");
  CHECK(stored_name() == "kitchen \"north\"");
  CHECK(!setup_wizard::set_device_name(""));
  CHECK(!setup_wizard::set_device_name(nullptr));
  CHECK(!setup_wizard::set_device_name("0123456789abcdef0123456789abcdefX"));
  CHECK(name_now() == "kitchen \"north\"");
}

// Each route, with every byte below 0x20: refused, the name kept, and both
// answers that carry the name still parse. An ordinary rename through each
// still lands.
void test_every_route_refuses_and_both_answers_parse() {
  host_nvs().clear();
  store_name("Porch", 5);
  boot();
  for (int b = 1; b < 0x20; ++b) {
    for (const std::string& n : {with_byte("", b, "kitchen"), with_byte("kit", b, "chen"),
                                 with_byte("kitchen", b, "")}) {
      CHECK(!wifi_connect_rename(n.c_str()));
      bool ok = true;
      const char* why = device_name_route(n.c_str(), &ok);
      CHECK(!ok);
      CHECK(why != nullptr && std::strcmp(why, device_name_rule::kRefusal) == 0);
      CHECK(name_now() == "Porch");
      CHECK(stored_name() == "Porch");
      CHECK(answers_parse());
    }
  }
  CHECK(wifi_connect_rename("Garage 2"));
  CHECK(name_now() == "Garage 2");
  CHECK(answers_parse());
  bool ok = false;
  CHECK(device_name_route("kitchen \"north\" \\ door", &ok) == nullptr && ok);
  CHECK(name_now() == "kitchen \"north\" \\ door");
  CHECK(stored_name() == "kitchen \"north\" \\ door");
  CHECK(answers_parse());
  // A name the routes refuse for its label is refused by both, as before.
  CHECK(!wifi_connect_rename("---"));
  CHECK(device_name_route("---", &ok) != nullptr && !ok);
  CHECK(name_now() == "kitchen \"north\" \\ door");
}

// A name stored before F226, holding control bytes: read back with each as a
// space, so both answers parse and the mDNS label is the one the stored
// bytes gave. The NVS row is not rewritten (init() opens read-only); the
// next rename replaces it.
void test_a_name_stored_before_reads_back_without_control_bytes() {
  for (int b = 1; b < 0x20; ++b) {
    for (const std::string& n : {with_byte("", b, "kitchen"), with_byte("kit", b, "chen"),
                                 with_byte("kitchen", b, ""), with_byte("a", b, std::string(30, 'z').c_str())}) {
      host_nvs().clear();
      store_name(n.data(), n.size());
      boot();
      std::string expect = n;
      for (char& c : expect)
        if ((unsigned char)c < 0x20) c = ' ';
      CHECK(name_now() == expect);
      CHECK(answers_parse());
      char before[setup_wizard::DEVICE_NAME_MAX + 1] = {0};
      char after[setup_wizard::DEVICE_NAME_MAX + 1] = {0};
      sanitize_mdns_label(n.c_str(), before, sizeof(before));
      sanitize_mdns_label(setup_wizard::get_device_name(), after, sizeof(after));
      CHECK(std::strcmp(before, after) == 0);
      CHECK(stored_name() == n);
      CHECK(host_nvs().error_logs == 0);
    }
  }
  // Every control byte at once, around a letter.
  {
    std::string n;
    for (int b = 1; b < 0x20; ++b) n.push_back((char)b);
    n.push_back('k');
    host_nvs().clear();
    store_name(n.data(), n.size());
    boot();
    CHECK(name_now() == std::string(31, ' ') + "k");
    CHECK(answers_parse());
  }
  // A name stored without control bytes reads back as it is.
  host_nvs().clear();
  store_name("Porch \"2\"", 9);
  boot();
  CHECK(name_now() == "Porch \"2\"");
  CHECK(answers_parse());
  // No name stored: no name, and both answers carry "".
  host_nvs().clear();
  boot();
  CHECK(setup_wizard::get_device_name() == nullptr);
  CHECK(answers_parse());
}

// POST /api/device-name's refusal says the rule the route applies (sweep
// F263): its words name each of usable()'s three clauses, and each clause
// the words name refuses a name on its own (one byte past the bound, an
// empty name, a control byte, a name with no letter or digit); a name that
// breaks none of them is taken, whatever else it holds (a space, a quote, a
// byte past 0x7f), which the old words ("letters/digits/hyphen") said was
// refused. The bound the words state is the one storable() applies, read
// from the words themselves. And the words name the bytes exactly (the F263
// review): a name of letters past ASCII alone is refused, and the words say
// "ASCII letter"; DEL and a C1 control are kept, and the words say "below
// 0x20", not "control characters".
void test_the_refusal_states_the_rule_the_route_applies() {
  const std::string words = device_name_rule::kRefusal;
  CHECK(words.find("1 to 32 bytes") != std::string::npos);
  CHECK(words.find("no byte below 0x20") != std::string::npos);
  CHECK(words.find("at least one ASCII letter or digit") != std::string::npos);
  CHECK(words.find("control") == std::string::npos);
  CHECK(words.find("hyphen") == std::string::npos);
  CHECK(device_name_rule::states_bound(device_name_rule::kRefusal, device_name_rule::kMaxLen));
  CHECK(!device_name_rule::states_bound(device_name_rule::kRefusal, device_name_rule::kMaxLen + 1));
  CHECK(!device_name_rule::states_bound("Invalid name (1-32 chars, letters/digits/hyphen)", 32));
  const std::string longest(device_name_rule::kMaxLen, 'n');
  bool ok = false;
  for (const std::string& refused : {longest + "n", std::string(), std::string("kit\x01" "chen"),
                                     std::string(" \"-\" "), std::string("\xe5\x8f\xb0\xe6\x89\x80"),
                                     std::string("\xce\xa3\xce\xb1\xce\xbb\xcf\x8c\xce\xbd\xce\xb9")}) {
    CHECK(!device_name_rule::usable(refused.c_str()));
    CHECK(device_name_route(refused.c_str(), &ok) == std::string(words) && !ok);
  }
  for (const std::string& taken : {longest, std::string("kitchen north"), std::string("kitchen \"north\""),
                                   std::string("K\xc3\xbc" "che"), std::string("a\x7f" "b"),
                                   std::string("\xc2\x85" "x")}) {
    CHECK(device_name_route(taken.c_str(), &ok) == nullptr && ok);
    CHECK(name_now() == taken);
  }
}

// The answers' model writes a control byte raw, as ArduinoJson does: a name
// holding one does not parse (what F226 refuses), so the parse checks above
// are not vacuous.
void test_a_control_byte_breaks_the_answers() {
  for (int b = 1; b < 0x20; ++b) {
    const std::string n = with_byte("kit", b, "chen");
    std::memset(setup_wizard::s_device_name, 0, sizeof(setup_wizard::s_device_name));
    std::memcpy(setup_wizard::s_device_name, n.data(), n.size());
    const bool escaped = (b == '\b' || b == '\f' || b == '\n' || b == '\r' || b == '\t');
    CHECK(answers_parse() == escaped);
  }
}

}  // namespace

int main() {
  test_the_rule_refuses_every_control_byte();
  test_usable_is_the_old_route_test_without_control_bytes();
  test_the_setter_refuses_every_control_byte();
  test_every_route_refuses_and_both_answers_parse();
  test_a_name_stored_before_reads_back_without_control_bytes();
  test_a_control_byte_breaks_the_answers();
  test_the_refusal_states_the_rule_the_route_applies();
  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL device_name TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
