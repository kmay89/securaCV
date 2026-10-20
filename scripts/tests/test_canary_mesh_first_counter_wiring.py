#!/usr/bin/env python3
"""The canary persists its replay counters when a member's first counter
lands (sweep F324) — a pin on firmware/canary/src/main.cpp.

mesh_session runs the first-counter handler on a member's first frame that
passes every check since it was registered with no saved counter: a member
just paired, one re-paired from its replay tombstone, or one no save had
caught before a reboot. main.cpp's replay counters are
otherwise saved every 5 minutes, so a receiver rebooted before that save
restored no counter for a member it had just paired with, and every frame
the member had signed under the opera (since F324, one heartbeat to each
other member every 30 s) verified again and stamped it live: host-probed,
240 recorded frames relayed from its address held a member that was gone
CONNECTED for 356 minutes.

The session half is host-tested (test_mesh_session.cpp:
test_a_members_first_counter_is_saved_at_once, which runs a stand-in for this
wiring). main.cpp itself is compiled only by CI's [env:full] build and is run
by nothing on a host, so these cases hold the wiring the stand-in mirrors:

* on_mesh_first_counter is exactly `{ (void)fp; persist_replay_counters(); }`;
* it is installed once, with mesh_session::set_first_counter_handler;
* persist_replay_counters still snapshots mesh_session::get_replay_counters
  and hands the entries to mesh_state::save_replay_counters.

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

from test_canary_mesh_pair_bind_wiring import _body, _seq, _strip  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
MAIN = REPO / "firmware/canary/src/main.cpp"

HANDLER_FN = "on_mesh_first_counter"
PERSIST_FN = "persist_replay_counters"

HANDLER = _seq(
    "static void on_mesh_first_counter(const uint8_t fp[mesh_crypto::FINGERPRINT_LEN]) "
    "{ (void)fp; persist_replay_counters(); }")
INSTALL = _seq("mesh_session::set_first_counter_handler(&on_mesh_first_counter);")
SNAPSHOT = _seq("const size_t n = mesh_session::get_replay_counters(fps, ctrs, "
                "mesh_session::MAX_REPLAY_COUNTERS);")
SAVE = _seq("mesh_state::save_replay_counters(entries, save_count);")


def check(src: str) -> list[str]:
    """Every rule the wiring breaks, as a message; empty when it holds."""
    code = _strip(src)
    problems: list[str] = []
    handlers = list(re.finditer(HANDLER, code))
    if len(handlers) != 1:
        problems.append(f"{HANDLER_FN} is not exactly `static void {HANDLER_FN}(const uint8_t "
                        f"fp[mesh_crypto::FINGERPRINT_LEN]) {{ (void)fp; {PERSIST_FN}(); }}` "
                        f"({len(handlers)} found): a member's first counter is not persisted")
    installs = len(re.findall(INSTALL, code))
    named = len(re.findall(r"\bset_first_counter_handler\s*\(", code))
    if installs != 1 or named != 1:
        problems.append(f"{HANDLER_FN} is not installed once with "
                        f"mesh_session::set_first_counter_handler(&{HANDLER_FN}) "
                        f"({installs} such installs, {named} calls of the setter)")
    persist = _body(code, PERSIST_FN)
    if persist is None:
        problems.append(f"{PERSIST_FN} is missing")
    else:
        if not re.search(SNAPSHOT, persist):
            problems.append(f"{PERSIST_FN} does not snapshot mesh_session::get_replay_counters")
        if not re.search(SAVE, persist):
            problems.append(f"{PERSIST_FN} does not save the snapshot "
                            f"(mesh_state::save_replay_counters)")
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
        src = self.mutate("  (void)fp;\n  persist_replay_counters();\n}", "  (void)fp;\n}")
        self.assertTrue(any("is not persisted" in p for p in check(src)))

    def test_a_commented_out_save_does_not_count(self) -> None:
        src = self.mutate("  (void)fp;\n  persist_replay_counters();\n}",
                          "  (void)fp;\n  // persist_replay_counters();\n}")
        self.assertTrue(any("is not persisted" in p for p in check(src)))

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
        src = self.mutate("  mesh_state::save_replay_counters(entries, save_count);\n", "")
        self.assertTrue(any("does not save the snapshot" in p for p in check(src)))


if __name__ == "__main__":
    unittest.main()
