// Host tests for common/csi/src/csi_event_wire.h — the one builder of the
// MQTT `events` body both the canary-wap sketch and the canary PIO tree
// publish. Byte-exact goldens: the HA integration's verify_event rebuilds
// the signed canonical from these exact fields
// (custom_components/securacv/tests/test_signature.py verifies a body of
// this shape against a real Ed25519 key), so any change here is a change to
// what Home Assistant parses and trusts.
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cstdio>
#include <cstring>

#include "csi_event_wire.h"

using csi_event_wire::Signer;
using csi_event_wire::build_event_body;
using csi_event_wire::build_tamper_bridge_body;

static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

#define CHECK_STR(got, want)                                            \
  do {                                                                  \
    if (std::strcmp((got), (want)) != 0) {                              \
      std::fprintf(stderr, "FAIL %s:%d:\n  got  %s\n  want %s\n",        \
                   __FILE__, __LINE__, (got), (want));                  \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

// A fake signer: records the canonical tuple it was handed (the fields HA's
// verify_event rebuilds) and writes a fixed 86-char b64url "signature".
static char g_seen[160];
static bool g_sign_ok = true;
static const char kFakeSig[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    "ABCDEFGHIJKLMNOPQRSTUV";  // 86 chars
static bool fake_sign(uint32_t event_id, const char* state, const char* cat,
                      const char* priv, int motion, int breath, int bpm,
                      char* out, size_t cap) {
  std::snprintf(g_seen, sizeof(g_seen), "%u|%s|%s|%s|%d|%d|%d",
                (unsigned)event_id, state, cat, priv, motion, breath, bpm);
  if (!g_sign_ok || cap < sizeof(kFakeSig)) return false;
  std::memcpy(out, kFakeSig, sizeof(kFakeSig));
  return true;
}

static csi_event_values_t presence_values() {
  csi_event_values_t v;
  std::memset(&v, 0, sizeof(v));
  v.category = CSI_CATEGORY_EVENT;
  std::strcpy(v.state_name, "present");
  std::strcpy(v.confidence, "likely");
  v.motion_score = 42;
  v.breathing_score = 17;
  v.breathing_rate_bpm = 14;
  v.duration_sec = 120;
  return v;
}

static int test_unsigned_golden() {
  const Signer none = {nullptr, 1, "ed25519", "0123456789abcdef"};
  const csi_event_values_t v = presence_values();
  char body[768];
  const size_t n = build_event_body(body, sizeof(body), 1234, "core.presence",
                                    "presence", CSI_CATEGORY_EVENT, CSI_PRIVACY_P1,
                                    &v, 98765, 3, false, none);
  CHECK(n == std::strlen(body));
  // No signature rides the body, so `signed` must say false.
  CHECK_STR(body,
      "{\"event_id\":1234,\"event_type\":\"present\",\"timestamp\":98,\"zone\":\"\","
      "\"confidence\":\"likely\",\"signed\":false,\"module\":\"core.presence\","
      "\"type\":\"presence\",\"category\":\"event\",\"privacy\":\"p1\","
      "\"state\":\"present\",\"motion\":42,\"breathing\":17,\"bpm\":14,"
      "\"duration_sec\":120,\"bundled\":3,\"replay\":false,\"v\":1}");
  return 0;
}

static int test_signed_golden_and_canonical_tuple() {
  g_sign_ok = true;
  const Signer s = {&fake_sign, 1, "ed25519", "0123456789abcdef"};
  const csi_event_values_t v = presence_values();
  char body[768];
  const size_t n = build_event_body(body, sizeof(body), 1234, "core.presence",
                                    "presence", CSI_CATEGORY_EVENT, CSI_PRIVACY_P1,
                                    &v, 98765, 3, false, s);
  CHECK(n == std::strlen(body));
  // The signer saw exactly the canonical fields HA rebuilds.
  CHECK_STR(g_seen, "1234|present|event|p1|42|17|14");
  CHECK_STR(body,
      "{\"event_id\":1234,\"event_type\":\"present\",\"timestamp\":98,\"zone\":\"\","
      "\"confidence\":\"likely\",\"signed\":true,\"module\":\"core.presence\","
      "\"type\":\"presence\",\"category\":\"event\",\"privacy\":\"p1\","
      "\"state\":\"present\",\"motion\":42,\"breathing\":17,\"bpm\":14,"
      "\"duration_sec\":120,\"bundled\":3,\"replay\":false,\"v\":1,"
      "\"alg\":\"ed25519\",\"fp\":\"0123456789abcdef\","
      "\"sig\":\"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
      "ABCDEFGHIJKLMNOPQRSTUV\"}");
  return 0;
}

static int test_system_integrity_row_golden() {
  // The tamper row a canary or canary-wap commits (tamper_events_module):
  // state = the kind, category anomaly, privacy p0, no scores, no
  // confidence (the builder's "tentative" default), sealed as one row.
  csi_event_values_t v;
  std::memset(&v, 0, sizeof(v));
  v.category = CSI_CATEGORY_ANOMALY;
  std::strcpy(v.state_name, "sd_remove");
  g_sign_ok = true;
  const Signer s = {&fake_sign, 1, "ed25519", "fedcba9876543210"};
  char body[768];
  CHECK(build_event_body(body, sizeof(body), 7, "system.integrity", "tamper",
                         CSI_CATEGORY_ANOMALY, CSI_PRIVACY_P0, &v, 5000, 1,
                         false, s) > 0);
  CHECK_STR(g_seen, "7|sd_remove|anomaly|p0|0|0|0");
  CHECK_STR(body,
      "{\"event_id\":7,\"event_type\":\"sd_remove\",\"timestamp\":5,\"zone\":\"\","
      "\"confidence\":\"tentative\",\"signed\":true,\"module\":\"system.integrity\","
      "\"type\":\"tamper\",\"category\":\"anomaly\",\"privacy\":\"p0\","
      "\"state\":\"sd_remove\",\"motion\":0,\"breathing\":0,\"bpm\":0,"
      "\"duration_sec\":0,\"bundled\":1,\"replay\":false,\"v\":1,"
      "\"alg\":\"ed25519\",\"fp\":\"fedcba9876543210\","
      "\"sig\":\"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
      "ABCDEFGHIJKLMNOPQRSTUV\"}");
  // And the tamper-topic bridge body for the same row.
  char tb[128];
  CHECK(build_tamper_bridge_body(tb, sizeof(tb), "system.integrity", "tamper", &v) > 0);
  CHECK_STR(tb, "{\"type\":\"sd_remove\",\"severity\":\"tamper\"}");
  return 0;
}

static int test_failed_signing_is_unsigned_not_a_claim() {
  g_sign_ok = false;
  const Signer s = {&fake_sign, 1, "ed25519", "0123456789abcdef"};
  const csi_event_values_t v = presence_values();
  char body[768];
  CHECK(build_event_body(body, sizeof(body), 9, "core.presence", "presence",
                         CSI_CATEGORY_EVENT, CSI_PRIVACY_P0, &v, 0, 1, false, s) > 0);
  CHECK(std::strstr(body, "\"signed\":false") != nullptr);
  CHECK(std::strstr(body, "\"sig\"") == nullptr);
  CHECK(std::strstr(body, "\"fp\"") == nullptr);
  const size_t len = std::strlen(body);
  CHECK(len > 7 && std::strcmp(body + len - 7, ",\"v\":1}") == 0);
  g_sign_ok = true;
  return 0;
}

static int test_replay_flag_and_defaults() {
  const Signer none = {nullptr, 1, "ed25519", nullptr};
  csi_event_values_t v;
  std::memset(&v, 0, sizeof(v));  // no state, no confidence
  char body[768];
  CHECK(build_event_body(body, sizeof(body), 1, nullptr, nullptr,
                         CSI_CATEGORY_AMBIENT, CSI_PRIVACY_P2, &v, 1999, 0,
                         true, none) > 0);
  CHECK(std::strstr(body, "\"replay\":true") != nullptr);
  CHECK(std::strstr(body, "\"state\":\"unknown\"") != nullptr);
  CHECK(std::strstr(body, "\"event_type\":\"unknown\"") != nullptr);
  CHECK(std::strstr(body, "\"confidence\":\"tentative\"") != nullptr);
  CHECK(std::strstr(body, "\"module\":\"\",\"type\":\"\"") != nullptr);
  CHECK(std::strstr(body, "\"category\":\"ambient\",\"privacy\":\"p2\"") != nullptr);
  CHECK(std::strstr(body, "\"timestamp\":1,") != nullptr);
  return 0;
}

static int test_overflow_returns_zero_never_a_clipped_body() {
  const Signer s = {&fake_sign, 1, "ed25519", "0123456789abcdef"};
  const csi_event_values_t v = presence_values();
  char body[768];
  const size_t full = build_event_body(body, sizeof(body), 1234, "core.presence",
                                       "presence", CSI_CATEGORY_EVENT,
                                       CSI_PRIVACY_P1, &v, 98765, 3, false, s);
  CHECK(full > 0);
  char small[768];
  CHECK(build_event_body(small, full, 1234, "core.presence", "presence",
                         CSI_CATEGORY_EVENT, CSI_PRIVACY_P1, &v, 98765, 3,
                         false, s) == 0);   // one byte short of the NUL
  CHECK(build_event_body(small, full + 1, 1234, "core.presence", "presence",
                         CSI_CATEGORY_EVENT, CSI_PRIVACY_P1, &v, 98765, 3,
                         false, s) == full);
  CHECK(build_event_body(small, 16, 1, "m", "t", CSI_CATEGORY_EVENT,
                         CSI_PRIVACY_P0, &v, 0, 0, false, s) == 0);
  CHECK(build_event_body(nullptr, 768, 1, "m", "t", CSI_CATEGORY_EVENT,
                         CSI_PRIVACY_P0, &v, 0, 0, false, s) == 0);
  CHECK(build_event_body(small, 768, 1, "m", "t", CSI_CATEGORY_EVENT,
                         CSI_PRIVACY_P0, nullptr, 0, 0, false, s) == 0);
  return 0;
}

static int test_worst_case_fits_the_canary_offline_slot() {
  // Every field at its widest. The canary buffers events across a broker
  // outage in slots of MQTT_OFFLINE_SLOT_BYTES (512 by default,
  // canary/lib/securacv_mqtt/src/securacv_mqtt.cpp), and a body over a slot
  // is refused there, not truncated — so the widest signed body must fit
  // one. Lengthening the wire (or shrinking the slot) goes red HERE.
  csi_event_values_t v;
  std::memset(&v, 0, sizeof(v));
  std::memset(v.state_name, 'x', sizeof(v.state_name) - 1);
  std::memset(v.confidence, 'y', sizeof(v.confidence) - 1);
  v.motion_score = 255;
  v.breathing_score = 255;
  v.breathing_rate_bpm = 255;
  v.duration_sec = 65535;
  char mod[CSI_EVENT_NAME_MAX];
  char typ[CSI_EVENT_NAME_MAX];
  std::memset(mod, 'm', sizeof(mod) - 1);
  mod[sizeof(mod) - 1] = '\0';
  std::memset(typ, 't', sizeof(typ) - 1);
  typ[sizeof(typ) - 1] = '\0';
  const Signer s = {&fake_sign, 1, "ed25519", "0123456789abcdef"};
  char body[768];
  const size_t n = build_event_body(body, sizeof(body), 0xFFFFFFFFu, mod, typ,
                                    CSI_CATEGORY_ANOMALY, CSI_PRIVACY_P2, &v,
                                    0xFFFFFFFFu, 65535, false, s);
  CHECK(n > 0);
  std::printf("test_csi_event_wire: worst-case signed body = %zu bytes\n", n);
  CHECK(n <= 512);
  // The WAP's own publish buffer (csi_mqtt.cpp) is 768.
  CHECK(n < 768);
  return 0;
}

static int test_tamper_bridge_only_for_integrity_rows() {
  csi_event_values_t v;
  std::memset(&v, 0, sizeof(v));
  std::strcpy(v.state_name, "enclosure");
  char tb[128];
  CHECK(build_tamper_bridge_body(tb, sizeof(tb), "system.integrity", "tamper", &v) > 0);
  CHECK_STR(tb, "{\"type\":\"enclosure\",\"severity\":\"tamper\"}");
  CHECK(build_tamper_bridge_body(tb, sizeof(tb), "core.presence", "tamper", &v) == 0);
  CHECK(build_tamper_bridge_body(tb, sizeof(tb), "system.integrity", "other", &v) == 0);
  CHECK(build_tamper_bridge_body(tb, sizeof(tb), "system.integrityX", "tamper", &v) == 0);
  CHECK(build_tamper_bridge_body(tb, 10, "system.integrity", "tamper", &v) == 0);
  v.state_name[0] = '\0';
  CHECK(build_tamper_bridge_body(tb, sizeof(tb), "system.integrity", "tamper", &v) == 0);
  return 0;
}

int main() {
  if (test_unsigned_golden()) return 1;
  if (test_signed_golden_and_canonical_tuple()) return 1;
  if (test_system_integrity_row_golden()) return 1;
  if (test_failed_signing_is_unsigned_not_a_claim()) return 1;
  if (test_replay_flag_and_defaults()) return 1;
  if (test_overflow_returns_zero_never_a_clipped_body()) return 1;
  if (test_worst_case_fits_the_canary_offline_slot()) return 1;
  if (test_tamper_bridge_only_for_integrity_rows()) return 1;
  std::printf("test_csi_event_wire: %d checks passed\n", g_checks);
  return 0;
}
