/**
 * @file test_sentinel_fusion.cpp
 * @brief Host tests for sentinel.fusion (common/fusion/sentinel_fusion).
 *
 * Proves the load-bearing behaviours of the fusion brain WITHOUT hardware:
 *   - scoring monotonicity and the present/confirmed thresholds,
 *   - the independence bonus (two independent modalities => Confirmed; a
 *     single strong channel, however loud, never Confirms),
 *   - the fraud-detection posture: a Denied (blinded) channel raises anomaly;
 *     blinding a channel WHILE a body is present escalates to Anomaly,
 *   - the "silent body" rule: an uncorroborated dwelling body -> Anomaly,
 *   - FSM debounce (rising) and clear debounce (falling), and dwell -> Loiter,
 *   - staleness decay of votes.
 *
 * Compiled by tests_host/Makefile with -DSENTINEL_FUSION_TEST_HOST under
 * g++ -std=c++17 -Wall -Wextra -Werror, same contract as the other suites.
 */

#include "sentinel_fusion.h"
#include "sentinel_channels.h"

#include <cstdio>
#include <cstdlib>

using namespace securacv::fusion;

// ── tiny test harness ────────────────────────────────────────────────────────
static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    ++g_checks;                                                             \
    if (!(cond)) {                                                          \
      ++g_fails;                                                            \
      std::printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);      \
    }                                                                       \
  } while (0)

#define CHECK_EQ_LEVEL(got, want, msg)                                      \
  do {                                                                      \
    ++g_checks;                                                             \
    if ((got) != (want)) {                                                  \
      ++g_fails;                                                            \
      std::printf("  FAIL: %s  got=%s want=%s  (%s:%d)\n", (msg),           \
                  level_name(got), level_name(want), __FILE__, __LINE__);   \
    }                                                                       \
  } while (0)

// Advance the engine to `until_ms` in fixed steps, re-asserting the same votes
// each tick so debounce timers see a sustained condition. Votes are supplied by
// the caller via a lambda that (re)observes at each step.
template <typename ObserveFn>
static FusionResult run_until(FusionEngine& e, ObserveFn observe,
                              uint32_t start_ms, uint32_t until_ms,
                              uint32_t step_ms) {
  FusionResult r = e.last();
  for (uint32_t t = start_ms; t <= until_ms; t += step_ms) {
    observe(e, t);
    r = e.evaluate(t);
  }
  return r;
}

// ── modality mapping is honest (the whole anti-evasion argument rests on it) ──
static void test_modality_grouping() {
  std::printf("test_modality_grouping\n");
  // WiFi-RF and BLE MUST share a modality class: both die when the target
  // carries no powered radio, so they cannot fake independent corroboration.
  CHECK(modality_of(Channel::WifiRf) == modality_of(Channel::Ble),
        "WifiRf and Ble must be the same modality class");
  CHECK(modality_of(Channel::Pir) != modality_of(Channel::Radar),
        "PIR and radar are independent modalities");
  CHECK(modality_of(Channel::Radar) != modality_of(Channel::WifiCsi),
        "radar and CSI are independent modalities");
  CHECK(modality_of(Channel::Light) == modality_of(Channel::Vision),
        "light and vision share the optical class");
}

// ── a single loud channel is Present, never Confirmed ────────────────────────
static void test_single_channel_never_confirms() {
  std::printf("test_single_channel_never_confirms\n");
  FusionEngine e(default_standard_config());

  // Radar screaming at full quality, sustained well past debounce.
  auto obs = [](FusionEngine& en, uint32_t t) {
    en.observe(Channel::Radar, Vote::Strong, 100, t);
  };
  FusionResult r = run_until(e, obs, 0, 5000, 200);
  CHECK(r.confidence >= e.config().present_score, "radar-alone reaches present score");
  CHECK(r.strong_modalities == 1, "one strong modality only");
  // Radar alone is a body-present modality with nothing corroborating -> it is
  // the "silent body" case: Present, then Anomaly once it dwells (below).
  CHECK(r.level == Level::Present, "radar-alone commits to Present, not Confirmed");
}

// ── two INDEPENDENT modalities corroborate -> Confirmed ──────────────────────
static void test_independence_confirms() {
  std::printf("test_independence_confirms\n");
  FusionEngine e(default_standard_config());
  auto obs = [](FusionEngine& en, uint32_t t) {
    en.observe(Channel::Radar, Vote::Strong, 100, t);   // radio-reflection
    en.observe(Channel::Pir,   Vote::Strong, 100, t);   // thermal (independent)
  };
  FusionResult r = run_until(e, obs, 0, 5000, 200);
  CHECK(r.strong_modalities >= 2, "two independent modalities strong");
  CHECK(r.confidence >= e.config().confirmed_score, "score crosses confirmed");
  CHECK_EQ_LEVEL(r.level, Level::Confirmed, "independent corroboration -> Confirmed");
}

