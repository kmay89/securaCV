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
  // From Off, nothing — not even Confirm (a physical press) — can make it emit.
  // This is the anti-BadUSB core: a device on which the feature is off types
  // nothing regardless of what happens on the button or the console.
  CHECK(step(State::Off, Event::Request).next == State::Off);
  CHECK(step(State::Off, Event::Confirm).emit == false);
  CHECK(step(State::Off, Event::Confirm).next == State::Off);
  CHECK(step(State::Off, Event::Unplug).next == State::Off);
  // Enable is the only way out of Off.
  CHECK(step(State::Off, Event::Enable).next == State::Idle);
}

static void test_one_tap_confirm_types() {
  // Frictionless: a physical BOOT press (Confirm) from Idle types directly —
  // no serial console, no pre-arming needed. The press IS the consent, and a
  // dropped device won't press its own button, so this stays anti-BadUSB.
  Outcome tap = step(State::Idle, Event::Confirm);
  CHECK(tap.next == State::Launched);
  CHECK(tap.emit == true);

  // A console Request still offers the announced-preview path (Armed), and a
  // press then confirms it — but Request ALONE never types.
  Outcome armed = step(State::Idle, Event::Request);
  CHECK(armed.next == State::Armed);
  CHECK(armed.announce == true);
  CHECK(armed.emit == false);
  Outcome confirmed = step(State::Armed, Event::Confirm);
  CHECK(confirmed.next == State::Launched);
  CHECK(confirmed.emit == true);
}

static void test_timeout_and_cancel_relock() {
  CHECK(step(State::Armed, Event::Timeout).next == State::Idle);
  CHECK(step(State::Armed, Event::Timeout).relock == true);
  CHECK(step(State::Armed, Event::Cancel).next == State::Idle);
  // Timeout/Cancel only act on an armed preview; they never emit.
  CHECK(step(State::Armed, Event::Timeout).emit == false);
  CHECK(step(State::Armed, Event::Cancel).emit == false);
}

static void test_unplug_relocks_but_keeps_disabled() {
  CHECK(step(State::Launched, Event::Unplug).next == State::Idle);
  CHECK(step(State::Armed, Event::Unplug).next == State::Idle);
  CHECK(step(State::Off, Event::Unplug).next == State::Off);
}

static void test_relaunch_on_each_press() {
  // Frictionless: each deliberate physical press re-opens the page — tapping
  // BOOT again from Launched types again (every press is its own consent).
  Outcome again = step(State::Launched, Event::Confirm);
  CHECK(again.next == State::Launched);
  CHECK(again.emit == true);
  // A console Request from Launched still offers the announced-preview path.
  Outcome rearm = step(State::Launched, Event::Request);
  CHECK(rearm.next == State::Armed);
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

// ── 5. START-HERE link files (zero-touch, zero-injection) ───────────────────

static void test_start_here_html() {
  char buf[512];
  const char* url = "https://securacv.com/canary?d=x&r=onboard";
  size_t n = build_start_here_html(url, kBase, buf, sizeof(buf));
  CHECK(n > 0);
  CHECK(std::strstr(buf, "http-equiv=\"refresh\"") != nullptr);
  // The '&' query separator is escaped for well-formed HTML.
  CHECK(std::strstr(buf, "d=x&amp;r=onboard") != nullptr);
  CHECK(std::strstr(buf, "d=x&r=onboard") == nullptr);
}

static void test_url_shortcut() {
  char buf[256];
  size_t n = build_url_shortcut("https://securacv.com/canary?d=x", kBase, buf, sizeof(buf));
  CHECK(n > 0);
  CHECK(std::strcmp(buf, "[InternetShortcut]\r\nURL=https://securacv.com/canary?d=x\r\n") == 0);
}

static void test_webloc_escapes_and_wraps() {
  char buf[512];
  size_t n = build_webloc("https://securacv.com/canary?d=x&r=onboard", kBase, buf, sizeof(buf));
  CHECK(n > 0);
  CHECK(std::strstr(buf, "<plist") != nullptr);
  CHECK(std::strstr(buf, "<string>https://securacv.com/canary?d=x&amp;r=onboard</string>") != nullptr);
}

static void test_link_files_refuse_bad_url() {
  char buf[256];
  // A disallowed URL yields an empty file from every builder — never written.
  CHECK(build_start_here_html("https://evil.example/x", kBase, buf, sizeof(buf)) == 0);
  CHECK(buf[0] == '\0');
  CHECK(build_url_shortcut("http://securacv.com/canary", kBase, buf, sizeof(buf)) == 0);
  CHECK(build_webloc("https://securacv.com/canary; rm", kBase, buf, sizeof(buf)) == 0);
}

int main() {
  test_disabled_never_types();
  test_one_tap_confirm_types();
  test_timeout_and_cancel_relock();
  test_unplug_relocks_but_keeps_disabled();
  test_relaunch_on_each_press();
  test_arm_window();
  test_build_help_url();
  test_url_sanitizes_injection();
  test_url_truncation_terminates();
  test_allowlist_accepts_help_url();
  test_allowlist_rejects_everything_else();
  test_manual_plan_types_url_only();
  test_os_methods_carry_only_help_url();
  test_plan_refuses_bad_url();
  test_start_here_html();
  test_url_shortcut();
  test_webloc_escapes_and_wraps();
  test_link_files_refuse_bad_url();

  if (g_failures == 0) {
    std::printf("PASS test_usb_onboard_logic (all assertions)\n");
    return 0;
  }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
