// The canary-wap's identity answers (sweep F212), through the REAL
// identity_json.h (on the REAL wap_json_writer.h) that handle_device_info()
// and send_provisioning_receipt() build them with.
//
// Before, both were one snprintf() each into a fixed buffer, with every
// string written by an unescaped %s. GET /api/device-info carries the device
// name a person typed, and the routes in front of
// setup_wizard::set_device_name() accept `"`, `\` and control bytes (they
// bound the length and want one byte to survive the mDNS-label sanitizer), so
// a name holding `"` made the answer unparseable. This suite holds the
// builders to what a reader must get: the old answers' bytes for every
// ordinary input (the old formats are spelled here, verbatim), an answer that
// parses under JSON.parse's rules (json_strict.h) for every name a person can
// type, with the name read back byte for byte, and a length measured first
// and written whole or not at all.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "identity_json.h"
#include "json_strict.h"

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

// The device-info answer as the old handler spelled it (canary_wap.ino before
// F212, its format verbatim), for comparing bytes.
std::string old_device_info(const identity_json::DeviceInfo& in) {
  char json[4096];
  std::snprintf(json, sizeof(json),
    "{"
    "\"device_id\":\"%s\","
    "\"device_name\":\"%s\","
    "\"mdns_host\":\"%s\","
    "\"firmware\":\"%s\","
    "\"pubkey_fp\":\"%s\","
    "\"hw_token\":\"%s\","
    "\"uptime_ms\":%lu,"
    "\"chain_length\":%lu,"
    "\"born_day\":%lu,"
    "\"born_exact\":%s,"
    "\"auth_required\":true,"
    "\"tls_enabled\":%s,"
    "\"ap_auth\":\"%s\","
    "\"provisioning_gate\":\"physical_button\""
    "}",
    in.device_id,
    in.device_name ? in.device_name : "",
    in.mdns_host,
    in.firmware,
    in.pubkey_fp,
    in.hw_token,
    (unsigned long)in.uptime_ms,
    (unsigned long)in.chain_length,
    (unsigned long)in.born_day,
    in.born_exact ? "true" : "false",
    in.tls_enabled ? "true" : "false",
    in.ap_auth);
  return json;
}

// The receipt as the old send_provisioning_receipt() spelled it.
std::string old_receipt(const identity_json::Receipt& in) {
  char json[4096];
  std::snprintf(json, sizeof(json),
    "{\n"
    "  \"device_id\": \"%s\",\n"
    "  \"base_url\": \"%s://%s\",\n"
    "  \"token\": \"%s\",\n"
    "  \"pubkey_fp\": \"%s\",\n"
    "  \"firmware\": \"%s\",\n"
    "  \"hw_token\": \"%s\",\n"
    "  \"ap_ssid\": \"%s\",\n"
    "  \"ap_password\": \"%s\",\n"
    "  \"tls_cert_fp\": \"%s\",\n"
    "  \"provisioned_at\": \"boot:%lu\"\n"
    "}",
    in.device_id,
    in.tls_enabled ? "https" : "http",
    in.ap_ip,
    in.token,
    in.pubkey_fp,
    in.firmware,
    in.hw_token,
    in.ap_ssid,
    in.ap_password,
    in.tls_cert_fp,
    (unsigned long)in.boot_count);
  return json;
}

identity_json::DeviceInfo typical_info(const char* name) {
  identity_json::DeviceInfo in = {};
  in.device_id = "canary-wap-Kx7d";
  in.device_name = name;
  in.mdns_host = "canary-kitchen";
  in.firmware = "2.4.1";
  in.pubkey_fp = "a1b2c3d4e5f60718";
  in.hw_token = "0f1e2d3c4b5a6978";
  in.uptime_ms = 4294967295u;
  in.chain_length = 1234;
  in.born_day = 20345;
  in.born_exact = true;
  in.tls_enabled = false;
  in.ap_auth = "wpa2";
  return in;
}

identity_json::Receipt typical_receipt() {
  identity_json::Receipt in = {};
  in.device_id = "canary-wap-Kx7d";
  in.tls_enabled = true;
  in.ap_ip = "192.168.4.1";
  in.token = "cv_0123456789abcdefABCDEF0123456789";
  in.pubkey_fp = "a1b2c3d4e5f60718";
  in.firmware = "2.4.1";
  in.hw_token = "0f1e2d3c4b5a6978";
  in.ap_ssid = "SecuraCV-Kx7d";
  in.ap_password = "cv-abcdefghjkmn";
  in.tls_cert_fp = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
  in.boot_count = 42;
  return in;
}

// What the handler does: measure, allocate exactly that plus the NUL, write.
std::string handler_info(const identity_json::DeviceInfo& in) {
  const size_t need = identity_json::device_info(in, nullptr, 0) + 1;
  std::vector<char> buf(need, '\x7f');
  const size_t n = identity_json::device_info(in, buf.data(), need);
  CHECK(n + 1 == need);
  CHECK(buf[n] == '\0' && std::strlen(buf.data()) == n);
  return std::string(buf.data());
}

std::string handler_receipt(const identity_json::Receipt& in) {
  const size_t need = identity_json::provisioning_receipt(in, nullptr, 0) + 1;
  std::vector<char> buf(need, '\x7f');
  const size_t n = identity_json::provisioning_receipt(in, buf.data(), need);
  CHECK(n + 1 == need);
  CHECK(buf[n] == '\0' && std::strlen(buf.data()) == n);
  return std::string(buf.data());
}

std::string str_field(const json_strict::Value& v, const char* key) {
  const json_strict::Value* f = v.get(key);
  return (f && f->kind == json_strict::Value::String) ? f->text : std::string("<missing>");
}

