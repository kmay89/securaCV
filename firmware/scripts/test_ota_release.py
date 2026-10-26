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

    def test_manifest_message_cross_language_fixture(self):
        # Shared with firmware/common/ota/test_ota_logic.cpp — if either
        # implementation drifts, devices reject every release manifest.
        msg = ota_release.manifest_signed_message(
            product="securacv-canary",
            version="2.2.0",
            min_version="2.1.0",
            url="https://example.com/fw.bin",
            sha256_hex="AAbb" + "0" * 60,  # mixed case: must canonicalize lower
            size=123456,
            release_notes="Notes, with punctuation!",
            release_url="https://example.com/notes",
        )
        assert msg.hex() == (
            "7363762d6d616e69666573742d76310073656375726163762d63616e61727900322e322e3000322e312e300068747470733a2f2f6578616d706c652e636f6d2f66772e62696e006161626230303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303000313233343536004e6f7465732c20776974682070756e6374756174696f6e210068747470733a2f2f6578616d706c652e636f6d2f6e6f74657300"
        )

    def test_signed_message_rejects_bad_inputs(self):
        with pytest.raises(ValueError):
            ota_release.signed_message(0, b"\x00" * 32)
        with pytest.raises(ValueError):
            ota_release.signed_message(10, b"\x00" * 31)
        with pytest.raises(ValueError):
            ota_release.signed_message(2**32, b"\x00" * 32)


# ──────────────────────────────────────────────────────────────────────────
# BLE OTA protocol-v2 header: must match ble_ota_policy.h byte for byte
# ──────────────────────────────────────────────────────────────────────────

