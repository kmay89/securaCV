#!/usr/bin/env python3
"""Tests for scripts/lint_wifi_join_policy.py — the guard needs a guard.

That lint decides whether an `ESP.restart()` is legitimately gated by walking
C++ brace depth with regexes. That is a heuristic, and a heuristic that nobody
tests is a guard that reads as covered while catching nothing. Three separate
defects were found in this one before it worked, and every one of them is
pinned below:

1. **A proximity window missed its own bug class.** v1 accepted any restart
   with `ever_online` within 20 lines above it — which includes the lines that
   populate the `WifiRetry` struct, so an ungated reboot placed just above the
   shared switch sailed through. A false negative on the exact defect the lint
   exists to prevent.

2. **Checking only the innermost block flagged correct code.** v2 rejected
   `if (s_ever_online && ...) { if (radio_ok()) { restart } }` because the inner
   `if` says nothing about being online. False positives are the worse failure:
   they block correct work and train people to route around the check.

3. **`\\bever_online\\b` does not match `s_ever_online`.** `_` is a word
   character, so there is no boundary before `ever`. The real tree hid this
   because its reboot goes through `wifi_next_action`; only an adversarial case
   surfaced it.

The second half pins the "direct consumer" rules: a supervisor must include
the shared header itself, keep a `WifiRetry`, feed the policy its board's
tunables by name, and carry no local schedule. The emulator was the file that
needed those rules — it reached the header transitively while keeping its own
copy of the outage decision, and the copy had drifted — so one test holds
`emu_net.cpp` to the display's exact policy spelling.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "lint_wifi_join_policy", REPO / "scripts" / "lint_wifi_join_policy.py"
)
lint = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lint)


def gated(src: str) -> bool:
    """Run enclosing_guard against the single restart in `src`."""
    lines = src.splitlines()
    hits = [i for i, text in enumerate(lines) if lint.RESTART.search(text)]
    assert len(hits) == 1, f"fixture must contain exactly one restart, got {len(hits)}"
    return lint.enclosing_guard(lines, hits[0])


class LegitimateRestarts(unittest.TestCase):
    """Shapes that MUST be accepted, or the lint blocks correct work."""

    def test_direct_ever_online_guard(self):
        self.assertTrue(gated("""
void wifi_loop(uint32_t now_ms) {
  if (s_ever_online && (int32_t)(now_ms - s_lost_since_ms) >= (int32_t)LIMIT) {
    ESP.restart();
  }
}"""))

    def test_nested_inside_an_ever_online_guard(self):
        # Regression: v2 checked only the innermost block and rejected this.
        self.assertTrue(gated("""
void wifi_loop(uint32_t now_ms) {
  if (s_ever_online && now_ms - s_lost_since_ms >= LIMIT) {
    if (radio_ok()) {
      ESP.restart();
    }
  }
}"""))

    def test_case_reboot_from_the_shared_policy(self):
        self.assertTrue(gated("""
void wifi_loop(uint32_t now_ms) {
  switch (canary::net::wifi_next_action(retry_policy(), st, now_ms, s_jitter)) {
    case canary::net::WifiAction::Reboot:
      log_line("WIFI", "Outage persisted on a link that was working.");
      delay(200);
      ESP.restart();
      break;
  }
}"""))

    def test_nested_inside_case_reboot(self):
        self.assertTrue(gated("""
void wifi_loop(uint32_t now_ms) {
  switch (canary::net::wifi_next_action(p, st, now_ms, j)) {
    case canary::net::WifiAction::Reboot:
      if (safe_to_reboot()) {
        ESP.restart();
      }
      break;
  }
}"""))

    def test_condition_split_across_lines(self):
        self.assertTrue(gated("""
void wifi_loop(uint32_t now_ms) {
  if (s_ever_online &&
      (int32_t)(now_ms - s_lost_since_ms) >= (int32_t)LIMIT) {
    ESP.restart();
  }
}"""))

    def test_emulator_flag_spelling(self):
        # The emulator's FORMER flag name (g_wifi_ever_up); it now keeps a
        # WifiRetry like the boards, but the spelling stays accepted.
        self.assertTrue(gated("""