// ── same-modality pair does NOT fake independence ────────────────────────────
static void test_same_modality_no_fake_independence() {
  std::printf("test_same_modality_no_fake_independence\n");
  FusionEngine e(default_standard_config());
  // WifiRf + Ble are the SAME modality class. Even both Strong, that is one
  // independent class, so it must not reach Confirmed on independence alone.
  auto obs = [](FusionEngine& en, uint32_t t) {
    en.observe(Channel::WifiRf, Vote::Strong, 100, t);
    en.observe(Channel::Ble,    Vote::Strong, 100, t);
  };
  FusionResult r = run_until(e, obs, 0, 5000, 200);
  CHECK(r.strong_modalities == 1, "carried-radio pair counts as ONE modality");
  CHECK(r.level != Level::Confirmed, "same-modality pair does not Confirm");
}

// ── denied/blinded channel is suspicion, and blinding + body -> Anomaly ──────
static void test_denied_channel_is_suspicion() {
  std::printf("test_denied_channel_is_suspicion\n");

  // (a) A single blinded channel alone does not alarm (could be a fault).
  {
    FusionEngine e(default_standard_config());
    auto obs = [](FusionEngine& en, uint32_t t) {
      en.observe(Channel::Light, Vote::Denied, 100, t);
    };
    FusionResult r = run_until(e, obs, 0, 3000, 200);
    CHECK(r.anomaly > 0, "a denied channel raises the anomaly accumulator");
    CHECK(r.level != Level::Anomaly, "one blinded channel alone does not alarm");
  }

  // (b) Blinding a channel WHILE a body is present is the evasion attempt ->
  //     escalates to Anomaly immediately (no debounce wait).
  {
    FusionEngine e(default_standard_config());
    auto obs = [](FusionEngine& en, uint32_t t) {
      en.observe(Channel::Radar, Vote::Strong, 100, t);  // a body is there
      en.observe(Channel::Light, Vote::Denied, 100, t);  // and a sensor is blinded
    };
    FusionResult r = run_until(e, obs, 0, 1000, 200);
    CHECK(r.anomaly >= e.config().anomaly_score, "body + blinded amplifies to alarm");
    CHECK_EQ_LEVEL(r.level, Level::Anomaly, "blinding-while-present -> Anomaly");
  }
}

// ── silent-body rule: an uncorroborated dwelling body -> Anomaly ─────────────
static void test_silent_body_dwell_anomaly() {
  std::printf("test_silent_body_dwell_anomaly\n");
  FusionConfig cfg = default_standard_config();
  cfg.loiter_dwell_ms = 4000;  // shorten dwell for the test
  FusionEngine e(cfg);

  auto obs = [](FusionEngine& en, uint32_t t) {
    en.observe(Channel::Radar, Vote::Strong, 100, t);  // still body, nothing else
  };
  // Before dwell: Present.
  FusionResult before = run_until(e, obs, 0, 2000, 200);
  CHECK_EQ_LEVEL(before.level, Level::Present, "uncorroborated body is Present pre-dwell");
  // After dwell: an uncorroborated body that lingers is surfaced as Anomaly,
  // never quietly accepted as routine Loiter.
  FusionResult after = run_until(e, obs, 2200, 9000, 200);
  CHECK_EQ_LEVEL(after.level, Level::Anomaly, "silent body past dwell -> Anomaly");
}

// ── a CORROBORATED body that dwells -> Loiter (not Anomaly) ───────────────────
static void test_corroborated_dwell_loiter() {
  std::printf("test_corroborated_dwell_loiter\n");
  FusionConfig cfg = default_standard_config();
  cfg.loiter_dwell_ms = 4000;
  FusionEngine e(cfg);
  auto obs = [](FusionEngine& en, uint32_t t) {
    en.observe(Channel::Radar, Vote::Strong, 100, t);
    en.observe(Channel::Pir,   Vote::Strong, 100, t);
    en.observe(Channel::WifiCsi, Vote::Strong, 100, t);
  };
  FusionResult r = run_until(e, obs, 0, 9000, 200);
  CHECK_EQ_LEVEL(r.level, Level::Loiter, "corroborated body past dwell -> Loiter");
}

