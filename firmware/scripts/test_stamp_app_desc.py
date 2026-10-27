"""Tests for stamp_app_desc.py — the product and version a release writes into
each app image's esp_app_desc_t.

Up to fw-v2.4.17 every SecuraCV image's descriptor named the Arduino core
(`arduino-lib-builder`, `esp-idf: v4.4.7 38eeba213a`), so neither flasher
could say what a connected board runs. The release now stamps the product's
asset_stem and the firmware's version into it after the build. A wrong byte
here is an image the bootloader refuses, so these tests build real images
with esptool from synthetic parts, stamp them, and read them back with
esptool's image_info (checksum, hash, name, version) and byte for byte.

Run with: pytest firmware/scripts/test_stamp_app_desc.py -q

The esptool tests skip without esptool; CI sets MAKE_FACTORY_ESPTOOL=require
(the switch the factory merge tests beside these use) so a missing esptool
fails there instead. A real release image round-trips too when
STAMP_APP_DESC_FACTORY names one or more downloaded factory images
(`<asset_stem>-<version>-factory.bin`, separated like PATH): the app is cut
out of each at its partition table's app offset, stamped, and read back.
"""

import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
STAMP = HERE / "stamp_app_desc.py"
sys.path.insert(0, str(HERE))

import build_flash_manifest  # noqa: E402
import make_factory  # noqa: E402
import stamp_app_desc as sad  # noqa: E402

HAVE_ESPTOOL = importlib.util.find_spec("esptool") is not None
CHIPS = ["esp32", "esp32s3", "esp32c3", "esp32c6"]
ARDUINO_VERSION = b"esp-idf: v4.4.7 38eeba213a"
ARDUINO_PROJECT = b"arduino-lib-builder"


@pytest.fixture()
def esptool_present():
    if HAVE_ESPTOOL:
        return
    if os.environ.get("MAKE_FACTORY_ESPTOOL") == "require":
        pytest.fail("esptool is not installed, and MAKE_FACTORY_ESPTOOL=require")
    pytest.skip("esptool is not installed (pip install esptool==4.12.0)")


def app_desc(version: bytes = ARDUINO_VERSION, project: bytes = ARDUINO_PROJECT) -> bytes:
    """An esp_app_desc_t the way the Arduino core's libraries leave it."""
    d = bytearray(sad.DESC_LEN)
    struct.pack_into("<II", d, 0, sad.DESC_MAGIC, 3)          # magic, secure_version
    d[16:16 + len(version)] = version
    d[48:48 + len(project)] = project
    d[80:88] = b"12:29:20"
    d[96:107] = b"Mar  5 2024"
    d[112:124] = b"v4.4.7-dirty"
    d[144:176] = bytes(range(0x40, 0x60))                     # app_elf_sha256
    d[176:256] = bytes(range(0x80, 0xD0))                     # efuse revs, mmu page size, reserved
    return bytes(d)


DROM_TAIL = b"\0canary-vision\x002.4.18\0" + bytes(range(256)) * 3


def esptool_image(tmp_path: Path, chip: str, *, hashed: bool = True,
                  desc: bytes | None = None, tail: bytes = DROM_TAIL) -> Path:
    """A real app image, written by esptool's own image classes: the
    descriptor at the start of a flash-mapped rodata segment (image offset
    0x20, as elf2image puts it) and a RAM segment after it."""
    from esptool.bin_image import ELFSection
    from esptool.targets import CHIP_DEFS
    rom = CHIP_DEFS[chip]
    dram = next(lo for lo, _, kind in rom.MEMORY_MAP if kind == "DRAM")
    img = rom.BOOTLOADER_IMAGE()
    img.entrypoint = rom.IROM_MAP_START
    img.append_digest = hashed
    img.segments = [ELFSection(b".flash.appdesc", rom.DROM_MAP_START + 0x20, (desc or app_desc()) + tail),
                    ELFSection(b".dram0.data", dram, b"\x13" * 0x101)]
    path = tmp_path / f"{chip}-{'hashed' if hashed else 'plain'}.bin"
    img.save(str(path))
    return path


