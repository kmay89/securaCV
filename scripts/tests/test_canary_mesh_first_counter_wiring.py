#!/usr/bin/env python3
"""The canary persists its replay counters when a member's first counter
lands (sweep F324) — a pin on firmware/canary/src/main.cpp.

mesh_session runs the first-counter handler on a member's first frame that
passes every check since it was registered with no saved counter (a member
just paired, one re-paired from its replay tombstone, or one no save had
caught before a reboot, or caught at 0), and once more 66 s later if the
counter has moved, since that first frame need not be the member's newest.
The handler returns whether the save landed: on false the session keeps the
counter unsaved and runs it again on the member's next verified frame, or,
for the second save, 66 s after the pass that failed (the Codex review of
#1839 found main.cpp dropping save_replay_counters()'s result and the
session marking a failed save done). main.cpp's replay counters are
otherwise saved every 5 minutes, so a receiver rebooted before that save
restored no counter for a member it had just paired with, and every frame
the member had signed under the opera (since F324, one heartbeat to each
other member every 30 s) verified again and stamped it live: host-probed,
240 recorded frames relayed from its address held a member that was gone
CONNECTED for 356 minutes.

The session half is host-tested (test_mesh_session.cpp:
test_a_members_first_counter_is_saved_at_once,
test_an_older_first_frame_is_saved_over_once_more,
test_a_failed_first_counter_save_is_tried_again and
test_a_failed_second_save_is_tried_again_a_deadline_later, which run a
stand-in for this wiring). main.cpp itself is compiled only by CI's
[env:full] build and is run by nothing on a host, so these cases hold the
wiring the stand-in mirrors:

* on_mesh_first_counter is exactly
  `static bool ...{ (void)fp; return persist_replay_counters(); }`;
* it is installed once, with mesh_session::set_first_counter_handler;
* persist_replay_counters is `static bool`, still snapshots
  mesh_session::get_replay_counters, answers true with nothing to save
  (`if (n == 0) return true;`), and returns
  mesh_state::save_replay_counters()'s result, with no other return.

The forms are pinned token for token, after comments and literals are
blanked, so a commented-out call is no call. Each rule is shown to fail on a
mutation of the real file.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_canary_mesh_pair_bind_wiring import _seq, _strip  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
MAIN = REPO / "firmware/canary/src/main.cpp"

HANDLER_FN = "on_mesh_first_counter"
PERSIST_FN = "persist_replay_counters"

HANDLER = _seq(
    "static bool on_mesh_first_counter(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN]) "
    "{ (void)fp; return persist_replay_counters(); }")
INSTALL = _seq("mesh_session::set_first_counter_handler(&on_mesh_first_counter);")
SNAPSHOT = _seq("const size_t n = mesh_session::get_replay_counters(fps, ctrs, "
                "mesh_session::MAX_REPLAY_COUNTERS);")
NOTHING = _seq("if (n == 0) return true;")
SAVE = _seq("return mesh_state::save_replay_counters(entries, save_count);")


def _bool_body(code: str, name: str) -> str | None:
    """The brace-matched body of `static bool name(...)`, or None."""
    m = re.search(r"static\s+bool\s+" + re.escape(name) + r"\s*\(", code)
    if m is None:
        return None
    start = code.find("{", m.end())
    if start < 0:
        return None
    depth = 0
    for i in range(start, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[start : i + 1]
    return None


def check(src: str) -> list[str]:
    """Every rule the wiring breaks, as a message; empty when it holds."""
    code = _strip(src)
    problems: list[str] = []
    handlers = list(re.finditer(HANDLER, code))
    if len(handlers) != 1:
        problems.append(f"{HANDLER_FN} is not exactly `static bool {HANDLER_FN}(const uint8_t "
                        f"fp[mesh_crypto::FINGERPRINT_LEN]) {{ (void)fp; return {PERSIST_FN}(); }}` "
                        f"({len(handlers)} found): a member's first counter is not persisted, "
                        f"or a failed save is not reported to the session")
    installs = len(re.findall(INSTALL, code))
    named = len(re.findall(r"\bset_first_counter_handler\s*\(", code))
    if installs != 1 or named != 1:
        problems.append(f"{HANDLER_FN} is not installed once with "
                        f"mesh_session::set_first_counter_handler(&{HANDLER_FN}) "
                        f"({installs} such installs, {named} calls of the setter)")
    persist = _bool_body(code, PERSIST_FN)
    if persist is None:
        problems.append(f"{PERSIST_FN} is missing, or is not `static bool`: "
                        f"the first-counter handler cannot report a failed save")
    else:
        if not re.search(SNAPSHOT, persist):
            problems.append(f"{PERSIST_FN} does not snapshot mesh_session::get_replay_counters")
        if not re.search(SAVE, persist):
            problems.append(f"{PERSIST_FN} does not return the save's result "
                            f"(return mesh_state::save_replay_counters(entries, save_count);)")
        if not re.search(NOTHING, persist):
            problems.append(f"{PERSIST_FN} does not answer true with nothing to save "
                            f"(if (n == 0) return true;)")
        returns = len(re.findall(r"\breturn\b", persist))
        if returns != 2:
            problems.append(f"{PERSIST_FN} has {returns} returns, not the two above: "
                            f"another one can answer for a save that did not land")
    return problems


class FirstCounterWiring(unittest.TestCase):
    def setUp(self) -> None:
        self.src = MAIN.read_text(encoding="utf-8")

    def mutate(self, old: str, new: str) -> str:
        self.assertEqual(self.src.count(old), 1, f"mutation anchor not unique: {old!r}")
        return self.src.replace(old, new)

    def test_the_real_main_cpp_holds(self) -> None:
        self.assertEqual(check(self.src), [])

    def test_a_handler_that_saves_nothing_fails(self) -> None:
        src = self.mutate("  (void)fp;\n  return persist_replay_counters();\n}",
                          "  (void)fp;\n  return true;\n}")
        self.assertTrue(any("is not persisted" in p for p in check(src)))

    def test_a_commented_out_save_does_not_count(self) -> None:
        src = self.mutate("  (void)fp;\n  return persist_replay_counters();\n}",
                          "  (void)fp;\n  // return persist_replay_counters();\n  return true;\n}")
        self.assertTrue(any("is not persisted" in p for p in check(src)))

    def test_a_handler_that_drops_the_result_fails(self) -> None:
        src = self.mutate("  (void)fp;\n  return persist_replay_counters();\n}",
                          "  (void)fp;\n  (void)persist_replay_counters();\n  return true;\n}")
        self.assertTrue(any("is not persisted" in p for p in check(src)))

    def test_the_old_shape_fails(self) -> None:
        """The wiring before the Codex review of #1839: a void handler and a
        void persist that dropped save_replay_counters()'s result."""
        src = self.mutate("static bool on_mesh_first_counter(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN]) {\n"
                          "  (void)fp;\n  return persist_replay_counters();\n}",
                          "static void on_mesh_first_counter(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN]) {\n"
                          "  (void)fp;\n  persist_replay_counters();\n}")
        src = src.replace("static bool persist_replay_counters() {", "static void persist_replay_counters() {", 1)
        src = src.replace("  if (n == 0) return true;\n", "  if (n == 0) return;\n", 1)
        src = src.replace("  return mesh_state::save_replay_counters(entries, save_count);\n",
                          "  mesh_state::save_replay_counters(entries, save_count);\n", 1)
        problems = check(src)
        self.assertTrue(any("is not persisted" in p for p in problems))
        self.assertTrue(any("is not `static bool`" in p for p in problems))

    def test_the_handler_not_installed_fails(self) -> None:
        src = self.mutate("    mesh_session::set_first_counter_handler(&on_mesh_first_counter);\n",
                          "")
        self.assertTrue(any("is not installed once" in p for p in check(src)))

    def test_a_commented_out_install_does_not_count(self) -> None:
        src = self.mutate("    mesh_session::set_first_counter_handler(&on_mesh_first_counter);\n",
                          "    // mesh_session::set_first_counter_handler(&on_mesh_first_counter);\n")
        self.assertTrue(any("is not installed once" in p for p in check(src)))

    def test_a_cleared_install_fails(self) -> None:
        src = self.mutate("    mesh_session::set_first_counter_handler(&on_mesh_first_counter);\n",
                          "    mesh_session::set_first_counter_handler(&on_mesh_first_counter);\n"
                          "    mesh_session::set_first_counter_handler(nullptr);\n")
        self.assertTrue(any("is not installed once" in p for p in check(src)))

    def test_a_persist_that_saves_nothing_fails(self) -> None:
        src = self.mutate("  return mesh_state::save_replay_counters(entries, save_count);\n",
                          "  return true;\n")
        self.assertTrue(any("does not return the save's result" in p for p in check(src)))

    def test_a_persist_that_drops_the_save_result_fails(self) -> None:
        src = self.mutate("  return mesh_state::save_replay_counters(entries, save_count);\n",
                          "  mesh_state::save_replay_counters(entries, save_count);\n  return true;\n")
        self.assertTrue(any("does not return the save's result" in p for p in check(src)))

    def test_a_persist_that_fails_with_nothing_to_save_fails(self) -> None:
        src = self.mutate("  if (n == 0) return true;\n", "  if (n == 0) return false;\n")
        self.assertTrue(any("does not answer true with nothing to save" in p for p in check(src)))

    def test_an_early_return_in_persist_fails(self) -> None:
        src = self.mutate("  const size_t save_count",
                          "  if (n > 1) return true;\n  const size_t save_count")
        self.assertTrue(any("returns, not the two above" in p for p in check(src)))


if __name__ == "__main__":
    unittest.main()
