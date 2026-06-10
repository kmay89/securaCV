"""Tests for ota_release.py — the OTA release signing and manifest tool.

Run with: pytest firmware/scripts/test_ota_release.py -q
"""

import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

import pytest
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

sys.path.insert(0, str(Path(__file__).parent))

import ota_release  # noqa: E402

FIRMWARE = b"\xe9SecuraCV test firmware image\x00" * 64


@pytest.fixture()
def private_key():
    return Ed25519PrivateKey.generate()


@pytest.fixture()
def key_pem(tmp_path, private_key):
    from cryptography.hazmat.primitives import serialization

    path = tmp_path / "releaser.pem"
    path.write_bytes(
        private_key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    return path


# ──────────────────────────────────────────────────────────────────────────
# Signature scheme: must match ble_ota.cpp — (image_size_LE32 || sha256)
# ──────────────────────────────────────────────────────────────────────────

class TestSignatureScheme:
    def test_signed_message_layout(self):
        digest = hashlib.sha256(FIRMWARE).digest()
        msg = ota_release.signed_message(len(FIRMWARE), digest)
        assert len(msg) == 36
        assert msg[:4] == struct.pack("<I", len(FIRMWARE))
        assert msg[4:] == digest

    def test_sign_verify_roundtrip(self, private_key):
        sig = ota_release.sign_firmware(private_key, FIRMWARE)
        assert len(sig) == 64
        ota_release.verify_firmware(private_key.public_key(), FIRMWARE, sig)

    def test_verify_rejects_tampered_image(self, private_key):
        sig = ota_release.sign_firmware(private_key, FIRMWARE)
        with pytest.raises(InvalidSignature):
            ota_release.verify_firmware(
                private_key.public_key(), FIRMWARE + b"x", sig
            )

    def test_verify_rejects_wrong_key(self, private_key):
        sig = ota_release.sign_firmware(private_key, FIRMWARE)
        other = Ed25519PrivateKey.generate()
        with pytest.raises(InvalidSignature):
            ota_release.verify_firmware(other.public_key(), FIRMWARE, sig)

    def test_signed_message_rejects_bad_inputs(self):
        with pytest.raises(ValueError):
            ota_release.signed_message(0, b"\x00" * 32)
        with pytest.raises(ValueError):
            ota_release.signed_message(10, b"\x00" * 31)
        with pytest.raises(ValueError):
            ota_release.signed_message(2**32, b"\x00" * 32)


# ──────────────────────────────────────────────────────────────────────────
# Manifest build + verify
# ──────────────────────────────────────────────────────────────────────────

class TestManifest:
    def build(self, private_key, **overrides):
        kwargs = dict(
            private_key=private_key,
            firmware=FIRMWARE,
            product="securacv-canary",
            version="2.2.0",
            url="https://example.com/fw.bin",
            min_version="2.1.0",
            release_notes="Better updates.",
            release_url="https://example.com/notes",
        )
        kwargs.update(overrides)
        return ota_release.build_manifest(**kwargs)

    def test_roundtrip_verifies(self, private_key):
        manifest = self.build(private_key)
        problems = ota_release.verify_manifest(
            manifest, FIRMWARE, private_key.public_key()
        )
        assert problems == []

    def test_manifest_fields(self, private_key):
        manifest = self.build(private_key)
        assert manifest["manifest_version"] == 1
        assert manifest["product"] == "securacv-canary"
        assert manifest["version"] == "2.2.0"
        assert manifest["size"] == len(FIRMWARE)
        assert manifest["sha256"] == hashlib.sha256(FIRMWARE).hexdigest()
        assert len(manifest["signature"]) == 128
        assert manifest["signing_key_id"] == ota_release.signing_key_id(
            private_key.public_key()
        )

    def test_rejects_bad_semver(self, private_key):
        with pytest.raises(ValueError):
            self.build(private_key, version="2.2")
        with pytest.raises(ValueError):
            self.build(private_key, version="v2.2.0")

    def test_verify_catches_size_tamper(self, private_key):
        manifest = self.build(private_key)
        manifest["size"] += 1
        problems = ota_release.verify_manifest(
            manifest, FIRMWARE, private_key.public_key()
        )
        assert any("size mismatch" in p for p in problems)

    def test_verify_catches_sha_tamper(self, private_key):
        manifest = self.build(private_key)
        manifest["sha256"] = "0" * 64
        problems = ota_release.verify_manifest(
            manifest, FIRMWARE, private_key.public_key()
        )
        assert any("sha256 mismatch" in p for p in problems)

    def test_verify_catches_signature_tamper(self, private_key):
        manifest = self.build(private_key)
        sig = bytearray.fromhex(manifest["signature"])
        sig[0] ^= 0xFF
        manifest["signature"] = sig.hex()
        problems = ota_release.verify_manifest(
            manifest, FIRMWARE, private_key.public_key()
        )
        assert any("signature verification failed" in p for p in problems)

    def test_verify_catches_wrong_key(self, private_key):
        manifest = self.build(private_key)
        other = Ed25519PrivateKey.generate()
        problems = ota_release.verify_manifest(manifest, FIRMWARE, other.public_key())
        assert problems  # signature + key id mismatch


# ──────────────────────────────────────────────────────────────────────────
# Public key header generation / parsing
# ──────────────────────────────────────────────────────────────────────────

class TestPubkeyHeader:
    def test_header_contains_key_bytes(self, private_key):
        from cryptography.hazmat.primitives import serialization

        header = ota_release.pubkey_header(private_key.public_key())
        raw = private_key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw
        )
        for byte in raw:
            assert f"0x{byte:02x}" in header
        assert "SECURACV_OTA_RELEASE_PUBKEY" in header

    def test_header_roundtrips_through_loader(self, private_key, tmp_path):
        header_path = tmp_path / "ota_release_key.h"
        header_path.write_text(ota_release.pubkey_header(private_key.public_key()))
        loaded = ota_release.load_public_key(str(header_path))
        sig = ota_release.sign_firmware(private_key, FIRMWARE)
        ota_release.verify_firmware(loaded, FIRMWARE, sig)

    def test_load_public_key_from_hex(self, private_key):
        from cryptography.hazmat.primitives import serialization

        raw = private_key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw
        )
        loaded = ota_release.load_public_key(raw.hex())
        sig = ota_release.sign_firmware(private_key, FIRMWARE)
        ota_release.verify_firmware(loaded, FIRMWARE, sig)


