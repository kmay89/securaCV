#!/usr/bin/env python3
"""The canary persists a paired member's radio address only after the
session bound it (sweep F102) — a pin on firmware/canary/src/main.cpp.

mesh_session's NOTIFY_PAIRED runs the PairedCallback, then binds the new
member to the address it paired from. main.cpp used to persist that address
from the PairedCallback (register_paired_peer -> save_peer_mac), before the
bind, so an address the session refused (one another member holds, as a
re-pair relayed from a member's copied address presents it) was written
anyway, and the next boot's restore, which binds the stored addresses in
order, could give it to the wrong member: host-probed, the member that owned
the address was then not heard at all.

The session half is host-tested (test_mesh_session.cpp:
test_paired_peer_bound_reports_the_bind and
test_refused_repair_bind_is_not_persisted_across_reboot, which run a
stand-in for this wiring). main.cpp itself is compiled only by CI's
[env:full] build and is run by nothing on a host, so these cases hold the
wiring the stand-in mirrors:

* save_peer_mac is called once, from on_mesh_paired_peer_bound, after an
  `if (!bound)` block that returns;
* register_paired_peer, the PairedCallback's path, saves no address and no
  longer reads the partner's address at all;
* the bound callback is installed with set_paired_peer_bound_callback.

Each rule is shown to fail on a mutation of the real file, so the pin cannot
read as covered while catching nothing.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAIN = REPO / "firmware/canary/src/main.cpp"

BOUND_FN = "on_mesh_paired_peer_bound"
PAIRED_FN = "register_paired_peer"


def _strip(src: str) -> str:
    """Blank out comments and string/char literals, keeping offsets."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                if out[k] != "\n":
                    out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


def _body(code: str, name: str) -> str | None:
    """The brace-matched body of `static void name(...)`, or None."""
    m = re.search(r"static\s+void\s+" + re.escape(name) + r"\s*\(", code)
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

    calls = len(re.findall(r"\bsave_peer_mac\s*\(", code))
    if calls != 1:
        problems.append(f"save_peer_mac is called {calls} times; expected once, "
                        f"from {BOUND_FN}")

    bound = _body(code, BOUND_FN)
    if bound is None:
        problems.append(f"{BOUND_FN} is missing")
    else:
        save = re.search(r"\bsave_peer_mac\s*\(", bound)
        guard = re.search(r"if\s*\(\s*!\s*bound\s*\)\s*\{", bound)
        if save is None:
            problems.append(f"{BOUND_FN} does not save the address")
        elif guard is None or guard.start() > save.start():
            problems.append(f"{BOUND_FN} saves the address without first "
                            f"returning on `if (!bound)`")
        else:
            block = _body("static void g(" + bound[guard.end() - 1 :], "g") or ""
            if not re.search(r"\breturn\s*;", block):
                problems.append(f"{BOUND_FN}'s `if (!bound)` block does not return")

    paired = _body(code, PAIRED_FN)
    if paired is None:
        problems.append(f"{PAIRED_FN} is missing")
    elif re.search(r"\b(save_peer_mac|get_paired_peer_mac)\s*\(", paired):
        problems.append(f"{PAIRED_FN} (the PairedCallback, before the bind) "
                        f"touches the partner's address")

    if not re.search(r"set_paired_peer_bound_callback\s*\(\s*&?\s*" + BOUND_FN + r"\s*\)",
                     code):
        problems.append(f"{BOUND_FN} is not installed with "
                        f"set_paired_peer_bound_callback")
    return problems


class PairBindWiring(unittest.TestCase):
    def setUp(self) -> None:
        self.src = MAIN.read_text(encoding="utf-8")

    def mutate(self, old: str, new: str) -> str:
        self.assertEqual(self.src.count(old), 1, f"mutation anchor not unique: {old!r}")
        return self.src.replace(old, new)

    def test_the_real_main_cpp_holds(self) -> None:
        self.assertEqual(check(self.src), [])

    def test_the_save_back_in_the_paired_callback_fails(self) -> None:
        # The code before F102: the address saved from register_paired_peer.
        src = self.mutate(
            "    const bool peer_set_ok  = mesh_session::register_trusted_peer(peer_pub);\n",
            "    const bool peer_set_ok  = mesh_session::register_trusted_peer(peer_pub);\n"
            "    uint8_t peer_mac[6];\n"
            "    if (mesh_session::get_paired_peer_mac(peer_mac)) {\n"
            "      uint8_t peer_fp[8];\n"
            "      mesh_crypto::compute_fingerprint(peer_pub, peer_fp);\n"
            "      mesh_state::save_peer_mac(peer_fp, peer_mac);\n"
            "    }\n")
        problems = check(src)
        self.assertTrue(any("touches the partner's address" in p for p in problems), problems)
        self.assertTrue(any("called 2 times" in p for p in problems), problems)

    def test_a_save_without_the_bound_check_fails(self) -> None:
        src = self.mutate("  if (!bound) {\n", "  if (false) {\n")
        self.assertTrue(any("without first returning" in p for p in check(src)))

    def test_a_bound_check_that_does_not_return_fails(self) -> None:
        body = _body(self.src, BOUND_FN)
        self.assertIsNotNone(body)
        assert body is not None
        self.assertEqual(body.count("    return;\n"), 1)
        src = self.src.replace(body, body.replace("    return;\n", "", 1))
        self.assertTrue(any("does not return" in p for p in check(src)))

    def test_the_callback_not_installed_fails(self) -> None:
        src = self.mutate(
            "mesh_session::set_paired_peer_bound_callback(&on_mesh_paired_peer_bound);",
            "")
        self.assertTrue(any("is not installed" in p for p in check(src)))

    def test_a_commented_out_install_does_not_count(self) -> None:
        src = self.mutate(
            "mesh_session::set_paired_peer_bound_callback(&on_mesh_paired_peer_bound);",
            "// mesh_session::set_paired_peer_bound_callback(&on_mesh_paired_peer_bound);")
        self.assertTrue(any("is not installed" in p for p in check(src)))


if __name__ == "__main__":
    unittest.main()
