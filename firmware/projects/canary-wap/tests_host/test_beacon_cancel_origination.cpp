// Host test for Beacon CANCEL origination (spec/beacon_channel_v0.md §10
// /api/beacon/cancel, §6.2 step 4 for the solo path, §5.4 for the reference).
//
// Unlike test_beacon_origination.cpp — which mirrors the receive path with
// re-declared constants — this test includes the REAL beacon_wire.h and
// beacon_cancel_policy.h the firmware compiles, so a moved field or a changed
// decision fails here first. It pins:
//   - reason -> all-clear template (0x80 / 0x81 / 0x82), unknown refused
//   - the canonical a CANCEL signs (msg_type, CAP defaults, scope, reference,
//     TTL, solo forcing certainty = Observed and one fingerprint for both)
//   - the refusal order the REST layer names reasons from
//   - what a cosigner agrees to be asked to sign
//   - the emitted header following the signed canonical (the frame used to be
//     emitted with msg_type hardcoded to ALERT, which every receiver drops
//     for a CANCEL canonical) and passing the receive path's header rules
//   - the originator adopting its own frame (raise on ALERT, clear on a
//     CANCEL that names the alarm), with no rate input by construction
//   - the wire struct sizes, so a moved struct cannot silently change size.
//
/* Build & run (the canary-wap tests_host Makefile's `run` target does this):
 *
 *   g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_beacon_cancel_origination.cpp \
 *       -o /tmp/test_beacon_cancel_origination && /tmp/test_beacon_cancel_origination
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "beacon_wire.h"
#include "beacon_cancel_policy.h"

using namespace beacon_channel;
using namespace beacon_cancel_policy;

namespace {

int failures = 0;
int checks = 0;
#define EXPECT(cond, msg) do { \
    checks++; \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
  } while (0)

constexpr uint64_t NOW = 1800000000ULL;  // a synced wall clock

void fill(uint8_t* p, uint8_t v, size_t n) { std::memset(p, v, n); }

// ── Reason -> template ──────────────────────────────────────────────────────

void test_reason_maps_to_all_clear_template() {
  CancelReason r = CANCEL_REASON_SAFE;
  EXPECT(parse_cancel_reason("resolved", &r) && r == CANCEL_REASON_RESOLVED, "resolved parses");
  EXPECT(cancel_template_for(r) == BCN_CLR_RESOLVED && BCN_CLR_RESOLVED == 0x80,
         "resolved -> BCN_CLR_RESOLVED (0x80)");
  EXPECT(parse_cancel_reason("safe", &r) && r == CANCEL_REASON_SAFE, "safe parses");
  EXPECT(cancel_template_for(r) == BCN_CLR_SAFE && BCN_CLR_SAFE == 0x81,
         "safe -> BCN_CLR_SAFE (0x81)");
  EXPECT(parse_cancel_reason("false_alarm", &r) && r == CANCEL_REASON_FALSE_ALARM,
         "false_alarm parses");
  EXPECT(cancel_template_for(r) == BCN_CLR_FALSE_ALARM && BCN_CLR_FALSE_ALARM == 0x82,
         "false_alarm -> BCN_CLR_FALSE_ALARM (0x82)");

  // An all-clear that says something the operator did not choose is worse
  // than none: unknown words are refused, not defaulted.
  const char* unknown[] = {"", "clear", "RESOLVED", "false alarm", "all_clear", "fire"};
  for (const char* u : unknown) {
    EXPECT(!parse_cancel_reason(u, &r), "an unknown reason is refused");
  }
  EXPECT(!parse_cancel_reason(nullptr, &r), "a missing reason string is refused by the parser");

  EXPECT(is_cancel_template(0x80) && is_cancel_template(0x81) && is_cancel_template(0x82),
         "the three all-clear templates are cancel templates");
  EXPECT(!is_cancel_template(BCN_EMERG_FIRE_VISIBLE), "a hazard template is not an all-clear");
  EXPECT(!is_cancel_template(0x7F) && !is_cancel_template(0x83) && !is_cancel_template(0x00),
         "ids next to the all-clear range are not all-clears");
}

// ── The canonical a CANCEL signs ────────────────────────────────────────────

void test_dual_cancel_canonical() {
  uint8_t nonce[BEACON_NONCE_SIZE]; fill(nonce, 0x5A, sizeof(nonce));
  uint8_t me[DEVICE_FP_SIZE];       fill(me, 0xAA, sizeof(me));
  uint8_t zero[DEVICE_FP_SIZE] = {};
  BeaconAlertCanonical c;
  fill(reinterpret_cast<uint8_t*>(&c), 0xEE, sizeof(c));  // prove every byte is written
  fill_cancel_canonical(c, NOW, 15, BCN_CLR_FALSE_ALARM, BCN_CERT_LIKELY, nonce, me, zero,
                        /*solo=*/false);

  EXPECT(c.msg_type == BEACON_MSG_CANCEL && c.msg_type == 2, "msg_type = Cancel (2)");
  EXPECT(c.template_id == 0x82, "template is the chosen all-clear");
  EXPECT(c.urgency == BCN_URG_PAST && c.urgency == 3, "urgency = Past (spec §4 all-clear default)");
  EXPECT(c.severity == BCN_SEV_MINOR && c.severity == 3, "severity = Minor (spec §4 all-clear default)");
  EXPECT(c.certainty == BCN_CERT_LIKELY, "the two-device path keeps the caller's certainty");
  EXPECT(c.scope == BCN_SCOPE_PRIVATE && c.scope == 2, "scope = Private, always (invariant 5)");
  EXPECT(c.detail_slot == BCN_DETAIL_NONE, "no detail slot on an all-clear");
  EXPECT(c.reserved == 0, "reserved byte is zero");
  EXPECT(c.effective == NOW, "effective = now");
  EXPECT(c.expires == NOW + 15ULL * 60ULL, "expires = effective + ttl minutes");
  EXPECT(std::memcmp(c.ref_canceled_nonce, nonce, BEACON_NONCE_SIZE) == 0,
         "ref_canceled_nonce names the active alarm's frame nonce");
  EXPECT(!nonce_is_zero(c.ref_canceled_nonce), "the reference is non-zero (spec §5.4)");
  EXPECT(std::memcmp(c.originator_fp, me, DEVICE_FP_SIZE) == 0, "originator is this device");
  EXPECT(std::memcmp(c.cosigner_fp, zero, DEVICE_FP_SIZE) == 0,
         "two-device path: cosigner_fp is left for the candidate pick (filled before signing)");
}

