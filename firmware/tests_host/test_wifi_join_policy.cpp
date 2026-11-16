// Host tests for firmware/common/network/wifi_join_policy.h — the fleet-wide
// answer to "the uplink didn't come up".
//
// The headline case is `a_link_that_never_worked_is_never_rebooted`. That is
// the 4-inch display's "setup loop" reproduced as a unit test: the boot join
// times out, the board reboots, the identical join fails again, forever — so
// the device never finishes booting and the setup portal that could have fixed
// the password never appears. It is silent, it looks like a dead board, and it
// cost a real operator an evening.
//
// The other properties worth pinning are quieter but each has a way of rotting:
// millis() wrap safety (an ESP32 that has been up 49 days must not decide every
// deadline just passed), the backoff schedule staying bounded, jitter never
// pushing a retry outside its window, and every JoinFailure carrying non-empty
// text for all three surfaces (label / detail / hint) so a new enum value can't
// reach the glass as a blank line.

#include "../common/network/wifi_join_policy.h"
#include "../common/network/wifi_store.h"  // the joined bit the setup rule reads after a power cut

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

// The two supervisors that hand the setup rule its state (read below for the
// wiring a header test cannot show). Relative to firmware/tests_host, where
// the Makefile runs this.
#ifndef WIFI_SUPERVISOR_SRC
#define WIFI_SUPERVISOR_SRC "../common/network/wifi_supervisor.cpp"
#endif
#ifndef DISPLAY_WIFI_MGR_SRC
#define DISPLAY_WIFI_MGR_SRC "../projects/canary-display/src/net/wifi_mgr.cpp"
#endif

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

static const JoinFailure kAllFailures[] = {
    JoinFailure::NotFound,
    JoinFailure::BadPassword,
    JoinFailure::NoAddress,
    JoinFailure::Unknown,
};

// ── the bug this header exists to prevent ───────────────────────────────────

static void a_link_that_never_worked_is_never_rebooted() {
  WifiRetryPolicy p;
  WifiRetry s;
  s.online = false;
  s.ever_online = false;  // never associated since power-on
  s.lost_since_ms = 0;
  s.attempts = 99;

  // Far past the outage deadline — a reboot would be "due" under the old rule.
  const uint32_t now = p.outage_reboot_ms * 10;
  s.last_attempt_ms = now;  // and not yet due for a retry
  CHECK(wifi_next_action(p, s, now, 0) == WifiAction::Wait,
        "a never-online link must wait, not reboot");

  // Even after an eternity, and even with a retry due, it retries — never
  // reboots. Rebooting re-runs the same failed join against the same network.
  s.last_attempt_ms = 0;
  CHECK(wifi_next_action(p, s, now, 0) == WifiAction::Retry,
        "a never-online link must retry, not reboot");
}

static void a_link_that_worked_and_dropped_may_reboot() {
  WifiRetryPolicy p;
  WifiRetry s;
  s.ever_online = true;  // the radio or the lease may genuinely be wedged
  s.lost_since_ms = 1000;
  s.last_attempt_ms = 1000;

  const uint32_t just_short = 1000 + p.outage_reboot_ms - 1;
  CHECK(wifi_next_action(p, s, just_short, 0) != WifiAction::Reboot,
        "must not reboot before the outage deadline");

  const uint32_t just_past = 1000 + p.outage_reboot_ms;
  CHECK(wifi_next_action(p, s, just_past, 0) == WifiAction::Reboot,
        "a previously-working link must reboot once the outage persists");
}

// ── time math ───────────────────────────────────────────────────────────────

static void deadlines_survive_the_millis_wrap() {
  // millis() wraps every ~49.7 days. Unsigned comparison across the wrap reads
  // as "an enormous amount of time has passed", which would reboot a healthy
  // device the moment its uptime rolled over.
  WifiRetryPolicy p;
  WifiRetry s;
  s.ever_online = true;
  s.lost_since_ms = 0xFFFFFF00u;          // just before the wrap
  s.last_attempt_ms = 0xFFFFFF00u;
  const uint32_t now = 0x00000100u;        // just after: 512 ms later, really

  CHECK(wifi_next_action(p, s, now, 0) == WifiAction::Wait,
        "wrapping millis must not look like a five-minute outage");

  // And a genuine outage that straddles the wrap still fires.
  s.lost_since_ms = 0xFFFFFF00u;
  const uint32_t later = 0xFFFFFF00u + p.outage_reboot_ms;  // wraps
  s.last_attempt_ms = later;
  CHECK(wifi_next_action(p, s, later, 0) == WifiAction::Reboot,
        "a real outage across the wrap must still reboot");
}

