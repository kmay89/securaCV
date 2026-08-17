// tests_host/test_motion.cpp — the motion engine's pure half
// (include/canary/ui/motion_core.h): capability model, tier derivation,
// easing curves, the frame-time governor, and the weather-scene math.
//
// The board rows below are the REAL fleet (firmware/boards/*/pins/pins.h) —
// if a pins.h changes, the tier a glass lands in is re-derived here first.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "canary/ui/motion_core.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
      g_fail++;                                                \
    }                                                          \
  } while (0)

using namespace canary::ui::motion;

// ── The real fleet, as capability rows ───────────────────────────────────

static Caps watch_caps() {       // xiao-esp32s3-round: 240x240 GC9A01 SPI
  return {240, 240, Bus::Spi, 40000000u, true, 240, false, true};
}
static Caps nightstand_s3() {    // ws-s3-lcd147: 172x320 ST7789 SPI
  return {172, 320, Bus::Spi, 40000000u, true, 240, false, true};
}
static Caps touch169() {         // ws-s3-touch-lcd169: 240x280 SPI
  return {240, 280, Bus::Spi, 40000000u, true, 240, false, true};
}
static Caps nightstand_c6() {    // ws-c6-lcd147: no PSRAM, lean build
  return {172, 320, Bus::Spi, 40000000u, false, 160, true, true};
}
static Caps nightlight_c3() {    // ws-c3-lcd147: 180x320 @ 80 MHz, lean
  return {180, 320, Bus::Spi, 80000000u, false, 160, true, true};
}
static Caps dash43() {           // ws-s3-lcd43: 800x480 RGB scanout
  return {800, 480, Bus::RgbPanel, 16000000u, true, 240, false, false};
}

static void test_caps_and_tiers() {
  // Full-frame flush math: the watch's documented ~23 ms (+overhead).
  const uint32_t w_us = full_frame_us(watch_caps());
  CHECK(w_us > 23000 && w_us < 28000, "watch full frame ~23-28 ms");
  // RGB scanout is not bus-bound.
  CHECK(full_frame_us(dash43()) == 0, "RGB panel not bus-bound");

  CHECK(tier_for(watch_caps()) == Tier::Standard, "watch -> Standard");
  CHECK(tier_for(nightstand_s3()) == Tier::Standard, "nightstand-s3 -> Standard");
  CHECK(tier_for(touch169()) == Tier::Standard, "touch169 -> Standard");
  CHECK(tier_for(nightstand_c6()) == Tier::Lean, "c6 -> Lean (no PSRAM)");
  CHECK(tier_for(nightlight_c3()) == Tier::Lean, "c3 -> Lean");
  CHECK(tier_for(dash43()) == Tier::Full, "dash -> Full");

  // A hypothetical fast small SPI glass with PSRAM on an S3 earns Full.
  Caps fast = {172, 172, Bus::Spi, 80000000u, true, 240, false, true};
  CHECK(full_frame_us(fast) < FULL_FLUSH_CEILING_US, "fast SPI under ceiling");
  CHECK(tier_for(fast) == Tier::Full, "fast SPI -> Full");

  // The Lab preview contract (review catch on #1566): the emulator derives
  // tiers from each flavor's PHYSICAL bus, so substituting the bus away
  // must never be reintroduced — an Emulated-bus watch row would land in
  // Full (no bus budget) while the silicon earns Standard. This pins the
  // divergence the substitution caused, so the glue's "keep the physical
  // bus" rule has a named failure if it regresses.
  Caps emu_watch = watch_caps();
  emu_watch.bus = Bus::Emulated;
  CHECK(tier_for(emu_watch) == Tier::Full && tier_for(watch_caps()) == Tier::Standard,
        "bus substitution changes the watch's tier — derive from physical");

  // Frame budgets loosen as tiers drop: a lean glass is allowed slower
  // frames before the governor calls them heavy, never the reverse.
  CHECK(frame_budget_us(Tier::Full) < frame_budget_us(Tier::Standard),
        "full budget tightest");
  CHECK(frame_budget_us(Tier::Standard) < frame_budget_us(Tier::Lean),
        "lean budget loosest");
}