void test_solo_cancel_canonical() {
  uint8_t nonce[BEACON_NONCE_SIZE]; fill(nonce, 0x11, sizeof(nonce));
  uint8_t me[DEVICE_FP_SIZE];       fill(me, 0xAA, sizeof(me));
  uint8_t other[DEVICE_FP_SIZE];    fill(other, 0xBB, sizeof(other));
  BeaconAlertCanonical c;
  fill_cancel_canonical(c, NOW, 30, BCN_CLR_RESOLVED, BCN_CERT_LIKELY, nonce, me, other,
                        /*solo=*/true);
  EXPECT(c.certainty == BCN_CERT_OBSERVED,
         "solo CANCEL is certainty = Observed whatever the caller asked (spec §6.2)");
  EXPECT(std::memcmp(c.originator_fp, me, DEVICE_FP_SIZE) == 0 &&
         std::memcmp(c.cosigner_fp, me, DEVICE_FP_SIZE) == 0,
         "solo CANCEL names this device as both signers");
  EXPECT(c.msg_type == BEACON_MSG_CANCEL, "solo CANCEL is still msg_type = Cancel");
  EXPECT(c.expires == NOW + 30ULL * 60ULL, "solo TTL honored");
}

// ── Refusal order ───────────────────────────────────────────────────────────

