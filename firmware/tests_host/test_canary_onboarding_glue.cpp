// Host tests for the PIO canary's glue onto the fleet's shared onboarding
// headers — the two pure pieces that tree adds beside them:
//
//   firmware/canary/lib/securacv_network/src/wifi_join_reason.h
//     wl_status_t + the raw disconnect reason -> JoinFailure (the words
//     GET /api/wifi/status reports as `reason`), and the saved-network
//     rotation rule (NotFound twice, another network saved).
//   firmware/canary/include/reset_scope_keys.h
//     what the 10 s BOOT release clears, what it keeps — the identity and the
//     chain state must never be on the first list, and every spelling on it
//     must be one the owning source still uses (paths passed in by the
//     Makefile, so a renamed key fails here rather than silently surviving a
//     reset).

#include "wifi_join_reason.h"
#include "reset_scope_keys.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace canary::net;

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

static std::string read_file(const char* path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// ── wifi_join_reason::classify ─────────────────────────────────────────────

static void the_status_alone_decides_the_three_the_siblings_decide() {
  using namespace join_reason;
  CHECK(classify(kWlNoSsidAvail, kReasonNone) == JoinFailure::NotFound, "no SSID -> not found");
  CHECK(classify(kWlConnectFailed, kReasonNone) == JoinFailure::BadPassword, "connect failed -> wrong password");
  CHECK(classify(kWlIdleStatus, kReasonNone) == JoinFailure::NoAddress, "associated, no address -> no address");
  // The status wins over a stale reason: these are the display's words too.
  CHECK(classify(kWlNoSsidAvail, kReasonAuthFail) == JoinFailure::NotFound, "status first");
}

static void the_core_2_0_17_wrong_password_shapes_read_wrong_password() {
  using namespace join_reason;
  // WL_CONNECTION_LOST (5) / WL_DISCONNECTED (6) are where core 2.0.17 files a
  // handshake timeout; only the reason tells it from a dropped link.
  CHECK(classify(5, kReason4wayHandshakeTimeout) == JoinFailure::BadPassword, "4-way handshake timeout");
  CHECK(classify(5, kReasonHandshakeTimeout) == JoinFailure::BadPassword, "handshake timeout");
  CHECK(classify(6, kReasonAuthExpire) == JoinFailure::BadPassword, "auth expire");
  CHECK(classify(6, kReasonAuthFail) == JoinFailure::BadPassword, "auth fail on the first try");
  CHECK(classify(6, kReasonNoApFound) == JoinFailure::NotFound, "no AP found before the core relabels it");
}

static void anything_else_is_couldnt_connect() {
  using namespace join_reason;
  CHECK(classify(6, kReasonNone) == JoinFailure::Unknown, "disconnected, no reason");
  CHECK(classify(5, 200) == JoinFailure::Unknown, "beacon timeout is a dropped link, not a verdict");
  CHECK(classify(6, 203) == JoinFailure::Unknown, "assoc fail says nothing a human can fix");
  CHECK(!std::strcmp(join_failure_label(classify(6, kReasonNone)), "Couldn't connect"), "the label");
}

// ── wifi_join_reason::should_rotate ────────────────────────────────────────

static void rotation_needs_a_second_network_two_misses_and_not_found() {
  using namespace join_reason;
  wifi_store::Store s;
  wifi_store::init(s);
  CHECK(wifi_store::add(s, "home", "password1"), "add home");
  CHECK(!should_rotate(s, 0, JoinFailure::NotFound), "one saved network: nowhere to go");
  CHECK(wifi_store::add(s, "shop", "password2"), "add shop");
  CHECK(!should_rotate(s, 0, JoinFailure::NotFound), "no misses yet");
  wifi_store::note_result(s, 0, false);
  CHECK(!should_rotate(s, 0, JoinFailure::NotFound), "one miss: a slow AP keeps its turn");
  wifi_store::note_result(s, 0, false);
  CHECK(should_rotate(s, 0, JoinFailure::NotFound), "two misses: move on");
  CHECK(!should_rotate(s, 0, JoinFailure::BadPassword), "a wrong password is for a human");
  CHECK(!should_rotate(s, 0, JoinFailure::NoAddress), "a silent router is not another network's fault");
  CHECK(!should_rotate(s, 0, JoinFailure::Unknown), "nor is a mystery");
  CHECK(!should_rotate(s, -1, JoinFailure::NotFound), "a network not in the ring has no count");
  CHECK(!should_rotate(s, 2, JoinFailure::NotFound), "out of range");
  // And the pick the supervisor then takes is the slot after the failing one.
  CHECK(wifi_store::pick(s, nullptr, nullptr, 0, 0) == 1, "round-robin to the next saved network");
  CHECK(kRotateAfterFails == 2, "the brief's two attempts");
}

// ── reset_scope_keys ───────────────────────────────────────────────────────

static bool in_list(const char* const* list, size_t n, const char* key) {
  for (size_t i = 0; i < n; i++) if (!std::strcmp(list[i], key)) return true;
  return false;
}

static void the_identity_never_goes_with_credentials() {
  using namespace canary::reset_scope;
  for (size_t i = 0; i < kIdentityKeptCount; i++) {
    CHECK(!in_list(kCredentialsAndSettings, kCredentialsAndSettingsCount, kIdentityKept[i]),
          "%s is identity/chain state and must survive the 10 s reset", kIdentityKept[i]);
  }
  CHECK(in_list(kIdentityKept, kIdentityKeptCount, NVS_KEY_PRIV), "the key itself is on the kept list");
  CHECK(in_list(kIdentityKept, kIdentityKeptCount, NVS_KEY_CHAINST), "and the chain state");
  // The brief's list, by name.
  CHECK(in_list(kCredentialsAndSettings, kCredentialsAndSettingsCount, NVS_KEY_WIFI_SSID), "wifi_ssid");
  CHECK(in_list(kCredentialsAndSettings, kCredentialsAndSettingsCount, NVS_KEY_WIFI_PASS), "wifi_pass");
  CHECK(in_list(kCredentialsAndSettings, kCredentialsAndSettingsCount, wifi_store::NVS_KEY), "the wifi_nets ring");
  CHECK(in_list(kCredentialsAndSettings, kCredentialsAndSettingsCount, NVS_KEY_SETUP_OK), "setup_ok");
  CHECK(in_list(kCredentialsAndSettings, kCredentialsAndSettingsCount, NVS_KEY_DEV_NAME), "the device name");
  CHECK(in_list(kCredentialsAndSettings, kCredentialsAndSettingsCount, NVS_KEY_TOKEN), "the bearer token");
  CHECK(in_list(kCredentialsAndSettings, kCredentialsAndSettingsCount, "mqtt_host"), "the broker");
  // No key twice: a duplicate would count a removal twice in the serial line.
  for (size_t i = 0; i < kCredentialsAndSettingsCount; i++)
    for (size_t j = i + 1; j < kCredentialsAndSettingsCount; j++)
      CHECK(std::strcmp(kCredentialsAndSettings[i], kCredentialsAndSettings[j]) != 0,
            "%s listed twice", kCredentialsAndSettings[i]);
  // NVS keys are at most 15 bytes.
  for (size_t i = 0; i < kCredentialsAndSettingsCount; i++)
    CHECK(std::strlen(kCredentialsAndSettings[i]) <= 15, "%s is not an NVS key", kCredentialsAndSettings[i]);
}

// Every spelling on the clear list is still spelled by a source that owns it:
// canary_config.h (the macros), securacv_mqtt.cpp (NVS_KEY_MQTT_*),
// common/network/mqtt_transport.h (the TLS trio), securacv_setup.cpp (tz).
static void every_cleared_key_is_spelled_by_its_owner(const char* const* owners, size_t n_owners) {
  using namespace canary::reset_scope;
  std::string all;
  for (size_t i = 0; i < n_owners; i++) {
    const std::string s = read_file(owners[i]);
    CHECK(!s.empty(), "could not read %s", owners[i]);
    all += s;
    all += '\n';
  }
  for (size_t i = 0; i < kCredentialsAndSettingsCount; i++) {
    const std::string quoted = std::string("\"") + kCredentialsAndSettings[i] + "\"";
    CHECK(all.find(quoted) != std::string::npos,
          "%s: no owning source spells this key any more", kCredentialsAndSettings[i]);
  }
}

int main(int argc, char** argv) {
  the_status_alone_decides_the_three_the_siblings_decide();
  the_core_2_0_17_wrong_password_shapes_read_wrong_password();
  anything_else_is_couldnt_connect();
  rotation_needs_a_second_network_two_misses_and_not_found();
  the_identity_never_goes_with_credentials();
  if (argc > 1) {
    every_cleared_key_is_spelled_by_its_owner(argv + 1, (size_t)(argc - 1));
  } else {
    std::printf("FAIL: pass the owning sources (canary_config.h, securacv_mqtt.cpp, mqtt_transport.h, securacv_setup.cpp)\n");
    ++g_failures;
  }
  if (g_failures) {
    std::printf("test_canary_onboarding_glue: %d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("test_canary_onboarding_glue: all checks passed\n");
  return 0;
}