# ──────────────────────────────────────────────────────────────────────────
# CLI end-to-end
# ──────────────────────────────────────────────────────────────────────────

class TestCli:
    def run_cli(self, *args):
        return subprocess.run(
            [sys.executable, str(Path(__file__).parent / "ota_release.py"), *args],
            capture_output=True,
            text=True,
        )

    def test_full_release_flow(self, tmp_path):
        key = tmp_path / "releaser.pem"
        fw = tmp_path / "firmware.bin"
        manifest = tmp_path / "manifest-canary.json"
        header = tmp_path / "ota_release_key.h"
        fw.write_bytes(FIRMWARE)

        assert self.run_cli("keygen", "--private-key", str(key)).returncode == 0
        assert (
            self.run_cli(
                "pubkey-header", "--private-key", str(key), "--out", str(header)
            ).returncode
            == 0
        )
        result = self.run_cli(
            "manifest",
            "--private-key", str(key),
            "--product", "securacv-canary",
            "--version", "2.2.0",
            "--url", "https://example.com/fw.bin",
            "--min-version", "2.1.0",
            "--release-notes", "Better updates.",
            "--out", str(manifest),
            str(fw),
        )
        assert result.returncode == 0, result.stderr

        # Verify against the C header (the artifact devices actually carry).
        result = self.run_cli(
            "verify", "--pubkey", str(header), "--manifest", str(manifest), str(fw)
        )
        assert result.returncode == 0, result.stderr
        assert "OK:" in result.stdout

        # Tampered binary must fail verification.
        fw.write_bytes(FIRMWARE + b"!")
        result = self.run_cli(
            "verify", "--pubkey", str(header), "--manifest", str(manifest), str(fw)
        )
        assert result.returncode == 1

    def test_index(self, tmp_path):
        out = tmp_path / "manifest-index.json"
        result = self.run_cli(
            "index",
            "securacv-canary=https://example.com/manifest-canary.json",
            "securacv-canary-wap=https://example.com/manifest-canary-wap.json",
            "--out", str(out),
        )
        assert result.returncode == 0, result.stderr
        index = json.loads(out.read_text())
        assert index["manifest_version"] == 1
        assert set(index["products"]) == {"securacv-canary", "securacv-canary-wap"}

    def test_keygen_refuses_overwrite(self, tmp_path):
        key = tmp_path / "releaser.pem"
        assert self.run_cli("keygen", "--private-key", str(key)).returncode == 0
        assert self.run_cli("keygen", "--private-key", str(key)).returncode != 0
