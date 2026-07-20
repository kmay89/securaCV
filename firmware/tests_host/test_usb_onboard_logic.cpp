/* Host tests for firmware/common/usb/usb_onboard_logic.h — the trust model of
 * the "plug me in" USB onboarding feature (see docs/design/usb_onboard.md).
 *
 * These pin the property that makes a self-typing keyboard safe: the device
 * types ONLY after a physical confirmation that followed an announced arming,
 * and it can type ONLY the allow-listed https help URL — never a shell command.
 *
 * Build & run (CI: firmware.yml host tests):
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/common \
 *       firmware/tests_host/test_usb_onboard_logic.cpp \
 *       -o /tmp/test_usb_onboard_logic && /tmp/test_usb_onboard_logic
 */

#include <cstdio>
#include <cstring>

#include "usb/usb_onboard_logic.h"

using namespace usb_onboard;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static const char* kBase = "https://securacv.com/canary";

// ── 1. Consent state machine ────────────────────────────────────────────────

static void test_disabled_never_types() {
  // From DISABLED, nothing — not even CONFIRM — can make it emit.
  CHECK(step(State::DISABLED, Event::REQUEST).next == State::DISABLED);
  CHECK(step(State::DISABLED, Event::CONFIRM).emit == false);
  CHECK(step(State::DISABLED, Event::CONFIRM).next == State::DISABLED);
  CHECK(step(State::DISABLED, Event::UNPLUG).next == State::DISABLED);
  // ENABLE is the only way out of DISABLED.
  CHECK(step(State::DISABLED, Event::ENABLE).next == State::IDLE);
}

static void test_confirm_only_types_after_arming() {
  // A bare CONFIRM from IDLE must NOT type — this is the anti-BadUSB core.
  Outcome idle_confirm = step(State::IDLE, Event::CONFIRM);
  CHECK(idle_confirm.emit == false);
  CHECK(idle_confirm.next == State::IDLE);

  // The only path that types: IDLE --REQUEST--> ARMED --CONFIRM--> LAUNCHED.
  Outcome armed = step(State::IDLE, Event::REQUEST);
  CHECK(armed.next == State::ARMED);
  CHECK(armed.announce == true);   // the URL is announced before any typing
  CHECK(armed.emit == false);

  Outcome launched = step(State::ARMED, Event::CONFIRM);
  CHECK(launched.next == State::LAUNCHED);
  CHECK(launched.emit == true);
}

static void test_timeout_and_cancel_relock() {
  CHECK(step(State::ARMED, Event::TIMEOUT).next == State::IDLE);
  CHECK(step(State::ARMED, Event::TIMEOUT).relock == true);
  CHECK(step(State::ARMED, Event::CANCEL).next == State::IDLE);
  // After a timeout re-lock, a stray CONFIRM does nothing.
  CHECK(step(State::IDLE, Event::CONFIRM).emit == false);
}

static void test_unplug_relocks_but_keeps_disabled() {
  CHECK(step(State::LAUNCHED, Event::UNPLUG).next == State::IDLE);
  CHECK(step(State::ARMED, Event::UNPLUG).next == State::IDLE);
  CHECK(step(State::DISABLED, Event::UNPLUG).next == State::DISABLED);
}

static void test_relaunch_requires_reconfirm() {
  // LAUNCHED is one-shot: a second CONFIRM without a new REQUEST won't retype.
  CHECK(step(State::LAUNCHED, Event::CONFIRM).emit == false);
  // Re-arming is allowed (owner wants the page again) and re-announces.
  Outcome rearm = step(State::LAUNCHED, Event::REQUEST);
  CHECK(rearm.next == State::ARMED);
  CHECK(rearm.announce == true);
}

static void test_arm_window() {
  CHECK(arm_expired(1000, 1000 + kDefaultArmWindowMs) == true);
  CHECK(arm_expired(1000, 1000 + kDefaultArmWindowMs - 1) == false);
  // Wrap-around (millis rollover) still measures a positive elapsed span.
  CHECK(arm_expired(0xFFFFFF00u, 0xFFFFFF00u + kDefaultArmWindowMs) == true);
}

// ── 2. Help-URL construction ────────────────────────────────────────────────

static void test_build_help_url() {
  char buf[256];
  size_t n = build_help_url(kBase, "canary-7fA3", "recover", buf, sizeof(buf));
  CHECK(std::strcmp(buf, "https://securacv.com/canary?d=canary-7fA3&r=recover") == 0);
  CHECK(n == std::strlen(buf));

  // Base only.
  build_help_url(kBase, "", "", buf, sizeof(buf));
  CHECK(std::strcmp(buf, "https://securacv.com/canary") == 0);

  // reason without id uses '?'.
  build_help_url(kBase, nullptr, "unseal", buf, sizeof(buf));
  CHECK(std::strcmp(buf, "https://securacv.com/canary?r=unseal") == 0);
}

