#!/usr/bin/env python3
"""Host tests for the sealed-snapshot vault's python half.

Run:  python3 tools/test_unseal_snapshot.py     (requires: pip install cryptography)

Covers:
  - GOLDEN_HEADER_HEX: byte-exact header build/parse against the SAME hex
    constant as firmware/projects/canary-wap/tests_host/test_vault_logic.cpp,
    pinning the .svlt layout across C++ and python.
  - seal -> unseal round-trip through the real X25519/HKDF/ChaCha20-Poly1305
    path (the exact construction vault_snapshot.cpp implements on-device).
  - Negatives: wrong private key, tampered ciphertext, tampered header
    (AAD) — including the reserved byte parse_header never looks at, so the
    AAD must be the file's own header bytes — truncated file, malformed
    headers.
  - tools/fixtures/vault/svlt_parity.json (tools/gen_svlt_parity.py): the
    committed fixture is fresh, carries this file's golden header, and its
    sealed bytes open with the fixed test-only key to the pinned plaintext —
    the same file ios SnapshotVaultTests opens with CryptoKit.

Prints "ALL unseal_snapshot TESTS PASSED" on success (CI marker).
"""

import hashlib
import json
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey,
    X25519PublicKey,
)
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.exceptions import InvalidTag

import gen_svlt_parity as gp
import unseal_snapshot as us

# Shared verbatim with test_vault_logic.cpp — trigger=T3(1), bucket=87,
# key_id=01..08, ephemeral=A0..BF, nonce=C0..CB, ct_len=128000. If the
# layout ever shifts, BOTH tests fail.
GOLDEN_HEADER_HEX = (
    "53564c54"  # "SVLT"
    "01"        # version
    "01"        # trigger t3 smoke
    "57"        # bucket 87
    "00"        # reserved
    "0102030405060708"
    "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
    "c0c1c2c3c4c5c6c7c8c9cacb"
    "00f40100"  # ct_len 128000 LE
)

_failures = 0


def check(cond: bool, what: str) -> None:
    global _failures
    if cond:
        print(f"  ok: {what}")
    else:
        _failures += 1
        print(f"  FAIL: {what}", file=sys.stderr)


def raises(fn, what: str) -> None:
    try:
        fn()
    except (ValueError, InvalidTag):
        check(True, what)
    else:
        check(False, what + " (no exception raised)")


def test_golden_header() -> None:
    print("golden header (cross-language byte layout)")
    golden = bytes.fromhex(GOLDEN_HEADER_HEX)
    check(len(golden) == us.HEADER_SIZE, "golden constant is exactly 64 bytes")

    built = us.build_header(
        trigger=1,
        bucket=87,
        key_id=bytes(range(0x01, 0x09)),
        ephemeral_pub=bytes(range(0xA0, 0xC0)),
        nonce=bytes(range(0xC0, 0xCC)),
        ct_len=128000,
    )
    check(built == golden, "build_header output matches the golden bytes")

    h = us.parse_header(golden)
    check(h["trigger"] == 1 and h["trigger_tag"] == "smoke", "trigger parses")
    check(h["time_bucket"] == 87, "time bucket parses")
    check(h["key_id"] == bytes(range(0x01, 0x09)), "key id parses")
    check(h["ephemeral_pub"] == bytes(range(0xA0, 0xC0)), "ephemeral pub parses")
    check(h["nonce"] == bytes(range(0xC0, 0xCC)), "nonce parses")
    check(h["ct_len"] == 128000, "ct_len parses (little-endian)")


