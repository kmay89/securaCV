// Host tests for firmware/common/network/hub_handoff.h — the hub hand-off
// over the Bluetooth setup door: the JSON a phone writes to the companion
// service's HUB characteristic right after the Improv join (the broker it
// found over Bonjour and the login it minted), read strictly, and the status
// byte it reads back. The iOS side is written against the same document.
//
// The rows that matter most:
//   * the documented example parses field for field; every optional field
//     has its default (port 1883, no credentials, tls false);
//   * a host is a DNS name or an IPv4 literal and nothing else: a trailing
//     dot, a doubled dot, a hyphen at a label's edge, an underscore, a
//     64th byte, a digits-and-dots name that is not an address, an octet
//     past 255 or with a leading zero are all BadHost; a missing host too;
//   * a port is a plain integer 1..65535: 0, 65536, a sign, a fraction, an
//     exponent, a leading zero are BadPort; a quoted number is Malformed;
//   * user and pass travel together: one without the other, an empty user,
//     a 65th byte in either is BadCredentials; an empty pass is allowed;
//   * the three escapes a password can need (\" \\ \/) are unescaped; every
//     other escape (\n, \uXXXX, a bare backslash), a control byte and a NUL
//     inside a string are Malformed;
//   * a duplicate key is Malformed (pinned: not "last wins"); an unknown
//     key, a non-object, a trailing comma, trailing bytes, a wrong type are
//     Malformed; a malformed document is Malformed whatever its fields said;
//   * 241 bytes is TooLong before a byte is read; 240 parses;
//   * every refusal leaves the record wiped (no password behind), and the
//     field verdicts come in the order BadHost, BadPort, BadCredentials;
//   * the status byte's values, the UUIDs and the payload bound are pinned.

#include "../common/network/hub_handoff.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace canary::net::hub_handoff;

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

static Verdict parse(const std::string& doc, HubHandoff& out) {
  return parse_hub_handoff((const uint8_t*)doc.data(), doc.size(), out);
}

static bool all_zero(const HubHandoff& h) {
  const uint8_t* b = (const uint8_t*)&h;
  for (size_t i = 0; i < sizeof(h); ++i) {
    if (b[i] != 0) return false;
  }
  return true;
}

// A refused document must leave nothing behind — checked on every refusal
// row below through this helper, which also pins the verdict.
static void expect_refused(const char* why, const std::string& doc, Verdict expected) {
  HubHandoff out;
  memset(&out, 0xA5, sizeof(out));   // a dirty record going in
  const Verdict v = parse(doc, out);
  CHECK(v == expected, "%s: expected %s, got %s for %s", why, verdict_name(expected),
        verdict_name(v), doc.c_str());
  CHECK(all_zero(out), "%s: the record is not wiped after %s", why, verdict_name(v));
}

static void the_documented_example_parses_field_for_field() {
  HubHandoff out;
  const Verdict v = parse(
      "{\"host\":\"homeassistant.local\",\"port\":1883,\"user\":\"canary\",\"pass\":\"s3cret\",\"tls\":false}",
      out);
  CHECK(v == Verdict::Ok, "verdict %s", verdict_name(v));
  CHECK(std::strcmp(out.host, "homeassistant.local") == 0, "host %s", out.host);
  CHECK(out.port == 1883, "port %u", out.port);
  CHECK(out.has_credentials, "credentials present");
  CHECK(std::strcmp(out.user, "canary") == 0, "user %s", out.user);
  CHECK(std::strcmp(out.pass, "s3cret") == 0, "pass %s", out.pass);
  CHECK(!out.tls, "tls false");
}

static void optional_fields_take_their_defaults() {
  HubHandoff out;
  CHECK(parse("{\"host\":\"homeassistant.local\"}", out) == Verdict::Ok, "host alone");
  CHECK(out.port == DEFAULT_PORT && out.port == 1883, "default port %u", out.port);
  CHECK(!out.has_credentials, "no credentials");
  CHECK(out.user[0] == '\0' && out.pass[0] == '\0', "user/pass empty");
  CHECK(!out.tls, "tls defaults false");

  CHECK(parse("{\"host\":\"ha\",\"port\":8883}", out) == Verdict::Ok, "host + port");
  CHECK(out.port == 8883 && !out.has_credentials, "port taken, no credentials");

  CHECK(parse("{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"p\"}", out) == Verdict::Ok, "host + credentials");
  CHECK(out.port == 1883 && out.has_credentials, "default port, credentials");

  CHECK(parse("{\"host\":\"ha\",\"tls\":true}", out) == Verdict::Ok, "host + tls");
  CHECK(out.tls, "tls true taken");

  // Key order is the writer's choice.
  CHECK(parse("{\"tls\":true,\"pass\":\"p\",\"port\":1,\"user\":\"u\",\"host\":\"ha\"}", out) == Verdict::Ok,
        "any key order");
  CHECK(out.port == 1 && out.tls && out.has_credentials, "fields taken in any order");

  // Whitespace anywhere JSON allows it.
  CHECK(parse(" \t\r\n{ \"host\" : \"ha\" , \"port\" : 1883 }\n", out) == Verdict::Ok, "whitespace");
}

