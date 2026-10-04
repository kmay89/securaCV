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

Since F118 the session refuses, before anything is sealed or installed, a
partner it could not hold (test_mesh_session.cpp:
test_a_partner_the_initiator_cannot_hold_fails_the_pairing and its
neighbors), and tells the FailedCallback why. main.cpp's half:

* on_mesh_pairing_failed opens with `if (why != ...PARTNER_REFUSED) { ...
  return; }` (no health log in it), then logs the refusal, naming the
  partner, in one unguarded log_health call with no return before it;
* it is installed with set_failed_callback.

And the boot restore of the stored addresses (F119, F120;
test_boot_restore_binds_neither_member_of_a_shared_address and
test_boot_restore_drops_entries_of_peers_no_longer_trusted run the
stand-in, and test_stored_mac_must_drop_truth_table holds the drop
decision for every verdict and both peers_loaded values):

* `peers_loaded` is load_trusted_peers' result, assigned once, and under
  `if (peers_loaded) {` setup registers the stored pubkeys
  (register_trusted_peer) before it hands the stored addresses, copied
  from the loaded entries, to mesh_session::restore_peer_macs; main.cpp
  binds no address by hand (no bind_peer_mac call);
* the drop loop is exactly `for (size_t i = 0; i < n_macs; ++i) { if
  (!mesh_session::stored_mac_must_drop(verdicts[i], peers_loaded))
  continue; ... remove_peer_mac(mac_fps[i]);` (the one remove_peer_mac
  call), so it drops what the host-tested helper says and nothing else,
  and a SHARED address goes to the health log.

These forms are pinned token for token, not by the presence of a token: an
inverted `!=`, a `continue` turned into `{}`, `true` for `peers_loaded` each
keep every token and turn the code into its opposite. Each rule is shown to
fail on a mutation of the real file, so the pin cannot read as covered while
catching nothing.

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
FAILED_FN = "on_mesh_pairing_failed"


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


_TOKEN = re.compile(r"\w+|::|->|\+\+|&&|\|\||[!=<>]=|\S")


def _seq(snippet: str) -> str:
    """A regex for `snippet`'s exact token sequence, whitespace-insensitive.

    The drop loop and the refusal branch are pinned by their exact form,
    not by the presence of a token: `!=` for `==`, a `continue` for `{}`,
    `true` for `peers_loaded` are each one token, and each turns the boot
    restore or the log into its opposite while every token is still there
    (the review of F118-F120 found the presence checks passing all of
    those)."""
    toks = _TOKEN.findall(snippet)
    out = []
    for i, t in enumerate(toks):
        if i:
            word = re.match(r"\w", toks[i - 1][-1]) and re.match(r"\w", t[0])
            out.append(r"\s+" if word else r"\s*")
        out.append(re.escape(t))
    return "".join(out)


