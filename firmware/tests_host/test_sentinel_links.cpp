/**
 * @file test_sentinel_links.cpp
 * @brief canary-sentinel's sensor links (projects/canary-sentinel's
 *        canary/sentinel_links.h): the radar UART's health object and the
 *        BH1750's absent / blinded / reading word, alone and through the
 *        fusion engine under the `door` preset.
 *
 * Pinned here:
 *   * The radar object src/main.cpp now hands the health publish reports the
 *     link by the rule read_radar() votes on: link_ok is false exactly when
 *     the vote would be Denied (a link that spoke, then went quiet past the
 *     preset's SENT_STALE_RADAR), and false for a link that never spoke once
 *     a whole stall window has passed; before a frame or that window, there is
 *     no object (Home Assistant keeps "unknown", never "down"). Before this
 *     change the Sentinel computed the stall and published no object at all.
 *   * A BH1750 that did not answer begin() at boot is Absent, and an absent
 *     light channel never votes. One that answered and then fails a read is
 *     Blinded: Denied, suspicion (requirement R2). Before, the begin() result
 *     was ignored and every failed read voted Denied, so a unit with no light
 *     sensor read as blinded for good.
 *   * What that meant: under the door preset, a body that PIR and radar both
 *     see reads Anomaly with the light channel Denied, and Confirmed with it
 *     absent. A door unit without the sensor raised Anomaly whenever the
 *     radar saw someone.
 *
 * Built against configs/canary-sentinel/door (on the include path, as
 * test_sentinel_presets.cpp does) and linked with the real fusion engine.
 * Host-tested only: no Sentinel has run on a bench (its README's checklist).
 */

#include "canary/sentinel_config.h"
#include "canary/sentinel_links.h"
#include "fusion/sentinel_channels.h"
#include "fusion/sentinel_fusion.h"

#include <cstdio>
#include <cstdlib>

using namespace securacv::fusion;
using canary::sentinel::LightLink;
using canary::sentinel::RadarLink;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      ++g_fails;                                                               \
      std::printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);          \
    }                                                                          \
  } while (0)

static const uint32_t kStall = SENT_STALE_RADAR;  // the door preset's window

static void test_radar_link_before_and_after_the_first_frame() {
  std::printf("-- radar: no object before a frame or a whole stall window\n");
  RadarLink l;
  canary::sentinel::radar_link_open(l, 1000);
  CHECK(!canary::sentinel::radar_link_judged(l, 1000, kStall), "not judged at the UART's opening");
  CHECK(!canary::sentinel::radar_link_judged(l, 1000 + kStall - 1, kStall),
        "not judged one ms short of a whole stall window");
  CHECK(!canary::sentinel::radar_stalled(l, 1000 + 10 * kStall, kStall),
        "a link that never spoke is not stalled (that channel never votes)");

  // A frame decodes: judged at once, and up.
  canary::sentinel::radar_link_frame(l, 1500);
  CHECK(canary::sentinel::radar_link_judged(l, 1500, kStall), "judged on the first frame");
  CHECK(canary::sentinel::radar_link_ok(l, 1500, kStall), "up on a fresh frame");
  CHECK(canary::sentinel::radar_frame_age_ms(l, 1700) == 200, "the age of the last frame");
}

static void test_radar_link_dead_from_boot() {
  std::printf("-- radar: a UART silent from boot reports down once judged\n");
  RadarLink l;
  canary::sentinel::radar_link_open(l, 5000);
  const uint32_t t = 5000 + kStall;
  CHECK(canary::sentinel::radar_link_judged(l, t, kStall), "judged after a whole stall window");
  CHECK(!canary::sentinel::radar_link_ok(l, t, kStall), "and down: it never spoke");
  CHECK(canary::sentinel::radar_frame_age_ms(l, t) == kStall, "its age counts from the UART's opening");
}