def test_malformed_headers() -> None:
    print("malformed header rejects")
    golden = bytearray(bytes.fromhex(GOLDEN_HEADER_HEX))

    raises(lambda: us.parse_header(bytes(golden[:63])), "short buffer rejected")

    bad = bytearray(golden)
    bad[0] = ord("X")
    raises(lambda: us.parse_header(bytes(bad)), "bad magic rejected")

    bad = bytearray(golden)
    bad[4] = 2
    raises(lambda: us.parse_header(bytes(bad)), "unknown version rejected")

    bad = bytearray(golden)
    bad[5] = 6  # 4/5 are motion/mesh now; 6 is unused
    raises(lambda: us.parse_header(bytes(bad)), "unknown trigger rejected")

    bad = bytearray(golden)
    bad[6] = 144
    raises(lambda: us.parse_header(bytes(bad)), "bucket 144 rejected")

    bad = bytearray(golden)
    bad[60:64] = (0).to_bytes(4, "little")
    raises(lambda: us.parse_header(bytes(bad)), "zero ct_len rejected")

    bad = bytearray(golden)
    bad[60:64] = (us.MAX_CIPHERTEXT + 1).to_bytes(4, "little")
    raises(lambda: us.parse_header(bytes(bad)), "oversize ct_len rejected")

    # test trigger (9) is valid
    ok = bytearray(golden)
    ok[5] = 9
    check(us.parse_header(bytes(ok))["trigger_tag"] == "test",
          "test trigger (9) accepted")


def seal_blob(plain: bytes, operator_pub_raw: bytes,
              trigger: int = 9, bucket: int = 87) -> bytes:
    """Seal exactly the way cmd_seal / the firmware does; returns the file bytes."""
    eph = X25519PrivateKey.generate()
    eph_pub = eph.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    shared = eph.exchange(X25519PublicKey.from_public_bytes(operator_pub_raw))
    key = us.derive_key(shared, eph_pub, operator_pub_raw)
    nonce = hashlib.sha256(b"fixed-test-nonce" + plain).digest()[:12]
    header = us.build_header(trigger, bucket, us.key_id_of(operator_pub_raw),
                             eph_pub, nonce, len(plain))
    return header + ChaCha20Poly1305(key).encrypt(nonce, plain, header)


def unseal_blob(blob: bytes, priv: X25519PrivateKey) -> bytes:
    """Unseal exactly the way cmd_unseal does; raises on any failure."""
    h = us.parse_header(blob[:us.HEADER_SIZE])
    body = blob[us.HEADER_SIZE:]
    if len(body) != h["ct_len"] + us.TAG_SIZE:
        raise ValueError("file length mismatch")
    pub_raw = priv.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    if us.key_id_of(pub_raw) != h["key_id"]:
        raise ValueError("key id mismatch")
    shared = priv.exchange(X25519PublicKey.from_public_bytes(h["ephemeral_pub"]))
    key = us.derive_key(shared, h["ephemeral_pub"], pub_raw)
    return ChaCha20Poly1305(key).decrypt(h["nonce"], body, blob[:us.HEADER_SIZE])


def test_roundtrip_and_negatives() -> None:
    print("seal -> unseal round-trip + negatives")
    operator = X25519PrivateKey.generate()
    operator_pub = operator.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    plain = bytes(range(256)) * 300  # 76800 bytes, JPEG-ish size

    blob = seal_blob(plain, operator_pub)
    check(len(blob) == us.HEADER_SIZE + len(plain) + us.TAG_SIZE,
          "sealed file is header + ct + tag")

    h = us.parse_header(blob[:us.HEADER_SIZE])
    check(h["key_id"] == us.key_id_of(operator_pub),
          "header key id is SHA-256(operator_pub)[:8]")
    check(h["ct_len"] == len(plain), "ct_len equals plaintext length")

    ct = blob[us.HEADER_SIZE:us.HEADER_SIZE + h["ct_len"]]
    check(ct != plain[:len(ct)], "ciphertext differs from plaintext")

    out = unseal_blob(blob, operator)
    check(out == plain, "round-trip recovers the exact plaintext")

    # The witness-chain note: first 16 hex of SHA-256(ciphertext).
    note = hashlib.sha256(ct).hexdigest()[:16]
    check(len(note) == 16, "witness note is 16 hex chars of ct sha256")

    wrong = X25519PrivateKey.generate()
    raises(lambda: unseal_blob(blob, wrong), "wrong private key rejected")

    tampered = bytearray(blob)
    tampered[us.HEADER_SIZE + 100] ^= 0x01
    raises(lambda: unseal_blob(bytes(tampered), operator),
           "flipped ciphertext bit fails the tag")

    tampered = bytearray(blob)
    tampered[len(blob) - 1] ^= 0x01
    raises(lambda: unseal_blob(bytes(tampered), operator),
           "flipped tag bit fails to verify")

    # Header is the AAD: flipping the trigger byte must break decryption
    # even though the ciphertext and tag are untouched.
    tampered = bytearray(blob)
    tampered[5] = 3  # smoke -> glass (still a valid trigger, parses fine)
    raises(lambda: unseal_blob(bytes(tampered), operator),
           "tampered header (AAD) fails the tag")

    # The reserved byte (7) is not validated by parse_header, so it is ONLY
    # the AAD that catches it — which holds only when the AAD is the file's
    # own header bytes, never one rebuilt from the parsed fields (a rebuild
    # writes 0 there and would open this file). Every port owes this case;
    # ios SnapshotVaultTests pins it for the CryptoKit one.
    tampered = bytearray(blob)
    tampered[7] ^= 0x01
    check(us.parse_header(bytes(tampered[:us.HEADER_SIZE]))["trigger"] == h["trigger"],
          "reserved-byte flip still parses (so only the tag can catch it)")
    raises(lambda: unseal_blob(bytes(tampered), operator),
           "tampered reserved byte (AAD) fails the tag")

    raises(lambda: unseal_blob(blob[:-1], operator), "truncated file rejected")


