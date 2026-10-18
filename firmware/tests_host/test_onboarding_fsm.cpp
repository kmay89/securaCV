// Host tests for firmware/common/network/onboarding_fsm.h — the fleet's one
// onboarding state machine: the five state words, the AP hold after a join,
// the recovery re-raise, the setup AP's name and the serial label lines.

#include "../common/network/onboarding_fsm.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace canary::net::onboard;

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

static void the_five_states_have_their_words() {
  CHECK(!std::strcmp(state_label(State::Unprovisioned), "unprovisioned"), "unprovisioned");
  CHECK(!std::strcmp(state_label(State::Provisioning), "provisioning"), "provisioning");
  CHECK(!std::strcmp(state_label(State::Joining), "joining"), "joining");
  CHECK(!std::strcmp(state_label(State::Provisioned), "provisioned"), "provisioned");
  CHECK(!std::strcmp(state_label(State::Recovery), "recovery"), "recovery");
  CHECK(!std::strcmp(state_label((State)9), "unknown"), "an unknown byte reads unknown");
}

static void the_state_is_derived_from_facts() {
  Facts f{};
  CHECK(derive(f) == State::Unprovisioned, "box state");
  f.ap_up = true;
  CHECK(derive(f) == State::Unprovisioned, "AP up, nobody has typed yet");
  f.credentials_submitted = true;
  CHECK(derive(f) == State::Provisioning, "a phone handed credentials");
  f.join_in_flight = true;
  CHECK(derive(f) == State::Joining, "testing them");
  f.sta_online = true;
  CHECK(derive(f) == State::Provisioned, "online wins even with the AP lingering");

  Facts saved{};
  saved.have_credentials = true;
  CHECK(derive(saved) == State::Joining, "saved network, between retries: joining");
  saved.join_in_flight = true;
  CHECK(derive(saved) == State::Joining, "saved network, retry in flight");
  saved.join_in_flight = false;
  saved.ap_up = true;
  CHECK(derive(saved) == State::Recovery, "saved network unreachable, AP re-raised");
  saved.sta_online = true;
  CHECK(derive(saved) == State::Provisioned, "back online");
}

static void the_ap_holds_two_minutes_unless_the_phone_confirmed() {
  Timing t;
  CHECK(t.ap_hold_after_join_ms == 120000, "the brief's two minutes");
  CHECK(t.ap_hold_after_ack_ms == 1600, "the bench-tuned beat after an ack");
  const uint32_t joined = 1000;
  CHECK(!ap_teardown_due(joined + 119999, joined, false, 0, t), "119.999 s, no ack: hold");
  CHECK(ap_teardown_due(joined + 120001, joined, false, 0, t), "past two minutes: down");
  // The phone acked at +5 s: the beat, then down — long before the cap.
  const uint32_t acked = joined + 5000;
  CHECK(!ap_teardown_due(acked + 1600, joined, true, acked, t), "exactly the beat: not yet");
  CHECK(ap_teardown_due(acked + 1601, joined, true, acked, t), "past the beat: down");
  // Wrap-safe: a join just before the 32-bit millis wrap.
  const uint32_t late = 0xFFFFFF00u;
  CHECK(!ap_teardown_due(late + 60000, late, false, 0, t), "across the wrap, still holding");
  CHECK(ap_teardown_due(late + 120001, late, false, 0, t), "across the wrap, due");
}

static void recovery_reraises_after_ten_minutes_without_wiping() {
  Timing t;
  CHECK(t.recovery_reraise_ms == 600000, "ten minutes");
  const uint32_t lost = 5000;
  CHECK(!recovery_reraise_due(lost + 700000, false, false, false, true, lost, t), "no saved network: nothing to recover");
  CHECK(!recovery_reraise_due(lost + 700000, true, true, false, true, lost, t), "online: no");
  CHECK(!recovery_reraise_due(lost + 700000, true, false, true, true, lost, t), "AP already up: no");
  CHECK(!recovery_reraise_due(lost + 599999, true, false, false, true, lost, t), "9:59.999: wait");
  CHECK(recovery_reraise_due(lost + 600000, true, false, false, true, lost, t), "10:00: raise");
  CHECK(recovery_reraise_due(lost + 600000, true, false, false, false, lost, t),
        "a never-online unit failing for an unfixable reason gets the same ten minutes");
}

