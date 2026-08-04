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

#include <cstdio>
#include <cstring>
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

// ── the words on the screen ─────────────────────────────────────────────────

static void every_failure_has_text_for_every_surface() {
  // A new enum value that falls through a switch reaches the glass as a blank
  // line, which reads as a hung device rather than a fixable mistake.
  for (JoinFailure f : kAllFailures) {
    const char* label = join_failure_label(f);
    const char* detail = join_failure_detail(f);
    const char* hint = join_failure_hint(f);
    CHECK(label && *label, "label missing for failure %u", (unsigned)f);
    CHECK(detail && *detail, "detail missing for failure %u", (unsigned)f);
    CHECK(hint && *hint, "hint missing for failure %u", (unsigned)f);
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
  every_failure_has_text_for_every_surface();
  the_two_common_failures_name_their_real_cause();

  if (g_failures == 0) {
    std::printf("test_wifi_join_policy: all checks passed\n");
    return 0;
  }
  std::printf("test_wifi_join_policy: %d check(s) FAILED\n", g_failures);
  return 1;
}
