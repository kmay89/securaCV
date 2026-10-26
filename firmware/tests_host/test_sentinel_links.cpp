/**
 * @file test_sentinel_links.cpp
 * @brief canary-sentinel's sensor links (projects/canary-sentinel's
 *        canary/sentinel_links.h): the radar UART's health object and the
 *        BH1750's absent / blinded / reading word, alone and through the
 *        fusion engine under the `door` preset, and src/main.cpp held to
 *        calling them (its source is passed in by the Makefile).
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
 *   * Absent is never silent (requirement R6): the state topic's
 *     channel_denied, which Home Assistant's "Channel blinded" problem sensor
 *     reads, is true for it, though the engine's denied_any is not. So a
 *     sensor unplugged while the unit was off still reaches the owner, as it
 *     did when it voted Denied, without the false Anomaly.
 *   * src/main.cpp takes these rules and no other: its read_light(),
 *     light_absent_at_boot() and radar_link_health() bodies, the snapshot's
 *     channel_denied and both health publishes are held to their source text
 *     (comments and spacing aside). The functions above are only as good as
 *     the firmware's calls to them, and no host suite compiles main.cpp.
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
#include <fstream>
#include <sstream>
#include <string>

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

static void test_light_sample_votes() {
  std::printf("-- light: the vote one sample gives the engine\n");
  using canary::sentinel::LightChannel;
  using canary::sentinel::LightSample;
  using canary::sentinel::light_sample;

  LightChannel c;
  LightSample s = light_sample(c, /*answered_at_boot=*/false, -1.0f);
  CHECK(!s.votes, "absent: no vote at all (it was Denied, quality 100)");
  s = light_sample(c, false, 300.0f);
  CHECK(!s.votes, "absent whatever a read says");
  CHECK(c.baseline < 0.0f, "an absent sensor never seeds the baseline");

  s = light_sample(c, true, -1.0f);
  CHECK(s.votes && s.vote == Vote::Denied && s.quality == 100,
        "blinded: Denied at quality 100, suspicion (R2)");
  CHECK(c.baseline < 0.0f, "a failed read never seeds the baseline");

  s = light_sample(c, true, 200.0f);
  CHECK(s.votes && s.vote == Vote::None && s.quality == 80,
        "the first reading seeds the baseline and votes None at quality 80");
  CHECK(c.baseline == 200.0f, "seeded at the first reading");
  s = light_sample(c, true, 200.0f + canary::sentinel::kLightWeakDeltaLux);
  CHECK(s.votes && s.vote == Vote::Weak, "a change of the weak delta is Weak corroboration");
  CHECK(c.baseline > 200.0f && c.baseline < 201.5f, "the baseline tracks slowly (2% a sample)");
  s = light_sample(c, true, c.baseline + 10.0f);
  CHECK(s.votes && s.vote == Vote::None, "a small change is no evidence");
  s = light_sample(c, true, 0.0f);
  CHECK(s.votes && s.vote == Vote::Weak, "lights out against a lit baseline: Weak, never Denied");
}

static void test_channel_denied_reported() {
  std::printf("-- light: an absent sensor is reported, never silent (R6)\n");
  using canary::sentinel::channel_denied_reported;
  CHECK(!channel_denied_reported(false, false), "nothing denied, nothing absent");
  CHECK(channel_denied_reported(true, false), "a channel voting Denied");
  CHECK(channel_denied_reported(false, true),
        "a light sensor absent since boot, though it never votes");
  CHECK(channel_denied_reported(true, true), "both");
}