static void the_factory_reset_hold_is_ten_seconds() {
  Timing t;
  CHECK(t.factory_reset_hold_ms == 10000, "ten seconds");
}

static void the_setup_ap_is_named_by_type_and_a_mac_free_suffix() {
  char out[SSID_CAP];
  CHECK(ap_ssid(out, sizeof(out), NODE_SENSE, "4dC2", nullptr) == 19, "len");
  CHECK(!std::strcmp(out, "SecuraCV-sense-4dC2"), "got %s", out);
  CHECK(ap_ssid(out, sizeof(out), NODE_SENTINEL, "4dC2xyz", "k7") > 0, "tagged");
  CHECK(!std::strcmp(out, "SecuraCV-sentinel-4dC2-k7"), "longest shape: %s", out);
  CHECK(std::strlen(out) <= 32, "fits an SSID");
  CHECK(ap_ssid(out, sizeof(out), nullptr, "4dC2", nullptr) > 0, "legacy shape");
  CHECK(!std::strcmp(out, "SecuraCV-4dC2"), "got %s", out);
  CHECK(ap_ssid(out, sizeof(out), "", "4dC2", "k7") > 0, "legacy tagged");
  CHECK(!std::strcmp(out, "SecuraCV-4dC2-k7"), "got %s", out);
  CHECK(ap_ssid(out, sizeof(out), NODE_DISPLAY, "", nullptr) > 0, "empty suffix");
  CHECK(!std::strcmp(out, "SecuraCV-display-CNRY"), "falls back to CNRY: %s", out);
  CHECK(ap_ssid(out, sizeof(out), NODE_WAP, "ab", nullptr) > 0, "short suffix");
  CHECK(!std::strcmp(out, "SecuraCV-wap-abXX"), "padded: %s", out);
  CHECK(ap_ssid(out, sizeof(out), "averyverylongtype", "4dC2", nullptr) > 0, "overlong type");
  CHECK(!std::strcmp(out, "SecuraCV-4dC2"), "an overlong type is dropped, not truncated: %s", out);
  char tiny[12];
  CHECK(ap_ssid(tiny, sizeof(tiny), NODE_SENSE, "4dC2", nullptr) == 0, "too small: 0");
  CHECK(tiny[0] == '\0', "and empty");
  CHECK(ap_ssid(nullptr, 0, NODE_SENSE, "4dC2", nullptr) == 0, "null out");
}

static void the_node_type_comes_off_the_device_type() {
  CHECK(!std::strcmp(node_type_from_device_type("canary-sense"), NODE_SENSE), "sense");
  CHECK(!std::strcmp(node_type_from_device_type("canary-sentinel"), NODE_SENTINEL), "sentinel");
  CHECK(!std::strcmp(node_type_from_device_type("canary-vision"), NODE_VISION), "vision");
  CHECK(!std::strcmp(node_type_from_device_type("wap"), NODE_WAP), "a bare word passes through");
  CHECK(!std::strcmp(node_type_from_device_type("canary-"), "canary-"), "a bare prefix is not a type");
  CHECK(node_type_from_device_type(nullptr) == nullptr, "null");
}

