#!/usr/bin/env python3
"""SecuraCV firmware release signing and manifest tool.

Single source of truth for the OTA release format shared by:
  - the pull-OTA engine (firmware/common/ota/) on Canary devices,
  - the BLE OTA path (firmware/projects/canary-wap/.../ble_ota.cpp),
  - the firmware-release GitHub Actions workflow,
  - the local mock/LAN server (firmware/projects/canary-ota/tools/mock_ota_server.py).

Signature scheme (identical to ble_ota.cpp so one key signs every channel):

    message   = image_size as uint32 little-endian (4 bytes)
              || sha256(firmware.bin)              (32 bytes)
    signature = Ed25519.sign(message)              (64 bytes)

Manifest signature (closes the hostile-mirror metadata-forgery window —
without it a local update server could forge release notes shown in Home
Assistant, URLs, and version metadata):

    msg = "scv-manifest-v1\0" + product + "\0" + version + "\0"
        + min_version + "\0" + url + "\0" + sha256hex + "\0"
        + str(size) + "\0" + release_notes + "\0" + release_url + "\0"
    manifest_signature = Ed25519.sign(msg.encode())   (64 bytes, same key)

Absent optional fields are empty strings. The NUL separators make the
encoding unambiguous (JSON strings cannot contain NUL), so the device
rebuilds the exact bytes from its parsed manifest without canonical-JSON
machinery. Keep in sync with securacv_ota_build_manifest_message() in
firmware/common/ota/src/securacv_ota.cpp.

Manifest schema v1 (per-variant flat JSON, one file per product):

    {
      "manifest_version": 1,
      "product": "securacv-canary",
      "version": "2.2.0",
      "min_version": "2.1.0",
      "url": "https://.../canary-2.2.0.bin",
      "sha256": "<64 hex>",
      "size": 1048576,
      "signature": "<128 hex>",
      "manifest_signature": "<128 hex>",
      "signing_key_id": "<first 16 hex of sha256(pubkey)>",
      "release_notes": "...",
      "release_url": "https://.../releases/tag/fw-v2.2.0"
    }

Commands:
    keygen          Generate an Ed25519 release keypair (PEM private key).
    pubkey-header   Emit ota_release_key.h contents for a keypair.
    sign            Print the signature for a firmware binary.
    manifest        Emit a signed per-variant manifest JSON.
    index           Emit a manifest-index.json pointing at per-variant manifests.
    verify          Verify a manifest + binary against a public key.

The private key must live OFF-DEVICE and outside the repository — on a
release engineer's machine or in the OTA_SIGNING_KEY_PEM CI secret.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)

MANIFEST_VERSION = 1
SEMVER_PARTS = 3


# ──────────────────────────────────────────────────────────────────────────
# Core format helpers (the canonical definitions — keep in sync with
# firmware/common/ota/src/securacv_ota.h and ble_ota.cpp)
# ──────────────────────────────────────────────────────────────────────────

def signed_message(image_size: int, sha256_digest: bytes) -> bytes:
    """Build the 36-byte message that is Ed25519-signed for a firmware image."""
    if len(sha256_digest) != 32:
        raise ValueError("sha256 digest must be 32 bytes")
    if not 0 < image_size <= 0xFFFFFFFF:
        raise ValueError("image_size out of range for uint32")
    return struct.pack("<I", image_size) + sha256_digest


def sign_firmware(private_key: Ed25519PrivateKey, firmware: bytes) -> bytes:
    """Return the 64-byte Ed25519 signature for a firmware image."""
    digest = hashlib.sha256(firmware).digest()
    return private_key.sign(signed_message(len(firmware), digest))


def verify_firmware(public_key: Ed25519PublicKey, firmware: bytes, signature: bytes) -> None:
    """Raise InvalidSignature if the signature doesn't match the image."""
    digest = hashlib.sha256(firmware).digest()
    public_key.verify(signature, signed_message(len(firmware), digest))


def manifest_signed_message(
    *,
    product: str,
    version: str,
    min_version: str,
    url: str,
    sha256_hex: str,
    size: int,
    release_notes: str,
    release_url: str,
) -> bytes:
    """Build the canonical byte string the manifest signature covers.

    Must stay byte-identical to securacv_ota_build_manifest_message() in
    firmware/common/ota/src/securacv_ota.cpp (cross-checked by the shared
    fixture in test_ota_release.py / test_ota_logic.cpp).
    """
    fields = [product, version, min_version, url, sha256_hex.lower(),
              str(size), release_notes, release_url]
    for f in fields:
        if "\0" in f:
            raise ValueError("manifest fields must not contain NUL")
    return b"scv-manifest-v1\0" + b"".join(f.encode() + b"\0" for f in fields)


