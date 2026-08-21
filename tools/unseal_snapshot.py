#!/usr/bin/env python3
"""SecuraCV sealed-snapshot vault — off-device key generation and unlock.

The canary-wap device seals event-triggered camera frames (T3 smoke / T4 CO
/ glass break / Wi-Fi-sensing motion / mesh peer alarms, each opt-in and
OFF by default) into `.svlt` files on its SD
card, encrypted against an operator-held X25519 key. The device stores ONLY
the public key — it cannot decrypt what it wrote. This tool is the other
half: generate the keypair, inspect sealed files, and unseal them back into
viewable JPEGs.

Construction (mirrors vault_snapshot.cpp exactly):
  ephemeral X25519 keypair per snapshot
  shared  = X25519(ephemeral_priv, operator_pub)
  key     = HKDF-SHA256(salt = ephemeral_pub || operator_pub,
                        ikm = shared, info = b"securacv/vault/seal/v1", 32)
  cipher  = ChaCha20-Poly1305(key, nonce, aad = 64-byte header)
  file    = header(64) || ciphertext || tag(16)

Header layout (little-endian):
  0..3   magic "SVLT"        4..4   version (1)
  5..5   trigger (1=smoke, 2=co, 3=glass, 4=motion, 5=mesh, 9=test)
  6..6   time bucket (0..143 ten-minute buckets — the only time info stored)
  7..7   reserved
  8..15  recipient key id (first 8 bytes of SHA-256(operator_pub))
  16..47 ephemeral X25519 public key
  48..59 nonce
  60..63 ciphertext length (u32, excludes the 16-byte tag)

Usage:
  unseal_snapshot.py gen-key --out mykey            # writes mykey / mykey.pub
  unseal_snapshot.py inspect  sealed.svlt
  unseal_snapshot.py unseal   sealed.svlt --key mykey [--out photo.jpg]
  unseal_snapshot.py seal     photo.jpg --pub <64-hex> --out sealed.svlt
                              [--trigger test] [--bucket 0]   # test helper

Requires: pip install cryptography
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey,
    X25519PublicKey,
)
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

MAGIC = b"SVLT"
VERSION = 1
HEADER_SIZE = 64
TAG_SIZE = 16
MAX_CIPHERTEXT = 512 * 1024
HKDF_INFO = b"securacv/vault/seal/v1"
TRIGGERS = {1: "smoke", 2: "co", 3: "glass", 4: "motion", 5: "mesh", 9: "test"}
TRIGGER_BY_TAG = {v: k for k, v in TRIGGERS.items()}


def key_id_of(pub_raw: bytes) -> bytes:
    return hashlib.sha256(pub_raw).digest()[:8]


def build_header(trigger: int, bucket: int, key_id: bytes,
                 ephemeral_pub: bytes, nonce: bytes, ct_len: int) -> bytes:
    if trigger not in TRIGGERS:
        raise ValueError(f"unknown trigger {trigger}")
    if not 0 <= bucket <= 143:
        raise ValueError("time bucket out of range")
    if not 0 < ct_len <= MAX_CIPHERTEXT:
        raise ValueError("ciphertext length out of range")
    return (
        MAGIC
        + bytes([VERSION, trigger, bucket, 0])
        + key_id
        + ephemeral_pub
        + nonce
        + struct.pack("<I", ct_len)
    )


def parse_header(raw: bytes) -> dict:
    if len(raw) < HEADER_SIZE:
        raise ValueError("file shorter than a vault header")
    if raw[0:4] != MAGIC:
        raise ValueError("bad magic — not a .svlt vault file")
    if raw[4] != VERSION:
        raise ValueError(f"unsupported version {raw[4]}")
    trigger, bucket = raw[5], raw[6]
    if trigger not in TRIGGERS:
        raise ValueError(f"unknown trigger byte {trigger}")
    if bucket > 143:
        raise ValueError(f"time bucket {bucket} out of range")
    ct_len = struct.unpack("<I", raw[60:64])[0]
    if not 0 < ct_len <= MAX_CIPHERTEXT:
        raise ValueError(f"ciphertext length {ct_len} out of range")
    return {
        "trigger": trigger,
        "trigger_tag": TRIGGERS[trigger],
        "time_bucket": bucket,
        "key_id": raw[8:16],
        "ephemeral_pub": raw[16:48],
        "nonce": raw[48:60],
        "ct_len": ct_len,
    }


def derive_key(shared: bytes, ephemeral_pub: bytes, operator_pub: bytes) -> bytes:
    return HKDF(
        algorithm=hashes.SHA256(),
        length=32,
        salt=ephemeral_pub + operator_pub,
        info=HKDF_INFO,
    ).derive(shared)


def bucket_str(bucket: int) -> str:
    return f"{bucket} ({bucket * 10 // 60:02d}:{bucket * 10 % 60:02d}-ish local)"


def cmd_gen_key(args: argparse.Namespace) -> int:
    priv = X25519PrivateKey.generate()
    priv_raw = priv.private_bytes(
        serialization.Encoding.Raw,
        serialization.PrivateFormat.Raw,
        serialization.NoEncryption(),
    )
    pub_raw = priv.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )

    priv_path = args.out
    pub_path = args.out + ".pub"
    # Create the private key 0600 ATOMICALLY (os.open mode, not a post-hoc
    # chmod): under a permissive umask a plain open() would leave a window
    # where the file is world-readable — and this key unlocks every snapshot
    # sealed to it. O_EXCL doubles as the no-overwrite guard.
    # --force replaces an existing key. Unlink first, then create with O_EXCL,
    # so the new secret is never written into a pre-existing (possibly laxer,
    # possibly symlinked) file: the private key exists only at 0600 from the
    # instant of creation, closing the reuse-a-laxer-file window.
    if args.force:
        try:
            os.unlink(priv_path)
        except FileNotFoundError:
            pass
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(priv_path, flags, 0o600)
    except FileExistsError:
        print(f"refusing to overwrite {priv_path} (use --force)", file=sys.stderr)
        return 1
    with os.fdopen(fd, "w", encoding="ascii") as fh:
        fh.write(priv_raw.hex() + "\n")
    with open(pub_path, "w", encoding="ascii") as fh:
        fh.write(pub_raw.hex() + "\n")

    print("Vault unlock keypair generated.")
    print(f"  private key : {priv_path}   (KEEP OFFLINE — anyone with this")
    print("                file can view every snapshot this key seals)")
    print(f"  public key  : {pub_path}")
    print(f"  paste into the device dashboard: {pub_raw.hex()}")
    print(f"  key id      : {key_id_of(pub_raw).hex()}")
    return 0


def cmd_inspect(args: argparse.Namespace) -> int:
    data = open(args.file, "rb").read()
    h = parse_header(data[:HEADER_SIZE])
    ct = data[HEADER_SIZE:HEADER_SIZE + h["ct_len"]]
    expected = HEADER_SIZE + h["ct_len"] + TAG_SIZE
    print(f"file        : {args.file} ({len(data)} bytes, expected {expected})")
    print(f"trigger     : {h['trigger_tag']}")
    print(f"time bucket : {bucket_str(h['time_bucket'])}")
    print(f"key id      : {h['key_id'].hex()}")
    print(f"ciphertext  : {h['ct_len']} bytes")
    print(f"ct sha256   : {hashlib.sha256(ct).hexdigest()}")
    print(f"witness note: {hashlib.sha256(ct).hexdigest()[:16]}"
          "   <- compare to the frame_sealed event's note")
    if len(data) != expected:
        print("WARNING: file length mismatch — truncated or trailing data",
              file=sys.stderr)
        return 1
    return 0


def cmd_unseal(args: argparse.Namespace) -> int:
    data = open(args.file, "rb").read()
    h = parse_header(data[:HEADER_SIZE])
    body = data[HEADER_SIZE:]
    if len(body) != h["ct_len"] + TAG_SIZE:
        print("file length mismatch — truncated?", file=sys.stderr)
        return 1

    priv_hex = open(args.key, "r", encoding="ascii").read().strip()
    priv = X25519PrivateKey.from_private_bytes(bytes.fromhex(priv_hex))
    pub_raw = priv.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    if key_id_of(pub_raw) != h["key_id"]:
        print(
            f"key id mismatch: file was sealed for {h['key_id'].hex()}, "
            f"this key is {key_id_of(pub_raw).hex()}",
            file=sys.stderr,
        )
        return 1

    shared = priv.exchange(X25519PublicKey.from_public_bytes(h["ephemeral_pub"]))
    key = derive_key(shared, h["ephemeral_pub"], pub_raw)
    try:
        plain = ChaCha20Poly1305(key).decrypt(h["nonce"], body, data[:HEADER_SIZE])
    except Exception:
        print("DECRYPTION FAILED — wrong key, or the file was tampered with "
              "(the authentication tag did not verify).", file=sys.stderr)
        return 1

    out = args.out or (os.path.splitext(args.file)[0] + ".jpg")
    # The unsealed frame is raw witnessed media — the most sensitive artifact
    # this tool produces. Write it 0600 atomically (mode on os.open, not a
    # post-hoc chmod) so a permissive umask cannot leave a world-readable
    # window, and O_NOFOLLOW so a pre-planted symlink at `out` cannot redirect
    # the plaintext. O_TRUNC preserves the "overwrite the output" convenience.
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(out, flags, 0o600)
    except OSError as exc:
        print(f"refusing to write {out}: {exc}", file=sys.stderr)
        return 1
    with os.fdopen(fd, "wb") as fh:
        fh.write(plain)
    os.chmod(out, 0o600)  # enforce 0600 even if `out` already existed laxer
    print(f"unsealed {len(plain)} bytes -> {out}")
    print(f"trigger {h['trigger_tag']}, time bucket {bucket_str(h['time_bucket'])}")
    return 0


def cmd_seal(args: argparse.Namespace) -> int:
    """Test helper: seal a file exactly the way the firmware does."""
    operator_pub_raw = bytes.fromhex(args.pub)
    if len(operator_pub_raw) != 32:
        print("--pub must be 64 hex chars", file=sys.stderr)
        return 1
    plain = open(args.file, "rb").read()
    if not 0 < len(plain) <= MAX_CIPHERTEXT:
        print("input size out of range", file=sys.stderr)
        return 1

    eph = X25519PrivateKey.generate()
    eph_pub = eph.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    shared = eph.exchange(X25519PublicKey.from_public_bytes(operator_pub_raw))
    key = derive_key(shared, eph_pub, operator_pub_raw)
    nonce = os.urandom(12)
    header = build_header(
        TRIGGER_BY_TAG[args.trigger], args.bucket, key_id_of(operator_pub_raw),
        eph_pub, nonce, len(plain),
    )
    sealed = ChaCha20Poly1305(key).encrypt(nonce, plain, header)
    with open(args.out, "wb") as fh:
        fh.write(header + sealed)
    print(f"sealed {len(plain)} bytes -> {args.out}")
    print(f"ct sha256 prefix: {hashlib.sha256(sealed[:-TAG_SIZE]).hexdigest()[:16]}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("gen-key", help="generate an unlock keypair")
    g.add_argument("--out", default="vault_key", help="private key path")
    g.add_argument("--force", action="store_true")
    g.set_defaults(fn=cmd_gen_key)

    i = sub.add_parser("inspect", help="print a sealed file's header")
    i.add_argument("file")
    i.set_defaults(fn=cmd_inspect)

    u = sub.add_parser("unseal", help="decrypt a sealed snapshot")
    u.add_argument("file")
    u.add_argument("--key", required=True, help="private key file from gen-key")
    u.add_argument("--out", help="output JPEG (default: <file>.jpg)")
    u.set_defaults(fn=cmd_unseal)

    s = sub.add_parser("seal", help="test helper: seal a file firmware-style")
    s.add_argument("file")
    s.add_argument("--pub", required=True, help="operator public key (64 hex)")
    s.add_argument("--out", required=True)
    s.add_argument("--trigger", choices=sorted(TRIGGER_BY_TAG), default="test")
    s.add_argument("--bucket", type=int, default=0)
    s.set_defaults(fn=cmd_seal)

    args = p.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
