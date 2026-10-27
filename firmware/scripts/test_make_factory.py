"""Tests for make_factory.py — where the bootloader goes in a factory image.

fw-v2.4.17 published its two classic-ESP32 factory images (ESP32-CAM, WROOM
DevKit) with the second-stage bootloader at 0x0. Both flashers write a factory
image at flash 0x0, and a classic ESP32's mask ROM loads the bootloader from
0x1000, so neither board could have booted from them. make_factory.py now
takes the offset from --chip; these tests merge real images from small
synthetic parts and read the bytes back.

Run with: pytest firmware/scripts/test_make_factory.py -q

The merge itself runs esptool (as make_factory.py does). Without esptool
installed those tests skip; CI sets MAKE_FACTORY_ESPTOOL=require so a missing
esptool fails there instead of passing silently.
"""

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
MAKE_FACTORY = HERE / "make_factory.py"
sys.path.insert(0, str(HERE))

import build_flash_manifest  # noqa: E402
import make_factory  # noqa: E402

APP_OFFSET = 0x10000
HAVE_ESPTOOL = importlib.util.find_spec("esptool") is not None


@pytest.fixture()
def esptool_present():
    if HAVE_ESPTOOL:
        return
    if os.environ.get("MAKE_FACTORY_ESPTOOL") == "require":
        pytest.fail("esptool is not installed, and MAKE_FACTORY_ESPTOOL=require")
    pytest.skip("esptool is not installed (pip install esptool==4.12.0)")


def image(chip_id: int, body: bytes) -> bytes:
    """A minimal ESP image: 0xE9 magic, one segment, the chip id at byte 12."""
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 1
    struct.pack_into("<H", header, 12, chip_id)
    return bytes(header) + body


def partition_table() -> bytes:
    def entry(ptype, subtype, offset, size, label):
        return (struct.pack("<HBBII", 0x50AA, ptype, subtype, offset, size)
                + label.encode().ljust(16, b"\0") + b"\0" * 4)
    rows = entry(0x01, 0x02, 0x9000, 0x5000, "nvs") + entry(0x00, 0x00, APP_OFFSET, 0x100000, "factory")
    return rows + b"\xff" * (0xC00 - len(rows))


def build_dir(tmp_path: Path, chip: str) -> Path:
    chip_id = make_factory.ROM_BOOTLOADER[chip][1]
    d = tmp_path / f"build-{chip}"
    d.mkdir()
    (d / "bootloader.bin").write_bytes(image(chip_id, b"\x11" * 0x400))
    (d / "partitions.bin").write_bytes(partition_table())
    (d / "firmware.bin").write_bytes(image(chip_id, b"\x22" * 0x800))
    return d


def merge(build: Path, out: Path, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(MAKE_FACTORY), "--build-dir", str(build), "--out", str(out), *extra],
        capture_output=True, text=True)


# ── the table, held to esptool's own ─────────────────────────────────────────

def test_rom_table_matches_esptool():
    if not HAVE_ESPTOOL:
        if os.environ.get("MAKE_FACTORY_ESPTOOL") == "require":
            pytest.fail("esptool is not installed, and MAKE_FACTORY_ESPTOOL=require")
        pytest.skip("esptool is not installed")
    from esptool.targets import CHIP_DEFS
    for chip, (offset, chip_id) in make_factory.ROM_BOOTLOADER.items():
        rom = CHIP_DEFS[chip]
        assert offset == rom.BOOTLOADER_FLASH_OFFSET, chip
        assert chip_id == rom.IMAGE_CHIP_ID, chip


def test_classic_esp32_and_s2_load_from_0x1000_the_rest_from_0x0():
    assert make_factory.bootloader_offset("esp32", None) == 0x1000
    assert make_factory.bootloader_offset("esp32s2", None) == 0x1000
    for chip in ("esp32s3", "esp32c3", "esp32c6"):
        assert make_factory.bootloader_offset(chip, None) == 0x0, chip


def test_every_catalog_chip_has_a_rom_offset():
    # flash.json is the catalog both release paths build from; a chip it names
    # that the table does not list would drop that product from every release.
    catalog = json.loads((REPO / "canary-local/devices/flash.json").read_text(encoding="utf-8"))
    chips = {build_flash_manifest.esptool_chip(p["chip"]) for p in catalog["products"]}
    assert chips <= set(make_factory.ROM_BOOTLOADER), chips - set(make_factory.ROM_BOOTLOADER)


# ── merged images, read back ─────────────────────────────────────────────────

def test_esp32_factory_image_has_its_bootloader_at_0x1000(tmp_path, esptool_present):
    out = tmp_path / "esp32-factory.bin"
    r = merge(build_dir(tmp_path, "esp32"), out, "--chip", "esp32")
    assert r.returncode == 0, r.stdout + r.stderr
    data = out.read_bytes()
    assert data[0x1000] == 0xE9, "the classic ESP32 ROM loads the bootloader from 0x1000"
    assert struct.unpack_from("<H", data, 0x1000 + 12)[0] == 0
    assert data[:0x1000] == b"\xff" * 0x1000, "nothing but erased flash below the bootloader"
    assert data[0x8000:0x8002] == b"\xaa\x50"
    assert data[APP_OFFSET] == 0xE9