def signing_key_id(public_key: Ed25519PublicKey) -> str:
    """Short stable identifier for a release key: first 16 hex of sha256(pubkey)."""
    raw = public_key.public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    return hashlib.sha256(raw).hexdigest()[:16]


def parse_semver(version: str) -> tuple[int, int, int]:
    """Parse MAJOR.MINOR.PATCH, allowing a variant suffix after a hyphen.

    The WAP firmware reports its version with a variant suffix (e.g.
    "2.2.0-wap") and the manifest must carry that exact string — the
    device compares it numerically (suffix ignored) and string-equality
    checks it after the install reboot. Mirror the firmware's parser:
    numeric core required, suffix tolerated.
    """
    core = version.split("-", 1)[0]
    parts = core.split(".")
    if len(parts) != SEMVER_PARTS or not all(p.isdigit() for p in parts):
        raise ValueError(
            f"version must be MAJOR.MINOR.PATCH with an optional -suffix, got {version!r}"
        )
    return tuple(int(p) for p in parts)  # type: ignore[return-value]


def build_manifest(
    *,
    private_key: Ed25519PrivateKey,
    firmware: bytes,
    product: str,
    version: str,
    url: str,
    min_version: str | None = None,
    release_notes: str | None = None,
    release_url: str | None = None,
) -> dict:
    """Build a signed manifest dict for one firmware binary."""
    parse_semver(version)
    if min_version is not None:
        parse_semver(min_version)
    digest = hashlib.sha256(firmware).hexdigest()
    signature = sign_firmware(private_key, firmware)
    manifest_sig = private_key.sign(manifest_signed_message(
        product=product,
        version=version,
        min_version=min_version or "",
        url=url,
        sha256_hex=digest,
        size=len(firmware),
        release_notes=release_notes or "",
        release_url=release_url or "",
    ))
    manifest = {
        "manifest_version": MANIFEST_VERSION,
        "product": product,
        "version": version,
        "url": url,
        "sha256": digest,
        "size": len(firmware),
        "signature": signature.hex(),
        "manifest_signature": manifest_sig.hex(),
        "signing_key_id": signing_key_id(private_key.public_key()),
    }
    if min_version:
        manifest["min_version"] = min_version
    if release_notes:
        manifest["release_notes"] = release_notes
    if release_url:
        manifest["release_url"] = release_url
    return manifest


def verify_manifest(manifest: dict, firmware: bytes, public_key: Ed25519PublicKey) -> list[str]:
    """Return a list of problems; empty list means the manifest verifies."""
    problems: list[str] = []
    for field in ("product", "version", "url", "sha256", "size", "signature"):
        if field not in manifest:
            problems.append(f"missing required field: {field}")
    if problems:
        return problems

    try:
        parse_semver(manifest["version"])
    except ValueError as exc:
        problems.append(str(exc))

    if manifest["size"] != len(firmware):
        problems.append(
            f"size mismatch: manifest says {manifest['size']}, binary is {len(firmware)}"
        )

    digest = hashlib.sha256(firmware).hexdigest()
    if manifest["sha256"].lower() != digest:
        problems.append("sha256 mismatch between manifest and binary")

    try:
        signature = bytes.fromhex(manifest["signature"])
    except ValueError:
        problems.append("signature is not valid hex")
        return problems
    if len(signature) != 64:
        problems.append(f"signature must be 64 bytes, got {len(signature)}")
        return problems

    try:
        verify_firmware(public_key, firmware, signature)
    except Exception:
        problems.append("Ed25519 signature verification failed")

    # Manifest signature: covers the metadata itself (version, URL, notes…)
    # so a hostile mirror cannot forge what users see or where devices fetch.
    msig_hex = manifest.get("manifest_signature")
    if not msig_hex:
        problems.append("missing required field: manifest_signature")
    else:
        try:
            msig = bytes.fromhex(msig_hex)
            # `or ""` (not a .get default): an explicit JSON null parses to
            # None and would crash manifest_signed_message.
            public_key.verify(msig, manifest_signed_message(
                product=manifest["product"],
                version=manifest["version"],
                min_version=manifest.get("min_version") or "",
                url=manifest["url"],
                sha256_hex=manifest["sha256"],
                size=manifest["size"],
                release_notes=manifest.get("release_notes") or "",
                release_url=manifest.get("release_url") or "",
            ))
        except Exception:
            problems.append("manifest_signature verification failed")

    key_id = manifest.get("signing_key_id")
    if key_id and key_id != signing_key_id(public_key):
        problems.append(
            f"signing_key_id mismatch: manifest says {key_id}, "
            f"key is {signing_key_id(public_key)}"
        )
    return problems