class TestBleOtaHeader:
    def test_canonical_message_cross_language_fixture(self):
        # Shared with firmware/projects/canary-wap/tests_host/
        # test_ble_ota_policy.cpp (test_canonical_fixture) — if either side
        # drifts, every device refuses every signed BLE header.
        msg = ota_release.ble_ota_signed_message(
            product="securacv-canary-wap",
            version="2.4.15-wap",
            size=123456,
            sha256_hex="AAbb" + "0" * 60,  # mixed case: must canonicalize lower
        )
        assert len(msg) == 118
        assert msg.hex() == (
            "7363762d626c652d6f74612d76320073656375726163762d63616e6172792d77617000"
            "322e342e31352d7761700031323334353600"
            "61616262303030303030303030303030303030303030303030303030303030303030"
            "30303030303030303030303030303030303030303030303030303030303000"
        )
        assert msg.startswith(b"scv-ble-ota-v2\0")
        # Distinct from the manifest domain: the same key never signs the
        # same bytes for two purposes.
        assert not msg.startswith(b"scv-manifest-v1")

    @pytest.mark.parametrize(
        "kwargs",
        [
            dict(product="", version="1.0.0", size=1, sha256_hex="00" * 32),
            dict(product="p" * 32, version="1.0.0", size=1, sha256_hex="00" * 32),
            dict(product="has space", version="1.0.0", size=1, sha256_hex="00" * 32),
            dict(product="ok", version="1.0.0\t", size=1, sha256_hex="00" * 32),
            dict(product="ok", version="1.0.0", size=0, sha256_hex="00" * 32),
            dict(product="ok", version="1.0.0", size=2**32, sha256_hex="00" * 32),
            dict(product="ok", version="1.0.0", size=1, sha256_hex="00" * 31),
            dict(product="ok", version="1.0.0", size=1, sha256_hex="zz" * 32),
        ],
    )
    def test_canonical_message_rejects_bad_fields(self, kwargs):
        with pytest.raises(ValueError):
            ota_release.ble_ota_signed_message(**kwargs)

    def test_header_layout(self, private_key):
        sig = ota_release.sign_ble_ota(
            private_key, product="securacv-canary-wap", version="2.4.15-wap", firmware=FIRMWARE
        )
        digest = hashlib.sha256(FIRMWARE).hexdigest()
        header = ota_release.build_ble_ota_header(
            product="securacv-canary-wap",
            version="2.4.15-wap",
            size=len(FIRMWARE),
            sha256_hex=digest,
            ble_signature=sig,
        )
        assert len(header) == 168 == ota_release.BLE_OTA_HEADER_SIZE
        assert header[0:2] == b"SC"
        assert header[2] == 2
        assert header[3] == 0
        assert header[4:36] == b"securacv-canary-wap".ljust(32, b"\0")
        assert header[36:68] == b"2.4.15-wap".ljust(32, b"\0")
        assert header[68:72] == struct.pack("<I", len(FIRMWARE))
        assert header[72:104] == bytes.fromhex(digest)
        assert header[104:168] == sig
        # The signature in the header verifies over the canonical message
        # rebuilt from the header's own fields — what the device does.
        private_key.public_key().verify(
            header[104:168],
            ota_release.ble_ota_signed_message(
                product="securacv-canary-wap",
                version="2.4.15-wap",
                size=len(FIRMWARE),
                sha256_hex=digest,
            ),
        )

    def test_header_rejects_bad_signature_length(self):
        with pytest.raises(ValueError):
            ota_release.build_ble_ota_header(
                product="p", version="1.0.0", size=1, sha256_hex="00" * 32,
                ble_signature=b"\0" * 63,
            )

    def test_manifest_carries_verifying_ble_signature(self, private_key):
        manifest = ota_release.build_manifest(
            private_key=private_key,
            firmware=FIRMWARE,
            product="securacv-canary-wap",
            version="2.4.16-wap",
            url="https://example.com/fw.bin",
        )
        assert "ble_signature" in manifest
        private_key.public_key().verify(
            bytes.fromhex(manifest["ble_signature"]),
            ota_release.ble_ota_signed_message(
                product="securacv-canary-wap",
                version="2.4.16-wap",
                size=len(FIRMWARE),
                sha256_hex=manifest["sha256"],
            ),
        )
        # The image signature and the BLE signature are different bytes
        # over different messages, even though one key made both.
        assert manifest["ble_signature"] != manifest["signature"]
        assert ota_release.verify_manifest(manifest, FIRMWARE, private_key.public_key()) == []

    def test_verify_requires_ble_signature(self, private_key):
        manifest = ota_release.build_manifest(
            private_key=private_key,
            firmware=FIRMWARE,
            product="securacv-canary-wap",
            version="2.4.16-wap",
            url="https://example.com/fw.bin",
        )
        stale = dict(manifest)
        del stale["ble_signature"]
        problems = ota_release.verify_manifest(stale, FIRMWARE, private_key.public_key())
        assert any("ble_signature" in p for p in problems)

        tampered = dict(manifest)
        tampered["ble_signature"] = ("0" if manifest["ble_signature"][0] != "0" else "1") + manifest["ble_signature"][1:]
        problems = ota_release.verify_manifest(tampered, FIRMWARE, private_key.public_key())
        assert any("ble_signature verification failed" in p for p in problems)

        # A version edit after signing is caught by BOTH the manifest and
        # the BLE signature: the header can't be re-labeled independently.
        relabeled = dict(manifest)
        relabeled["version"] = "2.4.17-wap"
        problems = ota_release.verify_manifest(relabeled, FIRMWARE, private_key.public_key())
        assert any("manifest_signature" in p for p in problems)
        assert any("ble_signature" in p for p in problems)

    def test_products_the_header_cannot_carry_sign_and_verify_without_a_ble_signature(self, private_key):
        # The longest real OTA product id (37 bytes) — seven display ids exceed
        # the header's 31-byte slot. Before ble_header_fits() the tool raised
        # here, which aborted the release workflow's signing step mid-loop.
        long_product = "securacv-canary-display-nightlight-c3"
        assert len(long_product.encode()) > ota_release.BLE_OTA_FIELD_WIDTH - 1
        assert not ota_release.ble_header_fits(long_product, "2.4.16")
        manifest = ota_release.build_manifest(
            private_key=private_key,
            firmware=FIRMWARE,
            product=long_product,
            version="2.4.16",
            url="https://example.com/fw.bin",
        )
        assert "ble_signature" not in manifest
        assert ota_release.verify_manifest(manifest, FIRMWARE, private_key.public_key()) == []
        # Every OTA product the firmware polls must either carry the header or
        # be provably unable to: the rule is the same function on both sides.
        for product in ("securacv-canary", "securacv-canary-wap", "securacv-canary-sense",
                        "securacv-canary-vision", "securacv-canary-display-dash",
                        "securacv-canary-display-dash-modes", "securacv-canary-display-touch169",
                        "securacv-canary-display-nightstand-c6"):
            m = ota_release.build_manifest(private_key=private_key, firmware=FIRMWARE,
                                           product=product, version="2.4.16",
                                           url="https://example.com/fw.bin")
            assert ("ble_signature" in m) == ota_release.ble_header_fits(product, "2.4.16")
            assert ota_release.verify_manifest(m, FIRMWARE, private_key.public_key()) == []


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
        with pytest.raises(ValueError):
            self.build(private_key, version="2.2-wap")

    def test_accepts_variant_suffix(self, private_key):
        # The WAP firmware versions itself "X.Y.Z-wap" — the manifest must
        # carry that exact string (the device string-compares it after the
        # install reboot), so the signer must accept it.
        manifest = self.build(private_key, version="2.2.0-wap",
                              product="securacv-canary-wap")
        assert manifest["version"] == "2.2.0-wap"
        problems = ota_release.verify_manifest(
            manifest, FIRMWARE, private_key.public_key()
        )
        assert problems == []

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

    def test_verify_catches_metadata_tamper(self, private_key):
        # The manifest signature must catch edits to fields the image
        # signature does NOT cover — release notes are what users read.
        manifest = self.build(private_key)
        manifest["release_notes"] = "Totally legit, please install."
        problems = ota_release.verify_manifest(
            manifest, FIRMWARE, private_key.public_key()
        )
        assert any("manifest_signature" in p for p in problems)

    def test_verify_tolerates_explicit_null_optionals(self, private_key):
        # An explicit JSON null in an optional field must degrade to a
        # signature mismatch report, not a crash in the verifier.
        manifest = self.build(private_key)
        manifest["release_url"] = None
        problems = ota_release.verify_manifest(
            manifest, FIRMWARE, private_key.public_key()
        )
        assert any("manifest_signature" in p for p in problems)

    def test_verify_requires_manifest_signature(self, private_key):
        manifest = self.build(private_key)
        del manifest["manifest_signature"]
        problems = ota_release.verify_manifest(
            manifest, FIRMWARE, private_key.public_key()
        )
        assert any("manifest_signature" in p for p in problems)

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