static void test_url_sanitizes_injection() {
  char buf[256];
  // A device id carrying shell/URL-breaking bytes is stripped to safe chars.
  build_help_url(kBase, "a b\"; rm -rf /", "x", buf, sizeof(buf));
  CHECK(std::strcmp(buf, "https://securacv.com/canary?d=abrm-rf&r=x") == 0);
  // No shell metacharacter from the payload survived (& is the structural
  // query separator the builder itself inserts, so it is expected).
  CHECK(std::strpbrk(buf, " \"';|$<>`\\") == nullptr);
}

static void test_url_truncation_terminates() {
  char small[20];
  build_help_url(kBase, "device", "reason", small, sizeof(small));
  CHECK(small[sizeof(small) - 1] == '\0');  // never overruns
}

// ── 3. Allow-list ───────────────────────────────────────────────────────────

static void test_allowlist_accepts_help_url() {
  CHECK(is_allowed_help_url("https://securacv.com/canary", kBase));
  CHECK(is_allowed_help_url("https://securacv.com/canary?d=x&r=recover", kBase));
}

static void test_allowlist_rejects_everything_else() {
  // Wrong origin.
  CHECK(!is_allowed_help_url("https://evil.example/canary", kBase));
  // Not https.
  CHECK(!is_allowed_help_url("http://securacv.com/canary", kBase));
  // Command-injection attempts (would matter on an OS-hotkey launch).
  CHECK(!is_allowed_help_url("https://securacv.com/canary && calc", kBase));
  CHECK(!is_allowed_help_url("https://securacv.com/canary; rm", kBase));
  CHECK(!is_allowed_help_url("https://securacv.com/canary\n", kBase));
  CHECK(!is_allowed_help_url("https://securacv.com/canary\"x", kBase));
  CHECK(!is_allowed_help_url(nullptr, kBase));
}

// ── 4. Launch plan ──────────────────────────────────────────────────────────

static void test_manual_plan_types_url_only() {
  LaunchPlan p = build_launch_plan(LaunchMethod::MANUAL,
                                   "https://securacv.com/canary?d=x", kBase);
  CHECK(p.valid);
  CHECK(p.prelude.key == 0);       // no hotkey — types nothing but the URL
  CHECK(p.prelude.mods == MOD_NONE);
  CHECK(p.press_enter == false);   // person presses Enter themselves
  CHECK(std::strcmp(p.url, "https://securacv.com/canary?d=x") == 0);
}

static void test_os_methods_carry_only_help_url() {
  LaunchPlan win = build_launch_plan(LaunchMethod::WIN_RUN,
                                     "https://securacv.com/canary", kBase);
  CHECK(win.valid);
  CHECK(win.prelude.mods == MOD_GUI);
  CHECK(win.prelude.key == 'r');
  CHECK(win.press_enter == true);
  CHECK(std::strcmp(win.url, "https://securacv.com/canary") == 0);

  LaunchPlan mac = build_launch_plan(LaunchMethod::MAC_SPOTLIGHT,
                                     "https://securacv.com/canary", kBase);
  CHECK(mac.valid);
  CHECK(mac.prelude.mods == MOD_GUI);
  CHECK(mac.prelude.key == ' ');
}

static void test_plan_refuses_bad_url() {
  LaunchPlan p = build_launch_plan(LaunchMethod::WIN_RUN,
                                   "https://securacv.com/canary && calc", kBase);
  CHECK(p.valid == false);
  CHECK(p.url[0] == '\0');   // nothing to type
}

int main() {
  test_disabled_never_types();
  test_confirm_only_types_after_arming();
  test_timeout_and_cancel_relock();
  test_unplug_relocks_but_keeps_disabled();
  test_relaunch_requires_reconfirm();
  test_arm_window();
  test_build_help_url();
  test_url_sanitizes_injection();
  test_url_truncation_terminates();
  test_allowlist_accepts_help_url();
  test_allowlist_rejects_everything_else();
  test_manual_plan_types_url_only();
  test_os_methods_carry_only_help_url();
  test_plan_refuses_bad_url();

  if (g_failures == 0) {
    std::printf("PASS test_usb_onboard_logic (all assertions)\n");
    return 0;
  }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