static void test_durations() {
  // Micro snaps on Lean — that IS the adaptive story for the C3/C6.
  CHECK(dur_ms(Dur::Micro, Tier::Lean) == 0, "lean micro snaps");
  CHECK(dur_ms(Dur::Micro, Tier::Full) > 0, "full micro animates");
  // Short is the established page fade on the tiers that run it.
  CHECK(dur_ms(Dur::Short, Tier::Full) == 220, "short == MOTION_PAGE_MS");
  CHECK(dur_ms(Dur::Short, Tier::Standard) == 220, "short == MOTION_PAGE_MS");
  // Durations never grow as the tier drops.
  for (int d = 0; d < 4; d++) {
    CHECK(dur_ms((Dur)d, Tier::Lean) <= dur_ms((Dur)d, Tier::Standard),
          "lean <= standard");
    CHECK(dur_ms((Dur)d, Tier::Standard) <= dur_ms((Dur)d, Tier::Full),
          "standard <= full");
  }
}

static void test_gates() {
  const Gate calm{};                      // day, quiet, no modal
  const Gate night{true, false, false};
  const Gate alarm{false, true, false};
  const Gate modal{false, false, true};

  // Semantic and Transition are never vetoed.
  for (auto t : {Tier::Lean, Tier::Standard, Tier::Full}) {
    for (auto g : {calm, night, alarm, modal}) {
      CHECK(fx_allowed(Fx::Semantic, g, t), "semantic always allowed");
      CHECK(fx_allowed(Fx::Transition, g, t), "transition always allowed");
    }
  }
  // Micro yields to an unacked alarm, and only to that.
  CHECK(fx_allowed(Fx::Micro, calm, Tier::Full), "micro by day");
  CHECK(fx_allowed(Fx::Micro, night, Tier::Full), "micro at night");
  CHECK(!fx_allowed(Fx::Micro, alarm, Tier::Full), "micro yields to alarm");
  // Ambient is the most gated class.
  CHECK(fx_allowed(Fx::Ambient, calm, Tier::Full), "ambient on calm day");
  CHECK(!fx_allowed(Fx::Ambient, night, Tier::Full), "no ambient at night");
  CHECK(!fx_allowed(Fx::Ambient, alarm, Tier::Full), "no ambient in alarm");
  CHECK(!fx_allowed(Fx::Ambient, modal, Tier::Full), "no ambient under modal");
  CHECK(!fx_allowed(Fx::Ambient, calm, Tier::Lean), "no ambient on lean");
}

static void test_curves() {
  // Endpoints are exact for every curve.
  CHECK(ease_out_cubic(0) == 0 && ease_out_cubic(1024) == 1024, "cubic ends");
  CHECK(ease_in_out_cubic(0) == 0 && ease_in_out_cubic(1024) == 1024,
        "in-out ends");
  CHECK(ease_out_back(0) == 0 && ease_out_back(1024) == 1024, "back ends");
  CHECK(ease_out_quad(0) == 0 && ease_out_quad(1024) == 1024, "quad ends");

  // Monotone where the curve promises it; bounded overshoot where not.
  int32_t prev_c = -1, prev_io = -1, prev_q = -1, back_max = 0;
  for (int t = 0; t <= 1024; t += 8) {
    const int32_t c = ease_out_cubic(t);
    const int32_t io = ease_in_out_cubic(t);
    const int32_t q = ease_out_quad(t);
    const int32_t b = ease_out_back(t);
    CHECK(c >= prev_c, "ease_out_cubic monotone");
    CHECK(io >= prev_io, "ease_in_out_cubic monotone");
    CHECK(q >= prev_q, "ease_out_quad monotone");
    CHECK(b >= -8, "back never meaningfully undershoots zero");
    if (b > back_max) back_max = b;
    prev_c = c; prev_io = io; prev_q = q;
    CHECK(c >= 0 && c <= 1024, "cubic in range");
    CHECK(io >= 0 && io <= 1024, "in-out in range");
  }
  // The settle is a landing, not a bounce: a few percent past the target.
  CHECK(back_max > 1024, "back overshoots");
  CHECK(back_max < 1024 + 110, "overshoot stays subtle (<~10%)");

  // Ease-out means the first half covers most of the ground.
  CHECK(ease_out_cubic(512) > 820, "out-cubic front-loads");
  // In-out is symmetric around the midpoint.
  CHECK(ease_in_out_cubic(512) >= 500 && ease_in_out_cubic(512) <= 524,
        "in-out midpoint");

  // lerp respects direction.
  CHECK(lerp1024(100, 200, 512) == 150, "lerp forward");
  CHECK(lerp1024(200, 100, 512) == 150, "lerp backward");
  CHECK(lerp1024(-100, 100, 1024) == 100, "lerp signed");
}