class TestManifestV2:
    """manifest_version 2 signs the board and chip the image was built for.

    An image is already bound to its product; the board and chip close the
    gap where several boards share one product id (or a release is cut for
    the wrong chip under the right id). They are only worth having UNDER the
    signature, so naming them makes a v2 manifest, a v1 manifest may not
    carry them, and the firmware refuses a mismatch (docs/OTA.md).
    """

    def build(self, private_key, **overrides):
        kwargs = dict(
            private_key=private_key,
            firmware=FIRMWARE,
            product="securacv-canary-sense",
            version="2.5.0",
            url="https://example.com/sense.bin",
            min_version="2.4.0",
            board="xiao-esp32c6-mr60",
            chip="esp32c6",
        )
        kwargs.update(overrides)
        return ota_release.build_manifest(**kwargs)

    def test_naming_hardware_makes_a_v2_manifest_that_verifies(self, private_key):
        manifest = self.build(private_key)
        assert manifest["manifest_version"] == 2
        assert manifest["board"] == "xiao-esp32c6-mr60"
        assert manifest["chip"] == "esp32c6"
        assert ota_release.verify_manifest(manifest, FIRMWARE, private_key.public_key()) == []

    def test_the_hardware_fields_are_under_the_signature(self, private_key):
        manifest = self.build(private_key)
        for field, other in (("board", "xiao-esp32s3-sense"), ("chip", "esp32s3")):
            tampered = dict(manifest)
            tampered[field] = other
            problems = ota_release.verify_manifest(tampered, FIRMWARE, private_key.public_key())
            assert "manifest_signature verification failed" in problems, (field, problems)

    def test_the_v2_message_is_the_v1_message_plus_hardware_under_its_own_prefix(self):
        common = dict(product="p", version="1.0.0", min_version="", url="u", sha256_hex="AB",
                      size=3, release_notes="", release_url="")
        v1 = ota_release.manifest_signed_message(**common)
        v2 = ota_release.manifest_signed_message(**common, board="b", chip="c", manifest_version=2)
        assert v1.startswith(b"scv-manifest-v1\0")
        assert v2 == b"scv-manifest-v2\0" + v1[len(b"scv-manifest-v1\0"):] + b"b\0c\0"

    def test_a_v1_manifest_may_not_carry_unsigned_hardware(self, private_key):
        with pytest.raises(ValueError):
            self.build(private_key, manifest_version=1)
        v1 = ota_release.build_manifest(private_key=private_key, firmware=FIRMWARE,
                                        product="securacv-canary-sense", version="2.5.0",
                                        url="https://example.com/sense.bin")
        assert "board" not in v1 and v1["manifest_version"] == 1
        smuggled = dict(v1, board="xiao-esp32c6-mr60")
        problems = ota_release.verify_manifest(smuggled, FIRMWARE, private_key.public_key())
        assert any("unsigned" in p for p in problems), problems

    def test_an_explicit_v2_without_hardware_signs_empty_fields(self, private_key):
        manifest = self.build(private_key, board=None, chip=None, manifest_version=2)
        assert manifest["manifest_version"] == 2
        assert manifest["board"] == "" and manifest["chip"] == ""
        assert ota_release.verify_manifest(manifest, FIRMWARE, private_key.public_key()) == []

    def test_an_unknown_manifest_version_is_refused(self, private_key):
        with pytest.raises(ValueError):
            self.build(private_key, manifest_version=3)
        manifest = self.build(private_key)
        manifest["manifest_version"] = 3
        problems = ota_release.verify_manifest(manifest, FIRMWARE, private_key.public_key())
        assert any("unknown manifest_version" in p for p in problems), problems

    def test_cli_board_and_chip(self, private_key, tmp_path):
        key = tmp_path / "releaser.pem"
        fw = tmp_path / "firmware.bin"
        out = tmp_path / "manifest.json"
        fw.write_bytes(FIRMWARE)
        signer = Path(__file__).parent / "ota_release.py"

        def run(*a):
            return subprocess.run([sys.executable, str(signer), *a], capture_output=True, text=True)

        assert run("keygen", "--private-key", str(key)).returncode == 0
        result = run("manifest", "--private-key", str(key), "--product", "securacv-canary-sense",
                     "--version", "2.5.0", "--url", "https://example.com/sense.bin",
                     "--board", "xiao-esp32c6-mr60", "--chip", "esp32c6", "--out", str(out), str(fw))
        assert result.returncode == 0, result.stderr
        manifest = json.loads(out.read_text())
        assert manifest["manifest_version"] == 2 and manifest["chip"] == "esp32c6"
        header = tmp_path / "ota_release_key.h"
        assert run("pubkey-header", "--private-key", str(key), "--out", str(header)).returncode == 0
        assert run("verify", "--pubkey", str(header), "--manifest", str(out), str(fw)).returncode == 0