static void an_ipv4_literal_is_a_host() {
  HubHandoff out;
  CHECK(parse("{\"host\":\"192.168.1.10\"}", out) == Verdict::Ok, "a dotted quad");
  CHECK(std::strcmp(out.host, "192.168.1.10") == 0, "host %s", out.host);
  CHECK(parse("{\"host\":\"0.0.0.0\"}", out) == Verdict::Ok, "all zeros is syntactically an address");
  CHECK(parse("{\"host\":\"255.255.255.255\"}", out) == Verdict::Ok, "255 is the top octet");

  expect_refused("octet past 255", "{\"host\":\"192.168.1.256\"}", Verdict::BadHost);
  expect_refused("three octets", "{\"host\":\"192.168.1\"}", Verdict::BadHost);
  expect_refused("five octets", "{\"host\":\"1.2.3.4.5\"}", Verdict::BadHost);
  expect_refused("leading zero in an octet", "{\"host\":\"192.168.01.1\"}", Verdict::BadHost);
  expect_refused("four-digit octet", "{\"host\":\"1920.1.1.1\"}", Verdict::BadHost);
  expect_refused("digits only, not an address", "{\"host\":\"123\"}", Verdict::BadHost);
  expect_refused("digits and dots, not an address", "{\"host\":\"1.2\"}", Verdict::BadHost);
  expect_refused("address with a trailing dot", "{\"host\":\"192.168.1.10.\"}", Verdict::BadHost);
}

static void a_hostname_is_held_to_the_dns_rule() {
  HubHandoff out;
  CHECK(parse("{\"host\":\"localhost\"}", out) == Verdict::Ok, "a single label");
  CHECK(parse("{\"host\":\"ha\"}", out) == Verdict::Ok, "a two-letter label");
  CHECK(parse("{\"host\":\"1abc.local\"}", out) == Verdict::Ok, "a label starting with a digit");
  CHECK(parse("{\"host\":\"Home-Assistant.LOCAL\"}", out) == Verdict::Ok, "case and an inner hyphen");
  CHECK(std::strcmp(out.host, "Home-Assistant.LOCAL") == 0, "case kept: %s", out.host);
  CHECK(parse("{\"host\":\"a.b.c.d.e.example\"}", out) == Verdict::Ok, "many labels");

  const std::string h63(63, 'a');
  CHECK(parse("{\"host\":\"" + h63 + "\"}", out) == Verdict::Ok, "63 bytes fit");
  CHECK(std::strlen(out.host) == 63, "all 63 kept (%zu)", std::strlen(out.host));
  // A 63-byte label is the DNS bound, which is also the whole host bound
  // here, so the longest label that leaves room for a second one is 61.
  const std::string label61 = std::string(61, 'b') + ".x";
  CHECK(parse("{\"host\":\"" + label61 + "\"}", out) == Verdict::Ok, "a long label within bounds");

  expect_refused("64 bytes", "{\"host\":\"" + std::string(64, 'a') + "\"}", Verdict::BadHost);
  expect_refused("empty host", "{\"host\":\"\"}", Verdict::BadHost);
  expect_refused("missing host", "{\"port\":1883}", Verdict::BadHost);
  expect_refused("empty object", "{}", Verdict::BadHost);
  expect_refused("trailing dot", "{\"host\":\"homeassistant.local.\"}", Verdict::BadHost);
  expect_refused("leading dot", "{\"host\":\".local\"}", Verdict::BadHost);
  expect_refused("doubled dot", "{\"host\":\"ha..local\"}", Verdict::BadHost);
  expect_refused("only a dot", "{\"host\":\".\"}", Verdict::BadHost);
  expect_refused("label starting with a hyphen", "{\"host\":\"-ha.local\"}", Verdict::BadHost);
  expect_refused("label ending with a hyphen", "{\"host\":\"ha-.local\"}", Verdict::BadHost);
  expect_refused("name ending with a hyphen", "{\"host\":\"ha-\"}", Verdict::BadHost);
  expect_refused("underscore", "{\"host\":\"ha_1.local\"}", Verdict::BadHost);
  expect_refused("space", "{\"host\":\"home assistant\"}", Verdict::BadHost);
  expect_refused("a scheme is not a host", "{\"host\":\"mqtt://ha.local\"}", Verdict::BadHost);
  expect_refused("a port inside the host", "{\"host\":\"ha.local:1883\"}", Verdict::BadHost);
  expect_refused("an IPv6 literal", "{\"host\":\"[::1]\"}", Verdict::BadHost);
  expect_refused("a non-ASCII byte", "{\"host\":\"h\xC3\xA4.local\"}", Verdict::BadHost);
  expect_refused("an escaped quote in a host", "{\"host\":\"ha\\\".local\"}", Verdict::BadHost);
  expect_refused("host of another type", "{\"host\":1883}", Verdict::Malformed);
}

