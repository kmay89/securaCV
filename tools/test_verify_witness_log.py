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
  - a torn final line (power cut) is tolerated.

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