@pytest.mark.parametrize("chip", ["esp32s3", "esp32c3", "esp32c6"])
def test_s3_c3_c6_factory_images_keep_their_bootloader_at_0x0(tmp_path, esptool_present, chip):
    out = tmp_path / f"{chip}-factory.bin"
    r = merge(build_dir(tmp_path, chip), out, "--chip", chip)
    assert r.returncode == 0, r.stdout + r.stderr
    data = out.read_bytes()
    assert data[0] == 0xE9
    assert struct.unpack_from("<H", data, 12)[0] == make_factory.ROM_BOOTLOADER[chip][1]
    assert data[0x8000:0x8002] == b"\xaa\x50"
    assert data[APP_OFFSET] == 0xE9


def test_esp32s2_factory_image_has_its_bootloader_at_0x1000(tmp_path, esptool_present):
    out = tmp_path / "esp32s2-factory.bin"
    r = merge(build_dir(tmp_path, "esp32s2"), out, "--chip", "esp32s2")
    assert r.returncode == 0, r.stdout + r.stderr
    assert out.read_bytes()[0x1000] == 0xE9


def test_chip_auto_reads_the_bootloader_header(tmp_path, esptool_present):
    out = tmp_path / "auto-factory.bin"
    r = merge(build_dir(tmp_path, "esp32"), out, "--chip", "auto")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "esp32 bootloader @ 0x1000" in r.stdout
    assert out.read_bytes()[0x1000] == 0xE9


def test_release_command_inherits_the_chip_default(tmp_path, esptool_present):
    # build_flash_manifest.py passes --chip and no --bootloader-offset: the
    # fix has to reach the release path through the chip alone.
    product = {"id": "securacv-canary-esp32cam", "chip": "ESP32"}
    out = tmp_path / "canary-esp32cam-factory.bin"
    cmd = build_flash_manifest.make_factory_cmd(product, {"toolchain": "pio", "dir": "unused"}, out)
    assert "--bootloader-offset" not in cmd
    assert cmd[cmd.index("--chip") + 1] == "esp32"
    build = build_dir(tmp_path, "esp32")
    cmd[cmd.index("--build-dir") + 1] = str(build)
    cmd[0] = sys.executable
    subprocess.run(cmd, check=True, capture_output=True)
    assert out.read_bytes()[0x1000] == 0xE9


# ── refusals (no esptool needed: they stop before the merge) ─────────────────

def test_a_chip_the_bootloader_disagrees_with_is_refused(tmp_path):
    # dev_flash.sh used to fall back to esp32s3 for an env with no
    # board_build.mcu, which is exactly the classic-ESP32 reach envs.
    out = tmp_path / "wrong.bin"
    r = merge(build_dir(tmp_path, "esp32"), out, "--chip", "esp32s3", "--dry-run")
    assert r.returncode != 0
    assert "built for esp32" in r.stderr
    assert not out.exists()


def test_a_bootloader_without_the_image_magic_is_refused(tmp_path):
    build = build_dir(tmp_path, "esp32")
    (build / "bootloader.bin").write_bytes(b"\xff" * 0x400)
    r = merge(build, tmp_path / "x.bin", "--chip", "esp32", "--dry-run")
    assert r.returncode != 0
    assert "not an ESP image" in r.stderr


def test_a_chip_with_no_known_offset_needs_it_named(tmp_path):
    # The P4 and the C5 load from 0x2000: an unlisted chip is never given 0x0.
    build = build_dir(tmp_path, "esp32")
    (build / "bootloader.bin").write_bytes(image(18, b"\x11" * 0x400))
    r = merge(build, tmp_path / "p4.bin", "--chip", "esp32p4", "--dry-run")
    assert r.returncode != 0
    assert "--bootloader-offset" in r.stderr
    r = merge(build, tmp_path / "p4.bin", "--chip", "esp32p4", "--bootloader-offset", "0x2000", "--dry-run")
    assert r.returncode == 0, r.stderr
    assert " 0x2000 " in r.stdout


def test_an_explicit_offset_still_overrides_and_says_so(tmp_path):
    r = merge(build_dir(tmp_path, "esp32"), tmp_path / "o.bin",
              "--chip", "esp32", "--bootloader-offset", "0x0", "--dry-run")
    assert r.returncode == 0, r.stderr
    assert "esp32 bootloader @ 0x0," in r.stdout
    assert "overrides the esp32 ROM's 0x1000" in r.stderr


def test_the_merged_layout_check_refuses_the_2417_shape():
    # The shape fw-v2.4.17 published for the ESP32-CAM and the WROOM DevKit:
    # an ESP32 bootloader at 0x0 and nothing at 0x1000.
    broken = bytearray(b"\xff" * (APP_OFFSET + 0x100))
    broken[:0x400] = image(0, b"\x11" * (0x400 - 24))
    broken[0x8000:0x8000 + 0xC00] = partition_table()
    broken[APP_OFFSET] = 0xE9
    with pytest.raises(SystemExit, match="not an ESP image"):
        make_factory.check_factory_layout(bytes(broken), 0x1000, "esp32", APP_OFFSET)
    # The check reads where it is told; where that is comes from
    # bootloader_offset(), which says 0x1000 for this chip.
    assert make_factory.bootloader_offset("esp32", None) == 0x1000


def test_dev_flash_falls_back_to_auto_not_a_guessed_chip():
    sh = (HERE / "dev_flash.sh").read_text(encoding="utf-8")
    assert "get('board_build.mcu','auto')" in sh
    assert "|| echo auto)" in sh
    assert "esp32s3'" not in sh
