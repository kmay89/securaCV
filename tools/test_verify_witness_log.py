#!/usr/bin/env python3
"""Host tests for tools/verify_witness_log.py.

Run:  python3 tools/test_verify_witness_log.py   (requires: pip install cryptography)

Builds a real Ed25519-signed chain with the exact device construction
(sha256_domain / big-endian chain buffer), writes it as records.jsonl
lines, and checks:
  - a clean log verifies end-to-end, anchored to genesis;
  - every tamper class fails loudly: edited field, wrong key, re-signed
    line, reordered lines;
  - a gap (card-absent period) is reported as segments but still passes;
  - a torn final line (power cut) is tolerated;
  - a power-cut scar mid-file (a torn append the device sealed onto its
    own line, with the chain continuing contiguously across it) is
    tolerated and named; a malformed line at a gap boundary is reported
    as the gap it is (a destroyed record reads exactly like a deleted
    one, which the gap already reports); a malformed line the records
    around it cannot vouch for (a backward sequence, no record after it,
    a leading line the genesis does not bridge) fails.

Prints "ALL verify_witness_log TESTS PASSED" on success (CI marker).
"""

import contextlib
import io
import json
import os
import sys
import tempfile

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify_witness_log as vw  # noqa: E402

_failures = 0


def check(cond: bool, what: str) -> None:
    global _failures
    if cond:
        print(f"  ok: {what}")
    else:
        _failures += 1
        print(f"  FAIL: {what}", file=sys.stderr)


def build_chain(priv: Ed25519PrivateKey, device_id: str, n: int):
    """Build n signed records exactly the way the firmware does."""
    prev = vw.sha256_domain(vw.DOMAIN_GENESIS, device_id.encode())
    lines = []
    for seq in range(1, n + 1):
        ph = vw.hashlib.sha256(f"payload {seq}".encode()).digest()
        tb = seq % 144
        ch = vw.chain_hash(prev, ph, seq, tb)
        sig = priv.sign(ch)
        lines.append(json.dumps({
            "v": 1, "seq": seq, "tb": tb, "type": 1,
            "ph": ph.hex(), "prev": prev.hex(),
            "ch": ch.hex(), "sig": sig.hex(),
        }) + "\n")
        prev = ch
    return lines


def run_verify(lines, pubkey_hex, device_id=None):
    """Write lines to a temp file, run verify, return (exit_code, stdout+err)."""
    with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
        f.write("".join(lines))
        path = f.name
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = vw.verify(path, pubkey_hex, device_id)
    finally:
        os.unlink(path)
    return code, out.getvalue()