def test_parity_fixture() -> None:
    print("cross-language parity fixture (tools/fixtures/vault/svlt_parity.json)")
    check(gp.GOLDEN_HEADER_HEX == GOLDEN_HEADER_HEX,
          "generator carries this file's golden header verbatim")
    try:
        with open(gp.OUT, "rb") as fh:
            on_disk = fh.read()
    except FileNotFoundError:
        check(False, "fixture exists (run python3 tools/gen_svlt_parity.py)")
        return
    check(on_disk == gp.render(), "committed fixture is fresh (gen_svlt_parity.py --check)")

    fx = json.loads(on_disk)
    check(fx["golden_header_hex"] == GOLDEN_HEADER_HEX, "fixture carries the golden header")
    blob = bytes.fromhex(fx["file_hex"])
    operator = X25519PrivateKey.from_private_bytes(bytes.fromhex(fx["operator_priv_hex"]))
    operator_pub = operator.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    check(operator_pub.hex() == fx["operator_pub_hex"], "operator public key matches")
    check(us.key_id_of(operator_pub).hex() == fx["key_id_hex"], "key id matches")

    h = us.parse_header(blob[:us.HEADER_SIZE])
    check(h["trigger"] == fx["trigger"] and h["trigger_tag"] == fx["trigger_tag"],
          "fixture header trigger")
    check(h["time_bucket"] == fx["bucket"], "fixture header bucket")
    check(h["ephemeral_pub"].hex() == fx["ephemeral_pub_hex"], "fixture header ephemeral pub")
    check(h["nonce"].hex() == fx["nonce_hex"], "fixture header nonce")
    check(blob[:us.HEADER_SIZE].hex() == fx["header_hex"], "fixture header bytes")

    plain = unseal_blob(blob, operator)
    check(hashlib.sha256(plain).hexdigest() == fx["plaintext_sha256"],
          "fixture unseals to the pinned plaintext sha256")
    check(plain.hex() == fx["plaintext_hex"] and len(plain) == fx["plaintext_len"],
          "fixture unseals to the pinned plaintext bytes")

    # The strongest pin: re-sealing with the fixed ephemeral key and nonce
    # must reproduce the file byte for byte — what the Swift side does too.
    eph = X25519PrivateKey.from_private_bytes(bytes.fromhex(fx["ephemeral_priv_hex"]))
    eph_pub = eph.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    shared = eph.exchange(X25519PublicKey.from_public_bytes(operator_pub))
    key = us.derive_key(shared, eph_pub, operator_pub)
    nonce = bytes.fromhex(fx["nonce_hex"])
    header = us.build_header(fx["trigger"], fx["bucket"], us.key_id_of(operator_pub),
                             eph_pub, nonce, len(plain))
    check(header + ChaCha20Poly1305(key).encrypt(nonce, plain, header) == blob,
          "re-sealing with the fixed ephemeral key + nonce reproduces file_hex")


def main() -> int:
    test_golden_header()
    test_malformed_headers()
    test_roundtrip_and_negatives()
    test_parity_fixture()
    if _failures:
        print(f"{_failures} FAILURE(S)", file=sys.stderr)
        return 1
    print("ALL unseal_snapshot TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