def hand_image(*, chip_id: int = 5, hashed: bool = True, desc: bytes | None = None,
               tail: bytes = DROM_TAIL) -> bytes:
    """The same layout written by hand, for the refusals (no esptool needed).
    test_the_hand_built_image_is_one_esptool_accepts holds it to esptool."""
    segments = [(0x3C000020, (desc or app_desc()) + tail), (0x3FC80000, b"\x13" * 0x101)]
    out = bytearray(24)
    out[0], out[1], out[2], out[3] = 0xE9, len(segments), 2, 0x2F
    struct.pack_into("<I", out, 4, 0x42000020)
    out[8] = 0xEE                                             # wp pin: disabled
    struct.pack_into("<HBHH", out, 12, chip_id, 0, 0, 0xFFFF)
    out[23] = 1 if hashed else 0
    c = 0xEF
    for addr, data in segments:
        out += struct.pack("<II", addr, len(data)) + data
        for b in data:
            c ^= b
    out += bytes(15 - len(out) % 16) + bytes([c])
    if hashed:
        out += hashlib.sha256(out).digest()
    return bytes(out)


def desc_of(image: bytes) -> bytes:
    return image[sad.DESC_OFFSET:sad.DESC_OFFSET + sad.DESC_LEN]


def esptool_says(path: Path) -> dict:
    r = subprocess.run([sys.executable, "-m", "esptool", "image_info", "--version", "2", str(path)],
                       capture_output=True, text=True, check=True)
    return dict(line.split(": ", 1) for line in r.stdout.splitlines() if ": " in line)


# ── stamped, then read back by esptool ───────────────────────────────────────

@pytest.mark.parametrize("chip", CHIPS)
def test_a_stamped_image_is_one_esptool_calls_valid(tmp_path, esptool_present, chip):
    path = esptool_image(tmp_path, chip)
    before = path.read_bytes()
    assert esptool_says(path)["Project name"] == "arduino-lib-builder"

    assert sad.stamp_file(path, "canary-vision", "2.4.18") == []
    after = path.read_bytes()
    said = esptool_says(path)
    assert said["Checksum"].endswith("(valid)")
    assert said["Validation hash"].endswith("(valid)")
    assert said["Project name"] == "canary-vision"
    assert said["App version"] == "2.4.18"
    assert sad.identity(after) == ("canary-vision", "2.4.18")
    assert len(after) == len(before)


@pytest.mark.parametrize("chip", CHIPS)
def test_only_the_two_fields_the_checksum_and_the_hash_move(tmp_path, esptool_present, chip):
    path = esptool_image(tmp_path, chip)
    before = path.read_bytes()
    sad.stamp_file(path, "canary-vision", "2.4.18")
    after = path.read_bytes()
    lay = sad.layout(after)
    moved = {i for i in range(len(before)) if before[i] != after[i]}
    allowed = set(range(sad.DESC_OFFSET + 16, sad.DESC_OFFSET + 80))   # version + project_name
    allowed |= {lay.checksum_at} | set(range(lay.hash_at, lay.hash_at + 32))
    assert moved and moved <= allowed, sorted(moved - allowed)[:8]
    # secure_version, time, date, idf_ver, app_elf_sha256 and the tail of
    # the struct are exactly what the build wrote.
    assert desc_of(after)[:16] == desc_of(before)[:16]
    assert desc_of(after)[80:] == desc_of(before)[80:]
    # The fields keep their NUL padding: no byte of the old name survives.
    assert desc_of(after)[48:80] == b"canary-vision".ljust(32, b"\0")
    assert desc_of(after)[16:48] == b"2.4.18".ljust(32, b"\0")