static void test_radar_link_matches_the_vote() {
  std::printf("-- radar: link_ok is false exactly when read_radar() votes Denied\n");
  RadarLink l;
  canary::sentinel::radar_link_open(l, 0);
  // Frames every 100 ms until 3000, then silence.
  for (uint32_t t = 0; t <= 3000; t += 100) canary::sentinel::radar_link_frame(l, t);
  bool agreed = true;
  bool saw_up = false, saw_down = false;
  for (uint32_t t = 3000; t <= 3000 + 3 * kStall; t += 50) {
    const bool ok = canary::sentinel::radar_link_ok(l, t, kStall);
    const bool denied = canary::sentinel::radar_stalled(l, t, kStall);
    if (ok == denied) agreed = false;
    saw_up |= ok;
    saw_down |= !ok;
  }
  CHECK(agreed, "the health object and the fusion vote never disagree once a frame has decoded");
  CHECK(saw_up && saw_down, "the walk covered both sides of the stall window");
  CHECK(canary::sentinel::radar_link_ok(l, 3000 + kStall, kStall), "up at exactly the window");
  CHECK(!canary::sentinel::radar_link_ok(l, 3000 + kStall + 1, kStall), "down one ms past it");

  // Wrap-safe across a millis() rollover, as read_radar() always was.
  RadarLink w;
  canary::sentinel::radar_link_open(w, 0xFFFFFF00u);
  canary::sentinel::radar_link_frame(w, 0xFFFFFF80u);
  CHECK(canary::sentinel::radar_frame_age_ms(w, 0x00000080u) == 0x100u, "the age across the rollover");
  CHECK(canary::sentinel::radar_link_ok(w, 0x00000080u, kStall), "still up across the rollover");
}

static void test_light_link_words() {
  std::printf("-- light: absent, blinded or reading\n");
  CHECK(canary::sentinel::light_link(false, -1.0f) == LightLink::Absent,
        "nothing answered at boot: absent (it was voted Denied)");
  CHECK(canary::sentinel::light_link(false, 120.0f) == LightLink::Absent,
        "absent whatever a read says: begin()'s answer decides");
  CHECK(canary::sentinel::light_link(true, -1.0f) == LightLink::Blinded,
        "answered at boot, a read fails now: blinded");
  CHECK(canary::sentinel::light_link(true, 0.0f) == LightLink::Reading,
        "0 lux in the dark is a reading, not a blinding");
  CHECK(canary::sentinel::light_link(true, 350.0f) == LightLink::Reading, "a reading");
}

// One light sample the way src/main.cpp's read_light() takes it.
static void observe_light(FusionEngine& e, bool answered_at_boot, float lux, uint32_t t) {
  switch (canary::sentinel::light_link(answered_at_boot, lux)) {
    case LightLink::Absent:
      return;
    case LightLink::Blinded:
      e.observe(Channel::Light, Vote::Denied, 100, t);
      return;
    case LightLink::Reading:
      e.observe(Channel::Light, channels::light_vote(false, 0, 50), 80, t);
      return;
  }
}

// A body PIR and radar both see, for 6 s, with the light sampled as given.
static FusionResult visitor(bool answered_at_boot, float lux) {
  FusionEngine e(canary::build_fusion_config());
  FusionResult r = e.last();
  for (uint32_t t = 0; t <= 6000; t += 100) {
    e.observe(Channel::Pir, channels::pir_vote(true, 0, 1500), 95, t);
    e.observe(Channel::Radar, channels::radar_vote(false, true, false), 95, t);
    if (t % 1000 == 0) observe_light(e, answered_at_boot, lux, t);
    r = e.evaluate(t);
  }
  return r;
}

static void test_light_through_the_door_preset() {
  std::printf("-- light through the fusion engine (door preset)\n");
  const FusionResult absent = visitor(/*answered_at_boot=*/false, -1.0f);
  CHECK(!absent.denied_any, "an absent light channel is never a denied one");
  CHECK(absent.anomaly == 0, "and adds no suspicion");
  CHECK(absent.level != Level::Anomaly, "a visitor on a unit with no light sensor is not an Anomaly");
  CHECK(absent.level == Level::Confirmed || absent.level == Level::Loiter,
        "PIR and radar agreeing still confirm the visitor");

  const FusionResult blinded = visitor(/*answered_at_boot=*/true, -1.0f);
  CHECK(blinded.denied_any, "a sensor that answered at boot and fails now is denied");
  CHECK(blinded.level == Level::Anomaly,
        "blinding the light while a body is present is the evasion R2 flags: Anomaly "
        "(the verdict every visitor drew on a unit with no sensor, before)");

  const FusionResult dark = visitor(/*answered_at_boot=*/true, 0.0f);
  CHECK(!dark.denied_any && dark.level != Level::Anomaly, "a dark room is not a blinded sensor");
}

int main() {
  std::printf("== canary-sentinel links (door preset) ==\n");
  test_radar_link_before_and_after_the_first_frame();
  test_radar_link_dead_from_boot();
  test_radar_link_matches_the_vote();
  test_light_link_words();
  test_light_through_the_door_preset();
  std::printf("%d checks, %d failures\n", g_checks, g_fails);
  if (g_fails) {
    std::printf("SENTINEL LINKS FAILED\n");
    return EXIT_FAILURE;
  }
  std::printf("sentinel links ok\n");
  return EXIT_SUCCESS;
}