def _block_at(code: str, brace: int) -> str:
    """The brace-matched block whose `{` is at `brace`."""
    depth = 0
    for i in range(brace, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[brace : i + 1]
    return code[brace:]


# The boot restore, as main.cpp's setup must say it (F119, F120). Which
# entries are dropped is mesh_session::stored_mac_must_drop's call, which
# test_mesh_session holds for every verdict and both peers_loaded values; the
# loop asks it, drops on its yes and does nothing else on its no.
REGISTER = _seq(
    "const bool peers_loaded = mesh_state::load_trusted_peers(peers_buf, sizeof(peers_buf), "
    "&peers_count); if (peers_loaded) {")
REGISTER_LOOP = _seq(
    "for (size_t i = 0; i < peers_count; ++i) { if (mesh_session::register_trusted_peer("
    "peers_buf + i * mesh_crypto::PUBKEY_LEN)) {")
COPY_LOOP = _seq(
    "for (size_t i = 0; i < n_macs; ++i) { memcpy(mac_fps[i], macs[i].fingerprint, "
    "sizeof(mac_fps[i])); memcpy(mac_addrs[i], macs[i].mac, sizeof(mac_addrs[i])); }")
RESTORE = _seq(
    "const size_t bound = mesh_session::restore_peer_macs(mac_fps, mac_addrs, n_macs, verdicts);")
DROP_LOOP = _seq(
    "for (size_t i = 0; i < n_macs; ++i) { if (!mesh_session::stored_mac_must_drop(verdicts[i], "
    "peers_loaded)) continue; const bool dropped = mesh_state::remove_peer_mac(mac_fps[i]);")
SHARED_BRANCH = _seq("if (verdicts[i] == mesh_session::StoredMacVerdict::SHARED) {")

# The refusal branch of the FailedCallback (F118): every other reason
# returns first, and the one health-log line follows, unguarded.
FAILED_SIG = _seq(
    "static void on_mesh_pairing_failed(mesh_pairing::FailReason why, const uint8_t* partner_fp) {")
FAILED_EARLY = _seq("if (why != mesh_pairing::FailReason::PARTNER_REFUSED) {")
FAILED_HEX = _seq("if (partner_fp != nullptr) mesh_fp_hex(partner_fp, hex);")


def _failed_callback_problems(code: str) -> list[str]:
    sig = list(re.finditer(FAILED_SIG, code))
    if len(sig) != 1:
        return [f"{FAILED_FN} is missing, or not "
                f"`{FAILED_FN}(mesh_pairing::FailReason why, const uint8_t* partner_fp)`"]
    body = _block_at(code, sig[0].end() - 1)
    early = re.match(r"\{\s*" + FAILED_EARLY, body)
    if early is None:
        return [f"{FAILED_FN} does not open with "
                f"`if (why != mesh_pairing::FailReason::PARTNER_REFUSED) {{`: it does not "
                f"log a PARTNER_REFUSED, and only it, to the health log"]
    problems: list[str] = []
    block = _block_at(body, early.end() - 1)
    if not re.search(r"\breturn\s*;\s*\}$", block):
        problems.append(f"{FAILED_FN}'s other-reasons branch does not end in `return;`")
    if re.search(r"\blog_health\s*\(", block):
        problems.append(f"{FAILED_FN} logs a reason other than PARTNER_REFUSED to the "
                        f"health log")
    rest_at = early.end() - 1 + len(block)
    rest = body[rest_at:]
    logs = [m.start() for m in re.finditer(r"\blog_health\s*\(", rest)]
    if len(logs) != 1:
        problems.append(f"{FAILED_FN} does not log a PARTNER_REFUSED to the health log "
                        f"(log_health called {len(logs)} times after the early return)")
    else:
        before = rest[: logs[0]].rstrip()
        depth = before.count("{") - before.count("}")
        if not before.endswith(";") or depth != 0:
            problems.append(f"{FAILED_FN}'s refusal log is guarded (it must be a statement "
                            f"of its own, after the early return)")
        if re.search(r"\breturn\b", before):
            problems.append(f"{FAILED_FN} returns before it logs a PARTNER_REFUSED")
        if not re.search(FAILED_HEX, before):
            problems.append(f"{FAILED_FN} does not name the partner it logs "
                            f"(`if (partner_fp != nullptr) mesh_fp_hex(partner_fp, hex);`)")
    return problems


def _boot_restore_problems(code: str) -> list[str]:
    problems: list[str] = []
    if re.search(r"\bbind_peer_mac\s*\(", code):
        problems.append("main.cpp binds a stored address by hand (bind_peer_mac); "
                        "the boot restore is restore_peer_macs")
    defs = re.findall(r"\bpeers_loaded\s*=(?!=)", code)
    if len(defs) != 1:
        problems.append(f"peers_loaded is assigned {len(defs)} times; expected once, from "
                        f"load_trusted_peers")
    reg = list(re.finditer(REGISTER, code))
    if len(reg) != 1:
        problems.append("peers_loaded is not the result of load_trusted_peers, followed by "
                        "`if (peers_loaded) {` (the stored pubkeys are not registered when "
                        "the list was read)")
    elif not re.search(REGISTER_LOOP, _block_at(code, reg[0].end() - 1)):
        problems.append("the stored pubkeys are not registered before restore_peer_macs "
                        "(no register_trusted_peer loop under `if (peers_loaded)`)")
    restore = list(re.finditer(RESTORE, code))
    if not re.search(r"\brestore_peer_macs\s*\(", code):
        problems.append("the boot restore does not call restore_peer_macs")
    elif len(restore) != 1:
        problems.append("restore_peer_macs is not called once as "
                        "`restore_peer_macs(mac_fps, mac_addrs, n_macs, verdicts)`")
    elif reg and restore[0].start() < reg[0].start():
        problems.append("restore_peer_macs runs before the stored pubkeys are registered "
                        "(not registered before)")
    copy = list(re.finditer(COPY_LOOP, code))
    if len(copy) != 1 or (restore and copy[0].start() > restore[0].start()):
        problems.append("mac_fps/mac_addrs are not filled from the stored entries before "
                        "restore_peer_macs")
    removes = [m.start() for m in re.finditer(r"\bremove_peer_mac\s*\(", code)]
    drop = list(re.finditer(DROP_LOOP, code))
    if len(removes) != 1:
        problems.append(f"remove_peer_mac is called {len(removes)} times; expected once, "
                        f"in the boot restore")
    if len(drop) != 1:
        problems.append("the boot restore does not drop exactly what "
                        "mesh_session::stored_mac_must_drop(verdicts[i], peers_loaded) says: "
                        "the loop must be `for (size_t i = 0; i < n_macs; ++i) { if "
                        "(!mesh_session::stored_mac_must_drop(verdicts[i], peers_loaded)) "
                        "continue; const bool dropped = "
                        "mesh_state::remove_peer_mac(mac_fps[i]);`")
    elif restore and drop[0].start() < restore[0].start():
        problems.append("remove_peer_mac runs before restore_peer_macs")
    else:
        loop = _block_at(code, code.index("{", drop[0].start()))
        shared = re.search(SHARED_BRANCH, loop)
        if shared is None or not re.search(r"\blog_health\s*\(",
                                           _block_at(loop, shared.end() - 1)):
            problems.append("the boot restore does not log a SHARED address to the health "
                            "log (`if (verdicts[i] == mesh_session::StoredMacVerdict::SHARED) "
                            "{ ... log_health(...) }`)")
    return problems


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

    # F118: a refused pairing reaches the health log, and nothing else does.
    problems += _failed_callback_problems(code)
    if not re.search(r"set_failed_callback\s*\(\s*&?\s*" + FAILED_FN + r"\s*\)", code):
        problems.append(f"{FAILED_FN} is not installed with set_failed_callback")

    # F119, F120: the boot restore.
    problems += _boot_restore_problems(code)
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


    def test_the_failed_callback_not_installed_fails(self) -> None:
        src = self.mutate(
            "mesh_session::set_failed_callback(&on_mesh_pairing_failed);", "")
        self.assertTrue(any(f"{FAILED_FN} is not installed" in p for p in check(src)))

    def test_a_refusal_not_logged_fails(self) -> None:
        body = _body(self.src, FAILED_FN)
        self.assertIsNotNone(body)
        assert body is not None
        self.assertEqual(body.count("  log_health("), 1)
        src = self.src.replace(body, body.replace("  log_health(", "  (void)(", 1))
        self.assertTrue(any("does not log a PARTNER_REFUSED" in p for p in check(src)))

    def test_the_old_blob_order_bind_loop_fails(self) -> None:
        # The code before F119/F120: every stored entry bound in blob order.
        src = self.mutate(
            "              mesh_session::restore_peer_macs(mac_fps, mac_addrs, n_macs, verdicts);",
            "              0;\n"
            "          for (size_t i = 0; i < n_macs; ++i) "
            "mesh_session::bind_peer_mac(mac_fps[i], mac_addrs[i]);")
        problems = check(src)
        self.assertTrue(any("by hand" in p for p in problems), problems)
        self.assertTrue(any("does not call restore_peer_macs" in p for p in problems), problems)

    def test_restore_before_the_pubkeys_fails(self) -> None:
        src = self.mutate(
            "          if (mesh_session::register_trusted_peer(\n"
            "                  peers_buf + i * mesh_crypto::PUBKEY_LEN)) {",
            "          if (peers_buf[i]) {")
        self.assertTrue(any("not registered before" in p for p in check(src)))

    # The boot restore's drop loop and the refusal log, by their exact form.
    # Each mutation below keeps every token the presence checks looked for
    # (StoredMacVerdict::SHARED/UNTRUSTED, peers_loaded, PARTNER_REFUSED,
    # log_health) and turns the code into its opposite; the review of
    # F118-F120 found each one passing the pin as it stood.

    DROP_GUARD = ("            if (!mesh_session::stored_mac_must_drop(verdicts[i], "
                  "peers_loaded)) continue;\n")

    def assert_drop_caught(self, src: str) -> None:
        problems = check(src)
        self.assertTrue(any("does not drop exactly what" in p for p in problems), problems)

    def test_a_drop_of_bound_entries_fails(self) -> None:
        # The guard inverted: every BOUND and REFUSED entry is dropped at
        # every boot (every member unheard after the next reboot) and the
        # SHARED and UNTRUSTED ones are kept.
        self.assert_drop_caught(self.mutate(
            "if (!mesh_session::stored_mac_must_drop(", "if (mesh_session::stored_mac_must_drop("))

    def test_a_drop_guard_that_does_not_skip_fails(self) -> None:
        # `continue` gone: every stored address is dropped at every boot.
        self.assert_drop_caught(self.mutate(
            "verdicts[i], peers_loaded)) continue;", "verdicts[i], peers_loaded)) {}"))

    def test_a_dead_drop_guard_fails(self) -> None:
        self.assert_drop_caught(self.mutate(
            self.DROP_GUARD,
            "            (void)mesh_session::stored_mac_must_drop(verdicts[i], peers_loaded);\n"))

    def test_a_drop_without_the_pubkey_list_fails(self) -> None:
        # A failed pubkey read registers nobody, so every entry is UNTRUSTED.
        self.assert_drop_caught(self.mutate(
            "stored_mac_must_drop(verdicts[i], peers_loaded)",
            "stored_mac_must_drop(verdicts[i], true)"))

    def test_a_neutralized_pubkey_gate_fails(self) -> None:
        self.assert_drop_caught(self.mutate(
            "stored_mac_must_drop(verdicts[i], peers_loaded)",
            "stored_mac_must_drop(verdicts[i], peers_loaded || n_macs)"))

    def test_a_drop_only_when_the_list_was_not_read_fails(self) -> None:
        # The loop run only in the case the gate exists for.
        self.assert_drop_caught(self.mutate(
            "          for (size_t i = 0; i < n_macs; ++i) {\n" + self.DROP_GUARD,
            "          for (size_t i = 0; i < n_macs && !peers_loaded; ++i) {\n"
            + self.DROP_GUARD))

    def test_untrusted_entries_kept_fails(self) -> None:
        # F120 reverted in main.cpp: UNTRUSTED entries skipped.
        self.assert_drop_caught(self.mutate(
            self.DROP_GUARD,
            self.DROP_GUARD +
            "            if (verdicts[i] == mesh_session::StoredMacVerdict::UNTRUSTED) "
            "continue;\n"))

    def test_a_shadowed_peers_loaded_fails(self) -> None:
        src = self.mutate(
            "          mesh_session::StoredMacVerdict verdicts[mesh_state::MAX_TRUSTED_PEERS];\n",
            "          mesh_session::StoredMacVerdict verdicts[mesh_state::MAX_TRUSTED_PEERS];\n"
            "          const bool peers_loaded = true;\n")
        self.assertTrue(any("peers_loaded is assigned 2 times" in p for p in check(src)))

    def test_pubkeys_registered_only_without_the_list_fails(self) -> None:
        src = self.mutate("      if (peers_loaded) {\n        size_t registered = 0;",
                          "      if (!peers_loaded) {\n        size_t registered = 0;")
        problems = check(src)
        self.assertTrue(any("if (peers_loaded) {" in p for p in problems), problems)

    def test_a_shared_address_not_logged_fails(self) -> None:
        src = self.mutate(
            "if (verdicts[i] == mesh_session::StoredMacVerdict::SHARED) {",
            "if (verdicts[i] != mesh_session::StoredMacVerdict::SHARED) {")
        self.assertTrue(any("does not log a SHARED address" in p for p in check(src)))

    def test_a_restore_of_other_arrays_fails(self) -> None:
        src = self.mutate("memcpy(mac_fps[i], macs[i].fingerprint, sizeof(mac_fps[i]));",
                          "memcpy(mac_fps[i], macs[0].fingerprint, sizeof(mac_fps[i]));")
        self.assertTrue(any("are not filled from the stored entries" in p for p in check(src)))

    def test_the_refusal_test_inverted_fails(self) -> None:
        # Every timeout and cancel logged as "Opera pairing refused", and the
        # refusals sent to Serial only.
        src = self.mutate("if (why != mesh_pairing::FailReason::PARTNER_REFUSED) {",
                          "if (why == mesh_pairing::FailReason::PARTNER_REFUSED) {")
        self.assertTrue(any("does not open with" in p for p in check(src)))

    def test_a_return_before_the_refusal_log_fails(self) -> None:
        src = self.mutate(
            "  char hex[mesh_crypto::FINGERPRINT_LEN * 2 + 1] = \"?\";\n",
            "  return;\n  char hex[mesh_crypto::FINGERPRINT_LEN * 2 + 1] = \"?\";\n")
        self.assertTrue(any("returns before it logs" in p for p in check(src)))

    def test_a_guarded_refusal_log_fails(self) -> None:
        src = self.mutate(
            "  log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, \"Opera pairing refused\", hex);",
            "  if (partner_fp == nullptr) log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "
            "\"Opera pairing refused\", hex);")
        self.assertTrue(any("refusal log is guarded" in p for p in check(src)))

    def test_a_refusal_logged_without_its_partner_fails(self) -> None:
        # Inverted, it reads a null fingerprint and logs "?" for a real one.
        src = self.mutate("  if (partner_fp != nullptr) mesh_fp_hex(partner_fp, hex);",
                          "  if (partner_fp == nullptr) mesh_fp_hex(partner_fp, hex);")
        self.assertTrue(any("does not name the partner" in p for p in check(src)))

    def test_an_other_reasons_branch_that_falls_through_fails(self) -> None:
        body = _body(self.src, FAILED_FN)
        self.assertIsNotNone(body)
        assert body is not None
        self.assertEqual(body.count("    return;\n"), 1)
        src = self.src.replace(body, body.replace("    return;\n", "", 1))
        self.assertTrue(any("does not end in `return;`" in p for p in check(src)))

    def test_other_reasons_logged_fails(self) -> None:
        src = self.mutate(
            "    Serial.printf(\"[INFO] Opera pairing ended: %s\\n\", "
            "mesh_pairing::fail_reason_name(why));\n",
            "    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, \"Opera pairing ended\", "
            "mesh_pairing::fail_reason_name(why));\n")
        self.assertTrue(any("logs a reason other than PARTNER_REFUSED" in p
                            for p in check(src)))

if __name__ == "__main__":
    unittest.main()