def test_an_image_without_an_appended_hash_gets_none(tmp_path, esptool_present):
    path = esptool_image(tmp_path, "esp32c3", hashed=False)
    size = path.stat().st_size
    assert sad.layout(path.read_bytes()).hash_at is None
    assert sad.stamp_file(path, "canary-vision", "2.4.18") == []
    assert path.stat().st_size == size
    said = esptool_says(path)
    assert said["Checksum"].endswith("(valid)")
    assert "Validation hash" not in said


def test_stamping_again_changes_nothing(tmp_path, esptool_present):
    path = esptool_image(tmp_path, "esp32s3")
    sad.stamp_file(path, "canary-vision", "2.4.18")
    once = path.read_bytes()
    assert sad.stamp(once, "canary-vision", "2.4.18") == once
    sad.stamp_file(path, "canary-vision", "2.4.18")
    assert path.read_bytes() == once


def test_a_31_character_name_fills_the_field_and_keeps_its_nul(tmp_path, esptool_present):
    name = "c" * 31
    path = esptool_image(tmp_path, "esp32c6", tail=DROM_TAIL)
    assert sad.stamp_file(path, name, "2.4.18") == []
    assert esptool_says(path)["Project name"] == name
    assert desc_of(path.read_bytes())[48 + 31] == 0


