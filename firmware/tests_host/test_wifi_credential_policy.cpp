// Host tests for firmware/common/network/wifi_credential_policy.h — the one
// rule for the home Wi-Fi password a Canary is handed, and the one set of
// words beside the field.
//
// The headline case is `a_short_password_is_the_routers_business`: three
// clients used to refuse a 1..7 byte password on the router's behalf, which
// turned real WEP / WPA3 / short-PSK networks into "cannot be typed" and
// taught people the field wanted something new. The firmware never had that
// floor; now nothing may add one.
//
// The vectors below are the ones the JavaScript, Rust and Swift twins pin.

#include "../common/network/wifi_credential_policy.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace canary::net::wifi_credential;

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

static void the_bounds_are_the_firmware_buffers() {
  CHECK(SSID_MAX_BYTES == 32, "ssid buffer is char[33]");
  CHECK(PASSWORD_MAX_BYTES == 64, "password buffer is char[65]");
  CHECK(PASSWORD_MIN_BYTES == 0, "there is no floor");
}

static void a_short_password_is_the_routers_business() {
  // 1..7 bytes: accepted. The router decides; the join verdict reports.
  CHECK(check("Home", "a") == Verdict::Ok, "1 byte");
  CHECK(check("Home", "1234567") == Verdict::Ok, "7 bytes (would fail WPA2, fine for WEP/SAE)");
  CHECK(check("Home", "hunter22") == Verdict::Ok, "8 bytes");
  // 63 = the longest WPA2 passphrase; 64 = a raw hex PSK.
  CHECK(check("Home", std::string(63, 'p').c_str()) == Verdict::Ok, "63 bytes");
  CHECK(check("Home", std::string(64, 'f').c_str()) == Verdict::Ok, "64 bytes (hex PSK)");
  CHECK(check("Home", std::string(65, 'p').c_str()) == Verdict::PasswordTooLong, "65 bytes refused");
}

static void an_open_network_has_no_password() {
  CHECK(check("Cafe", "") == Verdict::Ok, "empty password is an open network");
  CHECK(check("Cafe", nullptr) == Verdict::Ok, "null password is an open network");
  CHECK(check("Cafe", 4, nullptr, 0) == Verdict::Ok, "explicit zero length");
}

static void the_ssid_bounds_hold() {
  CHECK(check("", "pw") == Verdict::SsidEmpty, "empty ssid");
  CHECK(check(nullptr, "pw") == Verdict::SsidEmpty, "null ssid");
  CHECK(check(std::string(32, 's').c_str(), "pw") == Verdict::Ok, "32 bytes");
  CHECK(check(std::string(33, 's').c_str(), "pw") == Verdict::SsidTooLong, "33 bytes refused");
  // A space is part of the name, not trimmed.
  CHECK(check(" Home ", "pw") == Verdict::Ok, "spaces kept");
  CHECK(check("Home", " pw ") == Verdict::Ok, "password spaces kept");
}

static void the_ssid_is_judged_before_the_password() {
  CHECK(check("", std::string(65, 'p').c_str()) == Verdict::SsidEmpty, "ssid first");
  CHECK(check(std::string(33, 's').c_str(), std::string(65, 'p').c_str()) == Verdict::SsidTooLong, "ssid first");
}

static void every_verdict_has_a_code_and_words() {
  const Verdict all[] = {Verdict::Ok, Verdict::SsidEmpty, Verdict::SsidTooLong, Verdict::PasswordTooLong};
  for (Verdict v : all) {
    CHECK(verdict_code(v)[0] != '\0', "code non-empty");
    if (v != Verdict::Ok) CHECK(verdict_label(v)[0] != '\0', "label non-empty for %s", verdict_code(v));
  }
  CHECK(std::strcmp(verdict_code(Verdict::Ok), "ok") == 0, "ok code");
  CHECK(std::strcmp(verdict_code(Verdict::SsidEmpty), "ssid_empty") == 0, "ssid_empty code");
  CHECK(std::strcmp(verdict_code(Verdict::SsidTooLong), "ssid_too_long") == 0, "ssid_too_long code");
  CHECK(std::strcmp(verdict_code(Verdict::PasswordTooLong), "password_too_long") == 0, "password_too_long code");
  CHECK(std::strcmp(verdict_label(Verdict::PasswordTooLong), "A Wi-Fi password is at most 64 characters.") == 0,
        "the too-long sentence is the one the apps show");
}

static void the_words_say_whose_password_it_is() {
  // The label names the network, so it cannot read as "create a password".
  CHECK(std::strstr(FIELD_LABEL, "network") != nullptr, "label names the network");
  CHECK(std::strcmp(FIELD_LABEL, "Wi-Fi network password") == 0, "the label every surface shows");
  CHECK(std::strstr(FIELD_HINT, "already use") != nullptr, "hint says it exists already");
  CHECK(std::strstr(FIELD_HINT, "Nothing to invent") != nullptr, "hint rules out inventing one");
  // No surface word may ask the person to make one up.
  const char* words[] = {FIELD_LABEL, FIELD_HINT, OPEN_NETWORK_NOTE,
                         verdict_label(Verdict::SsidEmpty), verdict_label(Verdict::SsidTooLong),
                         verdict_label(Verdict::PasswordTooLong)};
  for (const char* w : words) {
    CHECK(std::strstr(w, "create") == nullptr && std::strstr(w, "Create") == nullptr, "no 'create': %s", w);
    CHECK(std::strstr(w, "choose") == nullptr && std::strstr(w, "Choose") == nullptr, "no 'choose': %s", w);
    CHECK(std::strstr(w, "new password") == nullptr, "no 'new password': %s", w);
    CHECK(std::strstr(w, "at least") == nullptr, "no minimum in any sentence: %s", w);
  }
  CHECK(std::strstr(OPEN_NETWORK_NOTE, "no password") != nullptr, "open note says no password");
}

int main() {
  the_bounds_are_the_firmware_buffers();
  a_short_password_is_the_routers_business();
  an_open_network_has_no_password();
  the_ssid_bounds_hold();
  the_ssid_is_judged_before_the_password();
  every_verdict_has_a_code_and_words();
  the_words_say_whose_password_it_is();
  if (g_failures == 0) {
    std::printf("ALL wifi_credential_policy TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_failures);
  return 1;
}