// ── backoff ─────────────────────────────────────────────────────────────────

static void the_backoff_doubles_then_holds_at_the_cap() {
  WifiRetryPolicy p;  // 2s base, 30s cap, shift cap 5
  CHECK(wifi_backoff_ms(p, 0) == 2000, "first retry is prompt");
  CHECK(wifi_backoff_ms(p, 1) == 2000, "attempt 1 -> 2s");
  CHECK(wifi_backoff_ms(p, 2) == 4000, "attempt 2 -> 4s");
  CHECK(wifi_backoff_ms(p, 3) == 8000, "attempt 3 -> 8s");
  CHECK(wifi_backoff_ms(p, 4) == 16000, "attempt 4 -> 16s");
  CHECK(wifi_backoff_ms(p, 5) == 30000, "attempt 5 -> capped at 30s");

  // The shift cap is what keeps `base << n` from running away; without it a
  // device stuck overnight would compute a backoff of days, or overflow to a
  // tiny one and hammer the AP.
  for (uint32_t n = 6; n < 200; ++n) {
    CHECK(wifi_backoff_ms(p, n) == 30000, "attempt %u must stay capped", n);
  }
  CHECK(wifi_backoff_ms(p, 0xFFFFFFFFu) == 30000,
        "an absurd attempt count must still cap, not overflow");
}

static void jitter_stays_inside_its_quarter_window() {
  WifiRetryPolicy p;
  for (uint32_t attempts = 0; attempts <= 8; ++attempts) {
    const uint32_t base = wifi_backoff_ms(p, attempts);
    for (uint32_t j = 0; j < 100000; j += 997) {
      const uint32_t got = wifi_backoff_with_jitter_ms(p, attempts, j);
      CHECK(got >= base, "jitter must never shorten the backoff");
      CHECK(got <= base + base / 4,
            "jitter must stay within a quarter of the backoff (attempts=%u)",
            attempts);
    }
  }
  CHECK(wifi_backoff_with_jitter_ms(p, 3, 0) == wifi_backoff_ms(p, 3),
        "zero jitter must be exactly the plain backoff, for deterministic tests");
}

// ── setup fallback ──────────────────────────────────────────────────────────

static void setup_opens_only_for_a_problem_setup_can_fix() {
  WifiRetry s;
  s.ever_online = false;
  s.attempts = 3;

  CHECK(wifi_should_open_setup(s, JoinFailure::BadPassword, 3),
        "a wrong password is exactly what the wizard is for");
  CHECK(wifi_should_open_setup(s, JoinFailure::NotFound, 3),
        "a missing SSID (5GHz-only) needs someone to pick another network");
  CHECK(!wifi_should_open_setup(s, JoinFailure::NoAddress, 3),
        "re-typing a correct password does not fix a silent DHCP server");
  CHECK(!wifi_should_open_setup(s, JoinFailure::Unknown, 3),
        "an unknown failure is not evidence the credentials are wrong");
}

static void a_device_that_worked_before_never_throws_away_its_config() {
  // The nightmare: a Canary that has been running for months loses its AP for
  // ten minutes and responds by wiping into setup mode, advertising an open
  // provisioning network to the neighborhood and forgetting a good config.
  WifiRetry s;
  s.ever_online = true;
  s.attempts = 1000;
  for (JoinFailure f : kAllFailures) {
    CHECK(!wifi_should_open_setup(s, f, 3),
          "a link that has worked must never fall back to setup");
  }
}

static void setup_waits_out_a_slow_booting_router() {
  WifiRetry s;
  s.ever_online = false;
  for (uint32_t n = 0; n < 3; ++n) {
    s.attempts = n;
    CHECK(!wifi_should_open_setup(s, JoinFailure::BadPassword, 3),
          "attempt %u is too early to give up on the router", n);
  }
  s.attempts = 3;
  CHECK(wifi_should_open_setup(s, JoinFailure::BadPassword, 3),
        "the threshold attempt must open setup");
}

// ── a power cut is not a first boot ─────────────────────────────────────────