def main() -> int:
    priv = Ed25519PrivateKey.generate()
    pub_hex = priv.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()
    device_id = "canary-testdevice"
    lines = build_chain(priv, device_id, 6)

    print("clean chain")
    code, out = run_verify(lines, pub_hex, device_id)
    check(code == 0, "clean log verifies")
    check("chains from genesis" in out, "genesis anchor recognized")
    check("signatures verified : 6/6" in out, "all signatures verified")

    print("tampered field")
    bad = list(lines)
    rec = json.loads(bad[2])
    rec["tb"] = (rec["tb"] + 1) % 144  # edit one field, keep stored hash
    bad[2] = json.dumps(rec) + "\n"
    code, out = run_verify(bad, pub_hex)
    check(code == 1, "edited record fails")
    check("does not match its own fields" in out, "edit is named precisely")

    print("wrong public key")
    other = Ed25519PrivateKey.generate().public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()
    code, out = run_verify(lines, other)
    check(code == 1, "another device's key fails every signature")

    print("re-signed line (forged history)")
    forged = list(lines)
    rec = json.loads(forged[3])
    attacker = Ed25519PrivateKey.generate()
    rec["sig"] = attacker.sign(bytes.fromhex(rec["ch"])).hex()
    forged[3] = json.dumps(rec) + "\n"
    code, out = run_verify(forged, pub_hex)
    check(code == 1, "attacker-signed line fails")
    check("signature invalid" in out, "signature failure is named")

    print("reordered lines")
    swapped = list(lines)
    swapped[1], swapped[2] = swapped[2], swapped[1]
    code, out = run_verify(swapped, pub_hex)
    check(code == 1, "reordered log fails")

    print("gap (card-absent period)")
    gappy = lines[:2] + lines[4:]  # drop seq 3-4
    code, out = run_verify(gappy, pub_hex, device_id)
    check(code == 0, "gapped log still verifies per segment")
    check("gap(s)" in out, "the gap is reported, not hidden")

    print("torn final line (power cut)")
    torn = list(lines)
    torn[-1] = torn[-1][: len(torn[-1]) // 2]  # no trailing newline
    code, out = run_verify(torn, pub_hex, device_id)
    check(code == 0, "torn tail tolerated")
    check("torn final line" in out, "torn tail is noted")

    print("complete final line missing only its newline")
    noeol = list(lines)
    noeol[-1] = noeol[-1].rstrip("\n")  # full record, no terminator
    code, out = run_verify(noeol, pub_hex, device_id)
    check(code == 0, "unterminated-but-complete final record verifies")
    check("signatures verified : 6/6" in out,
          "the final record was NOT skipped as torn")
    # ...and if that final record is tampered, deleting the newline must
    # not hide it from verification.
    evil = list(noeol)
    rec = json.loads(evil[-1])
    rec["tb"] = (rec["tb"] + 1) % 144
    evil[-1] = json.dumps(rec)
    code, out = run_verify(evil, pub_hex, device_id)
    check(code == 1, "tampered newline-stripped final record still fails")

    print("power-cut scar mid-file (torn append sealed onto its own line)")
    # Records 1-3 landed; the append of a fourth record tore; the device
    # rebooted, recovered head = seq 3, sealed the fragment with a newline
    # and appended its NEW seq 4 chaining from seq 3 — exactly lines[3:].
    fragment = lines[3][: len(lines[3]) // 2] + "\n"
    scarred = lines[:3] + [fragment] + lines[3:]
    code, out = run_verify(scarred, pub_hex, device_id)
    check(code == 0, "a sealed scar bridged by a contiguous chain is tolerated")
    check("power-cut scar" in out, "the scar is named, not hidden")
    check("1 malformed line(s) tolerated" in out, "the count is printed")
    check("signatures verified : 6/6" in out, "every real record still verified")
    check("seq 1..6 (chains from genesis)" in out,
          "the scar does not split the segment")

    print("two consecutive scars (a second power cut during the sealing append)")
    twice = lines[:3] + [fragment, lines[3][: 40] + "\n"] + lines[3:]
    code, out = run_verify(twice, pub_hex, device_id)
    check(code == 0, "both fragments are bridged by the same contiguous link")
    check("2 malformed line(s) tolerated" in out, "both scars counted")
    check(out.count("power-cut scar") == 2, "both named as scars")

    print("torn first append, chain starts at genesis after it")
    first_torn = [lines[0][: 30] + "\n"] + lines
    code, out = run_verify(first_torn, pub_hex, device_id)
    check(code == 0, "a scar before seq 1 is bridged by the genesis anchor")
    code, out = run_verify(first_torn, pub_hex, None)
    check(code == 0, "without the genesis, a leading scar is noted, not judged")
    check("cannot be judged without --device-id" in out,
          "the note says which run decides it")
    wrong_genesis = [lines[0][: 30] + "\n"] + lines[1:]  # seq 2 first: no bridge
    code, out = run_verify(wrong_genesis, pub_hex, device_id)
    check(code == 1, "a leading malformed line that genesis does not bridge fails")

    print("garbage in place of a record reads as the gap it is")
    overwritten = list(lines)
    overwritten[2] = lines[2][: len(lines[2]) // 2] + "\n"  # seq 3 destroyed
    code, out = run_verify(overwritten, pub_hex, device_id)
    check(code == 0, "same verdict as deleting the line outright")
    check("gap boundary" in out, "the line is named as a gap boundary, not a scar")
    check("destroyed record" in out, "the note says a destroyed record is possible")
    check("1 gap(s)" in out, "the gap itself is reported as before")
    check("signatures verified : 5/5" in out, "the surviving records still verify")

    print("a scar followed by a card-absent gap (power cut, then no card)")
    scar_gap = lines[:3] + [fragment] + lines[4:]  # seq 4 never reached the card
    code, out = run_verify(scar_gap, pub_hex, device_id)
    check(code == 0, "an honest crash-then-no-card log is not a failure")
    check("gap boundary" in out, "reported as a gap boundary")

    print("a malformed line between a backward sequence is NOT a scar")
    replayed = lines[:4] + [fragment] + [lines[1]]  # seq 2 again after seq 4
    code, out = run_verify(replayed, pub_hex, device_id)
    check(code == 1, "the backward sequence fails, fragment or not")
    check("does not advance" in out, "the failure names the backward sequence")

    print("a scar as the last complete line before a torn tail")
    scar_then_torn = lines[:3] + [fragment] + lines[3:5] + [lines[5][: 50]]
    code, out = run_verify(scar_then_torn, pub_hex, device_id)
    check(code == 0, "scar and torn tail coexist (two power cuts, one file)")

    print("unreadable file")
    code = vw.verify("/nonexistent/records.jsonl", pub_hex, None)
    check(code == 1, "missing file reports cleanly instead of crashing")

    if _failures:
        print(f"{_failures} FAILURE(S)", file=sys.stderr)
        return 1
    print("ALL verify_witness_log TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