// ── rising debounce: presence is not asserted instantly ──────────────────────
static void test_rising_debounce() {
  std::printf("test_rising_debounce\n");
  FusionConfig cfg = default_standard_config();
  cfg.present_debounce_ms = 1200;
  FusionEngine e(cfg);

  // First tick with strong evidence: still Clear (debounce not satisfied).
  e.observe(Channel::Radar, Vote::Strong, 100, 0);
  e.observe(Channel::Pir,   Vote::Strong, 100, 0);
  FusionResult r0 = e.evaluate(0);
  CHECK_EQ_LEVEL(r0.level, Level::Clear, "no instant assert on first strong tick");

  // Just before the debounce window closes: still Clear.
  e.observe(Channel::Radar, Vote::Strong, 100, 1000);
  e.observe(Channel::Pir,   Vote::Strong, 100, 1000);
  FusionResult r1 = e.evaluate(1000);
  CHECK_EQ_LEVEL(r1.level, Level::Clear, "still debouncing at 1000ms");

  // After the window: committed.
  e.observe(Channel::Radar, Vote::Strong, 100, 1400);
  e.observe(Channel::Pir,   Vote::Strong, 100, 1400);
  FusionResult r2 = e.evaluate(1400);
  CHECK(r2.level == Level::Confirmed, "asserts after debounce window");
  CHECK(r2.changed, "transition reported as changed");
}

// ── falling clear debounce + staleness decay ─────────────────────────────────
static void test_clear_debounce_and_staleness() {
  std::printf("test_clear_debounce_and_staleness\n");
  FusionConfig cfg = default_standard_config();
  cfg.clear_debounce_ms = 8000;
  FusionEngine e(cfg);

  // Establish Confirmed.
  auto present = [](FusionEngine& en, uint32_t t) {
    en.observe(Channel::Radar, Vote::Strong, 100, t);
    en.observe(Channel::Pir,   Vote::Strong, 100, t);
  };
  FusionResult r = run_until(e, present, 0, 3000, 200);
  CHECK_EQ_LEVEL(r.level, Level::Confirmed, "established Confirmed");

  // Stop observing. Votes go stale (radar stale_ms=2000, pir=2500), evidence
  // decays to zero, but the FSM must hold through the clear debounce first.
  FusionResult mid = e.evaluate(3000 + 3000);  // 3s of silence: stale but within clear debounce
  CHECK(mid.level != Level::Clear, "holds through clear debounce despite stale votes");

  // Long past clear debounce: back to Clear.
  FusionResult end = e.evaluate(3000 + 20000);
  CHECK_EQ_LEVEL(end.level, Level::Clear, "clears after sustained quiet");
}

// ── disabled channels never contribute ───────────────────────────────────────
static void test_disabled_channel_ignored() {
  std::printf("test_disabled_channel_ignored\n");
  FusionConfig cfg = default_standard_config();
  // Disable everything except a disabled Vision channel; observe Vision.
  for (size_t i = 0; i < static_cast<size_t>(Channel::kCount); ++i) {
    cfg.channels[i].enabled = false;
  }
  FusionEngine e(cfg);
  auto obs = [](FusionEngine& en, uint32_t t) {
    en.observe(Channel::Vision, Vote::Strong, 100, t);  // disabled -> no-op
  };
  FusionResult r = run_until(e, obs, 0, 3000, 200);
  CHECK(r.confidence == 0, "disabled channel contributes nothing");
  CHECK_EQ_LEVEL(r.level, Level::Clear, "disabled channel cannot raise level");
}