void test_refusal_order() {
  // No alarm to name comes before everything — even an unsynced clock.
  EXPECT(decide_cancel_origination(false, false, false, false, false) == CANCEL_NO_ACTIVE_ALARM,
         "no active alarm is refused first");
  EXPECT(decide_cancel_origination(false, true, true, false, true) == CANCEL_NO_ACTIVE_ALARM,
         "no active alarm is refused first on the solo path too");
  EXPECT(decide_cancel_origination(true, false, false, true, true) == CANCEL_TIME_UNSYNCED,
         "an unsynced clock is refused next");

  // Two-device path.
  EXPECT(decide_cancel_origination(true, true, false, false, true) == CANCEL_NO_COSIGNER,
         "two-device CANCEL with nobody to ask is refused by name");
  EXPECT(decide_cancel_origination(true, true, false, true, false) == CANCEL_OK,
         "two-device CANCEL does not consult the BOOT button");

  // Solo path: the paired-cosigner check comes before the BOOT check, as in
  // originate_alert_solo — the BOOT button stands in for a missing key.
  EXPECT(decide_cancel_origination(true, true, true, true, false) == CANCEL_PAIRED_COSIGNER_AVAILABLE,
         "solo CANCEL with a fresh cosigner available is sent to the two-device path first");
  EXPECT(decide_cancel_origination(true, true, true, true, true) == CANCEL_PAIRED_COSIGNER_AVAILABLE,
         "holding BOOT does not buy a solo CANCEL while a cosigner is available");
  EXPECT(decide_cancel_origination(true, true, true, false, false) == CANCEL_BOOT_NOT_HELD,
         "solo CANCEL without the BOOT button held is refused (no software-only path)");
  EXPECT(decide_cancel_origination(true, true, true, false, true) == CANCEL_OK,
         "solo CANCEL with BOOT held and no cosigner available proceeds");
}

// ── What a cosigner agrees to be asked ──────────────────────────────────────

void test_cosigner_gate() {
  uint8_t held[BEACON_NONCE_SIZE];  fill(held, 0x42, sizeof(held));
  uint8_t other[BEACON_NONCE_SIZE]; fill(other, 0x43, sizeof(other));
  uint8_t zero[BEACON_NONCE_SIZE] = {};

  EXPECT(cosign_request_acceptable(BEACON_MSG_ALERT, BCN_EMERG_FIRE_VISIBLE, zero, false, zero),
         "ALERT requests keep working with no alarm held");
  EXPECT(cosign_request_acceptable(BEACON_MSG_CANCEL, BCN_CLR_FALSE_ALARM, held, true, held),
         "a CANCEL naming the alarm this device holds is signable");
  EXPECT(!cosign_request_acceptable(BEACON_MSG_CANCEL, BCN_CLR_FALSE_ALARM, other, true, held),
         "a CANCEL naming a different alarm is refused");
  EXPECT(!cosign_request_acceptable(BEACON_MSG_CANCEL, BCN_CLR_FALSE_ALARM, zero, true, zero),
         "a CANCEL with an all-zero reference is refused even against an all-zero local nonce");
  EXPECT(!cosign_request_acceptable(BEACON_MSG_CANCEL, BCN_CLR_FALSE_ALARM, held, false, held),
         "a CANCEL is refused when this device holds no alarm (never attest an all-clear it did not see)");
  EXPECT(!cosign_request_acceptable(BEACON_MSG_CANCEL, BCN_EMERG_FIRE_VISIBLE, held, true, held),
         "a CANCEL carrying a hazard template is refused");
  EXPECT(!cosign_request_acceptable(BEACON_MSG_UPDATE, BCN_EMERG_EVACUATION, held, true, held),
         "UPDATE requests are refused until they have their own reviewed path");
  EXPECT(!cosign_request_acceptable(BEACON_MSG_EXERCISE, BCN_EMERG_FIRE_VISIBLE, zero, false, zero),
         "EXERCISE requests are refused until they have their own reviewed path");
  EXPECT(!cosign_request_acceptable(BEACON_MSG_SELFTEST_OK, BCN_CLR_RESOLVED, held, true, held) &&
         !cosign_request_acceptable(BEACON_MSG_COSIGN_REQ, BCN_CLR_RESOLVED, held, true, held) &&
         !cosign_request_acceptable(0xFF, BCN_CLR_RESOLVED, held, true, held),
         "no other msg_type is signable");
}

// ── The emitted header vs the receive path ─────────────────────────────────

// The stateless header rules beacon_channel.cpp::handle_alert_frame applies
// before any signature work (scope, header msg_type == signed msg_type,
// EXERCISE <=> flag, life-safety template, solo rules). An originator whose
// frame fails these has emitted something no receiver accepts.
bool receiver_header_checks(uint8_t hdr_msg_type, uint8_t hdr_flags,
                            const BeaconAlertCanonical& c) {
  if (c.scope != BCN_SCOPE_PRIVATE) return false;
  if (c.msg_type != hdr_msg_type) return false;
  const bool exercise_flag = (hdr_flags & BCN_FLAG_IS_EXERCISE) != 0;
  if ((c.msg_type == BEACON_MSG_EXERCISE) != exercise_flag) return false;
  const uint8_t t = c.template_id;
  const bool life_safety = t == 0x10 || t == 0x12 || (t >= 0x20 && t <= 0x24) ||
                           (t >= 0x30 && t <= 0x32) || (t >= 0x80 && t <= 0x82);
  if (!life_safety) return false;
  if (hdr_flags & BCN_FLAG_SOLO_ORIGIN) {
    if (c.certainty != BCN_CERT_OBSERVED) return false;
    if (std::memcmp(c.originator_fp, c.cosigner_fp, DEVICE_FP_SIZE) != 0) return false;
  } else if (std::memcmp(c.originator_fp, c.cosigner_fp, DEVICE_FP_SIZE) == 0) {
    return false;
  }
  return true;
}