def test_the_command_line_stamps_in_place(tmp_path, esptool_present):
    path = esptool_image(tmp_path, "esp32")
    r = subprocess.run([sys.executable, str(STAMP), "image", "--project", "canary-wroom",
                        "--version", "2.4.18", str(path)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert esptool_says(path)["Project name"] == "canary-wroom"


def test_the_hand_built_image_is_one_esptool_accepts(tmp_path, esptool_present):
    # The refusal tests below start from hand_image(); this keeps it honest.
    path = tmp_path / "hand.bin"
    path.write_bytes(hand_image())
    said = esptool_says(path)
    assert said["Checksum"].endswith("(valid)") and said["Validation hash"].endswith("(valid)")
    assert said["Project name"] == "arduino-lib-builder"


# ── what it refuses (no esptool needed: nothing is written) ─────────────────

def test_the_hand_built_image_stamps():
    stamped = sad.stamp(hand_image(), "canary-vision", "2.4.18")
    sad.check_integrity(stamped, sad.layout(stamped))
    assert sad.identity(stamped) == ("canary-vision", "2.4.18")


def corrupt(image: bytes, at: int, value: int) -> bytes:
    b = bytearray(image)
    b[at] = value
    return bytes(b)


@pytest.mark.parametrize("image, why", [
    (b"\xff" * 64, "not an ESP image"),
    (corrupt(hand_image(), 12, 0x77), "chip id 119"),
    (corrupt(hand_image(), 1, 0), "0 segments"),
    (corrupt(hand_image(), 1, 17), "17 segments"),
    (corrupt(hand_image(), 23, 2), "hash_appended is 0x02"),
    (hand_image()[:-1], "truncated"),
    (hand_image() + b"\0", "something appended"),
    (hand_image(desc=b"\0" * 256), "no esp_app_desc_t"),
    (hand_image(desc=app_desc()[:200], tail=b""), "too short for esp_app_desc_t"),
])
def test_an_image_of_the_wrong_shape_is_refused(image, why):
    with pytest.raises(sad.Refused, match=why):
        sad.stamp(image, "canary-vision", "2.4.18")


def test_a_segment_that_overruns_the_file_is_refused():
    image = bytearray(hand_image())
    struct.pack_into("<I", image, 24 + 4, 0x7FFFFFFF)          # segment 0's data_len
    with pytest.raises(sad.Refused, match="segment 0 .* runs past the end"):
        sad.layout(bytes(image))


def test_an_image_already_corrupt_is_refused_not_resealed():
    image = hand_image()
    lay = sad.layout(image)
    with pytest.raises(sad.Refused, match="checksum byte"):
        sad.stamp(corrupt(image, lay.checksum_at, image[lay.checksum_at] ^ 1), "canary", "2.4.18")
    with pytest.raises(sad.Refused, match="SHA-256"):
        sad.stamp(corrupt(image, lay.hash_at, image[lay.hash_at] ^ 1), "canary", "2.4.18")
    # A flipped data byte: the checksum catches it before the stamp would
    # recompute both and bless the damage.
    with pytest.raises(sad.Refused, match="checksum byte"):
        sad.stamp(corrupt(image, sad.DESC_OFFSET + 300, image[sad.DESC_OFFSET + 300] ^ 1), "canary", "2.4.18")


@pytest.mark.parametrize("name, value, why", [
    ("project_name", "c" * 32, "32 characters; the field holds 31"),
    ("version", "2.4.18-" + "x" * 25, "32 characters"),
    ("project_name", "", "not printable ASCII"),
    ("project_name", "canary\0wap", "not printable ASCII"),
    ("version", "2.4.18\n", "not printable ASCII"),
    ("project_name", "canäry", "not printable ASCII"),
])
def test_a_value_the_field_cannot_hold_is_refused(name, value, why):
    args = {"project_name": "canary", "version": "2.4.18", name: value}
    with pytest.raises(sad.Refused, match=why):
        sad.stamp(hand_image(), args["project_name"], args["version"])


def test_a_version_the_firmware_does_not_carry_is_never_stamped():
    # The release's `strings | grep -Fx VERSION` guards would pass on the
    # stamp itself, so the stamp is written only over a literal the firmware
    # already carries — never as a whole run inside a longer string, and
    # never satisfied by the descriptor's own field.
    with pytest.raises(sad.NotCarried):
        sad.stamp(hand_image(), "canary-vision", "2.4.19")
    with pytest.raises(sad.NotCarried):
        sad.stamp(hand_image(), "canary-vision", "2.4.1")
    only_in_desc = hand_image(desc=app_desc(version=b"2.4.20"), tail=b"\0no literal here\0" + bytes(300))
    with pytest.raises(sad.NotCarried):
        sad.stamp(only_in_desc, "canary-vision", "2.4.20")
    stamped = sad.stamp(hand_image(), "canary-vision", "2.4.18")
    with pytest.raises(sad.NotCarried):
        sad.stamp(stamped, "canary-vision", "2.4.19")


# ── the release pass ─────────────────────────────────────────────────────────

def test_release_stamps_every_built_product_and_skips_what_did_not_build(tmp_path, esptool_present, capsys):
    pio = tmp_path / "pio-vision"
    pio.mkdir()
    esptool_image(tmp_path, "esp32c3").rename(pio / "firmware.bin")
    wap = tmp_path / "wap-build"
    wap.mkdir()
    esptool_image(tmp_path, "esp32s3", tail=b"\x002.4.18-wap\0" + bytes(300)).rename(wap / "canary_wap.ino.bin")
    old = tmp_path / "pio-old"
    old.mkdir()
    esptool_image(tmp_path, "esp32", tail=b"\x002.4.9\0" + bytes(300)).rename(old / "firmware.bin")
    catalog = {"products": [
        {"id": "securacv-canary-vision", "asset_stem": "canary-vision"},
        {"id": "securacv-canary-wap", "asset_stem": "canary-wap"},
        {"id": "securacv-canary-old", "asset_stem": "canary-old"},
        {"id": "securacv-canary-missing", "asset_stem": "canary-missing"},
        {"id": "securacv-canary-unmapped", "asset_stem": "canary-unmapped"},
    ]}
    builds = {
        "securacv-canary-vision": {"toolchain": "pio", "dir": str(pio), "vsuffix": ""},
        "securacv-canary-wap": {"toolchain": "arduino", "dir": str(wap), "sketch": "canary_wap", "vsuffix": "-wap"},
        "securacv-canary-old": {"toolchain": "pio", "dir": str(old), "vsuffix": ""},
        "securacv-canary-missing": {"toolchain": "pio", "dir": str(tmp_path / "nope"), "vsuffix": ""},
    }
    assert sad.release("2.4.18", catalog, builds) == 0
    out = capsys.readouterr().out
    assert sad.identity((pio / "firmware.bin").read_bytes()) == ("canary-vision", "2.4.18")
    assert sad.identity((wap / "canary_wap.ino.bin").read_bytes()) == ("canary-wap", "2.4.18-wap")
    # A forgotten version bump is left for the version guards to name.
    assert sad.identity((old / "firmware.bin").read_bytes()) == ("arduino-lib-builder", ARDUINO_VERSION.decode())
    assert "::warning::securacv-canary-old: left unstamped" in out
    assert "securacv-canary-missing: nothing built" in out
    assert "unmapped" not in out


def test_release_fails_on_an_image_it_cannot_stamp(tmp_path, capsys):
    d = tmp_path / "pio"
    d.mkdir()
    (d / "firmware.bin").write_bytes(hand_image()[:-1])
    catalog = {"products": [{"id": "securacv-canary", "asset_stem": "canary"}]}
    builds = {"securacv-canary": {"toolchain": "pio", "dir": str(d), "vsuffix": ""}}
    assert sad.release("2.4.18", catalog, builds) == 1
    assert "::error::securacv-canary" in capsys.readouterr().out


def test_release_reads_the_app_where_the_factory_merge_does():
    # The bytes stamped are the bytes make_factory.py merges and the OTA
    # step copies: one path helper serves both.
    for pid, build in build_flash_manifest.BUILD.items():
        cmd = build_flash_manifest.make_factory_cmd({"chip": "ESP32-S3"}, build, Path("/tmp/x.bin"))
        app = build_flash_manifest.app_path(build)
        if build["toolchain"] == "pio":
            assert cmd[cmd.index("--build-dir") + 1] == str(app.parent), pid
        else:
            assert cmd[cmd.index("--app") + 1] == str(app), pid


# ── a real release image, when one is at hand ────────────────────────────────

def real_factories() -> list[Path]:
    raw = os.environ.get("STAMP_APP_DESC_FACTORY", "")
    return [Path(p) for p in raw.split(os.pathsep) if p]


@pytest.mark.skipif(not real_factories(), reason="STAMP_APP_DESC_FACTORY names no downloaded factory image")
@pytest.mark.parametrize("factory", real_factories(), ids=lambda p: p.name)
def test_a_real_release_app_round_trips(tmp_path, esptool_present, factory):
    catalog = json.loads((REPO / "canary-local/devices/flash.json").read_text(encoding="utf-8"))
    stem, version = factory.name.removesuffix("-factory.bin").rsplit("-", 1)
    product = next(p for p in catalog["products"] if p["asset_stem"] == stem)
    want = version + build_flash_manifest.BUILD[product["id"]]["vsuffix"]
    data = factory.read_bytes()
    app_off, app_size, _ = make_factory.parse_app_offset(data[0x8000:0x8C00])
    # The app's own length, from its segment headers; the slot after it is erased flash.
    count, pos = data[app_off + 1], app_off + sad.HEADER_LEN
    for _ in range(count):
        pos += sad.SEGMENT_HEADER_LEN + struct.unpack_from("<I", data, pos + 4)[0]
    end = pos + 15 - pos % 16 + 1 + (sad.HASH_LEN if data[app_off + sad.HASH_APPENDED_AT] else 0)
    app = tmp_path / "app.bin"
    app.write_bytes(data[app_off:end])
    before = esptool_says(app)
    assert before["Checksum"].endswith("(valid)"), before

    assert sad.stamp_file(app, stem, want) == []
    after = esptool_says(app)
    assert (after["Project name"], after["App version"]) == (stem, want)
    for key in ("ELF file SHA256", "Compile time", "ESP-IDF", "Secure version"):
        assert after[key] == before[key], key