static void a_port_is_a_plain_integer_in_range() {
  HubHandoff out;
  CHECK(parse("{\"host\":\"ha\",\"port\":1}", out) == Verdict::Ok && out.port == 1, "1 is the floor");
  CHECK(parse("{\"host\":\"ha\",\"port\":65535}", out) == Verdict::Ok && out.port == 65535, "65535 is the ceiling");
  CHECK(parse("{\"host\":\"ha\",\"port\":8883}", out) == Verdict::Ok && out.port == 8883, "8883");

  expect_refused("port 0", "{\"host\":\"ha\",\"port\":0}", Verdict::BadPort);
  expect_refused("port 65536", "{\"host\":\"ha\",\"port\":65536}", Verdict::BadPort);
  expect_refused("a huge port", "{\"host\":\"ha\",\"port\":99999999999999999999}", Verdict::BadPort);
  expect_refused("a negative port", "{\"host\":\"ha\",\"port\":-1}", Verdict::BadPort);
  expect_refused("a fraction", "{\"host\":\"ha\",\"port\":1883.0}", Verdict::BadPort);
  expect_refused("an exponent", "{\"host\":\"ha\",\"port\":1e3}", Verdict::BadPort);
  expect_refused("a leading zero", "{\"host\":\"ha\",\"port\":01883}", Verdict::BadPort);
  expect_refused("a quoted port", "{\"host\":\"ha\",\"port\":\"1883\"}", Verdict::Malformed);
  expect_refused("a boolean port", "{\"host\":\"ha\",\"port\":true}", Verdict::Malformed);
  expect_refused("a null port", "{\"host\":\"ha\",\"port\":null}", Verdict::Malformed);
  expect_refused("a bare minus", "{\"host\":\"ha\",\"port\":-}", Verdict::Malformed);
}

static void user_and_pass_travel_together() {
  HubHandoff out;
  CHECK(parse("{\"host\":\"ha\",\"user\":\"canary\",\"pass\":\"\"}", out) == Verdict::Ok,
        "an empty pass is allowed");
  CHECK(out.has_credentials && out.pass[0] == '\0', "credentials present, pass empty");

  const std::string u64(64, 'u');
  const std::string p64(64, 'p');
  CHECK(parse("{\"host\":\"ha\",\"user\":\"" + u64 + "\",\"pass\":\"" + p64 + "\"}", out) == Verdict::Ok,
        "64 bytes each fit");
  CHECK(std::strlen(out.user) == 64 && std::strlen(out.pass) == 64, "all 64 kept (%zu/%zu)",
        std::strlen(out.user), std::strlen(out.pass));

  expect_refused("user without pass", "{\"host\":\"ha\",\"user\":\"canary\"}", Verdict::BadCredentials);
  expect_refused("pass without user", "{\"host\":\"ha\",\"pass\":\"s3cret\"}", Verdict::BadCredentials);
  expect_refused("an empty user", "{\"host\":\"ha\",\"user\":\"\",\"pass\":\"s3cret\"}", Verdict::BadCredentials);
  expect_refused("a 65-byte user",
                 "{\"host\":\"ha\",\"user\":\"" + std::string(65, 'u') + "\",\"pass\":\"p\"}",
                 Verdict::BadCredentials);
  expect_refused("a 65-byte pass",
                 "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"" + std::string(65, 'p') + "\"}",
                 Verdict::BadCredentials);
  expect_refused("a numeric user", "{\"host\":\"ha\",\"user\":7,\"pass\":\"p\"}", Verdict::Malformed);
  expect_refused("a null pass", "{\"host\":\"ha\",\"user\":\"u\",\"pass\":null}", Verdict::Malformed);
}