// ── channel adapters: raw reading -> Vote (the "what counts as evidence" layer)
static void test_channel_adapters() {
  std::printf("test_channel_adapters\n");
  using namespace securacv::fusion::channels;

  // PIR: live motion Strong, settle tail Weak, quiet None; never Denied.
  CHECK(pir_vote(true, 0, 1500) == Vote::Strong, "PIR live motion -> Strong");
  CHECK(pir_vote(false, 800, 1500) == Vote::Weak, "PIR settle tail -> Weak");
  CHECK(pir_vote(false, 3000, 1500) == Vote::None, "PIR long quiet -> None");

  // Radar: stalled UART is the fraud primitive -> Denied.
  CHECK(radar_vote(true, true, false) == Vote::Denied, "radar stall -> Denied");
  CHECK(radar_vote(false, true, false) == Vote::Strong, "radar present -> Strong");
  CHECK(radar_vote(false, false, true) == Vote::Weak, "radar settling -> Weak");
  CHECK(radar_vote(false, false, false) == Vote::None, "radar empty -> None");

  // CSI: confirmed vs observed vs quiet.
  CHECK(csi_vote(true, true) == Vote::Strong, "CSI confirmed -> Strong");
  CHECK(csi_vote(false, true) == Vote::Weak, "CSI observed -> Weak");
  CHECK(csi_vote(false, false) == Vote::None, "CSI quiet -> None");

  // Count channels (RF/BLE): threshold ladder on an aggregate count.
  CHECK(count_vote(0, 1, 3) == Vote::None, "count 0 -> None");
  CHECK(count_vote(1, 1, 3) == Vote::Weak, "count at weak threshold -> Weak");
  CHECK(count_vote(4, 1, 3) == Vote::Strong, "count at strong threshold -> Strong");

  // Light: corroboration only (never Strong); blinded -> Denied.
  CHECK(light_vote(true, 0, 50) == Vote::Denied, "light blinded -> Denied");
  CHECK(light_vote(false, 80, 50) == Vote::Weak, "light delta -> Weak");
  CHECK(light_vote(false, 10, 50) == Vote::None, "small light change -> None");

  // Contact + tamper (Heavy): open Strong, cut wire Denied.
  CHECK(contact_vote(true, false) == Vote::Strong, "door open -> Strong");
  CHECK(contact_vote(false, true) == Vote::Denied, "contact fault -> Denied");
  CHECK(tamper_vote(true, false) == Vote::Strong, "tamper disturbed -> Strong");

  // Coarse mappers: count -> bucket, cm -> band (the cm is consumed here).
  CHECK(occupancy_from_count(0) == Occupancy::Zero, "0 targets -> Zero");
  CHECK(occupancy_from_count(1) == Occupancy::One, "1 target -> One");
  CHECK(occupancy_from_count(5) == Occupancy::TwoPlus, "5 targets -> 2+");
  CHECK(range_from_cm(100, 150, 350) == RangeBand::Near, "100cm -> Near");
  CHECK(range_from_cm(300, 150, 350) == RangeBand::Mid, "300cm -> Mid");
  CHECK(range_from_cm(500, 150, 350) == RangeBand::Far, "500cm -> Far");
  CHECK(range_from_cm(0, 150, 350) == RangeBand::Unknown, "0cm -> Unknown");
}

// ── end-to-end: adapters feeding the engine (the composition the project does)
static void test_adapters_into_engine() {
  std::printf("test_adapters_into_engine\n");
  using namespace securacv::fusion::channels;
  FusionEngine e(default_standard_config());

  // Simulate a person walking up device-free in the dark: radar present + PIR
  // motion + CSI confirmed, phone left at home (no RF/BLE), lights off.
  auto obs = [](FusionEngine& en, uint32_t t) {
    en.observe(Channel::Radar, radar_vote(false, true, false), 95, t);
    en.observe(Channel::Pir,   pir_vote(true, 0, 1500), 90, t);
    en.observe(Channel::WifiCsi, csi_vote(true, true), 85, t);
    en.observe(Channel::WifiRf, count_vote(0, 1, 3), 100, t);   // no phone
    en.observe(Channel::Ble,    count_vote(0, 1, 3), 100, t);   // no phone
    en.set_range(range_from_cm(120, 150, 350));
    en.set_occupancy(occupancy_from_count(1));
  };
  FusionResult r = run_until(e, obs, 0, 5000, 200);
  CHECK_EQ_LEVEL(r.level, Level::Confirmed,
                 "device-free intruder in the dark still Confirms (3 modalities)");
  CHECK(r.range == RangeBand::Near, "coarse range surfaced");
  CHECK(r.occupancy == Occupancy::One, "coarse occupancy surfaced");
}

int main() {
  std::printf("== sentinel.fusion host tests ==\n");
  test_modality_grouping();
  test_single_channel_never_confirms();
  test_independence_confirms();
  test_same_modality_no_fake_independence();
  test_denied_channel_is_suspicion();
  test_silent_body_dwell_anomaly();
  test_corroborated_dwell_loiter();
  test_rising_debounce();
  test_clear_debounce_and_staleness();
  test_disabled_channel_ignored();
  test_channel_adapters();
  test_adapters_into_engine();

  std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
  if (g_fails) {
    std::printf("SENTINEL FUSION TESTS FAILED\n");
    return EXIT_FAILURE;
  }
  std::printf("all sentinel.fusion tests passed\n");
  return EXIT_SUCCESS;
}