// What a supervisor hands wifi_should_open_setup after a restart: this power
// cycle's RAM flag OR the ring's joined bit (wifi_store::any_joined).
static WifiRetry setup_state_after_reboot(const wifi_store::Store& ring, uint32_t attempts) {
  WifiRetry s;
  s.online = false;
  s.ever_online = wifi_store::any_joined(ring);  // the RAM flag is false after a restart
  s.attempts = attempts;
  return s;
}

static void a_reboot_with_a_joined_ring_never_opens_setup() {
  // The whole-house power blip: router and Canary restart together, the
  // Canary is up in 3 s, the router beacons again 90 s later. Every attempt
  // meanwhile is NotFound — a "fixable" reason — and the RAM flag is fresh.
  // The ring remembers the network joined; that is not a credential problem.
  wifi_store::Store before; wifi_store::init(before);
  CHECK(wifi_store::add(before, "Home", "hunter2"), "the network that worked");
  uint8_t blob[wifi_store::BLOB_BYTES];
  CHECK(wifi_store::serialize(before, blob, sizeof(blob)) == wifi_store::BLOB_BYTES, "to NVS");
  wifi_store::Store ring;
  CHECK(wifi_store::parse(blob, sizeof(blob), ring), "from NVS, after the power cut");

  for (uint32_t attempts = 0; attempts <= 1000; attempts += (attempts < 10 ? 1 : 99)) {
    const WifiRetry s = setup_state_after_reboot(ring, attempts);
    for (JoinFailure f : kAllFailures) {
      CHECK(!wifi_should_open_setup(s, f, 3),
            "a ring that has joined must not open setup (attempt %u)", attempts);
    }
  }

  // The outage reboot keeps judging by the RAM flag alone: a link that has
  // not associated THIS power cycle is still never rebooted, joined ring or
  // not — or the blip would become a reboot every five minutes.
  WifiRetryPolicy p;
  WifiRetry reboot_state;
  reboot_state.ever_online = false;   // retry_state(): RAM only
  reboot_state.lost_since_ms = 0;
  reboot_state.last_attempt_ms = 0;
  CHECK(wifi_next_action(p, reboot_state, p.outage_reboot_ms * 10, 0) != WifiAction::Reboot,
        "no association this power cycle: never a reboot");
}

static void a_seeded_ring_that_never_joined_still_opens_setup() {
  // The other side: credentials typed into a flasher and never proven. A
  // wrong password there is exactly what setup is for, reboot or no reboot.
  wifi_store::Store before; wifi_store::init(before);
  CHECK(wifi_store::import_legacy(before, "Home", "typo"), "seeded");
  wifi_store::note_result(before, 0, false);
  uint8_t blob[wifi_store::BLOB_BYTES];
  CHECK(wifi_store::serialize(before, blob, sizeof(blob)) == wifi_store::BLOB_BYTES, "to NVS");
  wifi_store::Store ring;
  CHECK(wifi_store::parse(blob, sizeof(blob), ring), "from NVS");

  CHECK(!wifi_should_open_setup(setup_state_after_reboot(ring, 2), JoinFailure::BadPassword, 3),
        "not before the threshold");
  CHECK(wifi_should_open_setup(setup_state_after_reboot(ring, 3), JoinFailure::BadPassword, 3),
        "a never-joined ring with a wrong password opens setup");
  CHECK(wifi_should_open_setup(setup_state_after_reboot(ring, 3), JoinFailure::NotFound, 3),
        "and with a network that is not there");

  // Its first join ends that, for good.
  wifi_store::note_result(ring, 0, true);
  CHECK(!wifi_should_open_setup(setup_state_after_reboot(ring, 1000), JoinFailure::NotFound, 3),
        "once it has joined, a later outage is not a setup problem");
}

static std::string squeezed(const char* path) {
  std::ifstream f(path);
  if (!f) return std::string();
  std::stringstream ss;
  ss << f.rdbuf();
  std::string out;
  for (char c : ss.str()) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') out += c;
  }
  return out;
}