// Every ordinary input answers the old bytes: the routes' readers see no change.
void test_ordinary_answers_are_the_old_bytes() {
  const char* const names[] = {"kitchen", "Front Door 2", "", nullptr};
  for (const char* name : names) {
    const identity_json::DeviceInfo in = typical_info(name);
    CHECK(handler_info(in) == old_device_info(in));
  }
  identity_json::DeviceInfo off = typical_info("garage");
  off.born_exact = false;
  off.tls_enabled = true;
  off.born_day = 0;
  off.uptime_ms = 0;
  CHECK(handler_info(off) == old_device_info(off));

  identity_json::Receipt r = typical_receipt();
  CHECK(handler_receipt(r) == old_receipt(r));
  r.tls_enabled = false;
  r.tls_cert_fp = "";
  r.boot_count = 4294967295u;
  CHECK(handler_receipt(r) == old_receipt(r));
}

// The item's case: a name holding `"` and `\`. The old answer does not parse;
// the new one does, and gives the name back as typed.
void test_a_name_with_quote_and_backslash() {
  const char* name = "kitchen \"north\" \\ door";
  const identity_json::DeviceInfo in = typical_info(name);
  json_strict::Value v;
  CHECK(!json_strict::parse(old_device_info(in), &v));   // the bug, as it was
  const std::string out = handler_info(in);
  CHECK(json_strict::parse(out, &v));
  CHECK(str_field(v, "device_name") == name);
  CHECK(str_field(v, "device_id") == "canary-wap-Kx7d");
  CHECK(str_field(v, "provisioning_gate") == "physical_button");
  CHECK(v.get("auth_required") && v.get("auth_required")->kind == json_strict::Value::Bool &&
        v.get("auth_required")->b);
  CHECK(v.get("uptime_ms") && v.get("uptime_ms")->text == "4294967295");
  CHECK(out.find("\"device_name\":\"kitchen \\\"north\\\" \\\\ door\"") != std::string::npos);
}

// Every byte a name can hold (the routes take any byte but NUL, 1 to 32 of
// them): each name parses and reads back exactly.
void test_every_byte_a_name_can_hold() {
  int names = 0;
  for (int start = 1; start < 256; start += 32) {
    std::string name;
    for (int b = start; b < start + 32 && b < 256; ++b) name.push_back((char)b);
    const identity_json::DeviceInfo in = typical_info(name.c_str());
    json_strict::Value v;
    const std::string out = handler_info(in);
    CHECK(json_strict::parse(out, &v));
    CHECK(str_field(v, "device_name") == name);
    ++names;
  }
  CHECK(names == 8);
  // The longest a name can spend: 32 control bytes at six bytes each.
  const std::string worst(32, '\x01');
  const identity_json::DeviceInfo in = typical_info(worst.c_str());
  CHECK(identity_json::device_info(in, nullptr, 0) ==
        identity_json::device_info(typical_info(""), nullptr, 0) + 32 * 6);
}

// Measured first, written whole or not at all: a buffer one byte short holds
// "" and nothing past it is written.
void test_measured_then_whole_or_nothing() {
  const identity_json::DeviceInfo in = typical_info("hall \"A\"");
  const size_t need = identity_json::device_info(in, nullptr, 0);
  CHECK(need == handler_info(in).size());
  for (size_t cap : {need, need - 1, (size_t)17, (size_t)1}) {
    std::vector<char> buf(cap + 8, '\x7f');
    CHECK(identity_json::device_info(in, buf.data(), cap) == need);
    CHECK(buf[0] == '\0');
    bool past = true;
    for (size_t i = cap; i < buf.size(); ++i) past = past && buf[i] == '\x7f';
    CHECK(past);
  }
  CHECK(identity_json::device_info(in, nullptr, 64) == need);

  const identity_json::Receipt r = typical_receipt();
  const size_t rneed = identity_json::provisioning_receipt(r, nullptr, 0);
  std::vector<char> rbuf(rneed, '\x7f');
  CHECK(identity_json::provisioning_receipt(r, rbuf.data(), rneed) == rneed);
  CHECK(rbuf[0] == '\0');
}

// The receipt escapes as well: every field reads back, the base URL whole.
void test_the_receipt_parses_whatever_its_fields_hold() {
  identity_json::Receipt r = typical_receipt();
  r.ap_ssid = "Secura\"CV\\\x01";
  r.ap_ip = "10.0.0.1";
  json_strict::Value v;
  CHECK(!json_strict::parse(old_receipt(r), &v));
  CHECK(json_strict::parse(handler_receipt(r), &v));
  CHECK(str_field(v, "ap_ssid") == "Secura\"CV\\\x01");
  CHECK(str_field(v, "base_url") == "https://10.0.0.1");
  CHECK(str_field(v, "token") == "cv_0123456789abcdefABCDEF0123456789");
  CHECK(str_field(v, "ap_password") == "cv-abcdefghjkmn");
  CHECK(str_field(v, "provisioned_at") == "boot:42");
  r.tls_enabled = false;
  CHECK(json_strict::parse(handler_receipt(r), &v));
  CHECK(str_field(v, "base_url") == "http://10.0.0.1");
}

}  // namespace

int main() {
  std::printf("identity answers (F212): GET /api/device-info and the provisioning receipt\n");
  test_ordinary_answers_are_the_old_bytes();
  test_a_name_with_quote_and_backslash();
  test_every_byte_a_name_can_hold();
  test_measured_then_whole_or_nothing();
  test_the_receipt_parses_whatever_its_fields_hold();
  if (g_failures) {
    std::printf("%d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("ALL identity_json TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