def pubkey_header(public_key: Ed25519PublicKey) -> str:
    """Render ota_release_key.h contents for a public key."""
    raw = public_key.public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    rows = []
    for i in range(0, 32, 8):
        rows.append("  " + ", ".join(f"0x{b:02x}" for b in raw[i : i + 8]) + ",")
    body = "\n".join(rows)
    return f"""/*
 * SecuraCV — Release Signing Public Key (Ed25519)
 *
 * The 32-byte Ed25519 public key whose private half signs every firmware
 * image accepted over the air (pull OTA and BLE OTA). The private key MUST
 * live off-device — on a release engineer's machine or in the
 * OTA_SIGNING_KEY_PEM CI secret; the device only needs the public half.
 *
 * If this key is all zeros (the default), OTA installs are HARD-DISABLED —
 * both the pull-OTA engine and the BLE OTA BEGIN handler refuse every
 * image. Generate and embed a real key with:
 *
 *     python firmware/scripts/ota_release.py keygen --private-key releaser.pem
 *     python firmware/scripts/ota_release.py pubkey-header --private-key releaser.pem
 *
 * Key id (sha256 of pubkey, first 16 hex): {signing_key_id(public_key)}
 *
 * Threat model: a stolen release private key permits firmware substitution.
 * Treat it like a code-signing certificate. Rotate by shipping a firmware
 * release (signed with the old key) that carries the new public key.
 */

#ifndef SECURACV_OTA_RELEASE_KEY_H
#define SECURACV_OTA_RELEASE_KEY_H

#include <stdint.h>

static const uint8_t SECURACV_OTA_RELEASE_PUBKEY[32] = {{
{body}
}};

#endif // SECURACV_OTA_RELEASE_KEY_H
"""


# ──────────────────────────────────────────────────────────────────────────
# Key loading
# ──────────────────────────────────────────────────────────────────────────

def load_private_key(path: Path) -> Ed25519PrivateKey:
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise SystemExit(f"{path} is not an Ed25519 private key")
    return key


def load_public_key(spec: str) -> Ed25519PublicKey:
    """Load a public key from a PEM file path, a 64-hex string, or an
    ota_release_key.h-style header file."""
    path = Path(spec)
    if path.exists():
        data = path.read_bytes()
        if b"-----BEGIN" in data:
            key = serialization.load_pem_public_key(data)
            if isinstance(key, Ed25519PublicKey):
                return key
            if isinstance(key, Ed25519PrivateKey):  # pragma: no cover - PEM mislabel
                return key.public_key()
            raise SystemExit(f"{path} is not an Ed25519 public key")
        if b"-----BEGIN" not in data and b"PRIVATE" not in data:
            # Try the C header format: collect 0xNN byte literals.
            text = data.decode("utf-8", errors="replace")
            hex_bytes = []
            for token in text.replace(",", " ").split():
                if token.startswith("0x") and len(token) == 4:
                    hex_bytes.append(int(token, 16))
            if len(hex_bytes) == 32:
                return Ed25519PublicKey.from_public_bytes(bytes(hex_bytes))
        raise SystemExit(f"could not parse a public key from {path}")
    # Bare hex string
    cleaned = spec.strip().lower()
    if len(cleaned) == 64 and all(c in "0123456789abcdef" for c in cleaned):
        return Ed25519PublicKey.from_public_bytes(bytes.fromhex(cleaned))
    raise SystemExit(f"public key spec not understood: {spec!r}")


# ──────────────────────────────────────────────────────────────────────────
# CLI commands
# ──────────────────────────────────────────────────────────────────────────