static void the_three_escapes_are_unescaped_and_no_other() {
  HubHandoff out;
  // pass: a"b\c/d — the three escapes a password can need.
  CHECK(parse("{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"a\\\"b\\\\c\\/d\"}", out) == Verdict::Ok,
        "the three escapes parse");
  CHECK(std::strcmp(out.pass, "a\"b\\c/d") == 0, "unescaped: %s", out.pass);
  // An escaped byte counts once toward the 64: 32 escaped quotes are 32 bytes.
  std::string quotes;
  for (int i = 0; i < 64; ++i) quotes += "\\\"";
  CHECK(parse("{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"" + quotes + "\"}", out) == Verdict::Ok,
        "64 escaped quotes are 64 bytes");
  CHECK(std::strlen(out.pass) == 64, "64 quotes kept (%zu)", std::strlen(out.pass));
  // A password of raw UTF-8 bytes is bytes: accepted, counted as bytes.
  CHECK(parse("{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"caf\xC3\xA9\"}", out) == Verdict::Ok, "UTF-8 bytes");
  CHECK(std::strcmp(out.pass, "caf\xC3\xA9") == 0, "bytes kept");

  expect_refused("\\n", "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"a\\nb\"}", Verdict::Malformed);
  expect_refused("\\t", "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"a\\tb\"}", Verdict::Malformed);
  expect_refused("\\b", "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"a\\bb\"}", Verdict::Malformed);
  expect_refused("\\uXXXX", "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"\\u0041\"}", Verdict::Malformed);
  expect_refused("\\u in a host", "{\"host\":\"h\\u0061\"}", Verdict::Malformed);
  expect_refused("a bare backslash at the end", "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"abc\\", Verdict::Malformed);
  expect_refused("an unknown escape", "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"a\\xb\"}", Verdict::Malformed);
  expect_refused("a raw newline in a string", "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"a\nb\"}", Verdict::Malformed);
  expect_refused("a raw tab in a string", "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"a\tb\"}", Verdict::Malformed);
  const std::string nul(1, '\0');
  expect_refused("a NUL in a string", "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"a" + nul + "b\"}",
                 Verdict::Malformed);
  expect_refused("a NUL between tokens", "{\"host\":\"ha\"" + nul + "}", Verdict::Malformed);
  expect_refused("a 0x7F is fine as a byte but 0x1F is not", "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"\x1F\"}",
                 Verdict::Malformed);
}

static void a_duplicate_key_is_malformed() {
  // Pinned: a duplicate is refused, not "last wins" — a document that says
  // two things is not believed on either.
  expect_refused("host twice", "{\"host\":\"ha\",\"host\":\"hb\"}", Verdict::Malformed);
  expect_refused("port twice", "{\"host\":\"ha\",\"port\":1,\"port\":2}", Verdict::Malformed);
  expect_refused("user twice", "{\"host\":\"ha\",\"user\":\"u\",\"user\":\"v\",\"pass\":\"p\"}", Verdict::Malformed);
  expect_refused("pass twice", "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"p\",\"pass\":\"q\"}", Verdict::Malformed);
  expect_refused("tls twice", "{\"host\":\"ha\",\"tls\":false,\"tls\":true}", Verdict::Malformed);
}

static void the_shape_is_held_strictly() {
  expect_refused("zero bytes", "", Verdict::Malformed);
  expect_refused("whitespace only", "  \n", Verdict::Malformed);
  expect_refused("an array", "[]", Verdict::Malformed);
  expect_refused("a string", "\"host\"", Verdict::Malformed);
  expect_refused("a bare brace", "{", Verdict::Malformed);
  expect_refused("unclosed object", "{\"host\":\"ha\"", Verdict::Malformed);
  expect_refused("unterminated string", "{\"host\":\"ha", Verdict::Malformed);
  expect_refused("missing colon", "{\"host\" \"ha\"}", Verdict::Malformed);
  expect_refused("missing comma", "{\"host\":\"ha\" \"port\":1883}", Verdict::Malformed);
  expect_refused("trailing comma", "{\"host\":\"ha\",}", Verdict::Malformed);
  expect_refused("leading comma", "{,\"host\":\"ha\"}", Verdict::Malformed);
  expect_refused("an unquoted key", "{host:\"ha\"}", Verdict::Malformed);
  expect_refused("single-quoted strings", "{'host':'ha'}", Verdict::Malformed);
  expect_refused("trailing bytes", "{\"host\":\"ha\"}x", Verdict::Malformed);
  expect_refused("two documents", "{\"host\":\"ha\"}{\"host\":\"hb\"}", Verdict::Malformed);
  expect_refused("an unknown key", "{\"host\":\"ha\",\"name\":\"kitchen\"}", Verdict::Malformed);
  expect_refused("a long unknown key", "{\"host\":\"ha\",\"hostname\":\"hb\"}", Verdict::Malformed);
  expect_refused("an uppercase key", "{\"Host\":\"ha\"}", Verdict::Malformed);
  expect_refused("an escaped key", "{\"ho\\/st\":\"ha\"}", Verdict::Malformed);
  expect_refused("a nested object", "{\"host\":{\"name\":\"ha\"}}", Verdict::Malformed);
  expect_refused("an array value", "{\"host\":[\"ha\"]}", Verdict::Malformed);
  expect_refused("tls as a string", "{\"host\":\"ha\",\"tls\":\"true\"}", Verdict::Malformed);
  expect_refused("tls as a number", "{\"host\":\"ha\",\"tls\":1}", Verdict::Malformed);
  expect_refused("tls as null", "{\"host\":\"ha\",\"tls\":null}", Verdict::Malformed);
  expect_refused("tls misspelled literal", "{\"host\":\"ha\",\"tls\":tru}", Verdict::Malformed);
  expect_refused("tls literal with a tail", "{\"host\":\"ha\",\"tls\":trueish}", Verdict::Malformed);
  expect_refused("a null host", "{\"host\":null}", Verdict::Malformed);
}

static void the_payload_bound_is_240_bytes() {
  // Exactly 240 bytes: a 63-byte host, a 64-byte user, every key, and a
  // pass sized to fill.
  const std::string host(63, 'h');
  const std::string user(64, 'u');
  std::string doc = "{\"host\":\"" + host + "\",\"port\":1883,\"user\":\"" + user + "\",\"pass\":\"";
  const std::string tail = "\",\"tls\":false}";
  CHECK(doc.size() + tail.size() < 240, "fixture: room for a pass");
  const size_t pass_len = 240 - doc.size() - tail.size();
  CHECK(pass_len <= CREDENTIAL_MAX, "fixture: pass of %zu bytes is within the credential bound", pass_len);
  doc += std::string(pass_len, 'p') + tail;
  CHECK(doc.size() == 240, "fixture: %zu bytes", doc.size());
  CHECK(doc.size() == MAX_PAYLOAD, "MAX_PAYLOAD is 240");
  HubHandoff out;
  CHECK(parse(doc, out) == Verdict::Ok, "240 bytes parse");
  CHECK(std::strlen(out.pass) == pass_len, "the pass came through whole");

  // 241 bytes: refused before a byte is read — a 241-byte document that
  // would otherwise be Malformed is TooLong, and the record is wiped.
  const std::string over(241, 'x');
  expect_refused("241 bytes of garbage", over, Verdict::TooLong);
  // And a 241-byte document that would otherwise be valid JSON is TooLong too.
  std::string valid241 = "{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"";
  const std::string t2 = "\"}";
  valid241 += std::string(241 - valid241.size() - t2.size(), 'p') + t2;
  CHECK(valid241.size() == 241, "fixture: %zu bytes", valid241.size());
  expect_refused("241 bytes of otherwise-valid JSON", valid241, Verdict::TooLong);

  // A null pointer with a length inside the bound is Malformed, not a read.
  HubHandoff out2;
  CHECK(parse_hub_handoff(nullptr, 10, out2) == Verdict::Malformed, "null data is Malformed");
  CHECK(parse_hub_handoff(nullptr, 241, out2) == Verdict::TooLong, "null data past the bound is TooLong");
}

static void field_verdicts_come_in_order_after_the_shape() {
  expect_refused("bad host beats bad port", "{\"host\":\"\",\"port\":0}", Verdict::BadHost);
  expect_refused("bad host beats bad credentials", "{\"host\":\"ha.\",\"user\":\"u\"}", Verdict::BadHost);
  expect_refused("bad port beats bad credentials", "{\"host\":\"ha\",\"port\":0,\"user\":\"u\"}", Verdict::BadPort);
  expect_refused("malformed beats bad host", "{\"host\":\"\",\"port\":0,}", Verdict::Malformed);
  expect_refused("malformed beats everything", "{\"host\":\"\",\"port\":0,\"user\":\"u\"}}", Verdict::Malformed);
  expect_refused("missing host with a bad port is bad host", "{\"port\":0}", Verdict::BadHost);
}

static void a_refusal_leaves_no_password_behind() {
  HubHandoff out;
  memset(&out, 0xA5, sizeof(out));
  // The pass is read into the record before the verdict (the user comes
  // after it here); the refusal must wipe it.
  const Verdict v = parse("{\"host\":\"ha\",\"pass\":\"s3cret\",\"user\":\"\"}", out);
  CHECK(v == Verdict::BadCredentials, "verdict %s", verdict_name(v));
  CHECK(all_zero(out), "wiped");

  // Ok fills the record; wipe() clears it.
  CHECK(parse("{\"host\":\"ha\",\"user\":\"u\",\"pass\":\"s3cret\"}", out) == Verdict::Ok, "ok");
  CHECK(out.pass[0] == 's', "pass present");
  wipe(out);
  CHECK(all_zero(out), "wipe() clears the whole record");

  // The raw wipe clears a buffer too.
  uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  wipe(buf, sizeof(buf));
  for (size_t i = 0; i < sizeof(buf); ++i) CHECK(buf[i] == 0, "byte %zu", i);
}

static void the_constants_are_pinned() {
  CHECK((uint8_t)Status::None == 0, "None is 0");
  CHECK((uint8_t)Status::Stored == 1, "Stored is 1");
  CHECK((uint8_t)Status::Rejected == 2, "Rejected is 2");
  CHECK((uint8_t)Status::LoginRefused == 3, "LoginRefused is 3");
  CHECK(std::strcmp(SERVICE_UUID, "8fc1cf00-b162-4401-9607-c8ac21383e90") == 0, "service UUID");
  CHECK(std::strcmp(HUB_UUID, "8fc1cf02-b162-4401-9607-c8ac21383e90") == 0, "HUB UUID");
  CHECK(MAX_PAYLOAD == 240, "payload bound");
  CHECK(HOST_MAX == 63, "host bound");
  CHECK(CREDENTIAL_MAX == 64, "credential bound");
  CHECK(DEFAULT_PORT == 1883, "default port");
  CHECK(sizeof(HubHandoff::host) == 64 && sizeof(HubHandoff::user) == 65 && sizeof(HubHandoff::pass) == 65,
        "buffers hold the bounds plus a NUL");
  CHECK(std::strcmp(verdict_name(Verdict::Ok), "ok") == 0, "ok name");
  CHECK(std::strcmp(verdict_name(Verdict::Malformed), "malformed") == 0, "malformed name");
  CHECK(std::strcmp(verdict_name(Verdict::TooLong), "too long") == 0, "too long name");
  CHECK(std::strcmp(verdict_name(Verdict::BadHost), "bad host") == 0, "bad host name");
  CHECK(std::strcmp(verdict_name(Verdict::BadPort), "bad port") == 0, "bad port name");
  CHECK(std::strcmp(verdict_name(Verdict::BadCredentials), "bad credentials") == 0, "bad credentials name");
}

int main() {
  the_documented_example_parses_field_for_field();
  optional_fields_take_their_defaults();
  an_ipv4_literal_is_a_host();
  a_hostname_is_held_to_the_dns_rule();
  a_port_is_a_plain_integer_in_range();
  user_and_pass_travel_together();
  the_three_escapes_are_unescaped_and_no_other();
  a_duplicate_key_is_malformed();
  the_shape_is_held_strictly();
  the_payload_bound_is_240_bytes();
  field_verdicts_come_in_order_after_the_shape();
  a_refusal_leaves_no_password_behind();
  the_constants_are_pinned();

  if (g_failures) {
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
  }
  std::printf("ALL hub_handoff TESTS PASSED\n");
  return 0;
}