static void the_matcher_accepts_every_shape_ever_emitted() {
  CHECK(is_setup_ssid("SecuraCV-4dC2"), "legacy");
  CHECK(is_setup_ssid("SecuraCV-4dC2-k7"), "legacy tagged");
  CHECK(is_setup_ssid("SecuraCV-sense-4dC2"), "typed");
  CHECK(is_setup_ssid("SecuraCV-sentinel-4dC2-k7"), "typed tagged");
  CHECK(is_setup_ssid("SecuraCV-display-CNRY"), "fallback suffix");
  CHECK(!is_setup_ssid("SecuraCV-4dC"), "three chars");
  CHECK(!is_setup_ssid("SecuraCV-4dC2x"), "five chars");
  CHECK(!is_setup_ssid("SecuraCV-4dC2-"), "dangling dash");
  CHECK(!is_setup_ssid("SecuraCV-4dC2-k"), "one-char tag");
  CHECK(!is_setup_ssid("SecuraCV-Sense-4dC2"), "a type is lowercase");
  CHECK(!is_setup_ssid("SecuraCV-s3nse-4dC2"), "a type has no digits");
  CHECK(!is_setup_ssid("SecuraCV-vision-4dC2-k7x"), "three-char tag");
  CHECK(!is_setup_ssid("SecuraCV-a-b-c-d"), "four parts");
  CHECK(!is_setup_ssid("securacv-4dC2"), "case matters in the prefix");
  CHECK(!is_setup_ssid("MyWiFi"), "not ours");
  CHECK(!is_setup_ssid(nullptr), "null");
  CHECK(!is_setup_ssid("SecuraCV-"), "bare prefix");
  CHECK(!is_setup_ssid("SecuraCV-4d C2"), "a space is not alphanumeric");
}

static void the_serial_lines_are_what_the_flashers_parse() {
  char line[160];
  const int n = announce_line(line, sizeof(line), "SecuraCV-sense-4dC2", "a7Kq2pMx", 1);
  CHECK(n > 0, "wrote");
  CHECK(!std::strcmp(line, "[SETUP] Setup network up: \"SecuraCV-sense-4dC2\"  password a7Kq2pMx  (channel 1)"),
        "flash-core.js SETUP_NETWORK_LINE_RES[0] shape: %s", line);
  // The cap is a runtime value so the compiler cannot prove the truncation
  // it is this test's job to provoke.
  volatile size_t tiny_cap = 20;
  char tiny[64];
  CHECK(announce_line(tiny, tiny_cap, "SecuraCV-sense-4dC2", "a7Kq2pMx", 1) == 0, "no truncated label");
  CHECK(tiny[0] == '\0', "empty on overflow");

  char qr[200];
  CHECK(qr_line(qr, sizeof(qr), "SecuraCV-sense-4dC2", "a7Kq2pMx") > 0, "qr wrote");
  CHECK(!std::strcmp(qr, "[SETUP] QR WIFI:T:WPA;S:SecuraCV-sense-4dC2;P:a7Kq2pMx;;"), "qr: %s", qr);
  CHECK(qr_line(qr, sizeof(qr), "Open Cafe", "") > 0, "open qr");
  CHECK(!std::strcmp(qr, "[SETUP] QR WIFI:T:nopass;S:Open Cafe;;"), "open qr: %s", qr);
  CHECK(qr_line(qr, sizeof(qr), "Bob's;Cafe", "p:w") > 0, "escaped qr");
  CHECK(!std::strcmp(qr, "[SETUP] QR WIFI:T:WPA;S:Bob's\\;Cafe;P:p\\:w;;"), "escaped: %s", qr);
  char small[16];
  CHECK(qr_line(small, sizeof(small), "SecuraCV-sense-4dC2", "a7Kq2pMx") == 0, "no half QR");
  CHECK(small[0] == '\0', "empty");
}

int main() {
  the_five_states_have_their_words();
  the_state_is_derived_from_facts();
  the_ap_holds_two_minutes_unless_the_phone_confirmed();
  recovery_reraises_after_ten_minutes_without_wiping();
  the_factory_reset_hold_is_ten_seconds();
  the_setup_ap_is_named_by_type_and_a_mac_free_suffix();
  the_node_type_comes_off_the_device_type();
  the_matcher_accepts_every_shape_ever_emitted();
  the_serial_lines_are_what_the_flashers_parse();
  if (g_failures) {
    std::printf("test_onboarding_fsm: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_onboarding_fsm: all passed\n");
  return 0;
}