static void test_sine() {
  CHECK(sin1024(0) == 0, "sin 0");
  CHECK(sin1024(256) == 1024, "sin quarter = 1");
  CHECK(sin1024(512) == 0, "sin half = 0");
  CHECK(sin1024(768) == -1024, "sin three-quarter = -1");
  for (uint32_t a = 0; a < 2048; a += 7) {
    const int32_t v = sin1024(a);
    CHECK(v >= -1024 && v <= 1024, "sine bounded");
    CHECK(sin1024(a) == sin1024(a + 1024), "sine periodic");
  }
}

static void test_governor() {
  const uint32_t budget = frame_budget_us(Tier::Full);
  Governor g;

  // Light frames never degrade.
  for (int i = 0; i < 1000; i++) governor_step(g, budget / 2, budget);
  CHECK(g.level == 0, "light frames stay rich");

  // A single spike is noise, not a trend.
  governor_step(g, budget * 4, budget);
  CHECK(g.level == 0, "one spike ignored");
  for (int i = 0; i < 10; i++) governor_step(g, budget / 2, budget);

  // A sustained overload steps down, one level per streak.
  for (int i = 0; i < (int)GOV_DEGRADE_STREAK; i++)
    governor_step(g, budget * 2, budget);
  CHECK(g.level == 1, "sustained overload trims");
  for (int i = 0; i < (int)GOV_DEGRADE_STREAK; i++)
    governor_step(g, budget * 2, budget);
  CHECK(g.level == 2, "continued overload parks decoration");
  // Never past the last level.
  for (int i = 0; i < 20; i++) governor_step(g, budget * 2, budget);
  CHECK(g.level == 2, "level bounded");

  // Recovery is deliberate: it takes sustained light frames, and the EWMA
  // has to drain first — no flapping at the boundary.
  int frames = 0;
  while (g.level > 1 && frames < 5000) {
    governor_step(g, budget / 3, budget);
    frames++;
  }
  CHECK(g.level == 1, "recovers eventually");
  CHECK(frames >= (int)GOV_RECOVER_FRAMES, "recovery is slow by design");
  // A heavy frame during recovery resets the calm counter.
  for (int i = 0; i < (int)GOV_RECOVER_FRAMES - 5; i++)
    governor_step(g, budget / 3, budget);
  governor_step(g, budget * 2, budget);
  CHECK(g.calm_frames == 0, "heavy frame resets recovery");
}

static void test_wx_scene_mapping() {
  // The closed word set from wx_core.h, mapped.
  CHECK(wx_scene_for_word("clear") == WxScene::Still, "clear still");
  CHECK(wx_scene_for_word("mostly clear") == WxScene::Still, "mostly clear");
  CHECK(wx_scene_for_word("some clouds") == WxScene::Clouds, "some clouds");
  CHECK(wx_scene_for_word("overcast") == WxScene::Clouds, "overcast");
  CHECK(wx_scene_for_word("fog") == WxScene::Fog, "fog");
  CHECK(wx_scene_for_word("drizzle") == WxScene::Rain, "drizzle");
  CHECK(wx_scene_for_word("rain") == WxScene::Rain, "rain");
  CHECK(wx_scene_for_word("showers") == WxScene::Rain, "showers");
  CHECK(wx_scene_for_word("thunderstorm") == WxScene::Rain, "thunderstorm");
  CHECK(wx_scene_for_word("snow") == WxScene::Snow, "snow");
  CHECK(wx_scene_for_word("snow showers") == WxScene::Snow, "snow showers");
  CHECK(wx_scene_for_word("") == WxScene::Still, "absent still");
  CHECK(wx_scene_for_word(nullptr) == WxScene::Still, "null still");
  CHECK(wx_scene_for_word("volcanic ash") == WxScene::Still, "unknown still");
}