def cmd_keygen(args: argparse.Namespace) -> int:
    out = Path(args.private_key)
    if out.exists() and not args.force:
        raise SystemExit(f"{out} already exists (use --force to overwrite)")
    key = Ed25519PrivateKey.generate()
    pem = key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(pem)
    out.chmod(0o600)
    pub = key.public_key()
    raw = pub.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    print(f"Private key written to {out} (keep this OFF the repository)")
    print(f"Public key (hex): {raw.hex()}")
    print(f"Key id: {signing_key_id(pub)}")
    print("Embed with: ota_release.py pubkey-header --private-key", out)
    return 0


def cmd_pubkey_header(args: argparse.Namespace) -> int:
    key = load_private_key(Path(args.private_key))
    header = pubkey_header(key.public_key())
    if args.out:
        Path(args.out).write_text(header)
        print(f"Wrote {args.out}")
    else:
        print(header, end="")
    return 0


def cmd_sign(args: argparse.Namespace) -> int:
    key = load_private_key(Path(args.private_key))
    firmware = Path(args.firmware).read_bytes()
    print(sign_firmware(key, firmware).hex())
    return 0


def cmd_manifest(args: argparse.Namespace) -> int:
    key = load_private_key(Path(args.private_key))
    firmware = Path(args.firmware).read_bytes()
    manifest = build_manifest(
        private_key=key,
        firmware=firmware,
        product=args.product,
        version=args.version,
        url=args.url,
        min_version=args.min_version,
        release_notes=args.release_notes,
        release_url=args.release_url,
    )
    text = json.dumps(manifest, indent=2) + "\n"
    if args.out:
        Path(args.out).write_text(text)
        print(f"Wrote {args.out}")
    else:
        print(text, end="")
    return 0


def cmd_index(args: argparse.Namespace) -> int:
    products: dict[str, str] = {}
    for entry in args.entries:
        if "=" not in entry:
            raise SystemExit(f"index entries must be product=url, got {entry!r}")
        product, url = entry.split("=", 1)
        products[product] = url
    index = {"manifest_version": MANIFEST_VERSION, "products": products}
    text = json.dumps(index, indent=2) + "\n"
    if args.out:
        Path(args.out).write_text(text)
        print(f"Wrote {args.out}")
    else:
        print(text, end="")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    public_key = load_public_key(args.pubkey)
    manifest = json.loads(Path(args.manifest).read_text())
    firmware = Path(args.firmware).read_bytes()
    problems = verify_manifest(manifest, firmware, public_key)
    if problems:
        for problem in problems:
            print(f"FAIL: {problem}", file=sys.stderr)
        return 1
    print(
        f"OK: {manifest['product']} {manifest['version']} "
        f"({manifest['size']} bytes, key {manifest.get('signing_key_id', '?')})"
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("keygen", help="generate an Ed25519 release keypair")
    p.add_argument("--private-key", required=True, help="output PEM path")
    p.add_argument("--force", action="store_true", help="overwrite existing key")
    p.set_defaults(fn=cmd_keygen)

    p = sub.add_parser("pubkey-header", help="emit ota_release_key.h contents")
    p.add_argument("--private-key", required=True)
    p.add_argument("--out", help="write to file instead of stdout")
    p.set_defaults(fn=cmd_pubkey_header)

    p = sub.add_parser("sign", help="print signature hex for a firmware binary")
    p.add_argument("--private-key", required=True)
    p.add_argument("firmware")
    p.set_defaults(fn=cmd_sign)

    p = sub.add_parser("manifest", help="emit a signed per-variant manifest")
    p.add_argument("--private-key", required=True)
    p.add_argument("--product", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--url", required=True, help="download URL for the binary")
    p.add_argument("--min-version")
    p.add_argument("--release-notes")
    p.add_argument("--release-url")
    p.add_argument("--out", help="write to file instead of stdout")
    p.add_argument("firmware")
    p.set_defaults(fn=cmd_manifest)

    p = sub.add_parser("index", help="emit manifest-index.json")
    p.add_argument("entries", nargs="+", metavar="product=url")
    p.add_argument("--out", help="write to file instead of stdout")
    p.set_defaults(fn=cmd_index)

    p = sub.add_parser("verify", help="verify manifest + binary against a pubkey")
    p.add_argument("--pubkey", required=True, help="PEM path, 64-hex string, or ota_release_key.h path")
    p.add_argument("--manifest", required=True)
    p.add_argument("firmware")
    p.set_defaults(fn=cmd_verify)

    args = parser.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