void test_emitted_header_follows_the_canonical() {
  uint8_t nonce[BEACON_NONCE_SIZE]; fill(nonce, 0x21, sizeof(nonce));
  uint8_t a[DEVICE_FP_SIZE];        fill(a, 0xAA, sizeof(a));
  uint8_t b[DEVICE_FP_SIZE];        fill(b, 0xBB, sizeof(b));

  BeaconAlertCanonical dual;
  fill_cancel_canonical(dual, NOW, 15, BCN_CLR_SAFE, BCN_CERT_LIKELY, nonce, a, b, false);
  std::memcpy(dual.cosigner_fp, b, DEVICE_FP_SIZE);  // what originate_canonical fills in
  EXPECT(frame_header_msg_type(dual) == BEACON_MSG_CANCEL && frame_header_msg_type(dual) == 2,
         "a CANCEL canonical is emitted with header msg_type = Cancel (was hardcoded to ALERT)");
  EXPECT(frame_header_flags(dual, false) == 0, "a dual CANCEL carries no flags");
  EXPECT(receiver_header_checks(frame_header_msg_type(dual), frame_header_flags(dual, false), dual),
         "the emitted dual CANCEL passes the receive path's header rules");
  // The regression the header derivation fixes: the old COSIGN_RESP handler
  // wrote BEACON_MSG_ALERT into every header.
  EXPECT(!receiver_header_checks(BEACON_MSG_ALERT, 0, dual),
         "a CANCEL canonical under a hardcoded ALERT header is dropped by every receiver");

  BeaconAlertCanonical solo;
  fill_cancel_canonical(solo, NOW, 15, BCN_CLR_RESOLVED, BCN_CERT_LIKELY, nonce, a, a, true);
  EXPECT(frame_header_flags(solo, true) == BCN_FLAG_SOLO_ORIGIN,
         "a solo CANCEL carries BCN_FLAG_SOLO_ORIGIN and nothing else");
  EXPECT(receiver_header_checks(frame_header_msg_type(solo), frame_header_flags(solo, true), solo),
         "the emitted solo CANCEL passes the receive path's solo rules");
  EXPECT(!receiver_header_checks(frame_header_msg_type(solo), 0, solo),
         "the same solo canonical without the SOLO flag is dropped (collapsed signers)");

  BeaconAlertCanonical alert = dual;
  alert.msg_type = BEACON_MSG_ALERT;
  EXPECT(frame_header_msg_type(alert) == BEACON_MSG_ALERT && frame_header_flags(alert, false) == 0,
         "an ALERT is emitted exactly as before (msg_type 0, no flags)");

  BeaconAlertCanonical drill = dual;
  drill.msg_type = BEACON_MSG_EXERCISE;
  EXPECT(frame_header_flags(drill, false) == BCN_FLAG_IS_EXERCISE,
         "an EXERCISE canonical would wear the exercise flag (spec §5.4 biconditional)");
  EXPECT(receiver_header_checks(frame_header_msg_type(drill), frame_header_flags(drill, false), drill),
         "and would pass the receive path's drill rule");
}

// ── The originator adopts its own frame ─────────────────────────────────────

// adopt_effect has no rate input by construction: the originator charged its
// own bucket when it originated, receivers charge theirs, and adopting its
// own emitted frame must not charge a second time (that would halve the
// 5-per-24 h budget). The API shape is the pin; these cases pin the effect.
void test_originator_adopts_its_own_frame() {
  EXPECT(adopt_effect(BEACON_MSG_ALERT, false) == ADOPT_RAISE_ALARM,
         "an originator enters ALARM for its own ALERT");
  EXPECT(adopt_effect(BEACON_MSG_CANCEL, true) == ADOPT_CLEAR_ALARM,
         "an originator leaves ALARM for its own CANCEL naming that alarm");
  EXPECT(adopt_effect(BEACON_MSG_CANCEL, false) == ADOPT_AUDIT_ONLY,
         "a CANCEL that names no alarm in force changes nothing (audited only)");
  EXPECT(adopt_effect(BEACON_MSG_UPDATE, true) == ADOPT_AUDIT_ONLY &&
         adopt_effect(BEACON_MSG_EXERCISE, false) == ADOPT_AUDIT_ONLY,
         "no other msg_type changes the originator's state on adoption");
}