// A body PIR and radar both see, for 6 s, with the light sampled as given.
static FusionResult visitor(bool answered_at_boot, float lux) {
  FusionEngine e(canary::build_fusion_config());
  canary::sentinel::LightChannel light;
  FusionResult r = e.last();
  for (uint32_t t = 0; t <= 6000; t += 100) {
    e.observe(Channel::Pir, channels::pir_vote(true, 0, 1500), 95, t);
    e.observe(Channel::Radar, channels::radar_vote(false, true, false), 95, t);
    if (t % 1000 == 0) {
      const canary::sentinel::LightSample s = canary::sentinel::light_sample(light, answered_at_boot, lux);
      if (s.votes) e.observe(Channel::Light, s.vote, s.quality, t);
    }
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
  CHECK(canary::sentinel::channel_denied_reported(absent.denied_any, /*light_absent_at_boot=*/true),
        "and the owner still sees a denied channel: the sensor this build carries is not there");

  const FusionResult blinded = visitor(/*answered_at_boot=*/true, -1.0f);
  CHECK(blinded.denied_any, "a sensor that answered at boot and fails now is denied");
  CHECK(blinded.level == Level::Anomaly,
        "blinding the light while a body is present is the evasion R2 flags: Anomaly "
        "(the verdict every visitor drew on a unit with no sensor, before)");

  const FusionResult dark = visitor(/*answered_at_boot=*/true, 0.0f);
  CHECK(!dark.denied_any && dark.level != Level::Anomaly, "a dark room is not a blinded sensor");
  CHECK(!canary::sentinel::channel_denied_reported(dark.denied_any, /*light_absent_at_boot=*/false),
        "and reports no denied channel");
}

// ── src/main.cpp calls these rules, and no others ─────────────────────────
//
// main.cpp is Arduino code no host suite compiles, so the wiring is held to
// its source: comments are dropped and every run of whitespace becomes one
// space, and then each function below must read exactly as written here.
// Each pin names a regression the functions above cannot see: a radar object
// that never reaches the publish, an absent sensor voting Denied again, the
// begin() answer bypassed, the periodic health publish losing its radar, the
// absent sensor dropped from channel_denied.

static std::string read_file(const char* path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// C++ source without its comments, every whitespace run one space. String and
// character literals are kept whole, so a "//" inside one is not a comment.
static std::string normalized_source(const std::string& src) {
  std::string out;
  bool space = false;
  auto put = [&](char ch) {
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
      space = true;
      return;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += ch;
  };
  for (size_t i = 0; i < src.size(); i++) {
    const char ch = src[i];
    if (ch == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      while (i < src.size() && src[i] != '\n') i++;
      put('\n');
    } else if (ch == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) i++;
      i++;
      put(' ');
    } else if (ch == '"' || ch == '\'') {
      put(ch);
      for (i++; i < src.size() && src[i] != ch; i++) {
        out += src[i];
        if (src[i] == '\\' && i + 1 < src.size()) out += src[++i];
      }
      if (i < src.size()) out += ch;
    } else {
      put(ch);
    }
  }
  return out;
}

static size_t count_of(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) n++;
  return n;
}

// The body of the one definition that opens with `signature {`, without its
// braces, or "" when there is not exactly one.
static std::string body_of(const std::string& src, const std::string& signature) {
  const std::string open = signature + " {";
  if (count_of(src, open) != 1) return "";
  size_t i = src.find(open) + open.size();
  const size_t start = i;
  for (int depth = 1; i < src.size(); i++) {
    if (src[i] == '{') depth++;
    if (src[i] == '}' && --depth == 0) break;
  }
  std::string body = src.substr(start, i - start);
  while (!body.empty() && body.front() == ' ') body.erase(body.begin());
  while (!body.empty() && body.back() == ' ') body.pop_back();
  return body;
}

static void check_body(const std::string& src, const char* signature, const char* expected) {
  const std::string got = body_of(src, signature);
  ++g_checks;
  if (got != expected) {
    ++g_fails;
    std::printf("  FAIL: main.cpp `%s` is not the body this suite holds it to\n"
                "    want: %s\n    got:  %s\n",
                signature, expected, got.empty() ? "(no single definition)" : got.c_str());
  }
}