void wifi_loop(uint32_t now_ms) {
  if (g_wifi_ever_up && now_ms - g_wifi_down_since >= LIMIT) {
    ESP.restart();
  }
}"""))


class UngatedRestarts(unittest.TestCase):
    """Shapes that MUST be rejected — each is a real bug that shipped."""

    def test_boot_timeout_reboot(self):
        # canary-sense and canary-vision did exactly this. It is the reboot
        # loop: the same failed join, forever, with the wizard never reachable.
        self.assertFalse(gated("""
void wifi_init_or_reboot() {
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_BOOT_TIMEOUT_MS) {
      log_line("WIFI", "Timeout. Rebooting...");
      delay(200);
      ESP.restart();
    }
  }
}"""))

    def test_outage_reboot_with_no_ever_online_check(self):
        self.assertFalse(gated("""
void wifi_loop(uint32_t now_ms) {
  if ((int32_t)(now_ms - s_lost_since_ms) >= (int32_t)WIFI_OUTAGE_REBOOT_MS) {
    ESP.restart();
  }
}"""))

    def test_ungated_reboot_beside_the_shared_switch(self):
        # Regression: v1's proximity window accepted this, because the struct
        # population above mentions ever_online. The restart itself is not
        # gated by anything.
        self.assertFalse(gated("""
void wifi_loop(uint32_t now_ms) {
  canary::net::WifiRetry st;
  st.online = s_online;
  st.ever_online = s_ever_online;
  st.lost_since_ms = s_lost_since_ms;
  if ((int32_t)(now_ms - s_lost_since_ms) >= (int32_t)LIMIT) {
    delay(200);
    ESP.restart();
  }
  switch (canary::net::wifi_next_action(retry_policy(), st, now_ms, j)) {
    case canary::net::WifiAction::Reboot:
      break;
  }
}"""))

    def test_wrong_case_label_is_not_a_gate(self):
        self.assertFalse(gated("""
void wifi_loop(uint32_t now_ms) {
  switch (some_other_thing()) {
    case Whatever::Retry:
      ESP.restart();
      break;
  }
}"""))

    def test_a_sibling_guard_does_not_cover_us(self):
        # The gate is on a block we are NOT inside.
        self.assertFalse(gated("""
void wifi_loop(uint32_t now_ms) {
  if (s_ever_online) {
    note_outage();
  }
  if (now_ms > deadline) {
    ESP.restart();
  }
}"""))


# A supervisor that does everything the lint asks. Each DirectConsumers case
# breaks exactly one thing about it.
GOOD = """\
#include "canary/config.h"
#include "network/wifi_join_policy.h"

/* A block comment that mentions WIFI_OUTAGE_REBOOT_MS must not count,
   nor may it shift the line numbers in a message. */
static canary::net::WifiRetryPolicy retry_policy() {
  canary::net::WifiRetryPolicy p;
  p.base_ms          = WIFI_RETRY_BASE_MS;
  p.max_ms           = WIFI_RETRY_MAX_MS;
  p.outage_reboot_ms = WIFI_OUTAGE_REBOOT_MS;  // the board's own numbers
  return p;
}

void wifi_loop(uint32_t now_ms) {
  canary::net::WifiRetry st;
  st.ever_online = s_ever_online;
  switch (canary::net::wifi_next_action(retry_policy(), st, now_ms, j)) {
    case canary::net::WifiAction::Reboot:
      ESP.restart();
      break;
    case canary::net::WifiAction::Retry:
      break;
    case canary::net::WifiAction::Wait:
      break;
  }
}
"""


def problems_of(src: str) -> list[str]:
    return lint.check_text("fixture.cpp", src)


class DirectConsumers(unittest.TestCase):
    """A supervisor must consume the shared header directly, not re-derive it."""

    def test_a_direct_consumer_is_clean(self):
        self.assertEqual(problems_of(GOOD), [])

    def test_transitive_include_is_not_enough(self):
        # The emulator's old state: the header arrived through wifi_mgr.h and
        # the file spoke the vocabulary, while keeping its own outage rule.
        src = GOOD.replace('#include "network/wifi_join_policy.h"',
                           '#include "canary/net/wifi_mgr.h"')
        found = problems_of(src)
        self.assertTrue(any("wifi_join_policy.h" in p and "itself" in p for p in found), found)

    def test_flattened_arduino_spelling_is_accepted(self):
        src = (GOOD.replace('#include "network/wifi_join_policy.h"',
                            '#include "wifi_join_policy.h"')
                   .replace('#include "canary/config.h"', '#include "config.h"'))
        self.assertEqual(problems_of(src), [])

    def test_hand_rolled_outage_check_is_flagged(self):
        # Gated on ever_online, so the restart rule is satisfied — but the
        # deadline itself is being decided beside the header. That is the copy.
        src = GOOD + """