// ── Wire layout pins ────────────────────────────────────────────────────────

constexpr size_t ESPNOW_V1_MAX_PAYLOAD = 250;  // mesh_network.h MAX_MESSAGE_SIZE

void test_wire_layout() {
  EXPECT(sizeof(BeaconHeader) == 24, "BeaconHeader is 24 bytes (spec §5.1)");
  EXPECT(offsetof(BeaconHeader, msg_type) == 2 && offsetof(BeaconHeader, flags) == 4 &&
         offsetof(BeaconHeader, payload_len) == 6 && offsetof(BeaconHeader, nonce) == 8,
         "BeaconHeader field offsets");
  EXPECT(sizeof(BeaconAlertCanonical) == 72,
         "BeaconAlertCanonical is 72 bytes (71 of fields + 1 reserved, no padding)");
  EXPECT(offsetof(BeaconAlertCanonical, template_id) == 16 &&
         offsetof(BeaconAlertCanonical, msg_type) == 17 &&
         offsetof(BeaconAlertCanonical, ref_canceled_nonce) == 24 &&
         offsetof(BeaconAlertCanonical, originator_fp) == 40 &&
         offsetof(BeaconAlertCanonical, cosigner_fp) == 56,
         "BeaconAlertCanonical field offsets (the signed bytes)");

  const size_t alert_frame = sizeof(BeaconHeader) + sizeof(BeaconAlertCanonical) +
                             2 * BEACON_SIGNATURE_SIZE;
  EXPECT(alert_frame == 224, "an ALERT/CANCEL frame is 224 bytes");
  EXPECT(alert_frame <= ESPNOW_V1_MAX_PAYLOAD, "an ALERT/CANCEL frame fits one ESP-NOW v1 payload");

  const size_t resp_frame = sizeof(BeaconHeader) + sizeof(BeaconCosignResponsePayload);
  EXPECT(sizeof(BeaconCosignResponsePayload) == 141 && resp_frame <= ESPNOW_V1_MAX_PAYLOAD,
         "a COSIGN_RESP frame (165 bytes) fits one ESP-NOW v1 payload");

  const size_t selftest_frame = sizeof(BeaconHeader) + sizeof(BeaconSelfTestPayload);
  EXPECT(sizeof(BeaconSelfTestPayload) == 96 && selftest_frame <= ESPNOW_V1_MAX_PAYLOAD,
         "a SELFTEST_OK frame (120 bytes) fits one ESP-NOW v1 payload");

  // Layout pin, not an endorsement: the COSIGN_REQ body reserves 160 bytes
  // of ciphertext for a 72-byte canonical, so its frame is 310 bytes — over
  // the 250-byte ESP-NOW v1 payload and over the mesh receive buffer that
  // would demultiplex it. Tracked as a precondition of wiring the channel into
  // the sketch loop; change this pin when that fix lands.
  EXPECT(sizeof(BeaconCosignRequestPayload) == 286, "BeaconCosignRequestPayload is 286 bytes");
  EXPECT(offsetof(BeaconCosignRequestPayload, ciphertext_len) == 60 &&
         offsetof(BeaconCosignRequestPayload, ciphertext) == 62 &&
         offsetof(BeaconCosignRequestPayload, originator_signature) == 222,
         "BeaconCosignRequestPayload field offsets");
}

}  // namespace

int main() {
  test_reason_maps_to_all_clear_template();
  test_dual_cancel_canonical();
  test_solo_cancel_canonical();
  test_refusal_order();
  test_cosigner_gate();
  test_emitted_header_follows_the_canonical();
  test_originator_adopts_its_own_frame();
  test_wire_layout();

  if (failures == 0) {
    std::printf("ALL %d beacon cancel origination checks PASSED\n", checks);
    return 0;
  }
  std::printf("%d of %d beacon cancel origination checks FAILED\n", failures, checks);
  return 1;
}