# ──────────────────────────────────────────────────────────────────────────
# The device's buffers: what a 2.4.17+ engine can hold, modeled by the signer
# ──────────────────────────────────────────────────────────────────────────

REPO = Path(__file__).resolve().parents[2]
ENGINE_HEADER = REPO / "firmware/common/ota/src/securacv_ota.h"

# The seven published Canary Display ids the pre-2.4.17 char product[32]
# could not hold (32-37 bytes): every manifest for them failed its signature
# check on the device.
LONG_PUBLISHED_IDS = (
    "securacv-canary-display-dash-modes",
    "securacv-canary-display-nightstand7",
    "securacv-canary-display-nightstand-s3",
    "securacv-canary-display-touch169",
    "securacv-canary-display-amoled241",
    "securacv-canary-display-nightstand-c6",
    "securacv-canary-display-nightlight-c3",
)


def engine_manifest_buffers() -> dict[str, int]:
    """securacv_ota_manifest_t's char buffers, read out of the engine header."""
    import re

    text = ENGINE_HEADER.read_text(encoding="utf-8")
    defines = {name: int(value) for name, value in
               re.findall(r"^#define\s+(SECURACV_OTA_\w+_MAX)\s+(\d+)\s*$", text, re.M)}
    body = re.search(r"typedef struct \{(.*?)\} securacv_ota_manifest_t;", text, re.S)
    assert body, "securacv_ota_manifest_t not found in the engine header"
    out: dict[str, int] = {}
    for name, size in re.findall(r"\bchar\s+(\w+)\[(\w+)\];", body.group(1)):
        out[name] = int(size) if size.isdigit() else defines[size]
    return out