void outage_watch(uint32_t now_ms) {
  if (s_ever_online &&
      (int32_t)(now_ms - s_lost_since_ms) >= (int32_t)WIFI_OUTAGE_REBOOT_MS) {
    ESP.restart();
  }
}
"""
        found = problems_of(src)
        self.assertTrue(any("outside the WifiRetryPolicy assignment" in p for p in found), found)
        self.assertFalse(any("no evidence it is gated" in p for p in found), found)

    def test_local_backoff_table_is_flagged(self):
        src = GOOD + "static const uint32_t kBackoffMs[] = {2000, 4000, 8000, 16000, 30000};\n"
        found = problems_of(src)
        self.assertTrue(any("local backoff schedule" in p for p in found), found)

    def test_doubling_on_the_attempt_counter_is_flagged(self):
        src = GOOD + "static uint32_t wait_ms() { return 2000u << (s_attempts - 1); }\n"
        found = problems_of(src)
        self.assertTrue(any("local backoff schedule" in p for p in found), found)

    def test_policy_built_from_a_literal_is_flagged(self):
        src = GOOD.replace("p.base_ms          = WIFI_RETRY_BASE_MS;",
                           "p.base_ms          = 2000;")
        found = problems_of(src)
        self.assertTrue(any("expected:" in p and "found:" in p and "2000" in p for p in found),
                        found)

    def test_policy_fed_the_wrong_symbol_is_flagged(self):
        src = GOOD.replace("p.max_ms           = WIFI_RETRY_MAX_MS;",
                           "p.max_ms           = WIFI_RETRY_BASE_MS;")
        found = problems_of(src)
        self.assertTrue(any("not built from the board's tunables" in p for p in found), found)

    def test_missing_shared_state_is_flagged(self):
        src = GOOD.replace("canary::net::WifiRetry st;", "auto st = current_state();")
        found = problems_of(src)
        self.assertTrue(any("no WifiRetry" in p for p in found), found)

    def test_unrelated_arrays_and_comments_are_not_schedules(self):
        # Shapes that exist in the real supervisors and must stay legal.
        src = GOOD + """
char token[device_pseudonym::HEX_LEN + 1] = {0};
char g_referral_host[64] = {0};
// we used to compare against WIFI_OUTAGE_REBOOT_MS right here
"""
        self.assertEqual(problems_of(src), [])


class EmulatorRunsTheDisplaysPolicy(unittest.TestCase):
    """canary.local previews the display; its Wi-Fi schedule must BE the display's."""

    EMULATOR = "canary-local/emulator/src/emu_net.cpp"
    DISPLAY = "firmware/projects/canary-display/src/net/wifi_mgr.cpp"

    def test_same_header_same_symbols(self):
        emu = (REPO / self.EMULATOR).read_text(encoding="utf-8")
        glass = (REPO / self.DISPLAY).read_text(encoding="utf-8")
        self.assertRegex(emu, lint.REQUIRED_INCLUDE)
        self.assertRegex(emu, lint.CONFIG_INCLUDE)
        self.assertEqual(lint.policy_assignments(emu), lint.POLICY_FIELDS)
        self.assertEqual(lint.policy_assignments(emu), lint.policy_assignments(glass))
        self.assertIn(self.EMULATOR, lint.WIFI_SUPERVISORS)


class RealTreeStaysClean(unittest.TestCase):
    def test_every_listed_supervisor_passes_today(self):
        # If this fails, either a board regressed or the lint did. Both are
        # worth stopping for. The generated Arduino copy joins when present.
        problems: list[str] = []
        for rel in lint.WIFI_SUPERVISORS:
            problems.extend(lint.check_file(rel))
        for rel in lint.GENERATED_COPIES:
            if (REPO / rel).exists():
                problems.extend(lint.check_file(rel))
        self.assertEqual(problems, [], "\n".join(problems))


if __name__ == "__main__":
    unittest.main()