static void test_wx_particles() {
  // Budgets: Lean carries none, Standard half, Still nothing anywhere.
  CHECK(wx_particle_count(WxScene::Rain, Tier::Lean) == 0, "lean carries none");
  CHECK(wx_particle_count(WxScene::Still, Tier::Full) == 0, "still is still");
  CHECK(wx_particle_count(WxScene::Rain, Tier::Full) == 14, "full rain field");
  CHECK(wx_particle_count(WxScene::Rain, Tier::Standard) == 7, "standard half");
  CHECK(wx_particle_count(WxScene::Clouds, Tier::Full) == 3, "three clouds");

  const int16_t W = 252, H = 300;
  const uint32_t seed = 0x5eed1234u;

  // Determinism: the same (seed, i, t) is the same particle.
  for (int i = 0; i < 14; i++) {
    const WxParticle a = wx_particle(WxScene::Rain, seed, i, 123456u, W, H);
    const WxParticle b = wx_particle(WxScene::Rain, seed, i, 123456u, W, H);
    CHECK(a.x == b.x && a.y == b.y && a.opa == b.opa, "deterministic");
  }

  // Rain falls: y advances with time (across a fall period, monotone until
  // the wrap) and stays inside the field.
  for (int i = 0; i < 14; i++) {
    int wraps = 0;
    int16_t prev_y = wx_particle(WxScene::Rain, seed, i, 0, W, H).y;
    for (uint32_t t = 33; t < 3000; t += 33) {
      const WxParticle p = wx_particle(WxScene::Rain, seed, i, t, W, H);
      CHECK(p.x >= 0 && p.x < W, "rain x in field");
      CHECK(p.y >= 0 && p.y < H, "rain y in field");
      if (p.y < prev_y) wraps++;
      prev_y = p.y;
    }
    CHECK(wraps >= 1 && wraps <= 4, "rain recycles at fall rate");
  }

  // Snow stays in the field through its sway.
  for (int i = 0; i < 10; i++) {
    for (uint32_t t = 0; t < 9000; t += 66) {
      const WxParticle p = wx_particle(WxScene::Snow, seed, i, t, W, H);
      CHECK(p.x >= 0 && p.x < W, "snow x in field");
      CHECK(p.y >= 0 && p.y < H, "snow y in field");
    }
  }

  // Clouds: parallax layers — deeper layers are fainter, and every layer
  // drifts (x changes over its long period).
  const WxParticle c0 = wx_particle(WxScene::Clouds, seed, 0, 0, W, H);
  const WxParticle c1 = wx_particle(WxScene::Clouds, seed, 1, 0, W, H);
  const WxParticle c2 = wx_particle(WxScene::Clouds, seed, 2, 0, W, H);
  CHECK(c0.opa > c1.opa && c1.opa > c2.opa, "nearer clouds read denser");
  bool moved = false;
  int16_t x0 = c0.x;
  for (uint32_t t = 1000; t < 60000; t += 1000) {
    if (wx_particle(WxScene::Clouds, seed, 0, t, W, H).x != x0) moved = true;
  }
  CHECK(moved, "clouds drift");

  // Tick pacing: falling scenes animate, drifting scenes crawl; Standard
  // never ticks faster than Full.
  CHECK(wx_tick_ms(WxScene::Rain, Tier::Full) <
            wx_tick_ms(WxScene::Clouds, Tier::Full),
        "rain ticks faster than clouds");
  CHECK(wx_tick_ms(WxScene::Rain, Tier::Standard) >=
            wx_tick_ms(WxScene::Rain, Tier::Full),
        "standard ticks no faster");
  // And the decorative timer is bounded on every scene and tier: never
  // faster than ~30 fps (the field must not out-spend the faces it
  // decorates), never so slow the drift stutters.
  for (auto s : {WxScene::Clouds, WxScene::Rain, WxScene::Snow, WxScene::Fog}) {
    for (auto t : {Tier::Lean, Tier::Standard, Tier::Full}) {
      const uint32_t p = wx_tick_ms(s, t);
      CHECK(p >= 33 && p <= 200, "scene tick within [33, 200] ms");
    }
  }
}

int main() {
  test_caps_and_tiers();
  test_durations();
  test_gates();
  test_curves();
  test_sine();
  test_governor();
  test_wx_scene_mapping();
  test_wx_particles();
  if (g_fail) {
    std::printf("test_motion: %d FAILURES\n", g_fail);
    return 1;
  }
  std::printf("test_motion: all passed\n");
  return 0;
}