static void test_main_cpp_wiring(const char* main_cpp) {
  std::printf("-- src/main.cpp calls these rules (%s)\n", main_cpp);
  const std::string raw = read_file(main_cpp);
  CHECK(!raw.empty(), "could not read canary-sentinel's src/main.cpp");
  if (raw.empty()) return;
  const std::string src = normalized_source(raw);

  // The radar object: the header's rules, its judged/ok/age, the parser's count.
  check_body(src, "static const canary::net::RadarLinkHealth* radar_link_health(uint32_t now_ms)",
             "#if defined(FEATURE_MMWAVE_RADAR) && FEATURE_MMWAVE_RADAR "
             "static canary::net::RadarLinkHealth h; "
             "if (!canary::sentinel::radar_link_judged(g_radar_link, now_ms, SENT_STALE_RADAR)) return nullptr; "
             "h.link_ok = canary::sentinel::radar_link_ok(g_radar_link, now_ms, SENT_STALE_RADAR); "
             "h.last_frame_age_ms = canary::sentinel::radar_frame_age_ms(g_radar_link, now_ms); "
             "h.frame_errors = g_radar_parser.error_count(); "
             "return &h; "
             "#else (void)now_ms; return nullptr; #endif");
  // ...handed to both health publishes: the one on connect and the periodic one.
  CHECK(count_of(src, "publish_health_retained(") == 2,
        "main.cpp publishes health from exactly two places");
  CHECK(count_of(src, "canary::net::publish_health_retained(TOPICS, radar_link_health(now));") == 2,
        "both health publishes carry radar_link_health(now)");
  // The vote reads the same link the object reports.
  CHECK(count_of(src, "canary::sentinel::radar_link_open(g_radar_link, millis());") == 1,
        "setup() opens the link the vote and the object read");
  CHECK(count_of(src, "canary::sentinel::radar_link_frame(g_radar_link, now);") == 1,
        "read_radar() records each decoded frame on that link");
  CHECK(count_of(src, "const bool stalled = canary::sentinel::radar_stalled(g_radar_link, now, SENT_STALE_RADAR);") == 1,
        "read_radar() votes Denied by the header's stall rule");

  // The light channel: one sample through the header's rule, with begin()'s answer.
  check_body(src, "static void read_light(uint32_t now)",
             "#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT "
             "const canary::sentinel::LightSample s = "
             "canary::sentinel::light_sample(g_light, g_lux.present(), g_lux.read_lux()); "
             "if (s.votes) g_engine.observe(Channel::Light, s.vote, s.quality, now); "
             "#else (void)now; #endif");
  CHECK(count_of(src, "Channel::Light") == 1,
        "no other light vote anywhere in main.cpp (an absent sensor voting Denied again)");

  // An absent sensor reaches the state topic's channel_denied.
  check_body(src, "static bool light_absent_at_boot()",
             "#if defined(FEATURE_AMBIENT_LIGHT) && FEATURE_AMBIENT_LIGHT "
             "return !g_lux.present(); "
             "#else return false; #endif");
  CHECK(count_of(src, "g_snap.denied_any =") == 1 &&
            count_of(src, "g_snap.denied_any = canary::sentinel::channel_denied_reported("
                          "r.denied_any, light_absent_at_boot());") == 1,
        "the snapshot's channel_denied is the engine's denied_any or the absent sensor");
}

int main(int argc, char** argv) {
  std::printf("== canary-sentinel links (door preset) ==\n");
  test_radar_link_before_and_after_the_first_frame();
  test_radar_link_dead_from_boot();
  test_radar_link_matches_the_vote();
  test_light_link_words();
  test_light_sample_votes();
  test_channel_denied_reported();
  test_light_through_the_door_preset();
  if (argc == 2) {
    test_main_cpp_wiring(argv[1]);
  } else {
    ++g_checks;
    ++g_fails;
    std::printf("  FAIL: pass canary-sentinel's src/main.cpp (the Makefile does)\n");
  }
  std::printf("%d checks, %d failures\n", g_checks, g_fails);
  if (g_fails) {
    std::printf("SENTINEL LINKS FAILED\n");
    return EXIT_FAILURE;
  }
  std::printf("sentinel links ok\n");
  return EXIT_SUCCESS;
}