static void both_supervisors_hand_the_setup_rule_the_ring() {
  // The composition above is the supervisors', and neither compiles on a
  // host: read them. The setup rule gets RAM-or-ring; the reboot decision
  // keeps RAM alone.
  const std::string sup = squeezed(WIFI_SUPERVISOR_SRC);
  CHECK(!sup.empty(), "cannot read %s (run from firmware/tests_host)", WIFI_SUPERVISOR_SRC);
  CHECK(sup.find("setup_st.ever_online=s_ever_online||wifi_store::any_joined(s_store);") != std::string::npos,
        "wifi_supervisor.cpp: the setup rule's ever_online is RAM or the ring");
  CHECK(sup.find("wifi_should_open_setup(setup_st,s_last_failure,") != std::string::npos,
        "wifi_supervisor.cpp: and that is the state the setup rule is handed");
  CHECK(sup.find("st.ever_online=s_ever_online;") != std::string::npos &&
        sup.find("wifi_next_action(s_cfg.retry,st,") != std::string::npos,
        "wifi_supervisor.cpp: the outage reboot judges by the RAM flag alone");

  const std::string disp = squeezed(DISPLAY_WIFI_MGR_SRC);
  CHECK(!disp.empty(), "cannot read %s (run from firmware/tests_host)", DISPLAY_WIFI_MGR_SRC);
  CHECK(disp.find("st.ever_online=s_ever_online||canary::cfg::wifi_saved_any_joined();") != std::string::npos,
        "display wifi_mgr.cpp: the setup rule's ever_online is RAM or the ring");
  CHECK(disp.find("st.ever_online=s_ever_online;") != std::string::npos &&
        disp.find("wifi_next_action(retry_policy(),st,") != std::string::npos,
        "display wifi_mgr.cpp: the outage reboot judges by the RAM flag alone");
}

// ── the words on the screen ─────────────────────────────────────────────────

static void every_failure_has_text_for_every_surface() {
  // A new enum value that falls through a switch reaches the glass as a blank
  // line, which reads as a hung device rather than a fixable mistake.
  for (JoinFailure f : kAllFailures) {
    const char* label = join_failure_label(f);
    const char* detail = join_failure_detail(f);
    const char* hint = join_failure_hint(f);
    const char* narrow = join_failure_hint_narrow(f);
    CHECK(label && *label, "label missing for failure %u", (unsigned)f);
    CHECK(detail && *detail, "detail missing for failure %u", (unsigned)f);
    CHECK(hint && *hint, "hint missing for failure %u", (unsigned)f);
    // The narrow form is what a row too narrow for the hint shows (F50): it
    // must exist and be shorter, or the fallback is no fallback. Whether it
    // fits each display's rows is the display's host test (onboard_layout).
    CHECK(narrow && *narrow, "narrow hint missing for failure %u", (unsigned)f);
    CHECK(narrow && hint && std::strlen(narrow) < std::strlen(hint),
          "narrow hint \"%s\" is not shorter than \"%s\"", narrow ? narrow : "",
          hint ? hint : "");
    CHECK(std::strlen(label) <= 32,
          "label must fit a small status line: \"%s\"", label);
    // The detail is what replaced a silent reboot; it must say the device is
    // still up, or the user reads it as a fatal error and power-cycles.
    CHECK(std::string(detail).find("Staying up") != std::string::npos,
          "detail must promise the device stays up: \"%s\"", detail);
  }
}

static void the_two_common_failures_name_their_real_cause() {
  // These two strings are the entire diagnostic an operator gets on a screen
  // the size of a matchbox, so their content is part of the contract.
  CHECK(std::string(join_failure_detail(JoinFailure::NotFound)).find("5GHz") !=
            std::string::npos,
        "a missing SSID must mention the 5 GHz trap");
  CHECK(std::string(join_failure_hint(JoinFailure::BadPassword))
                .find("case-sensitive") != std::string::npos,
        "a bad password must mention case sensitivity");
  // ...and the narrow forms a small screen falls back to still name them.
  CHECK(std::string(join_failure_hint_narrow(JoinFailure::BadPassword))
                .find("case-sensitive") != std::string::npos,
        "the narrow bad-password hint must still mention case sensitivity");
  CHECK(std::string(join_failure_hint_narrow(JoinFailure::NotFound))
                .find("2.4 GHz") != std::string::npos,
        "the narrow not-found hint must still name the 2.4 GHz band");
  CHECK(std::string(join_failure_hint_narrow(JoinFailure::NoAddress))
                .find("router") != std::string::npos,
        "the narrow no-address hint must still name the router");
  CHECK(std::string(join_failure_hint_narrow(JoinFailure::Unknown))
                .find("closer") != std::string::npos,
        "the narrow catch-all hint must still say to move closer");
}

static bool has_word(const std::string& text, const char* word) {
  const std::string w(word);
  for (size_t at = text.find(w); at != std::string::npos;
       at = text.find(w, at + 1)) {
    const bool starts = at == 0 || text[at - 1] == ' ';
    const size_t end = at + w.size();
    if (starts && (end == text.size() || text[end] == ' ')) return true;
  }
  return false;
}

