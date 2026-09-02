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
        # The emulator names it g_wifi_ever_up, not ever_online.
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


class RealTreeStaysClean(unittest.TestCase):
    def test_every_listed_supervisor_passes_today(self):
        # If this fails, either a board regressed or the lint did. Both are
        # worth stopping for.
        problems: list[str] = []
        for rel in lint.WIFI_SUPERVISORS:
            problems.extend(lint.check_file(rel))
        self.assertEqual(problems, [], "\n".join(problems))


if __name__ == "__main__":
    unittest.main()