def declared_product_ids() -> set[str]:
    """Every OTA product id the build files declare (the C++ host test reads
    the same files): the shared env .ini files and each flavors.json dir."""
    import re

    # The token as a whole word (not SECURACV_OTA_PRODUCT_MAX), then a
    # "securacv-..." string on the same line: #define, -D build flag, or the
    # WAP sketch's `OTA_PRODUCT = "..."`.
    pattern = re.compile(r'OTA_PRODUCT(?![A-Za-z0-9_])[^"\n]*"(securacv-[A-Za-z0-9._-]+)')
    texts = [p.read_text(encoding="utf-8", errors="replace")
             for p in sorted((REPO / "firmware/envs/platformio").glob("*.ini"))]
    for flavor in json.loads((REPO / "firmware/flavors.json").read_text()):
        root = REPO / flavor["dir"]
        for path in root.rglob("*"):
            if ".pio" in path.parts or not path.is_file():
                continue
            if path.suffix in (".ini", ".h", ".hpp", ".ino", ".cpp"):
                texts.append(path.read_text(encoding="utf-8", errors="replace"))
    ids: set[str] = set()
    for text in texts:
        ids.update(pattern.findall(text))
    return ids


class TestDeviceBuffers:
    def build(self, private_key, **overrides):
        kwargs = dict(
            private_key=private_key,
            firmware=FIRMWARE,
            product="securacv-canary",
            version="2.4.17",
            url="https://example.com/fw.bin",
            release_notes="Updates confirm after a healthy minute.",
            release_url="https://example.com/notes",
        )
        kwargs.update(overrides)
        return ota_release.build_manifest(**kwargs)

    def test_the_table_is_the_engine_struct(self):
        # One number per buffer, in two languages: read the C header so they
        # cannot drift.
        assert engine_manifest_buffers() == ota_release.DEVICE_FIELD_BYTES

    def test_every_declared_product_id_fits_the_device(self):
        ids = declared_product_ids()
        assert "securacv-canary" in ids and "securacv-canary-wap" in ids
        assert set(LONG_PUBLISHED_IDS) <= ids, set(LONG_PUBLISHED_IDS) - ids
        cap = ota_release.DEVICE_FIELD_BYTES["product"]
        too_long = sorted(i for i in ids if len(i.encode()) >= cap)
        assert not too_long, too_long
        # ...and the seven are exactly the published ones the old buffer cut.
        assert all(len(i.encode()) >= ota_release.LEGACY_PRODUCT_BYTES for i in LONG_PUBLISHED_IDS)

    @pytest.mark.parametrize("product", LONG_PUBLISHED_IDS)
    def test_the_long_display_ids_sign_and_verify(self, private_key, product):
        manifest = self.build(private_key, product=product)
        assert ota_release.verify_manifest(manifest, FIRMWARE, private_key.public_key()) == []
        notes = ota_release.legacy_engine_notes(manifest)
        assert any("2.4.16" in n and "Flasher app" in n for n in notes), notes

    def test_a_short_id_carries_no_legacy_note(self, private_key):
        manifest = self.build(private_key)
        assert ota_release.legacy_engine_notes(manifest) == []

    def test_a_v2_manifest_notes_it_needs_2_4_17(self, private_key):
        manifest = self.build(private_key, board="xiao-esp32c6-mr60", chip="esp32c6")
        assert any("2.4.17" in n for n in ota_release.legacy_engine_notes(manifest))

    @pytest.mark.parametrize("field", sorted(ota_release.DEVICE_FIELD_BYTES))
    def test_verify_refuses_any_field_the_device_cannot_hold(self, private_key, field):
        cap = ota_release.DEVICE_FIELD_BYTES[field]
        manifest = self.build(private_key)
        manifest[field] = "x" * (cap - 1)
        assert not any(p.startswith(f"{field} is ") and "the device holds" in p for p in
                       ota_release.verify_manifest(manifest, FIRMWARE, private_key.public_key()))
        manifest[field] = "x" * cap
        problems = ota_release.verify_manifest(manifest, FIRMWARE, private_key.public_key())
        assert any(p.startswith(f"{field} is {cap} bytes") for p in problems), problems

    def test_bytes_not_characters(self, private_key):
        cap = ota_release.DEVICE_FIELD_BYTES["release_notes"]
        notes = "é" * ((cap - 1) // 2)  # two bytes each: fits
        assert len(notes.encode()) < cap
        self.build(private_key, release_notes=notes)
        notes += "é"                    # one character more, two bytes over
        assert len(notes) < cap <= len(notes.encode())
        with pytest.raises(ValueError, match="release_notes"):
            self.build(private_key, release_notes=notes)

    def test_the_signer_refuses_what_no_device_can_hold(self, private_key):
        cap = ota_release.DEVICE_FIELD_BYTES["product"]
        self.build(private_key, product="securacv-" + "p" * (cap - 1 - len("securacv-")))
        with pytest.raises(ValueError, match="product is 48 bytes"):
            self.build(private_key, product="securacv-" + "p" * (cap - len("securacv-")))

    def test_cli_verify_passes_a_long_id_and_says_who_cannot_read_it(self, tmp_path):
        key = tmp_path / "releaser.pem"
        fw = tmp_path / "firmware.bin"
        out = tmp_path / "manifest.json"
        header = tmp_path / "ota_release_key.h"
        fw.write_bytes(FIRMWARE)
        signer = Path(__file__).parent / "ota_release.py"

        def run(*a):
            return subprocess.run([sys.executable, str(signer), *a], capture_output=True, text=True)

        assert run("keygen", "--private-key", str(key)).returncode == 0
        assert run("pubkey-header", "--private-key", str(key), "--out", str(header)).returncode == 0
        result = run("manifest", "--private-key", str(key),
                     "--product", "securacv-canary-display-nightlight-c3", "--version", "2.4.17",
                     "--url", "https://example.com/fw.bin", "--out", str(out), str(fw))
        assert result.returncode == 0, result.stderr
        result = run("verify", "--pubkey", str(header), "--manifest", str(out), str(fw))
        assert result.returncode == 0, result.stderr
        assert "NOTE:" in result.stderr and "2.4.16" in result.stderr

        manifest = json.loads(out.read_text())
        manifest["release_notes"] = "n" * ota_release.DEVICE_FIELD_BYTES["release_notes"]
        out.write_text(json.dumps(manifest))
        result = run("verify", "--pubkey", str(header), "--manifest", str(out), str(fw))
        assert result.returncode == 1
        assert "release_notes is 512 bytes" in result.stderr


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

    def test_ble_header_cli(self, tmp_path):
        key = tmp_path / "releaser.pem"
        fw = tmp_path / "firmware.bin"
        manifest = tmp_path / "manifest-canary-wap.json"
        header = tmp_path / "ota_release_key.h"
        out = tmp_path / "begin_v2.bin"
        fw.write_bytes(FIRMWARE)
        assert self.run_cli("keygen", "--private-key", str(key)).returncode == 0
        assert self.run_cli(
            "pubkey-header", "--private-key", str(key), "--out", str(header)
        ).returncode == 0
        result = self.run_cli(
            "manifest",
            "--private-key", str(key),
            "--product", "securacv-canary-wap",
            "--version", "2.4.16-wap",
            "--url", "https://example.com/fw.bin",
            "--out", str(manifest),
            str(fw),
        )
        assert result.returncode == 0, result.stderr

        result = self.run_cli(
            "ble-header", "--manifest", str(manifest), "--pubkey", str(header),
            "--out", str(out), str(fw),
        )
        assert result.returncode == 0, result.stderr
        blob = out.read_bytes()
        assert len(blob) == 168
        assert blob[:2] == b"SC" and blob[2] == 2
        assert blob[4:36].rstrip(b"\0") == b"securacv-canary-wap"
        assert blob[68:72] == struct.pack("<I", len(FIRMWARE))
        assert blob[104:] == bytes.fromhex(json.loads(manifest.read_text())["ble_signature"])

        # Wrong binary for the manifest: refused before a header is written.
        fw.write_bytes(FIRMWARE + b"!")
        out.unlink()
        result = self.run_cli("ble-header", "--manifest", str(manifest), "--out", str(out), str(fw))
        assert result.returncode == 1
        assert not out.exists()

        # A pre-v2 manifest (no ble_signature) is named as the reason.
        fw.write_bytes(FIRMWARE)
        stale = json.loads(manifest.read_text())
        del stale["ble_signature"]
        manifest.write_text(json.dumps(stale))
        result = self.run_cli("ble-header", "--manifest", str(manifest), "--out", str(out), str(fw))
        assert result.returncode == 1
        assert "ble_signature" in result.stderr

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