static void the_narrow_forms_say_no_more_than_the_hint() {
  // The narrow form is the same fix in fewer words (F50), not a firmer one.
  // NoAddress is inferred from a timeout (WL_IDLE_STATUS), so its hint is a
  // guess and says so; the catch-all is a suggestion and says so. A narrow
  // form that drops the hedge, or asks for a step the hint never did, turns
  // a guess into an order on the smallest glass.
  static const char* const kHedges[] = {"may", "try", "might", "maybe"};
  static const char* const kSteps[] = {"restart", "reboot", "reset",
                                       "unplug", "replace"};
  for (JoinFailure f : kAllFailures) {
    const std::string hint = join_failure_hint(f);
    const std::string narrow = join_failure_hint_narrow(f);
    for (const char* h : kHedges) {
      CHECK(!has_word(hint, h) || has_word(narrow, h),
            "\"%s\" drops the hedge \"%s\" of \"%s\"", narrow.c_str(), h,
            hint.c_str());
    }
    for (const char* v : kSteps) {
      CHECK(has_word(hint, v) || !has_word(narrow, v),
            "\"%s\" asks for a step (\"%s\") \"%s\" never does",
            narrow.c_str(), v, hint.c_str());
    }
    CHECK(narrow.find("...") == std::string::npos &&
              narrow.find("\xE2\x80\xA6") == std::string::npos,
          "\"%s\" is a cut, not a form", narrow.c_str());
  }
}

static void the_narrow_labels_are_the_label_in_fewer_words() {
  // The Fail scene's title on narrow glass (F65): never longer than the
  // label, never a cut, and every word it keeps is the label's own (the
  // label is a few words of fact; a shorter form drops words, it does not
  // say anything new). NoAddress has one: its label is 221 px at 16 px.
  for (JoinFailure f : kAllFailures) {
    const std::string label = join_failure_label(f);
    const char* n = join_failure_label_narrow(f);
    CHECK(n && *n, "narrow label missing for failure %u", (unsigned)f);
    if (!n) continue;
    const std::string narrow = n;
    CHECK(narrow.size() <= label.size(), "narrow label \"%s\" is longer than \"%s\"",
          narrow.c_str(), label.c_str());
    CHECK(narrow.find("...") == std::string::npos &&
              narrow.find("\xE2\x80\xA6") == std::string::npos,
          "\"%s\" is a cut, not a form", narrow.c_str());
    size_t at = 0;
    while (at < narrow.size()) {
      size_t end = narrow.find(' ', at);
      if (end == std::string::npos) end = narrow.size();
      const std::string word = narrow.substr(at, end - at);
      CHECK(has_word(label, word.c_str()) ||
                (at == 0 && word.size() > 0 &&
                 has_word(label, (std::string(1, (char)(word[0] | 0x20)) +
                                  word.substr(1)).c_str())),
            "\"%s\" says \"%s\", which \"%s\" does not", narrow.c_str(),
            word.c_str(), label.c_str());
      at = end + 1;
    }
  }
  CHECK(std::string(join_failure_label_narrow(JoinFailure::NoAddress)) ==
            "No address",
        "the no-address title's narrow form is \"%s\"",
        join_failure_label_narrow(JoinFailure::NoAddress));
}

int main() {
  a_link_that_never_worked_is_never_rebooted();
  a_link_that_worked_and_dropped_may_reboot();
  deadlines_survive_the_millis_wrap();
  the_backoff_doubles_then_holds_at_the_cap();
  jitter_stays_inside_its_quarter_window();
  setup_opens_only_for_a_problem_setup_can_fix();
  a_device_that_worked_before_never_throws_away_its_config();
  setup_waits_out_a_slow_booting_router();
  a_reboot_with_a_joined_ring_never_opens_setup();
  a_seeded_ring_that_never_joined_still_opens_setup();
  both_supervisors_hand_the_setup_rule_the_ring();
  every_failure_has_text_for_every_surface();
  the_two_common_failures_name_their_real_cause();
  the_narrow_forms_say_no_more_than_the_hint();
  the_narrow_labels_are_the_label_in_fewer_words();

  if (g_failures == 0) {
    std::printf("test_wifi_join_policy: all checks passed\n");
    return 0;
  }
  std::printf("test_wifi_join_policy: %d check(s) FAILED\n", g_failures);
  return 1;
}
